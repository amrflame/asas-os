#include "paging.h"

#pragma intrinsic(__readcr3)
UINT64 __readcr3(void);
#pragma intrinsic(__writecr3)
void __writecr3(UINT64 value);
#pragma intrinsic(__invlpg)
void __invlpg(void *address);
#pragma intrinsic(__readmsr)
UINT64 __readmsr(UINT32 register_number);
#pragma intrinsic(__writemsr)
void __writemsr(UINT32 register_number, UINT64 value);

#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define IA32_EFER 0xC0000080U
#define EFER_NXE (1ULL << 11)

static void clear_page(UINT64 physical_address)
{
    UINT64 *page = (UINT64 *)(UINTN)physical_address;
    UINT32 index;

    for (index = 0; index < 512; index++) {
        page[index] = 0;
    }
}

static UINT64 *next_table(
    ASAS_FRAME_ALLOCATOR *allocator,
    UINT64 *table,
    UINT32 index,
    UINT64 flags
)
{
    UINT64 frame;

    if ((table[index] & PAGE_PRESENT) != 0) {
        table[index] |= flags & ASAS_PAGE_USER;
        return (UINT64 *)(UINTN)(table[index] & PAGE_ADDRESS_MASK);
    }

    frame = frame_allocate(allocator);
    if (frame == 0) {
        return 0;
    }

    clear_page(frame);
    table[index] = frame | PAGE_PRESENT | PAGE_WRITABLE | (flags & ASAS_PAGE_USER);
    return (UINT64 *)(UINTN)frame;
}

void paging_initialize(ASAS_PAGE_TABLES *tables, ASAS_FRAME_ALLOCATOR *allocator)
{
    UINT64 *firmware_pml4 = (UINT64 *)(UINTN)(__readcr3() & PAGE_ADDRESS_MASK);
    UINT64 new_pml4_frame = frame_allocate(allocator);
    UINT32 index;

    if (new_pml4_frame == 0) {
        tables->pml4 = 0;
        tables->frame_allocator = allocator;
        return;
    }

    tables->pml4 = (UINT64 *)(UINTN)new_pml4_frame;
    tables->frame_allocator = allocator;

    for (index = 0; index < 512; index++) {
        tables->pml4[index] = firmware_pml4[index];
    }

    __writecr3(new_pml4_frame);
}

void paging_enable_nx(void)
{
    __writemsr(IA32_EFER, __readmsr(IA32_EFER) | EFER_NXE);
}

int map_page(ASAS_PAGE_TABLES *tables, UINT64 virtual_address, UINT64 physical_address, UINT64 flags)
{
    UINT32 pml4_index = (UINT32)((virtual_address >> 39) & 0x1FF);
    UINT32 pdpt_index = (UINT32)((virtual_address >> 30) & 0x1FF);
    UINT32 pd_index = (UINT32)((virtual_address >> 21) & 0x1FF);
    UINT32 pt_index = (UINT32)((virtual_address >> 12) & 0x1FF);
    if (tables->pml4 == 0) {
        return 0;
    }
    UINT64 *pdpt = next_table(tables->frame_allocator, tables->pml4, pml4_index, flags);
    UINT64 *pd;
    UINT64 *pt;

    if (pdpt == 0) {
        return 0;
    }

    pd = next_table(tables->frame_allocator, pdpt, pdpt_index, flags);
    if (pd == 0) {
        return 0;
    }

    pt = next_table(tables->frame_allocator, pd, pd_index, flags);
    if (pt == 0) {
        return 0;
    }

    pt[pt_index] = (physical_address & PAGE_ADDRESS_MASK) | flags | PAGE_PRESENT;
    __invlpg((void *)(UINTN)virtual_address);
    return 1;
}

UINT64 paging_page_flags(ASAS_PAGE_TABLES *tables, UINT64 virtual_address)
{
    UINT32 indices[4];
    UINT64 *table = tables->pml4;
    UINT32 level;

    indices[0] = (UINT32)((virtual_address >> 39) & 0x1FF);
    indices[1] = (UINT32)((virtual_address >> 30) & 0x1FF);
    indices[2] = (UINT32)((virtual_address >> 21) & 0x1FF);
    indices[3] = (UINT32)((virtual_address >> 12) & 0x1FF);

    for (level = 0; level < 3; level++) {
        UINT64 entry = table[indices[level]];

        if ((entry & PAGE_PRESENT) == 0) {
            return 0;
        }
        table = (UINT64 *)(UINTN)(entry & PAGE_ADDRESS_MASK);
    }

    return table[indices[3]];
}

int paging_create_address_space(
    ASAS_PAGE_TABLES *destination,
    const ASAS_PAGE_TABLES *kernel_tables,
    ASAS_FRAME_ALLOCATOR *allocator
)
{
    UINT64 pml4_frame = frame_allocate(allocator);
    UINT32 index;

    if (pml4_frame == 0) {
        return 0;
    }

    destination->pml4 = (UINT64 *)(UINTN)pml4_frame;
    destination->frame_allocator = allocator;
    clear_page(pml4_frame);

    for (index = 256; index < 512; index++) {
        destination->pml4[index] = kernel_tables->pml4[index];
    }

    return 1;
}

void paging_destroy_address_space(ASAS_PAGE_TABLES *tables)
{
    UINT32 pml4_index;

    if (tables->pml4 == 0 || tables->frame_allocator == 0) {
        return;
    }

    for (pml4_index = 0; pml4_index < 256; pml4_index++) {
        UINT64 pml4_entry = tables->pml4[pml4_index];
        UINT64 *pdpt;
        UINT32 pdpt_index;

        if ((pml4_entry & PAGE_PRESENT) == 0) {
            continue;
        }

        pdpt = (UINT64 *)(UINTN)(pml4_entry & PAGE_ADDRESS_MASK);
        for (pdpt_index = 0; pdpt_index < 512; pdpt_index++) {
            UINT64 pdpt_entry = pdpt[pdpt_index];
            UINT64 *pd;
            UINT32 pd_index;

            if ((pdpt_entry & PAGE_PRESENT) == 0) {
                continue;
            }

            pd = (UINT64 *)(UINTN)(pdpt_entry & PAGE_ADDRESS_MASK);
            for (pd_index = 0; pd_index < 512; pd_index++) {
                UINT64 pd_entry = pd[pd_index];
                UINT64 *pt;
                UINT32 pt_index;

                if ((pd_entry & PAGE_PRESENT) == 0) {
                    continue;
                }

                pt = (UINT64 *)(UINTN)(pd_entry & PAGE_ADDRESS_MASK);
                for (pt_index = 0; pt_index < 512; pt_index++) {
                    UINT64 pt_entry = pt[pt_index];

                    if ((pt_entry & PAGE_PRESENT) != 0) {
                        frame_free(tables->frame_allocator, pt_entry & PAGE_ADDRESS_MASK);
                        pt[pt_index] = 0;
                    }
                }

                frame_free(tables->frame_allocator, pd_entry & PAGE_ADDRESS_MASK);
                pd[pd_index] = 0;
            }

            frame_free(tables->frame_allocator, pdpt_entry & PAGE_ADDRESS_MASK);
            pdpt[pdpt_index] = 0;
        }

        frame_free(tables->frame_allocator, pml4_entry & PAGE_ADDRESS_MASK);
        tables->pml4[pml4_index] = 0;
    }

    frame_free(tables->frame_allocator, (UINT64)(UINTN)tables->pml4);
    tables->pml4 = 0;
    tables->frame_allocator = 0;
}
