/****************************************************************************
*
*    Copyright (C) 2005 - 2014 by Vivante Corp.
*
*    This program is free software; you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation; either version 2 of the license, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not write to the Free Software
*    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*****************************************************************************/



#include "gc_hal_kernel_precomp.h"

#define _GC_OBJ_ZONE    gcvZONE_MMU

typedef enum _gceMMU_TYPE
{
    gcvMMU_USED     = (0 << 4),
    gcvMMU_SINGLE   = (1 << 4),
    gcvMMU_FREE     = (2 << 4),
}
gceMMU_TYPE;

#define gcmENTRY_TYPE(x) (x & 0xF0)

#define gcdMMU_TABLE_DUMP       0

#define gcdUSE_MMU_EXCEPTION    1

/*
    gcdMMU_CLEAR_VALUE

        The clear value for the entry of the old MMU.
*/
#ifndef gcdMMU_CLEAR_VALUE
#   define gcdMMU_CLEAR_VALUE                   0x00000ABC
#endif

/*VIV: Start GPU address for gcvSURF_VERTEX.  */
#define gcdVERTEX_START      (128 << 10)

typedef struct _gcsMMU_STLB *gcsMMU_STLB_PTR;

typedef struct _gcsMMU_STLB
{
    gctPHYS_ADDR    physical;
    gctUINT32_PTR   logical;
    gctSIZE_T       size;
    gctUINT32       physBase;
    gctSIZE_T       pageCount;
    gctUINT32       mtlbIndex;
    gctUINT32       mtlbEntryNum;
    gcsMMU_STLB_PTR next;
} gcsMMU_STLB;

#if gcdSHARED_PAGETABLE
typedef struct _gcsSharedPageTable * gcsSharedPageTable_PTR;
typedef struct _gcsSharedPageTable
{
    /* Shared gckMMU object. */
    gckMMU          mmu;

    /* Hardwares which use this shared pagetable. */
    gckHARDWARE     hardwares[gcdMAX_GPU_COUNT];

    /* Number of cores use this shared pagetable. */
    gctUINT32       reference;
}
gcsSharedPageTable;

static gcsSharedPageTable_PTR sharedPageTable = gcvNULL;
#endif

#if gcdMIRROR_PAGETABLE
typedef struct _gcsMirrorPageTable * gcsMirrorPageTable_PTR;
typedef struct _gcsMirrorPageTable
{
    /* gckMMU objects. */
    gckMMU          mmus[gcdMAX_GPU_COUNT];

    /* Hardwares which use this shared pagetable. */
    gckHARDWARE     hardwares[gcdMAX_GPU_COUNT];

    /* Number of cores use this shared pagetable. */
    gctUINT32       reference;
}
gcsMirrorPageTable;

static gcsMirrorPageTable_PTR mirrorPageTable = gcvNULL;
static gctPOINTER mirrorPageTableMutex = gcvNULL;
#endif

typedef struct _gcsDynamicSpaceNode * gcsDynamicSpaceNode_PTR;
typedef struct _gcsDynamicSpaceNode
{
    gctUINT32       start;
    gctINT32        entries;
}
gcsDynamicSpaceNode;

static void
_WritePageEntry(
    IN gctUINT32_PTR PageEntry,
    IN gctUINT32     EntryValue
    )
{
    static gctUINT16 data = 0xff00;

    if (*(gctUINT8 *)&data == 0xff)
    {
        *PageEntry = gcmSWAB32(EntryValue);
    }
    else
    {
        *PageEntry = EntryValue;
    }
}

static gctUINT32
_ReadPageEntry(
    IN gctUINT32_PTR PageEntry
    )
{
    static gctUINT16 data = 0xff00;
    gctUINT32 entryValue;

    if (*(gctUINT8 *)&data == 0xff)
    {
        entryValue = *PageEntry;
        return gcmSWAB32(entryValue);
    }
    else
    {
        return *PageEntry;
    }
}

static gceSTATUS
_FillPageTable(
    IN gctUINT32_PTR PageTable,
    IN gctUINT32     PageCount,
    IN gctUINT32     EntryValue
)
{
    gctUINT i;

    for (i = 0; i < PageCount; i++)
    {
        _WritePageEntry(PageTable + i, EntryValue);
    }

    return gcvSTATUS_OK;
}

static gceSTATUS
_Link(
    IN gckMMU Mmu,
    IN gctUINT32 Index,
    IN gctUINT32 Next
    )
{
    if (Index >= Mmu->pageTableEntries)
    {
        /* Just move heap pointer. */
        Mmu->heapList = Next;
    }
    else
    {
        /* Address page table. */
        gctUINT32_PTR map = Mmu->mapLogical;

        /* Dispatch on node type. */
        switch (gcmENTRY_TYPE(_ReadPageEntry(&map[Index])))
        {
        case gcvMMU_SINGLE:
            /* Set single index. */
            _WritePageEntry(&map[Index], (Next << 8) | gcvMMU_SINGLE);
            break;

        case gcvMMU_FREE:
            /* Set index. */
            _WritePageEntry(&map[Index + 1], Next);
            break;

        default:
            gcmkFATAL("MMU table correcupted at index %u!", Index);
            return gcvSTATUS_HEAP_CORRUPTED;
        }
    }

    /* Success. */
    return gcvSTATUS_OK;
}

static gceSTATUS
_AddFree(
    IN gckMMU Mmu,
    IN gctUINT32 Index,
    IN gctUINT32 Node,
    IN gctUINT32 Count
    )
{
    gctUINT32_PTR map = Mmu->mapLogical;

    if (Count == 1)
    {
        /* Initialize a single page node. */
        _WritePageEntry(map + Node, (~((1U<<8)-1)) | gcvMMU_SINGLE);
    }
    else
    {
        /* Initialize the node. */
        _WritePageEntry(map + Node + 0, (Count << 8) | gcvMMU_FREE);
        _WritePageEntry(map + Node + 1, ~0U);
    }

    /* Append the node. */
    return _Link(Mmu, Index, Node);
}

static gceSTATUS
_Collect(
    IN gckMMU Mmu
    )
{
    gctUINT32_PTR map = Mmu->mapLogical;
    gceSTATUS status;
    gctUINT32 i, previous, start = 0, count = 0;

    previous = Mmu->heapList = ~0U;
    Mmu->freeNodes = gcvFALSE;

    /* Walk the entire page table. */
    for (i = 0; i < Mmu->pageTableEntries; ++i)
    {
        /* Dispatch based on type of page. */
        switch (gcmENTRY_TYPE(_ReadPageEntry(&map[i])))
        {
        case gcvMMU_USED:
            /* Used page, so close any open node. */
            if (count > 0)
            {
                /* Add the node. */
                gcmkONERROR(_AddFree(Mmu, previous, start, count));

                /* Reset the node. */
                previous = start;
                count    = 0;
            }
            break;

        case gcvMMU_SINGLE:
            /* Single free node. */
            if (count++ == 0)
            {
                /* Start a new node. */
                start = i;
            }
            break;

        case gcvMMU_FREE:
            /* A free node. */
            if (count == 0)
            {
                /* Start a new node. */
                start = i;
            }

            /* Advance the count. */
            count += _ReadPageEntry(&map[i]) >> 8;

            /* Advance the index into the page table. */
            i     += (_ReadPageEntry(&map[i]) >> 8) - 1;
            break;

        default:
            gcmkFATAL("MMU page table correcupted at index %u!", i);
            return gcvSTATUS_HEAP_CORRUPTED;
        }
    }

    /* See if we have an open node left. */
    if (count > 0)
    {
        /* Add the node to the list. */
        gcmkONERROR(_AddFree(Mmu, previous, start, count));
    }

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_MMU,
                   "Performed a garbage collection of the MMU heap.");

    /* Success. */
    return gcvSTATUS_OK;

OnError:
    /* Return the staus. */
    return status;
}

static gctUINT32
_SetPage(gctUINT32 PageAddress)
{
    return PageAddress
           /* writable */
           | (1 << 2)
           /* Ignore exception */
           | (0 << 1)
           /* Present */
           | (1 << 0);
}

#if gcdPROCESS_ADDRESS_SPACE
gctUINT32
_AddressToIndex(
    IN gckMMU Mmu,
    IN gctUINT32 Address
    )
{
    gctUINT32 mtlbOffset = (Address & gcdMMU_MTLB_MASK) >> gcdMMU_MTLB_SHIFT;
    gctUINT32 stlbOffset = (Address & gcdMMU_STLB_4K_MASK) >> gcdMMU_STLB_4K_SHIFT;

    return (mtlbOffset - Mmu->dynamicMappingStart) * gcdMMU_STLB_4K_ENTRY_NUM + stlbOffset;
}

gctUINT32
_MtlbOffset(
    gctUINT32 Address
    )
{
    return (Address & gcdMMU_MTLB_MASK) >> gcdMMU_MTLB_SHIFT;
}

gctUINT32
_StlbOffset(
    gctUINT32 Address
    )
{
    return (Address & gcdMMU_STLB_4K_MASK) >> gcdMMU_STLB_4K_SHIFT;
}

static gceSTATUS
_AllocateStlb(
    IN gckOS Os,
    OUT gcsMMU_STLB_PTR *Stlb
    )
{
    gceSTATUS status;
    gcsMMU_STLB_PTR stlb;
    gctPOINTER pointer;

    /* Allocate slave TLB record. */
    gcmkONERROR(gckOS_Allocate(Os, gcmSIZEOF(gcsMMU_STLB), &pointer));
    stlb = pointer;

    stlb->size = gcdMMU_STLB_4K_SIZE;

    /* Allocate slave TLB entries. */
    gcmkONERROR(gckOS_AllocateContiguous(
        Os,
        gcvFALSE,
        &stlb->size,
        &stlb->physical,
        (gctPOINTER)&stlb->logical
        ));

    gcmkONERROR(gckOS_GetPhysicalAddress(Os, stlb->logical, &stlb->physBase));

#if gcdUSE_MMU_EXCEPTION
    _FillPageTable(stlb->logical, stlb->size / 4, gcdMMU_STLB_EXCEPTION);
#else
    gckOS_ZeroMemory(stlb->logical, stlb->size);
#endif

    *Stlb = stlb;

    return gcvSTATUS_OK;

OnError:
    return status;
}

gceSTATUS
_SetupProcessAddressSpace(
    IN gckMMU Mmu
    )
{
    gceSTATUS status;
    gctINT numEntries = 0;
    gctUINT32_PTR map;

    numEntries = gcdPROCESS_ADDRESS_SPACE_SIZE
               /* Address space mapped by one MTLB entry. */
               / (1 << gcdMMU_MTLB_SHIFT);

    Mmu->dynamicMappingStart = 0;

    Mmu->pageTableSize = numEntries * 4096;

    Mmu->pageTableEntries = Mmu->pageTableSize / gcmSIZEOF(gctUINT32);

    gcmkONERROR(gckOS_Allocate(Mmu->os,
                               Mmu->pageTableSize,
                               (void **)&Mmu->mapLogical));

    /* Initilization. */
    map      = Mmu->mapLogical;
    _WritePageEntry(map,     (Mmu->pageTableEntries << 8) | gcvMMU_FREE);
    _WritePageEntry(map + 1, ~0U);
    Mmu->heapList  = 0;
    Mmu->freeNodes = gcvFALSE;

    return gcvSTATUS_OK;

OnError:
    return status;
}
#else
static gceSTATUS
_FillFlatMapping(
    IN gckMMU Mmu,
    IN gctUINT32 PhysBase,
    OUT gctSIZE_T Size
    )
{
    gceSTATUS status;
    gctBOOL mutex = gcvFALSE;
    gcsMMU_STLB_PTR head = gcvNULL, pre = gcvNULL;
    gctUINT32 start = PhysBase & (~gcdMMU_PAGE_64K_MASK);
    gctUINT32 end = (PhysBase + Size - 1) & (~gcdMMU_PAGE_64K_MASK);
    gctUINT32 mStart = start >> gcdMMU_MTLB_SHIFT;
    gctUINT32 mEnd = end >> gcdMMU_MTLB_SHIFT;
    gctUINT32 sStart = (start & gcdMMU_STLB_64K_MASK) >> gcdMMU_STLB_64K_SHIFT;
    gctUINT32 sEnd = (end & gcdMMU_STLB_64K_MASK) >> gcdMMU_STLB_64K_SHIFT;
    gctBOOL ace = gckHARDWARE_IsFeatureAvailable(Mmu->hardware, gcvFEATURE_ACE);

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(Mmu->os, Mmu->pageTableMutex, gcvINFINITE));
    mutex = gcvTRUE;

    while (mStart <= mEnd)
    {
        gcmkASSERT(mStart < gcdMMU_MTLB_ENTRY_NUM);
        if (*(Mmu->mtlbLogical + mStart) == 0)
        {
            gcsMMU_STLB_PTR stlb;
            gctPOINTER pointer = gcvNULL;
            gctUINT32 last = (mStart == mEnd) ? sEnd : (gcdMMU_STLB_64K_ENTRY_NUM - 1);
            gctUINT32 mtlbEntry;

            gcmkONERROR(gckOS_Allocate(Mmu->os, sizeof(struct _gcsMMU_STLB), &pointer));
            stlb = pointer;

            stlb->mtlbEntryNum = 0;
            stlb->next = gcvNULL;
            stlb->physical = gcvNULL;
            stlb->logical = gcvNULL;
            stlb->size = gcdMMU_STLB_64K_SIZE;
            stlb->pageCount = 0;

            if (pre == gcvNULL)
            {
                pre = head = stlb;
            }
            else
            {
                gcmkASSERT(pre->next == gcvNULL);
                pre->next = stlb;
                pre = stlb;
            }

            gcmkONERROR(
                    gckOS_AllocateContiguous(Mmu->os,
                                             gcvFALSE,
                                             &stlb->size,
                                             &stlb->physical,
                                             (gctPOINTER)&stlb->logical));

            gcmkONERROR(gckOS_ZeroMemory(stlb->logical, stlb->size));

            gcmkONERROR(gckOS_GetPhysicalAddress(
                Mmu->os,
                stlb->logical,
                &stlb->physBase));

            if (stlb->physBase & (gcdMMU_STLB_64K_SIZE - 1))
            {
                gcmkONERROR(gcvSTATUS_NOT_ALIGNED);
            }

            mtlbEntry = stlb->physBase
                      /* 64KB page size */
                      | (1 << 2)
                      /* Ignore exception */
                      | (0 << 1)
                      /* Present */
                      | (1 << 0);

            if (ace)
            {
                mtlbEntry = mtlbEntry
                          /* Secure */
                          | (1 << 4);
            }

            _WritePageEntry(Mmu->mtlbLogical + mStart, mtlbEntry);

#if gcdMMU_TABLE_DUMP
            gckOS_Print("%s(%d): insert MTLB[%d]: %08x\n",
                __FUNCTION__, __LINE__,
                mStart,
                _ReadPageEntry(Mmu->mtlbLogical + mStart));
#endif

            stlb->mtlbIndex = mStart;
            stlb->mtlbEntryNum = 1;
#if gcdMMU_TABLE_DUMP
            gckOS_Print("%s(%d): STLB: logical:%08x -> physical:%08x\n",
                    __FUNCTION__, __LINE__,
                    stlb->logical,
                    stlb->physBase);
#endif

            while (sStart <= last)
            {
                gcmkASSERT(!(start & gcdMMU_PAGE_64K_MASK));
                _WritePageEntry(stlb->logical + sStart, _SetPage(start));
#if gcdMMU_TABLE_DUMP
                gckOS_Print("%s(%d): insert STLB[%d]: %08x\n",
                    __FUNCTION__, __LINE__,
                    sStart,
                    _ReadPageEntry(stlb->logical + sStart));
#endif
                /* next page. */
                start += gcdMMU_PAGE_64K_SIZE;
                sStart++;
                stlb->pageCount++;
            }

            sStart = 0;
            ++mStart;
        }
        else
        {
            gcmkONERROR(gcvSTATUS_INVALID_REQUEST);
        }
    }

    /* Insert the stlb into staticSTLB. */
    if (Mmu->staticSTLB == gcvNULL)
    {
        Mmu->staticSTLB = head;
    }
    else
    {
        gcmkASSERT(pre == gcvNULL);
        gcmkASSERT(pre->next == gcvNULL);
        pre->next = Mmu->staticSTLB;
        Mmu->staticSTLB = head;
    }

    /* Release the mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));

    return gcvSTATUS_OK;

OnError:

    /* Roll back. */
    while (head != gcvNULL)
    {
        pre = head;
        head = head->next;

        if (pre->physical != gcvNULL)
        {
            gcmkVERIFY_OK(
                gckOS_FreeContiguous(Mmu->os,
                    pre->physical,
                    pre->logical,
                    pre->size));
        }

        if (pre->mtlbEntryNum != 0)
        {
            gcmkASSERT(pre->mtlbEntryNum == 1);
            _WritePageEntry(Mmu->mtlbLogical + pre->mtlbIndex, 0);
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Mmu->os, pre));
    }

    if (mutex)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));
    }

    return status;
}

static gceSTATUS
_FindDynamicSpace(
    IN gckMMU Mmu,
    OUT gcsDynamicSpaceNode_PTR *Array,
    OUT gctINT * Size
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gctPOINTER pointer = gcvNULL;
    gcsDynamicSpaceNode_PTR array = gcvNULL;
    gctINT size = 0;
    gctINT i = 0, nodeStart = -1, nodeEntries = 0;

    /* Allocate memory for the array. */
    gcmkONERROR(gckOS_Allocate(Mmu->os,
                               gcmSIZEOF(*array) * (gcdMMU_MTLB_ENTRY_NUM / 2),
                               &pointer));

    array = (gcsDynamicSpaceNode_PTR)pointer;

    /* Loop all the entries. */
    while (i < gcdMMU_MTLB_ENTRY_NUM)
    {
        if (!Mmu->mtlbLogical[i])
        {
            if (nodeStart < 0)
            {
                /* This is the first entry of the dynamic space. */
                nodeStart   = i;
                nodeEntries = 1;
            }
            else
            {
                /* Other entries of the dynamic space. */
                nodeEntries++;
            }
        }
        else if (nodeStart >= 0)
        {
            /* Save the previous node. */
            array[size].start   = nodeStart;
            array[size].entries = nodeEntries;
            size++;

            /* Reset the start. */
            nodeStart   = -1;
            nodeEntries = 0;
        }

        i++;
    }

    /* Save the previous node. */
    if (nodeStart >= 0)
    {
        array[size].start   = nodeStart;
        array[size].entries = nodeEntries;
        size++;
    }

#if gcdMMU_TABLE_DUMP
    for (i = 0; i < size; i++)
    {
        gckOS_Print("%s(%d): [%d]: start=%d, entries=%d.\n",
                __FUNCTION__, __LINE__,
                i,
                array[i].start,
                array[i].entries);
    }
#endif

    *Array = array;
    *Size  = size;

    return gcvSTATUS_OK;

OnError:
    if (pointer != gcvNULL)
    {
        gckOS_Free(Mmu->os, pointer);
    }

    return status;
}

static gceSTATUS
_SetupDynamicSpace(
    IN gckMMU Mmu
    )
{
    gceSTATUS status;
    gcsDynamicSpaceNode_PTR nodeArray = gcvNULL;
    gctINT i, nodeArraySize = 0;
    gctUINT32 physical;
    gctINT numEntries = 0;
    gctUINT32_PTR map;
    gctBOOL acquired = gcvFALSE;
    gctUINT32 mtlbEntry;
    gctBOOL ace = gckHARDWARE_IsFeatureAvailable(Mmu->hardware, gcvFEATURE_ACE);

    /* Find all the dynamic address space. */
    gcmkONERROR(_FindDynamicSpace(Mmu, &nodeArray, &nodeArraySize));

    /* TODO: We only use the largest one for now. */
    for (i = 0; i < nodeArraySize; i++)
    {
        if (nodeArray[i].entries > numEntries)
        {
            Mmu->dynamicMappingStart = nodeArray[i].start;
            numEntries               = nodeArray[i].entries;
        }
    }

    gckOS_Free(Mmu->os, (gctPOINTER)nodeArray);

    Mmu->pageTableSize = numEntries * 4096;

    gcmkSAFECASTSIZET(Mmu->pageTableEntries, Mmu->pageTableSize / gcmSIZEOF(gctUINT32));

    gcmkONERROR(gckOS_Allocate(Mmu->os,
                               Mmu->pageTableSize,
                               (void **)&Mmu->mapLogical));

    /* Construct Slave TLB. */
    gcmkONERROR(gckOS_AllocateContiguous(Mmu->os,
                gcvFALSE,
                &Mmu->pageTableSize,
                &Mmu->pageTablePhysical,
                (gctPOINTER)&Mmu->pageTableLogical));

#if gcdUSE_MMU_EXCEPTION
    gcmkONERROR(_FillPageTable(Mmu->pageTableLogical,
                               Mmu->pageTableEntries,
                               /* Enable exception */
                               1 << 1));
#else
    /* Invalidate all entries. */
    gcmkONERROR(gckOS_ZeroMemory(Mmu->pageTableLogical,
                Mmu->pageTableSize));
#endif

    /* Initilization. */
    map      = Mmu->mapLogical;
    _WritePageEntry(map,     (Mmu->pageTableEntries << 8) | gcvMMU_FREE);
    _WritePageEntry(map + 1, ~0U);
    Mmu->heapList  = 0;
    Mmu->freeNodes = gcvFALSE;

    gcmkONERROR(gckOS_GetPhysicalAddress(Mmu->os,
                Mmu->pageTableLogical,
                &physical));

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(Mmu->os, Mmu->pageTableMutex, gcvINFINITE));
    acquired = gcvTRUE;

    /* Map to Master TLB. */
    for (i = (gctINT)Mmu->dynamicMappingStart;
         i < (gctINT)Mmu->dynamicMappingStart + numEntries;
         i++)
    {
        mtlbEntry = physical
                  /* 4KB page size */
                  | (0 << 2)
                  /* Ignore exception */
                  | (0 << 1)
                  /* Present */
                  | (1 << 0);

        if (ace)
        {
            mtlbEntry = mtlbEntry
                      /* Secure */
                      | (1 << 4);
        }

        _WritePageEntry(Mmu->mtlbLogical + i, mtlbEntry);

#if gcdMMU_TABLE_DUMP
        gckOS_Print("%s(%d): insert MTLB[%d]: %08x\n",
                __FUNCTION__, __LINE__,
                i,
                _ReadPageEntry(Mmu->mtlbLogical + i));
#endif
        physical += gcdMMU_STLB_4K_SIZE;
    }

    /* Release the mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));

    return gcvSTATUS_OK;

OnError:
    if (Mmu->mapLogical)
    {
        gcmkVERIFY_OK(
            gckOS_Free(Mmu->os, (gctPOINTER) Mmu->mapLogical));


        gcmkVERIFY_OK(
            gckOS_FreeContiguous(Mmu->os,
                                 Mmu->pageTablePhysical,
                                 (gctPOINTER) Mmu->pageTableLogical,
                                 Mmu->pageTableSize));
    }

    if (acquired)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));
    }

    return status;
}
#endif

/*******************************************************************************
**
**  _Construct
**
**  Construct a new gckMMU object.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gctSIZE_T MmuSize
**          Number of bytes for the page table.
**
**  OUTPUT:
**
**      gckMMU * Mmu
**          Pointer to a variable that receives the gckMMU object pointer.
*/
gceSTATUS
_Construct(
    IN gckKERNEL Kernel,
    IN gctSIZE_T MmuSize,
    OUT gckMMU * Mmu
    )
{
    gckOS os;
    gckHARDWARE hardware;
    gceSTATUS status;
    gckMMU mmu = gcvNULL;
    gctUINT32_PTR map;
    gctPOINTER pointer = gcvNULL;
#if gcdPROCESS_ADDRESS_SPACE
    gctUINT32 i;
#endif

    gcmkHEADER_ARG("Kernel=0x%x MmuSize=%lu", Kernel, MmuSize);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Kernel, gcvOBJ_KERNEL);
    gcmkVERIFY_ARGUMENT(MmuSize > 0);
    gcmkVERIFY_ARGUMENT(Mmu != gcvNULL);

    /* Extract the gckOS object pointer. */
    os = Kernel->os;
    gcmkVERIFY_OBJECT(os, gcvOBJ_OS);

    /* Extract the gckHARDWARE object pointer. */
    hardware = Kernel->hardware;
    gcmkVERIFY_OBJECT(hardware, gcvOBJ_HARDWARE);

    /* Allocate memory for the gckMMU object. */
    gcmkONERROR(gckOS_Allocate(os, sizeof(struct _gckMMU), &pointer));

    mmu = pointer;

    /* Initialize the gckMMU object. */
    mmu->object.type      = gcvOBJ_MMU;
    mmu->os               = os;
    mmu->hardware         = hardware;
    mmu->pageTableMutex   = gcvNULL;
    mmu->pageTableLogical = gcvNULL;
    mmu->mtlbLogical      = gcvNULL;
    mmu->staticSTLB       = gcvNULL;
    mmu->enabled          = gcvFALSE;
    mmu->mapLogical       = gcvNULL;

    /* Create the page table mutex. */
    gcmkONERROR(gckOS_CreateMutex(os, &mmu->pageTableMutex));

    if (hardware->mmuVersion == 0)
    {
        mmu->pageTableSize = MmuSize;

        /* Construct address space management table. */
        gcmkONERROR(gckOS_Allocate(mmu->os,
                                   mmu->pageTableSize,
                                   &pointer));

        mmu->mapLogical = pointer;

        /* Construct page table read by GPU. */
        gcmkONERROR(gckOS_AllocateContiguous(mmu->os,
                    gcvFALSE,
                    &mmu->pageTableSize,
                    &mmu->pageTablePhysical,
                    (gctPOINTER)&mmu->pageTableLogical));


        /* Compute number of entries in page table. */
        gcmkSAFECASTSIZET(mmu->pageTableEntries, mmu->pageTableSize / sizeof(gctUINT32));

        /* Mark all pages as free. */
        map      = mmu->mapLogical;

#if gcdMMU_CLEAR_VALUE
        _FillPageTable(mmu->pageTableLogical, mmu->pageTableEntries, gcdMMU_CLEAR_VALUE);
#endif

        _WritePageEntry(map,     (mmu->pageTableEntries << 8) | gcvMMU_FREE);
        _WritePageEntry(map + 1, ~0U);
        mmu->heapList  = 0;
        mmu->freeNodes = gcvFALSE;

        /* Set page table address. */
        gcmkONERROR(
            gckHARDWARE_SetMMU(hardware, (gctPOINTER) mmu->pageTableLogical));
    }
    else
    {
        /* Allocate the 4K mode MTLB table. */
        mmu->mtlbSize = gcdMMU_MTLB_SIZE + 64;

        gcmkONERROR(
            gckOS_AllocateContiguous(os,
                                     gcvFALSE,
                                     &mmu->mtlbSize,
                                     &mmu->mtlbPhysical,
                                     &pointer));

        mmu->mtlbLogical = pointer;

#if gcdPROCESS_ADDRESS_SPACE
        _FillPageTable(pointer, mmu->mtlbSize / 4, gcdMMU_MTLB_EXCEPTION);

        /* Allocate a array to store stlbs. */
        gcmkONERROR(gckOS_Allocate(os, mmu->mtlbSize, &mmu->stlbs));

        gckOS_ZeroMemory(mmu->stlbs, mmu->mtlbSize);

        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
            gcmkONERROR(gckOS_AtomConstruct(os, &mmu->pageTableDirty[i]));
        }

        _SetupProcessAddressSpace(mmu);
#else
        /* Invalid all the entries. */
        gcmkONERROR(
            gckOS_ZeroMemory(pointer, mmu->mtlbSize));
#endif
    }

    /* Return the gckMMU object pointer. */
    *Mmu = mmu;

    /* Success. */
    gcmkFOOTER_ARG("*Mmu=0x%x", *Mmu);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    if (mmu != gcvNULL)
    {
        if (mmu->mapLogical != gcvNULL)
        {
            gcmkVERIFY_OK(
                gckOS_Free(os, (gctPOINTER) mmu->mapLogical));


            gcmkVERIFY_OK(
                gckOS_FreeContiguous(os,
                                     mmu->pageTablePhysical,
                                     (gctPOINTER) mmu->pageTableLogical,
                                     mmu->pageTableSize));
        }

        if (mmu->mtlbLogical != gcvNULL)
        {
            gcmkVERIFY_OK(
                gckOS_FreeContiguous(os,
                                     mmu->mtlbPhysical,
                                     (gctPOINTER) mmu->mtlbLogical,
                                     mmu->mtlbSize));
        }

        if (mmu->pageTableMutex != gcvNULL)
        {
            /* Delete the mutex. */
            gcmkVERIFY_OK(
                gckOS_DeleteMutex(os, mmu->pageTableMutex));
        }

        /* Mark the gckMMU object as unknown. */
        mmu->object.type = gcvOBJ_UNKNOWN;

        /* Free the allocates memory. */
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, mmu));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  _Destroy
**
**  Destroy a gckMMU object.
**
**  INPUT:
**
**      gckMMU Mmu
**          Pointer to an gckMMU object.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
_Destroy(
    IN gckMMU Mmu
    )
{
#if gcdPROCESS_ADDRESS_SPACE
    gctUINT32 i;
#endif
    gcmkHEADER_ARG("Mmu=0x%x", Mmu);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);

    while (Mmu->staticSTLB != gcvNULL)
    {
        gcsMMU_STLB_PTR pre = Mmu->staticSTLB;
        Mmu->staticSTLB = pre->next;

        if (pre->physical != gcvNULL)
        {
            gcmkVERIFY_OK(
                gckOS_FreeContiguous(Mmu->os,
                    pre->physical,
                    pre->logical,
                    pre->size));
        }

        if (pre->mtlbEntryNum != 0)
        {
            gcmkASSERT(pre->mtlbEntryNum == 1);
            _WritePageEntry(Mmu->mtlbLogical + pre->mtlbIndex, 0);
#if gcdMMU_TABLE_DUMP
            gckOS_Print("%s(%d): clean MTLB[%d]\n",
                __FUNCTION__, __LINE__,
                pre->mtlbIndex);
#endif
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Mmu->os, pre));
    }

    if (Mmu->hardware->mmuVersion != 0)
    {
        gcmkVERIFY_OK(
                gckOS_FreeContiguous(Mmu->os,
                    Mmu->mtlbPhysical,
                    (gctPOINTER) Mmu->mtlbLogical,
                    Mmu->mtlbSize));
    }

    /* Free address space management table. */
    if (Mmu->mapLogical != gcvNULL)
    {
        gcmkVERIFY_OK(
            gckOS_Free(Mmu->os, (gctPOINTER) Mmu->mapLogical));
    }

    if (Mmu->pageTableLogical != gcvNULL)
    {
        /* Free page table. */
        gcmkVERIFY_OK(
            gckOS_FreeContiguous(Mmu->os,
                                 Mmu->pageTablePhysical,
                                 (gctPOINTER) Mmu->pageTableLogical,
                                 Mmu->pageTableSize));
    }

    /* Delete the page table mutex. */
    gcmkVERIFY_OK(gckOS_DeleteMutex(Mmu->os, Mmu->pageTableMutex));

#if gcdPROCESS_ADDRESS_SPACE
    for (i = 0; i < Mmu->mtlbSize / 4; i++)
    {
        struct _gcsMMU_STLB *stlb = ((struct _gcsMMU_STLB **)Mmu->stlbs)[i];

        if (stlb)
        {
            gcmkVERIFY_OK(gckOS_FreeContiguous(
                Mmu->os,
                stlb->physical,
                stlb->logical,
                stlb->size));

            gcmkOS_SAFE_FREE(Mmu->os, stlb);
        }
    }

    gcmkOS_SAFE_FREE(Mmu->os, Mmu->stlbs);
#endif

    /* Mark the gckMMU object as unknown. */
    Mmu->object.type = gcvOBJ_UNKNOWN;

    /* Free the gckMMU object. */
    gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Mmu->os, Mmu));

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
** _AdjstIndex
**
**  Adjust the index from which we search for a usable node to make sure
**  index allocated is greater than Start.
*/
gceSTATUS
_AdjustIndex(
    IN gckMMU Mmu,
    IN gctUINT32 Index,
    IN gctUINT32 PageCount,
    IN gctUINT32 Start,
    OUT gctUINT32 * IndexAdjusted
    )
{
    gceSTATUS status;
    gctUINT32 index = Index;
    gctUINT32_PTR map = Mmu->mapLogical;

    gcmkHEADER();

    for (; index < Mmu->pageTableEntries;)
    {
        gctUINT32 result = 0;
        gctUINT32 nodeSize = 0;

        if (index >= Start)
        {
            break;
        }

        switch (gcmENTRY_TYPE(map[index]))
        {
        case gcvMMU_SINGLE:
            nodeSize = 1;
            break;

        case gcvMMU_FREE:
            nodeSize = map[index] >> 8;
            break;

        default:
            gcmkFATAL("MMU table correcupted at index %u!", index);
            gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
        }

        if (nodeSize > PageCount)
        {
            result = index + (nodeSize - PageCount);

            if (result >= Start)
            {
                break;
            }
        }

        switch (gcmENTRY_TYPE(map[index]))
        {
        case gcvMMU_SINGLE:
            index = map[index] >> 8;
            break;

        case gcvMMU_FREE:
            index = map[index + 1];
            break;

        default:
            gcmkFATAL("MMU table correcupted at index %u!", index);
            gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
        }
    }

    *IndexAdjusted = index;

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckMMU_Construct(
    IN gckKERNEL Kernel,
    IN gctSIZE_T MmuSize,
    OUT gckMMU * Mmu
    )
{
#if gcdSHARED_PAGETABLE
    gceSTATUS status;
    gctPOINTER pointer;

    gcmkHEADER_ARG("Kernel=0x%08x", Kernel);

    if (sharedPageTable == gcvNULL)
    {
        gcmkONERROR(
                gckOS_Allocate(Kernel->os,
                               sizeof(struct _gcsSharedPageTable),
                               &pointer));
        sharedPageTable = pointer;

        gcmkONERROR(
                gckOS_ZeroMemory(sharedPageTable,
                    sizeof(struct _gcsSharedPageTable)));

        gcmkONERROR(_Construct(Kernel, MmuSize, &sharedPageTable->mmu));
    }
    else if (Kernel->hardware->mmuVersion == 0)
    {
        /* Set page table address. */
        gcmkONERROR(
            gckHARDWARE_SetMMU(Kernel->hardware, (gctPOINTER) sharedPageTable->mmu->pageTableLogical));
    }

    *Mmu = sharedPageTable->mmu;

    sharedPageTable->hardwares[sharedPageTable->reference] = Kernel->hardware;

    sharedPageTable->reference++;

    gcmkFOOTER_ARG("sharedPageTable->reference=%lu", sharedPageTable->reference);
    return gcvSTATUS_OK;

OnError:
    if (sharedPageTable)
    {
        if (sharedPageTable->mmu)
        {
            gcmkVERIFY_OK(gckMMU_Destroy(sharedPageTable->mmu));
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Kernel->os, sharedPageTable));
    }

    gcmkFOOTER();
    return status;
#elif gcdMIRROR_PAGETABLE
    gceSTATUS status;
    gctPOINTER pointer;

    gcmkHEADER_ARG("Kernel=0x%08x", Kernel);

    if (mirrorPageTable == gcvNULL)
    {
        gcmkONERROR(
            gckOS_Allocate(Kernel->os,
                           sizeof(struct _gcsMirrorPageTable),
                           &pointer));
        mirrorPageTable = pointer;

        gcmkONERROR(
            gckOS_ZeroMemory(mirrorPageTable,
                    sizeof(struct _gcsMirrorPageTable)));

        gcmkONERROR(
            gckOS_CreateMutex(Kernel->os, &mirrorPageTableMutex));
    }

    gcmkONERROR(_Construct(Kernel, MmuSize, Mmu));

    mirrorPageTable->mmus[mirrorPageTable->reference] = *Mmu;

    mirrorPageTable->hardwares[mirrorPageTable->reference] = Kernel->hardware;

    mirrorPageTable->reference++;

    gcmkFOOTER_ARG("mirrorPageTable->reference=%lu", mirrorPageTable->reference);
    return gcvSTATUS_OK;

OnError:
    if (mirrorPageTable && mirrorPageTable->reference == 0)
    {
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Kernel->os, mirrorPageTable));
    }

    gcmkFOOTER();
    return status;
#else
    return _Construct(Kernel, MmuSize, Mmu);
#endif
}

gceSTATUS
gckMMU_Destroy(
    IN gckMMU Mmu
    )
{
#if gcdSHARED_PAGETABLE
    gckOS os = Mmu->os;

    sharedPageTable->reference--;

    if (sharedPageTable->reference == 0)
    {
        if (sharedPageTable->mmu)
        {
            gcmkVERIFY_OK(_Destroy(Mmu));
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, sharedPageTable));
    }

    return gcvSTATUS_OK;
#elif gcdMIRROR_PAGETABLE
    mirrorPageTable->reference--;

    if (mirrorPageTable->reference == 0)
    {
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Mmu->os, mirrorPageTable));
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Mmu->os, mirrorPageTableMutex));
    }

    return _Destroy(Mmu);
#else
    return _Destroy(Mmu);
#endif
}

/*******************************************************************************
**
**  gckMMU_AllocatePages
**
**  Allocate pages inside the page table.
**
**  INPUT:
**
**      gckMMU Mmu
**          Pointer to an gckMMU object.
**
**      gctSIZE_T PageCount
**          Number of pages to allocate.
**
**  OUTPUT:
**
**      gctPOINTER * PageTable
**          Pointer to a variable that receives the base address of the page
**          table.
**
**      gctUINT32 * Address
**          Pointer to a variable that receives the hardware specific address.
*/
gceSTATUS
_AllocatePages(
    IN gckMMU Mmu,
    IN gctSIZE_T PageCount,
    IN gceSURF_TYPE Type,
    OUT gctPOINTER * PageTable,
    OUT gctUINT32 * Address
    )
{
    gceSTATUS status;
    gctBOOL mutex = gcvFALSE;
    gctUINT32 index = 0, previous = ~0U, left;
    gctUINT32_PTR map;
    gctBOOL gotIt;
    gctUINT32 address;
    gctUINT32 pageCount;

    gcmkHEADER_ARG("Mmu=0x%x PageCount=%lu", Mmu, PageCount);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);
    gcmkVERIFY_ARGUMENT(PageCount > 0);
    gcmkVERIFY_ARGUMENT(PageTable != gcvNULL);

    if (PageCount > Mmu->pageTableEntries)
    {
        /* Not enough pages avaiable. */
        gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
    }

    gcmkSAFECASTSIZET(pageCount, PageCount);

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(Mmu->os, Mmu->pageTableMutex, gcvINFINITE));
    mutex = gcvTRUE;

    /* Cast pointer to page table. */
    for (map = Mmu->mapLogical, gotIt = gcvFALSE; !gotIt;)
    {
        index = Mmu->heapList;

        if ((Mmu->hardware->mmuVersion == 0) && (Type == gcvSURF_VERTEX))
        {
            gcmkONERROR(_AdjustIndex(
                Mmu,
                index,
                pageCount,
                gcdVERTEX_START / gcmSIZEOF(gctUINT32),
                &index
                ));
        }

        /* Walk the heap list. */
        for (; !gotIt && (index < Mmu->pageTableEntries);)
        {
            /* Check the node type. */
            switch (gcmENTRY_TYPE(_ReadPageEntry(&map[index])))
            {
            case gcvMMU_SINGLE:
                /* Single odes are valid if we only need 1 page. */
                if (pageCount == 1)
                {
                    gotIt = gcvTRUE;
                }
                else
                {
                    /* Move to next node. */
                    previous = index;
                    index    = _ReadPageEntry(&map[index]) >> 8;
                }
                break;

            case gcvMMU_FREE:
                /* Test if the node has enough space. */
                if (pageCount <= (_ReadPageEntry(&map[index]) >> 8))
                {
                    gotIt = gcvTRUE;
                }
                else
                {
                    /* Move to next node. */
                    previous = index;
                    index    = _ReadPageEntry(&map[index + 1]);
                }
                break;

            default:
                gcmkFATAL("MMU table correcupted at index %u!", index);
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }
        }

        /* Test if we are out of memory. */
        if (index >= Mmu->pageTableEntries)
        {
            if (Mmu->freeNodes)
            {
                /* Time to move out the trash! */
                gcmkONERROR(_Collect(Mmu));
            }
            else
            {
                /* Out of resources. */
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }
        }
    }

    switch (gcmENTRY_TYPE(_ReadPageEntry(&map[index])))
    {
    case gcvMMU_SINGLE:
        /* Unlink single node from free list. */
        gcmkONERROR(
            _Link(Mmu, previous, _ReadPageEntry(&map[index]) >> 8));
        break;

    case gcvMMU_FREE:
        /* Check how many pages will be left. */
        left = (_ReadPageEntry(&map[index]) >> 8) - pageCount;
        switch (left)
        {
        case 0:
            /* The entire node is consumed, just unlink it. */
            gcmkONERROR(
                _Link(Mmu, previous, _ReadPageEntry(&map[index + 1])));
            break;

        case 1:
            /* One page will remain.  Convert the node to a single node and
            ** advance the index. */
            _WritePageEntry(&map[index], (_ReadPageEntry(&map[index + 1]) << 8) | gcvMMU_SINGLE);
            index ++;
            break;

        default:
            /* Enough pages remain for a new node.  However, we will just adjust
            ** the size of the current node and advance the index. */
            _WritePageEntry(&map[index], (left << 8) | gcvMMU_FREE);
            index += left;
            break;
        }
        break;
    }

    /* Mark node as used. */
    gcmkONERROR(_FillPageTable(&map[index], pageCount, gcvMMU_USED));

    /* Return pointer to page table. */
    *PageTable = &Mmu->pageTableLogical[index];

    /* Build virtual address. */
    if (Mmu->hardware->mmuVersion == 0)
    {
        gcmkONERROR(
                gckHARDWARE_BuildVirtualAddress(Mmu->hardware, index, 0, &address));
    }
    else
    {
        gctUINT32 masterOffset = index / gcdMMU_STLB_4K_ENTRY_NUM
                               + Mmu->dynamicMappingStart;
        gctUINT32 slaveOffset = index % gcdMMU_STLB_4K_ENTRY_NUM;

        address = (masterOffset << gcdMMU_MTLB_SHIFT)
                | (slaveOffset << gcdMMU_STLB_4K_SHIFT);
    }

    if (Address != gcvNULL)
    {
        *Address = address;
    }

    /* Release the mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));

    /* Success. */
    gcmkFOOTER_ARG("*PageTable=0x%x *Address=%08x",
                   *PageTable, gcmOPT_VALUE(Address));
    return gcvSTATUS_OK;

OnError:

    if (mutex)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckMMU_FreePages
**
**  Free pages inside the page table.
**
**  INPUT:
**
**      gckMMU Mmu
**          Pointer to an gckMMU object.
**
**      gctPOINTER PageTable
**          Base address of the page table to free.
**
**      gctSIZE_T PageCount
**          Number of pages to free.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
_FreePages(
    IN gckMMU Mmu,
    IN gctPOINTER PageTable,
    IN gctSIZE_T PageCount
    )
{
    gctUINT32_PTR node;
    gceSTATUS status;
    gctBOOL acquired = gcvFALSE;
    gctUINT32 pageCount;

    gcmkHEADER_ARG("Mmu=0x%x PageTable=0x%x PageCount=%lu",
                   Mmu, PageTable, PageCount);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);
    gcmkVERIFY_ARGUMENT(PageTable != gcvNULL);
    gcmkVERIFY_ARGUMENT(PageCount > 0);

    gcmkSAFECASTSIZET(pageCount, PageCount);

    /* Get the node by index. */
    node = Mmu->mapLogical + ((gctUINT32_PTR)PageTable - Mmu->pageTableLogical);

    gcmkONERROR(gckOS_AcquireMutex(Mmu->os, Mmu->pageTableMutex, gcvINFINITE));
    acquired = gcvTRUE;

#if gcdMMU_CLEAR_VALUE
    if (Mmu->hardware->mmuVersion == 0)
    {
        _FillPageTable(PageTable, pageCount, gcdMMU_CLEAR_VALUE);
    }
#endif

    if (PageCount == 1)
    {
       /* Single page node. */
        _WritePageEntry(node, (~((1U<<8)-1)) | gcvMMU_SINGLE);
#if gcdUSE_MMU_EXCEPTION
        /* Enable exception */
        _WritePageEntry(PageTable, (1 << 1));
#endif
    }
    else
    {
        /* Mark the node as free. */
        _WritePageEntry(node, (pageCount << 8) | gcvMMU_FREE);
        _WritePageEntry(node + 1, ~0U);

#if gcdUSE_MMU_EXCEPTION
        /* Enable exception */
        gcmkVERIFY_OK(_FillPageTable(PageTable, pageCount, 1 << 1));
#endif
    }

    /* We have free nodes. */
    Mmu->freeNodes = gcvTRUE;

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));
    acquired = gcvFALSE;

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckMMU_AllocatePages(
    IN gckMMU Mmu,
    IN gctSIZE_T PageCount,
    OUT gctPOINTER * PageTable,
    OUT gctUINT32 * Address
    )
{
    return gckMMU_AllocatePagesEx(
                Mmu, PageCount, gcvSURF_TYPE_UNKNOWN, PageTable, Address);
}

gceSTATUS
gckMMU_AllocatePagesEx(
    IN gckMMU Mmu,
    IN gctSIZE_T PageCount,
    IN gceSURF_TYPE Type,
    OUT gctPOINTER * PageTable,
    OUT gctUINT32 * Address
    )
{
#if gcdMIRROR_PAGETABLE
    gceSTATUS status;
    gctPOINTER pageTable;
    gctUINT32 address;
    gctINT i;
    gckMMU mmu;
    gctBOOL acquired = gcvFALSE;
    gctBOOL allocated = gcvFALSE;

    gckOS_AcquireMutex(Mmu->os, mirrorPageTableMutex, gcvINFINITE);
    acquired = gcvTRUE;

    /* Allocate page table for current MMU. */
    for (i = 0; i < (gctINT)mirrorPageTable->reference; i++)
    {
        if (Mmu == mirrorPageTable->mmus[i])
        {
            gcmkONERROR(_AllocatePages(Mmu, PageCount, Type, PageTable, Address));
            allocated = gcvTRUE;
        }
    }

    /* Allocate page table for other MMUs. */
    for (i = 0; i < (gctINT)mirrorPageTable->reference; i++)
    {
        mmu = mirrorPageTable->mmus[i];

        if (Mmu != mmu)
        {
            gcmkONERROR(_AllocatePages(mmu, PageCount, Type, &pageTable, &address));
            gcmkASSERT(address == *Address);
        }
    }

    gckOS_ReleaseMutex(Mmu->os, mirrorPageTableMutex);
    acquired = gcvFALSE;

    return gcvSTATUS_OK;
OnError:

    if (allocated)
    {
        /* Page tables for multiple GPU always keep the same. So it is impossible
         * the fist one allocates successfully but others fail.
         */
        gcmkASSERT(0);
    }

    if (acquired)
    {
        gckOS_ReleaseMutex(Mmu->os, mirrorPageTableMutex);
    }

    return status;
#else
    return _AllocatePages(Mmu, PageCount, Type, PageTable, Address);
#endif
}

gceSTATUS
gckMMU_FreePages(
    IN gckMMU Mmu,
    IN gctPOINTER PageTable,
    IN gctSIZE_T PageCount
    )
{
#if gcdMIRROR_PAGETABLE
    gctINT i;
    gctUINT32 offset;
    gckMMU mmu;

    gckOS_AcquireMutex(Mmu->os, mirrorPageTableMutex, gcvINFINITE);

    gcmkVERIFY_OK(_FreePages(Mmu, PageTable, PageCount));

    offset = (gctUINT32)PageTable - (gctUINT32)Mmu->pageTableLogical;

    for (i = 0; i < (gctINT)mirrorPageTable->reference; i++)
    {
        mmu = mirrorPageTable->mmus[i];

        if (mmu != Mmu)
        {
            gcmkVERIFY_OK(_FreePages(mmu, mmu->pageTableLogical + offset/4, PageCount));
        }
    }

    gckOS_ReleaseMutex(Mmu->os, mirrorPageTableMutex);

    return gcvSTATUS_OK;
#else
    return _FreePages(Mmu, PageTable, PageCount);
#endif
}

gceSTATUS
gckMMU_Enable(
    IN gckMMU Mmu,
    IN gctUINT32 PhysBaseAddr,
    IN gctUINT32 PhysSize
    )
{
    gceSTATUS status;
#if gcdSHARED_PAGETABLE
    gckHARDWARE hardware;
    gctINT i;
#endif

    gcmkHEADER_ARG("Mmu=0x%x", Mmu);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);

#if gcdSHARED_PAGETABLE
    if (Mmu->enabled)
    {
        gcmkFOOTER_ARG("Status=%d", gcvSTATUS_SKIP);
        return gcvSTATUS_SKIP;
    }
#endif

    if (Mmu->hardware->mmuVersion == 0)
    {
        /* Success. */
        gcmkFOOTER_ARG("Status=%d", gcvSTATUS_SKIP);
        return gcvSTATUS_SKIP;
    }
    else
    {
#if gcdPROCESS_ADDRESS_SPACE
        gctUINT32 i;
        gctUINT32 physical;
        gckKERNEL kernel = Mmu->hardware->kernel;

        /* Map kernel command buffer in MMU. */
        for (i = 0; i < gcdCOMMAND_QUEUES; i++)
        {
            gcmkONERROR(gckOS_GetPhysicalAddress(
                Mmu->os,
                kernel->command->queues[i].logical,
                &physical
                ));

            gcmkONERROR(gckMMU_FlatMapping(Mmu, physical));
        }
#else
        if (PhysSize != 0)
        {
            gcmkONERROR(_FillFlatMapping(
                Mmu,
                PhysBaseAddr,
                PhysSize
                ));
        }

        gcmkONERROR(_SetupDynamicSpace(Mmu));
#endif

#if gcdSHARED_PAGETABLE
        for(i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
            hardware = sharedPageTable->hardwares[i];
            if (hardware != gcvNULL)
            {
                gcmkONERROR(
                    gckHARDWARE_SetMMUv2(
                        hardware,
                        gcvTRUE,
                        Mmu->mtlbLogical,
                        gcvMMU_MODE_4K,
                        (gctUINT8_PTR)Mmu->mtlbLogical + gcdMMU_MTLB_SIZE,
                        gcvFALSE
                        ));
            }
        }
#else
        gcmkONERROR(
            gckHARDWARE_SetMMUv2(
                Mmu->hardware,
                gcvTRUE,
                Mmu->mtlbLogical,
                gcvMMU_MODE_4K,
                (gctUINT8_PTR)Mmu->mtlbLogical + gcdMMU_MTLB_SIZE,
                gcvFALSE
                ));
#endif

        Mmu->enabled = gcvTRUE;

        /* Success. */
        gcmkFOOTER_NO();
        return gcvSTATUS_OK;
    }

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckMMU_SetPage(
    IN gckMMU Mmu,
    IN gctUINT32 PageAddress,
    IN gctUINT32 *PageEntry
    )
{
#if gcdMIRROR_PAGETABLE
    gctUINT32_PTR pageEntry;
    gctINT i;
    gckMMU mmu;
    gctUINT32 offset = (gctUINT32)PageEntry - (gctUINT32)Mmu->pageTableLogical;
#endif

    gcmkHEADER_ARG("Mmu=0x%x", Mmu);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);
    gcmkVERIFY_ARGUMENT(PageEntry != gcvNULL);
    gcmkVERIFY_ARGUMENT(!(PageAddress & 0xFFF));

    if (Mmu->hardware->mmuVersion == 0)
    {
        _WritePageEntry(PageEntry, PageAddress);
    }
    else
    {
        _WritePageEntry(PageEntry, _SetPage(PageAddress));
    }

#if gcdMIRROR_PAGETABLE
    for (i = 0; i < (gctINT)mirrorPageTable->reference; i++)
    {
        mmu = mirrorPageTable->mmus[i];

        if (mmu != Mmu)
        {
            pageEntry = mmu->pageTableLogical + offset / 4;

            if (mmu->hardware->mmuVersion == 0)
            {
                _WritePageEntry(pageEntry, PageAddress);
            }
            else
            {
                _WritePageEntry(pageEntry, _SetPage(PageAddress));
            }
        }

    }
#endif

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

#if gcdPROCESS_ADDRESS_SPACE
gceSTATUS
gckMMU_GetPageEntry(
    IN gckMMU Mmu,
    IN gctUINT32 Address,
    IN gctUINT32_PTR *PageTable
    )
{
    gceSTATUS status;
    struct _gcsMMU_STLB *stlb;
    struct _gcsMMU_STLB **stlbs = Mmu->stlbs;
    gctUINT32 offset = _MtlbOffset(Address);
    gctUINT32 mtlbEntry;
    gctBOOL ace = gckHARDWARE_IsFeatureAvailable(Mmu->hardware, gcvFEATURE_ACE);

    gcmkHEADER_ARG("Mmu=0x%x", Mmu);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);
    gcmkVERIFY_ARGUMENT((Address & 0xFFF) == 0);

    stlb = stlbs[offset];

    if (stlb == gcvNULL)
    {
        gcmkONERROR(_AllocateStlb(Mmu->os, &stlb));

        mtlbEntry = stlb->physBase
                  | gcdMMU_MTLB_4K_PAGE
                  | gcdMMU_MTLB_PRESENT
                  ;

        if (ace)
        {
            mtlbEntry = mtlbEntry
                      /* Secure */
                      | (1 << 4);
        }

        /* Insert Slave TLB address to Master TLB entry.*/
        _WritePageEntry(Mmu->mtlbLogical + offset, mtlbEntry);

        /* Record stlb. */
        stlbs[offset] = stlb;
    }

    *PageTable = &stlb->logical[_StlbOffset(Address)];

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

gceSTATUS
_CheckMap(
    IN gckMMU Mmu
    )
{
    gceSTATUS status;
    gctUINT32_PTR map = Mmu->mapLogical;
    gctUINT32 index;

    for (index = Mmu->heapList; index < Mmu->pageTableEntries;)
    {
        /* Check the node type. */
        switch (gcmENTRY_TYPE(_ReadPageEntry(&map[index])))
        {
        case gcvMMU_SINGLE:
            /* Move to next node. */
            index    = _ReadPageEntry(&map[index]) >> 8;
            break;

        case gcvMMU_FREE:
            /* Move to next node. */
            index    = _ReadPageEntry(&map[index + 1]);
            break;

        default:
            gcmkFATAL("MMU table correcupted at index [%u] = %x!", index, map[index]);
            gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
        }
    }

    return gcvSTATUS_OK;

OnError:
    return status;
}

gceSTATUS
gckMMU_FlatMapping(
    IN gckMMU Mmu,
    IN gctUINT32 Physical
    )
{
    gceSTATUS status;
    gctUINT32 index = _AddressToIndex(Mmu, Physical);
    gctUINT32 i;
    gctBOOL gotIt = gcvFALSE;
    gctUINT32_PTR map = Mmu->mapLogical;
    gctUINT32 previous = ~0U;
    gctUINT32_PTR pageTable;

    gckMMU_GetPageEntry(Mmu, Physical, &pageTable);

    _WritePageEntry(pageTable, _SetPage(Physical));

    if (map)
    {
        /* Find node which contains index. */
        for (i = 0; !gotIt && (i < Mmu->pageTableEntries);)
        {
            gctUINT32 numPages;

            switch (gcmENTRY_TYPE(_ReadPageEntry(&map[i])))
            {
            case gcvMMU_SINGLE:
                if (i == index)
                {
                    gotIt = gcvTRUE;
                }
                else
                {
                    previous = i;
                    i = _ReadPageEntry(&map[i]) >> 8;
                }
                break;

            case gcvMMU_FREE:
                numPages = _ReadPageEntry(&map[i]) >> 8;
                if (index >= i && index < i + numPages)
                {
                    gotIt = gcvTRUE;
                }
                else
                {
                    previous = i;
                    i = _ReadPageEntry(&map[i + 1]);
                }
                break;

            default:
                gcmkFATAL("MMU table correcupted at index %u!", index);
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }
        }

        switch (gcmENTRY_TYPE(_ReadPageEntry(&map[i])))
        {
        case gcvMMU_SINGLE:
            /* Unlink single node from free list. */
            gcmkONERROR(
                _Link(Mmu, previous, _ReadPageEntry(&map[i]) >> 8));
            break;

        case gcvMMU_FREE:
            /* Split the node. */
            {
                gctUINT32 start;
                gctUINT32 next = _ReadPageEntry(&map[i+1]);
                gctUINT32 total = _ReadPageEntry(&map[i]) >> 8;
                gctUINT32 countLeft = index - i;
                gctUINT32 countRight = total - countLeft - 1;

                if (countLeft)
                {
                    start = i;
                    _AddFree(Mmu, previous, start, countLeft);
                    previous = start;
                }

                if (countRight)
                {
                    start = index + 1;
                    _AddFree(Mmu, previous, start, countRight);
                    previous = start;
                }

                _Link(Mmu, previous, next);
            }
            break;
        }
    }

    return gcvSTATUS_OK;

OnError:

    /* Roll back. */
    return status;
}



gceSTATUS
gckMMU_FreePagesEx(
    IN gckMMU Mmu,
    IN gctUINT32 Address,
    IN gctSIZE_T PageCount
    )
{
    gctUINT32_PTR node;
    gceSTATUS status;

#if gcdUSE_MMU_EXCEPTION
    gctUINT32 i;
    struct _gcsMMU_STLB *stlb;
    struct _gcsMMU_STLB **stlbs = Mmu->stlbs;
#endif

    gcmkHEADER_ARG("Mmu=0x%x Address=0x%x PageCount=%lu",
                   Mmu, Address, PageCount);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);
    gcmkVERIFY_ARGUMENT(PageCount > 0);

    /* Get the node by index. */
    node = Mmu->mapLogical + _AddressToIndex(Mmu, Address);

    gcmkONERROR(gckOS_AcquireMutex(Mmu->os, Mmu->pageTableMutex, gcvINFINITE));

    if (PageCount == 1)
    {
       /* Single page node. */
        _WritePageEntry(node, (~((1U<<8)-1)) | gcvMMU_SINGLE);
    }
    else
    {
        /* Mark the node as free. */
        _WritePageEntry(node, (PageCount << 8) | gcvMMU_FREE);
        _WritePageEntry(node + 1, ~0U);
    }

    /* We have free nodes. */
    Mmu->freeNodes = gcvTRUE;

#if gcdUSE_MMU_EXCEPTION
    for (i = 0; i < PageCount; i++)
    {
        /* Get */
        stlb = stlbs[_MtlbOffset(Address)];

        /* Enable exception */
        stlb->logical[_StlbOffset(Address)] = gcdMMU_STLB_EXCEPTION;
    }
#endif

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Mmu->os, Mmu->pageTableMutex));


    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}
#endif

gceSTATUS
gckMMU_Flush(
    IN gckMMU Mmu,
    IN gceSURF_TYPE Type
    )
{
    gckHARDWARE hardware;
    gctUINT32 mask;
    gctINT i;

    if (Type == gcvSURF_VERTEX || Type == gcvSURF_INDEX)
    {
        mask = gcvPAGE_TABLE_DIRTY_BIT_FE;
    }
    else
    {
        mask = gcvPAGE_TABLE_DIRTY_BIT_OTHER;
    }

#if gcdPROCESS_ADDRESS_SPACE
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        gcmkVERIFY_OK(
            gckOS_AtomSetMask(Mmu->pageTableDirty[i], mask));
    }
#else
#if gcdSHARED_PAGETABLE
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        hardware = sharedPageTable->hardwares[i];
        if (hardware)
        {
            gcmkVERIFY_OK(gckOS_AtomSetMask(hardware->pageTableDirty, mask));
        }
    }
#elif gcdMIRROR_PAGETABLE
    for (i = 0; i < (gctINT)mirrorPageTable->reference; i++)
    {
        hardware = mirrorPageTable->hardwares[i];

        /* Notify cores who use this page table. */
        gcmkVERIFY_OK(
            gckOS_AtomSetMask(hardware->pageTableDirty, mask));
    }
#else
    hardware = Mmu->hardware;
    gcmkVERIFY_OK(
        gckOS_AtomSetMask(hardware->pageTableDirty, mask));
#endif
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
gckMMU_DumpPageTableEntry(
    IN gckMMU Mmu,
    IN gctUINT32 Address
    )
{
#if gcdPROCESS_ADDRESS_SPACE
    gcsMMU_STLB_PTR *stlbs = Mmu->stlbs;
    gcsMMU_STLB_PTR stlbDesc = stlbs[_MtlbOffset(Address)];
#else
    gctUINT32_PTR pageTable;
    gctUINT32 index;
    gctUINT32 mtlb, stlb;
#endif

    gcmkHEADER_ARG("Mmu=0x%08X Address=0x%08X", Mmu, Address);
    gcmkVERIFY_OBJECT(Mmu, gcvOBJ_MMU);

    gcmkASSERT(Mmu->hardware->mmuVersion > 0);

#if gcdPROCESS_ADDRESS_SPACE
    if (stlbDesc)
    {
        gcmkPRINT("    STLB entry = 0x%08X",
                  _ReadPageEntry(&stlbDesc->logical[_StlbOffset(Address)]));
    }
    else
    {
        gcmkPRINT("    MTLB entry is empty.");
    }
#else
    mtlb   = (Address & gcdMMU_MTLB_MASK) >> gcdMMU_MTLB_SHIFT;

    if (mtlb >= Mmu->dynamicMappingStart)
    {
        stlb   = (Address & gcdMMU_STLB_4K_MASK) >> gcdMMU_STLB_4K_SHIFT;

        pageTable = Mmu->pageTableLogical;

        index = (mtlb - Mmu->dynamicMappingStart)
              * gcdMMU_STLB_4K_ENTRY_NUM
              + stlb;

        gcmkPRINT("    Page table entry = 0x%08X", _ReadPageEntry(pageTable + index));
    }
    else
    {
        gcsMMU_STLB_PTR stlbObj = Mmu->staticSTLB;
        gctUINT32 entry = Mmu->mtlbLogical[mtlb];

        stlb = (Address & gcdMMU_STLB_64K_MASK) >> gcdMMU_STLB_64K_SHIFT;

        entry &= 0xFFFFFFF0;

        while (stlbObj)
        {

            if (entry == stlbObj->physBase)
            {
                gcmkPRINT("    Page table entry = 0x%08X", stlbObj->logical[stlb]);
                break;
            }

            stlbObj = stlbObj->next;
        }
    }
#endif

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/******************************************************************************
****************************** T E S T   C O D E ******************************
******************************************************************************/

