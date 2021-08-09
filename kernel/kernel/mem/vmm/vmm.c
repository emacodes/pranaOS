/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

// includes
#include <libkernel/bits/errno.h>
#include <libkernel/libkernel.h>
#include <libkernel/lock.h>
#include <libkernel/log.h>
#include <mem/kmalloc.h>
#include <mem/vmm/vmm.h>
#include <mem/vmm/zoner.h>
#include <platform/generic/cpu.h>
#include <platform/generic/system.h>
#include <platform/generic/vmm/mapping_table.h>
#include <platform/generic/vmm/pf_types.h>
#include <tasking/tasking.h>


#define pdir_t pdirectory_t
#define VMM_TOTAL_PAGES_PER_TABLE VMM_PTE_COUNT
#define VMM_TOTAL_TABLES_PER_DIRECTORY VMM_PDE_COUNT
#define PDIR_SIZE sizeof(pdirectory_t)
#define PTABLE_SIZE sizeof(ptable_t)
#define IS_INDIVIDUAL_PER_DIR(index) (index < VMM_KERNEL_TABLES_START || (index == VMM_OFFSET_IN_DIRECTORY(pspace_zone.start)))

static pdir_t* _vmm_kernel_pdir;
static lock_t _vmm_lock;
static zone_t pspace_zone;
static uint32_t kernel_ptables_start_paddr = 0x0;

#define vmm_kernel_pdir_phys2virt(paddr) ((void*)((uint32_t)paddr + KERNEL_BASE - KERNEL_PM_BASE))


inline static void* _vmm_pspace_get_vaddr_of_active_pdir();
inline static void* _vmm_pspace_get_nth_active_ptable(uint32_t n);
inline static void* _vmm_pspace_get_vaddr_of_active_ptable(uint32_t vaddr);
static int _vmm_split_pspace();
static void _vmm_pspace_init();
static void _vmm_pspace_gen(pdirectory_t*);
static void* _vmm_kernel_convert_vaddr2paddr(uint32_t vaddr);
static void* _vmm_convert_vaddr2paddr(uint32_t vaddr);

inline static void* _vmm_alloc_kernel_pdir();
inline static uint32_t _vmm_alloc_pdir_paddr();
inline static uint32_t _vmm_alloc_ptable_paddr();
inline static uint32_t _vmm_alloc_page_paddr();
static bool _vmm_init_switch_to_kernel_pdir();
static void _vmm_map_init_kernel_pages(uint32_t paddr, uint32_t vaddr);
static bool _vmm_create_kernel_ptables();
static bool _vmm_map_kernel();

inline static uint32_t _vmm_round_ceil_to_page(uint32_t value);
inline static uint32_t _vmm_round_floor_to_page(uint32_t value);
inline static table_desc_t* _vmm_pdirectory_lookup(pdirectory_t* t_pdir, uint32_t t_addr);
inline static page_desc_t* _vmm_ptable_lookup(ptable_t* t_ptable, uint32_t t_addr);

static bool _vmm_is_copy_on_write(uint32_t vaddr);
static int _vmm_resolve_copy_on_write(proc_t* p, uint32_t vaddr);
static void _vmm_ensure_cow_for_page(uint32_t vaddr);
static void _vmm_ensure_cow_for_range(uint32_t vaddr, uint32_t length);
static int _vmm_copy_page_to_resolve_cow(proc_t* p, uint32_t vaddr, ptable_t* src_ptable, int page_index);

static bool _vmm_is_zeroing_on_demand(uint32_t vaddr);
static void _vmm_resolve_zeroing_on_demand(uint32_t vaddr);

static int _vmm_self_test();

inline static void* _vmm_alloc_kernel_pdir()
{
    uint32_t paddr = (uint32_t)pmm_alloc_aligned(PDIR_SIZE, PDIR_SIZE);
    return vmm_kernel_pdir_phys2virt(paddr);
}

inline static uint32_t _vmm_alloc_pdir_paddr()
{
    return (uint32_t)pmm_alloc_aligned(PDIR_SIZE, PDIR_SIZE);
}

inline static uint32_t _vmm_alloc_ptable_paddr()
{
    return (uint32_t)pmm_alloc(PTABLE_SIZE);
}

inline static uint32_t _vmm_alloc_ptables_to_cover_page()
{
    return (uint32_t)pmm_alloc_aligned(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
}

inline static void _vmm_free_ptables_to_cover_page(uint32_t addr)
{
    pmm_free((void*)addr, VMM_PAGE_SIZE);
}

inline static uint32_t _vmm_alloc_page_paddr()
{
    return (uint32_t)pmm_alloc_aligned(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
}

inline static void _vmm_free_page_paddr(uint32_t addr)
{
    pmm_free((void*)addr, VMM_PAGE_SIZE);
}

static zone_t _vmm_alloc_mapped_zone(uint32_t size, uint32_t alignment)
{
    if (size % VMM_PAGE_SIZE) {
        size += VMM_PAGE_SIZE - (size % VMM_PAGE_SIZE);
    }
    if (alignment % VMM_PAGE_SIZE) {
        alignment += VMM_PAGE_SIZE - (alignment % VMM_PAGE_SIZE);
    }

    zone_t zone = zoner_new_zone_aligned(size, alignment);
    uint32_t paddr = (uint32_t)pmm_alloc_aligned(size, alignment);
    vmm_map_pages_lockless(zone.start, paddr, size / VMM_PAGE_SIZE, PAGE_READABLE | PAGE_WRITABLE);
    return zone;
}

static int _vmm_free_mapped_zone(zone_t zone)
{
    ptable_t* ptable = (ptable_t*)_vmm_pspace_get_vaddr_of_active_ptable(zone.start);
    page_desc_t* page = _vmm_ptable_lookup(ptable, zone.start);
    pmm_free((void*)page_desc_get_frame(*page), zone.len);
    vmm_unmap_pages_lockless(zone.start, zone.len / VMM_PAGE_SIZE);
    zoner_free_zone(zone);
    return 0;
}

inline static pdirectory_t* _vmm_alloc_pdir()
{
    zone_t zone = _vmm_alloc_mapped_zone(PDIR_SIZE, PDIR_SIZE);
    return (pdirectory_t*)zone.ptr;
}

inline static void _vmm_free_pdir(pdirectory_t* pdir)
{
    if (pdir == _vmm_kernel_pdir) {
        return;
    }

    zone_t zone;
    zone.start = (uint32_t)pdir;
    zone.len = PDIR_SIZE;
    _vmm_free_mapped_zone(zone);
}


inline static void* _vmm_pspace_get_vaddr_of_active_pdir()
{
    return (void*)THIS_CPU->pdir;
}

inline static void* _vmm_pspace_get_nth_active_ptable(uint32_t n)
{
    return (void*)(pspace_zone.start + n * PTABLE_SIZE);
}

inline static void* _vmm_pspace_get_vaddr_of_active_ptable(uint32_t vaddr)
{
    return (void*)_vmm_pspace_get_nth_active_ptable(VMM_OFFSET_IN_DIRECTORY(vaddr));
}

static int _vmm_split_pspace()
{
    pspace_zone = zoner_new_zone(4 * MB);

    if (VMM_OFFSET_IN_TABLE(pspace_zone.start) != 0) {
        kpanic("WRONG PSPACE START ADDR");
    }

    _vmm_kernel_pdir = (pdir_t*)_vmm_alloc_kernel_pdir();
    THIS_CPU->pdir = (pdir_t*)_vmm_kernel_pdir;
    memset((void*)THIS_CPU->pdir, 0, PDIR_SIZE);
    return 0;
}

static void _vmm_pspace_init()
{
    uint32_t kernel_ptabels_vaddr = pspace_zone.start + VMM_KERNEL_TABLES_START * PTABLE_SIZE; 
    uint32_t kernel_ptabels_paddr = kernel_ptables_start_paddr; 
    uint32_t ptables_per_page = VMM_PAGE_SIZE / PTABLE_SIZE;
    for (int i = VMM_KERNEL_TABLES_START; i < VMM_TOTAL_TABLES_PER_DIRECTORY; i += ptables_per_page) {
        table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, kernel_ptabels_vaddr);
        if (!table_desc_is_present(*ptable_desc)) {
            kpanic("PSPACE_6335 : BUG\n");
        }
        ptable_t* ptable_vaddr = (ptable_t*)(kernel_ptables_start_paddr + (VMM_OFFSET_IN_DIRECTORY(kernel_ptabels_vaddr) - VMM_KERNEL_TABLES_START) * PTABLE_SIZE);
        page_desc_t* page = _vmm_ptable_lookup(ptable_vaddr, kernel_ptabels_vaddr);
        page_desc_set_attrs(page, PAGE_DESC_PRESENT | PAGE_DESC_WRITABLE);
        page_desc_set_frame(page, kernel_ptabels_paddr);
        kernel_ptabels_vaddr += VMM_PAGE_SIZE;
        kernel_ptabels_paddr += VMM_PAGE_SIZE;
    }
}

static void _vmm_pspace_gen(pdirectory_t* pdir)
{
    ptable_t* cur_ptable = (ptable_t*)_vmm_pspace_get_nth_active_ptable(VMM_OFFSET_IN_DIRECTORY(pspace_zone.start));
    uint32_t ptables_per_page = VMM_PAGE_SIZE / PTABLE_SIZE;
    uint32_t ptable_paddr = _vmm_alloc_ptables_to_cover_page();
    zone_t tmp_zone = zoner_new_zone(VMM_PAGE_SIZE);
    ptable_t* new_ptable = (ptable_t*)tmp_zone.start;

    vmm_map_page_lockless((uint32_t)new_ptable, ptable_paddr, PAGE_READABLE | PAGE_WRITABLE);

    memcpy(new_ptable, cur_ptable, VMM_PAGE_SIZE);

    page_desc_t pspace_page;
    page_desc_init(&pspace_page);
    page_desc_set_attrs(&pspace_page, PAGE_DESC_PRESENT | PAGE_DESC_WRITABLE);
    page_desc_set_frame(&pspace_page, ptable_paddr);

    new_ptable->entities[VMM_OFFSET_IN_DIRECTORY(pspace_zone.start) / ptables_per_page] = pspace_page;

    uint32_t table_coverage = VMM_PAGE_SIZE * VMM_TOTAL_PAGES_PER_TABLE;
    uint32_t ptable_vaddr_for = pspace_zone.start;
    uint32_t ptable_paddr_for = ptable_paddr;
    for (int i = 0; i < ptables_per_page; i++, ptable_vaddr_for += table_coverage, ptable_paddr_for += PTABLE_SIZE) {
        table_desc_t pspace_table;
        table_desc_init(&pspace_table);
        table_desc_set_attrs(&pspace_table, TABLE_DESC_PRESENT | TABLE_DESC_WRITABLE);
        table_desc_set_frame(&pspace_table, ptable_paddr_for);
        pdir->entities[VMM_OFFSET_IN_DIRECTORY(ptable_vaddr_for)] = pspace_table;
    }
    vmm_unmap_page_lockless((uint32_t)new_ptable);
    zoner_free_zone(tmp_zone);
}

static void _vmm_free_pspace(pdirectory_t* pdir)
{
    table_desc_t* ptable_desc = &pdir->entities[VMM_OFFSET_IN_DIRECTORY(pspace_zone.start)];
    if (!table_desc_has_attrs(*ptable_desc, TABLE_DESC_PRESENT)) {
        return;
    }
    pmm_free_block((void*)table_desc_get_frame(*ptable_desc));
    table_desc_del_frame(ptable_desc);
}

static void* _vmm_kernel_convert_vaddr2paddr(uint32_t vaddr)
{
    ptable_t* ptable_paddr = (ptable_t*)(kernel_ptables_start_paddr + (VMM_OFFSET_IN_DIRECTORY(vaddr) - VMM_KERNEL_TABLES_START) * PTABLE_SIZE);
    page_desc_t* page_desc = _vmm_ptable_lookup(ptable_paddr, vaddr);
    return (void*)((page_desc_get_frame(*page_desc)) | (vaddr & 0xfff));
}

static void* _vmm_convert_vaddr2paddr(uint32_t vaddr)
{
    ptable_t* ptable_vaddr = (ptable_t*)_vmm_pspace_get_vaddr_of_active_ptable(vaddr);
    page_desc_t* page_desc = _vmm_ptable_lookup(ptable_vaddr, vaddr);
    return (void*)((page_desc_get_frame(*page_desc)) | (vaddr & 0xfff));
}

static bool _vmm_init_switch_to_kernel_pdir()
{
    THIS_CPU->pdir = _vmm_kernel_pdir;
    system_disable_interrupts();
    system_set_pdir((uint32_t)_vmm_kernel_convert_vaddr2paddr((uint32_t)THIS_CPU->pdir));
    system_enable_interrupts();
    return true;
}

static void _vmm_map_init_kernel_pages(uint32_t paddr, uint32_t vaddr)
{
    ptable_t* ptable_paddr = (ptable_t*)(kernel_ptables_start_paddr + (VMM_OFFSET_IN_DIRECTORY(vaddr) - VMM_KERNEL_TABLES_START) * PTABLE_SIZE);
    for (uint32_t phyz = paddr, virt = vaddr, i = 0; i < VMM_TOTAL_PAGES_PER_TABLE; phyz += VMM_PAGE_SIZE, virt += VMM_PAGE_SIZE, i++) {
        page_desc_t new_page;
        page_desc_init(&new_page);
        page_desc_set_attrs(&new_page, PAGE_DESC_PRESENT | PAGE_DESC_WRITABLE);
        page_desc_set_frame(&new_page, phyz);
        ptable_paddr->entities[i] = new_page;
    }
}

static bool _vmm_create_kernel_ptables()
{
    uint32_t table_coverage = VMM_PAGE_SIZE * VMM_TOTAL_PAGES_PER_TABLE;
    uint32_t kernel_ptabels_vaddr = VMM_KERNEL_TABLES_START * table_coverage;

    for (int i = VMM_KERNEL_TABLES_START; i < VMM_TOTAL_TABLES_PER_DIRECTORY; i++, kernel_ptabels_vaddr += table_coverage) {

        table_desc_t* ptable_desc = _vmm_pdirectory_lookup(_vmm_kernel_pdir, kernel_ptabels_vaddr);
        uint32_t paddr = _vmm_alloc_ptable_paddr();
        if (!paddr) {
            kpanic("PADDR_5546 : BUG\n");
        }

        if (!kernel_ptables_start_paddr) {
            kernel_ptables_start_paddr = paddr;
        }
        table_desc_init(ptable_desc);
        table_desc_set_attrs(ptable_desc, TABLE_DESC_PRESENT | TABLE_DESC_WRITABLE);

        if (i > VMM_OFFSET_IN_DIRECTORY(pspace_zone.start)) {
            table_desc_set_attrs(ptable_desc, TABLE_DESC_USER);
        }

        table_desc_set_frame(ptable_desc, paddr);
    }

    int te = 0;
    do {
        _vmm_map_init_kernel_pages(kernel_mapping_table[te].paddr, kernel_mapping_table[te].vaddr);
    } while (!kernel_mapping_table[te++].last);

    return true;
}

static bool _vmm_map_kernel()
{
    int te = 0;
    do {
        vmm_map_pages(extern_mapping_table[te].paddr, extern_mapping_table[te].vaddr, extern_mapping_table[te].pages, extern_mapping_table[te].flags);
    } while (!extern_mapping_table[te++].last);
    return true;
}

int vmm_setup()
{
    lock_init(&_vmm_lock);
    zoner_init(0xc0400000);
    _vmm_split_pspace();
    _vmm_create_kernel_ptables();
    _vmm_pspace_init();
    _vmm_init_switch_to_kernel_pdir();
    _vmm_map_kernel();
    zoner_place_bitmap();
    kmalloc_init();
    return 0;
}

int vmm_setup_secondary_cpu()
{
    _vmm_init_switch_to_kernel_pdir();
    return 0;
}


inline static uint32_t _vmm_round_ceil_to_page(uint32_t value)
{
    if ((value & (VMM_PAGE_SIZE - 1)) != 0) {
        value += VMM_PAGE_SIZE;
        value &= (0xffffffff - (VMM_PAGE_SIZE - 1));
    }
    return value;
}

inline static uint32_t _vmm_round_floor_to_page(uint32_t value)
{
    return (value & (0xffffffff - (VMM_PAGE_SIZE - 1)));
}

inline static table_desc_t* _vmm_pdirectory_lookup(pdirectory_t* pdir, uint32_t vaddr)
{
    if (pdir) {
        return &pdir->entities[VMM_OFFSET_IN_DIRECTORY(vaddr)];
    }
    return 0;
}


inline static page_desc_t* _vmm_ptable_lookup(ptable_t* ptable, uint32_t vaddr)
{
    if (ptable) {
        return &ptable->entities[VMM_OFFSET_IN_TABLE(vaddr)];
    }
    return 0;
}

static inline void _vmm_table_desc_init_from_allocated_state(table_desc_t* ptable_desc)
{
    uint32_t frame = table_desc_get_frame(*ptable_desc);
    table_desc_init(ptable_desc);
    table_desc_set_attrs(ptable_desc, TABLE_DESC_PRESENT | TABLE_DESC_WRITABLE | TABLE_DESC_USER);
    table_desc_set_frame(ptable_desc, frame);
}

static int vmm_allocate_ptable_lockless(uint32_t vaddr)
{
    if (!THIS_CPU->pdir) {
        return -VMM_ERR_PDIR;
    }

    table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);
    if (table_desc_is_in_allocated_state(ptable_desc)) {
        goto skip_allocation;
    }

    uint32_t ptables_paddr = _vmm_alloc_ptables_to_cover_page();
    if (!ptables_paddr) {
        log_error(" vmm_allocate_ptable: No free space in pmm to alloc ptables");
        return -VMM_ERR_NO_SPACE;
    }

    uint32_t ptable_vaddr_start = PAGE_START((uint32_t)_vmm_pspace_get_vaddr_of_active_ptable(vaddr));
    uint32_t ptables_per_page = VMM_PAGE_SIZE / PTABLE_SIZE;
    uint32_t table_coverage = VMM_PAGE_SIZE * VMM_TOTAL_PAGES_PER_TABLE;
    uint32_t ptable_serve_vaddr_start = (vaddr / (table_coverage * ptables_per_page)) * (table_coverage * ptables_per_page);

    for (uint32_t i = 0, pvaddr = ptable_serve_vaddr_start, ptable_paddr = ptables_paddr; i < ptables_per_page; i++, pvaddr += table_coverage, ptable_paddr += PTABLE_SIZE) {
        table_desc_t* tmp_ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, pvaddr);
        table_desc_clear(tmp_ptable_desc);
        table_desc_set_allocated_state(tmp_ptable_desc);
        table_desc_set_frame(tmp_ptable_desc, ptable_paddr);
    }

    int err = vmm_map_page_lockless(ptable_vaddr_start, ptables_paddr, PAGE_READABLE | PAGE_WRITABLE | PAGE_EXECUTABLE | PAGE_CHOOSE_OWNER(vaddr));
    if (err) {
        return err;
    }
    memset((void*)ptable_vaddr_start, 0, VMM_PAGE_SIZE);

skip_allocation:
    _vmm_table_desc_init_from_allocated_state(ptable_desc);
    return 0;
}

int vmm_allocate_ptable(uint32_t vaddr)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_allocate_ptable_lockless(vaddr);
    lock_release(&_vmm_lock);
    return res;
}

static ALWAYS_INLINE int vmm_force_allocate_ptable_lockless(uint32_t vaddr)
{
    table_desc_t* ptable_desc_orig = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);
    table_desc_clear(ptable_desc_orig);
    return vmm_allocate_ptable_lockless(vaddr);
}

int vmm_force_allocate_ptable(uint32_t vaddr)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_force_allocate_ptable_lockless(vaddr);
    lock_release(&_vmm_lock);
    return res;
}

static ALWAYS_INLINE int vmm_free_ptable_lockless(uint32_t vaddr, dynamic_array_t* zones)
{
    if (!THIS_CPU->pdir) {
        return -VMM_ERR_PDIR;
    }

    table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);

    if (!table_desc_has_attrs(*ptable_desc, TABLE_DESC_PRESENT)) {
        return -EFAULT;
    }

    if (table_desc_has_attrs(*ptable_desc, TABLE_DESC_COPY_ON_WRITE)) {
        return -EFAULT;
    }

    uint32_t frame = table_desc_get_frame(*ptable_desc);
    table_desc_set_allocated_state(ptable_desc);
    table_desc_set_frame(ptable_desc, frame);

    ptable_t* ptable = (ptable_t*)_vmm_pspace_get_vaddr_of_active_ptable(vaddr);
    uint32_t pages_vstart = TABLE_START(vaddr);
    for (uint32_t i = 0, pages_voffset = 0; i < VMM_TOTAL_PAGES_PER_TABLE; i++, pages_voffset += VMM_PAGE_SIZE) {
        page_desc_t* page = &ptable->entities[i];
        vmm_free_page_lockless(pages_vstart + pages_voffset, page, zones);
    }

    uint32_t ptable_vaddr_start = PAGE_START((uint32_t)_vmm_pspace_get_vaddr_of_active_ptable(vaddr));
    uint32_t ptables_per_page = VMM_PAGE_SIZE / PTABLE_SIZE;
    uint32_t table_coverage = VMM_PAGE_SIZE * VMM_TOTAL_PAGES_PER_TABLE;
    uint32_t ptable_serve_vaddr_start = (vaddr / (table_coverage * ptables_per_page)) * (table_coverage * ptables_per_page);

    for (uint32_t i = 0, pvaddr = ptable_serve_vaddr_start; i < ptables_per_page; i++, pvaddr += table_coverage) {
        table_desc_t* ptable_desc_c = _vmm_pdirectory_lookup(THIS_CPU->pdir, pvaddr);
        if (table_desc_has_attrs(*ptable_desc_c, TABLE_DESC_PRESENT)) {
            return 0;
        }
    }

    table_desc_t* ptable_desc_first = _vmm_pdirectory_lookup(THIS_CPU->pdir, ptable_serve_vaddr_start);
    _vmm_free_ptables_to_cover_page(table_desc_get_frame(*ptable_desc_first));

    for (uint32_t i = 0, pvaddr = ptable_serve_vaddr_start; i < ptables_per_page; i++, pvaddr += table_coverage) {
        table_desc_t* ptable_desc_c = _vmm_pdirectory_lookup(THIS_CPU->pdir, pvaddr);
        table_desc_clear(ptable_desc_c);
    }

    vmm_unmap_page_lockless(ptable_vaddr_start);
    return 0;
}

int vmm_free_ptable(uint32_t vaddr, dynamic_array_t* zones)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_free_ptable_lockless(vaddr, zones);
    lock_release(&_vmm_lock);
    return res;
}

static bool _vmm_is_page_present(uint32_t vaddr)
{
    table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);
    if (!table_desc_is_present(*ptable_desc)) {
        return false;
    }

    ptable_t* ptable = (ptable_t*)_vmm_pspace_get_vaddr_of_active_ptable(vaddr);
    page_desc_t* page = _vmm_ptable_lookup(ptable, vaddr);
    return page_desc_is_present(*page);
}

static ALWAYS_INLINE int vmm_map_page_lockless(uint32_t vaddr, uint32_t paddr, uint32_t settings)
{
    if (!THIS_CPU->pdir) {
        return -VMM_ERR_PDIR;
    }

    bool is_writable = ((settings & PAGE_WRITABLE) > 0);
    bool is_readable = ((settings & PAGE_READABLE) > 0);
    bool is_executable = ((settings & PAGE_EXECUTABLE) > 0);
    bool is_not_cacheable = ((settings & PAGE_NOT_CACHEABLE) > 0);
    bool is_cow = ((settings & PAGE_COW) > 0);
    bool is_user = ((settings & PAGE_USER) > 0);

    table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);
    if (!table_desc_is_present(*ptable_desc)) {
        vmm_allocate_ptable_lockless(vaddr);
    }

    ptable_t* ptable = (ptable_t*)_vmm_pspace_get_vaddr_of_active_ptable(vaddr);
    page_desc_t* page = _vmm_ptable_lookup(ptable, vaddr);

    page_desc_init(page);
    page_desc_set_attrs(page, PAGE_DESC_PRESENT);
    page_desc_set_frame(page, paddr);

    if (is_writable && !table_desc_is_copy_on_write(*ptable_desc)) {
        page_desc_set_attrs(page, PAGE_DESC_WRITABLE);
    }

    if (is_user) {
        page_desc_set_attrs(page, PAGE_DESC_USER);
    }

    if (is_not_cacheable) {
        page_desc_set_attrs(page, PAGE_DESC_NOT_CACHEABLE);
    }

#ifdef VMM_DEBUG
    log("Page mapped %x in pdir: %x", vaddr, vmm_get_active_pdir());
#endif

    system_flush_tlb_entry(vaddr);

    return 0;
}

int vmm_map_page(uint32_t vaddr, uint32_t paddr, uint32_t settings)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_map_page_lockless(vaddr, paddr, settings);
    lock_release(&_vmm_lock);
    return res;
}

static ALWAYS_INLINE int vmm_unmap_page_lockless(uint32_t vaddr)
{
    if (!THIS_CPU->pdir) {
        return -VMM_ERR_PDIR;
    }

    table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);
    if (!table_desc_is_present(*ptable_desc)) {
        return -VMM_ERR_PTABLE;
    }

    ptable_t* ptable = (ptable_t*)_vmm_pspace_get_vaddr_of_active_ptable(vaddr);
    page_desc_t* page = _vmm_ptable_lookup(ptable, vaddr);
    page_desc_del_attrs(page, PAGE_DESC_PRESENT);
    page_desc_del_attrs(page, PAGE_DESC_WRITABLE);
    page_desc_del_frame(page);
    system_flush_tlb_entry(vaddr);

    return 0;
}

int vmm_unmap_page(uint32_t vaddr)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_unmap_page_lockless(vaddr);
    lock_release(&_vmm_lock);
    return res;
}

static ALWAYS_INLINE int vmm_map_pages_lockless(uint32_t vaddr, uint32_t paddr, uint32_t n_pages, uint32_t settings)
{
    if ((paddr & 0xfff) || (vaddr & 0xfff)) {
        return -VMM_ERR_BAD_ADDR;
    }

    int status = 0;
    for (; n_pages; paddr += VMM_PAGE_SIZE, vaddr += VMM_PAGE_SIZE, n_pages--) {
        if ((status = vmm_map_page_lockless(vaddr, paddr, settings) < 0)) {
            return status;
        }
    }

    return 0;
}

int vmm_map_pages(uint32_t vaddr, uint32_t paddr, uint32_t n_pages, uint32_t settings)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_map_pages_lockless(vaddr, paddr, n_pages, settings);
    lock_release(&_vmm_lock);
    return res;
}

static ALWAYS_INLINE int vmm_unmap_pages_lockless(uint32_t vaddr, uint32_t n_pages)
{
    if (vaddr & 0xfff) {
        return -VMM_ERR_BAD_ADDR;
    }

    int status = 0;
    for (; n_pages; vaddr += VMM_PAGE_SIZE, n_pages--) {
        if ((status = vmm_unmap_page_lockless(vaddr) < 0)) {
            return status;
        }
    }

    return 0;
}

int vmm_unmap_pages(uint32_t vaddr, uint32_t n_pages)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_unmap_pages_lockless(vaddr, n_pages);
    lock_release(&_vmm_lock);
    return res;
}

static inline void _vmm_tables_set_cow(uint32_t table_index, table_desc_t* cur, table_desc_t* new)
{
#ifdef __i386__
    table_desc_del_attrs(cur, TABLE_DESC_WRITABLE);
    table_desc_set_attrs(cur, TABLE_DESC_COPY_ON_WRITE);
    table_desc_del_attrs(new, TABLE_DESC_WRITABLE);
    table_desc_set_attrs(new, TABLE_DESC_COPY_ON_WRITE);
#elif __arm__
    table_desc_set_attrs(cur, TABLE_DESC_COPY_ON_WRITE);
    table_desc_set_attrs(new, TABLE_DESC_COPY_ON_WRITE);

    ptable_t* ptable = _vmm_pspace_get_nth_active_ptable(table_index);
    for (int i = 0; i < VMM_TOTAL_PAGES_PER_TABLE; i++) {
        page_desc_t* page = &ptable->entities[i];
        page_desc_del_attrs(page, PAGE_DESC_WRITABLE);
    }
#endif
}

static bool _vmm_is_copy_on_write(uint32_t vaddr)
{
    table_desc_t* ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, vaddr);
    return table_desc_is_copy_on_write(*ptable_desc);
}

static int _vmm_resolve_copy_on_write(proc_t* p, uint32_t vaddr)
{
    table_desc_t orig_table_desc[VMM_PAGE_SIZE / PTABLE_SIZE];
    uint32_t ptables_per_page = VMM_PAGE_SIZE / PTABLE_SIZE;
    uint32_t table_coverage = VMM_PAGE_SIZE * VMM_TOTAL_PAGES_PER_TABLE;
    uint32_t ptable_serve_vaddr_start = (vaddr / (table_coverage * ptables_per_page)) * (table_coverage * ptables_per_page);

    zone_t src_ptable_zone = _vmm_alloc_mapped_zone(VMM_PAGE_SIZE, VMM_PAGE_SIZE);
    ptable_t* src_ptable = (ptable_t*)src_ptable_zone.ptr;
    ptable_t* root_ptable = (ptable_t*)PAGE_START((uint32_t)_vmm_pspace_get_vaddr_of_active_ptable(ptable_serve_vaddr_start));
    memcpy(src_ptable, root_ptable, VMM_PAGE_SIZE);

    for (int it = 0; it < ptables_per_page; it++) {
        table_desc_t* tmp_table_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, ptable_serve_vaddr_start);
        orig_table_desc[it] = tmp_table_desc[it];
    }

    int err = vmm_force_allocate_ptable_lockless(vaddr);
    if (err) {
        return err;
    }

    uint32_t table_start = TABLE_START(ptable_serve_vaddr_start);
    table_desc_t* start_ptable_desc = _vmm_pdirectory_lookup(THIS_CPU->pdir, ptable_serve_vaddr_start);
    for (int ptable_idx = 0; ptable_idx < ptables_per_page; ptable_idx++) {
        if (table_desc_is_present(orig_table_desc[ptable_idx])) {
            _vmm_table_desc_init_from_allocated_state(&start_ptable_desc[ptable_idx]);
            for (int page_idx = 0; page_idx < VMM_TOTAL_PAGES_PER_TABLE; page_idx++) {
                uint32_t offset_in_table_set = ptable_idx * VMM_TOTAL_PAGES_PER_TABLE + page_idx;
                uint32_t page_vaddr = table_start + (offset_in_table_set * VMM_PAGE_SIZE);
                page_desc_t* page_desc = &src_ptable->entities[offset_in_table_set];
                if (page_desc_is_present(*page_desc)) {
                    _vmm_copy_page_to_resolve_cow(p, page_vaddr, src_ptable, offset_in_table_set);
                }
            }
        }
    }

    return _vmm_free_mapped_zone(src_ptable_zone);
}

static void _vmm_ensure_cow_for_page(uint32_t vaddr)
{
    if (_vmm_is_copy_on_write(vaddr)) {
        proc_t* holder_proc = tasking_get_proc_by_pdir(vmm_get_active_pdir());
        if (!holder_proc) {
            kpanic("No proc with the pdir\n");
        }
        _vmm_resolve_copy_on_write(holder_proc, vaddr);
    }
}

static void _vmm_ensure_cow_for_range(uint32_t vaddr, uint32_t length)
{
    uint32_t page_addr = PAGE_START(vaddr);
    while (page_addr < vaddr + length) {
        _vmm_ensure_cow_for_page(page_addr);
        page_addr += VMM_PAGE_SIZE;
    }
}

static int _vmm_load_page_with_perm(uint32_t vaddr)
{
    if (PAGE_CHOOSE_OWNER(vaddr) == PAGE_USER && vmm_get_active_pdir() != vmm_get_kernel_pdir()) {
        proc_t* holder_proc = tasking_get_proc_by_pdir(vmm_get_active_pdir());
        if (!holder_proc) {
            kpanic("No proc with the pdir\n");
        }

        proc_zone_t* zone = proc_find_zone(holder_proc, vaddr);
        if (!zone) {
            return SHOULD_CRASH;
        }

#ifdef VMM_DEBUG
        log("Mmap[ensure_write_to] page %x for %d pid: %x", vaddr, RUNNING_THREAD->process->pid, zone->flags);
#endif
        vmm_load_page_lockless(vaddr, zone->flags);
    } else {

        vmm_load_page_lockless(vaddr, PAGE_READABLE | PAGE_WRITABLE | PAGE_EXECUTABLE);
    }
    return OK;
}

static void _vmm_ensure_write_to_page(uint32_t vaddr)
{
    if (!_vmm_is_page_present(vaddr)) {
        _vmm_load_page_with_perm(vaddr);
    }
    _vmm_ensure_cow_for_page(vaddr);
}

static void _vmm_ensure_write_to_range(uint32_t vaddr, uint32_t length)
{
    uint32_t page_addr = PAGE_START(vaddr);
    while (page_addr < vaddr + length) {
        _vmm_ensure_write_to_page(page_addr);
        page_addr += VMM_PAGE_SIZE;
    }
}

static int _vmm_copy_page_to_resolve_cow(proc_t* p, uint32_t vaddr, ptable_t* src_ptable, int page_index)
{
    page_desc_t* old_page_desc = &src_ptable->entities[page_index];

    proc_zone_t* zone = proc_find_zone(p, vaddr);
    if (!zone) {
        log_error("Cow: No page in zone");
        return SHOULD_CRASH;
    }

    if ((zone->type & ZONE_TYPE_MAPPED_FILE_SHAREDLY)) {
        uint32_t old_page_paddr = page_desc_get_frame(*old_page_desc);
        return vmm_map_page_lockless(vaddr, old_page_paddr, zone->flags);
    }

    vmm_load_page_lockless(vaddr, zone->flags);

    zone_t tmp_zone = zoner_new_zone(VMM_PAGE_SIZE);
    uint32_t old_page_vaddr = (uint32_t)tmp_zone.start;
    uint32_t old_page_paddr = page_desc_get_frame(*old_page_desc);
    vmm_map_page_lockless(old_page_vaddr, old_page_paddr, PAGE_READABLE);

    memcpy((uint8_t*)vaddr, (uint8_t*)old_page_vaddr, VMM_PAGE_SIZE);

    vmm_unmap_page_lockless(old_page_vaddr);
    zoner_free_zone(tmp_zone);
    return 0;
}

static ALWAYS_INLINE int vmm_switch_pdir_lockless(pdirectory_t* pdir)
{
    if (!pdir) {
        return -1;
    }

    if (((uint32_t)pdir & (PDIR_SIZE - 1)) != 0) {
        kpanic("vmm_switch_pdir: wrong pdir");
    }

    system_disable_interrupts();
    if (THIS_CPU->pdir == pdir) {
        system_enable_interrupts();
        return 0;
    }
    THIS_CPU->pdir = pdir;
    system_set_pdir((uint32_t)_vmm_convert_vaddr2paddr((uint32_t)pdir));
    system_enable_interrupts();
    return 0;
}

int vmm_switch_pdir(pdirectory_t* pdir)
{
    lock_acquire(&_vmm_lock);
    int res = vmm_switch_pdir_lockless(pdir);
    lock_release(&_vmm_lock);
    return res;
}

void vmm_enable_paging()
{
    system_enable_paging();
}

void vmm_disable_paging()
{
    system_disable_paging();
}


static int _vmm_self_test()
{
    vmm_map_pages(0x00f0000, 0x8f000000, 1, PAGE_READABLE | PAGE_WRITABLE | PAGE_EXECUTABLE);
    bool correct = true;
    correct &= ((uint32_t)_vmm_convert_vaddr2paddr(KERNEL_BASE) == KERNEL_PM_BASE);
    correct &= ((uint32_t)_vmm_convert_vaddr2paddr(0xffc00000) == 0x0);
    correct &= ((uint32_t)_vmm_convert_vaddr2paddr(0x100) == 0x100);
    correct &= ((uint32_t)_vmm_convert_vaddr2paddr(0x8f000000) == 0x00f0000);
    if (correct) {
        return 0;
    }
    return 1;
}

static bool vmm_test_pspace_vaddr_of_active_ptable()
{
    uint32_t vaddr = 0xc0000000;
    ptable_t* pt = _vmm_pspace_get_vaddr_of_active_ptable(vaddr);
    page_desc_t* ppage = _vmm_ptable_lookup(pt, vaddr);
    page_desc_del_attrs(ppage, PAGE_DESC_PRESENT);
    uint32_t* kek1 = (uint32_t*)vaddr;
    *kek1 = 1;

    while (1) {
    }
    return true;
}