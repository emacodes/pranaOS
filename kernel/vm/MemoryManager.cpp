/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

// includes
#include <base/Assertions.h>
#include <base/Memory.h>
#include <base/StringView.h>
#include <kernel/BootInfo.h>
#include <kernel/CMOS.h>
#include <kernel/filesystem/Inode.h>
#include <kernel/heap/kmalloc.h>
#include <kernel/Multiboot.h>
#include <kernel/Panic.h>
#include <kernel/Process.h>
#include <kernel/Sections.h>
#include <kernel/StdLib.h>
#include <kernel/vm/AnonymousVMObject.h>
#include <kernel/vm/ContiguousVMObject.h>
#include <kernel/vm/MemoryManager.h>
#include <kernel/vm/PageDirectory.h>
#include <kernel/vm/PhysicalRegion.h>
#include <kernel/vm/SharedInodeVMObject.h>

extern u8 start_of_kernel_image[];
extern u8 end_of_kernel_image[];
extern u8 start_of_kernel_text[];
extern u8 start_of_kernel_data[];
extern u8 end_of_kernel_bss[];
extern u8 start_of_ro_after_init[];
extern u8 end_of_ro_after_init[];
extern u8 start_of_unmap_after_init[];
extern u8 end_of_unmap_after_init[];
extern u8 start_of_kernel_ksyms[];
extern u8 end_of_kernel_ksyms[];

extern multiboot_module_entry_t multiboot_copy_boot_modules_array[16];
extern size_t multiboot_copy_boot_modules_count;

__attribute__((section(".super_pages"))) static u8 super_pages[1 * MiB];

namespace Kernel {

static MemoryManager* s_the;
RecursiveSpinLock s_mm_lock;

MemoryManager& MM
{
    return *s_the;
}

bool MemoryManager::is_initialized()
{
    return s_the != nullptr;
}

UNMAP_AFTER_INIT MemoryManager::MemoryManager()
{
    s_the = this;

    ScopedSpinLock lock(s_mm_lock);
    parse_memory_map();
    write_cr3(kernel_page_directory().cr3());
    protect_kernel_image();

    if (!commit_user_physical_pages(2))
        VERIFY_NOT_REACHED();

    m_shared_zero_page = allocate_committed_user_physical_page();

    m_lazy_committed_page = allocate_committed_user_physical_page();
}

UNMAP_AFTER_INIT MemoryManager::~MemoryManager()
{
}

UNMAP_AFTER_INIT void MemoryManager::protect_kernel_image()
{
    ScopedSpinLock page_lock(kernel_page_directory().get_lock());
    for (auto i = start_of_kernel_text; i < start_of_kernel_data; i += PAGE_SIZE) {
        auto& pte = *ensure_pte(kernel_page_directory(), VirtualAddress(i));
        pte.set_writable(false);
    }
    if (Processor::current().has_feature(CPUFeature::NX)) {        
        for (auto i = start_of_kernel_data; i < end_of_kernel_image; i += PAGE_SIZE) {
            auto& pte = *ensure_pte(kernel_page_directory(), VirtualAddress(i));
            pte.set_execute_disabled(true);
        }
    }
}

UNMAP_AFTER_INIT void MemoryManager::protect_readonly_after_init_memory()
{
    ScopedSpinLock mm_lock(s_mm_lock);
    ScopedSpinLock page_lock(kernel_page_directory().get_lock());

    for (auto i = (FlatPtr)&start_of_ro_after_init; i < (FlatPtr)&end_of_ro_after_init; i += PAGE_SIZE) {
        auto& pte = *ensure_pte(kernel_page_directory(), VirtualAddress(i));
        pte.set_writable(false);
        flush_tlb(&kernel_page_directory(), VirtualAddress(i));
    }
}

void MemoryManager::unmap_text_after_init()
{
    ScopedSpinLock mm_lock(s_mm_lock);
    ScopedSpinLock page_lock(kernel_page_directory().get_lock());

    auto start = page_round_down((FlatPtr)&start_of_unmap_after_init);
    auto end = page_round_up((FlatPtr)&end_of_unmap_after_init);

    for (auto i = start; i < end; i += PAGE_SIZE) {
        auto& pte = *ensure_pte(kernel_page_directory(), VirtualAddress(i));
        pte.clear();
        flush_tlb(&kernel_page_directory(), VirtualAddress(i));
    }

    dmesgln("Unmapped {} KiB of kernel text after init! :^)", (end - start) / KiB);
}

void MemoryManager::unmap_ksyms_after_init()
{
    ScopedSpinLock mm_lock(s_mm_lock);
    ScopedSpinLock page_lock(kernel_page_directory().get_lock());

    auto start = page_round_down((FlatPtr)start_of_kernel_ksyms);
    auto end = page_round_up((FlatPtr)end_of_kernel_ksyms);

    for (auto i = start; i < end; i += PAGE_SIZE) {
        auto& pte = *ensure_pte(kernel_page_directory(), VirtualAddress(i));
        pte.clear();
        flush_tlb(&kernel_page_directory(), VirtualAddress(i));
    }

    dmesgln("Unmapped {} KiB of kernel symbols after init! :^)", (end - start) / KiB);
}

UNMAP_AFTER_INIT void MemoryManager::register_reserved_ranges()
{
    VERIFY(!m_physical_memory_ranges.is_empty());
    ContiguousReservedMemoryRange range;
    for (auto& current_range : m_physical_memory_ranges) {
        if (current_range.type != PhysicalMemoryRangeType::Reserved) {
            if (range.start.is_null())
                continue;
            m_reserved_memory_ranges.append(ContiguousReservedMemoryRange { range.start, current_range.start.get() - range.start.get() });
            range.start.set((FlatPtr) nullptr);
            continue;
        }
        if (!range.start.is_null()) {
            continue;
        }
        range.start = current_range.start;
    }
    if (m_physical_memory_ranges.last().type != PhysicalMemoryRangeType::Reserved)
        return;
    if (range.start.is_null())
        return;
    m_reserved_memory_ranges.append(ContiguousReservedMemoryRange { range.start, m_physical_memory_ranges.last().start.get() + m_physical_memory_ranges.last().length - range.start.get() });
}

bool MemoryManager::is_allowed_to_mmap_to_userspace(PhysicalAddress start_address, Range const& range) const
{
    VERIFY(!m_reserved_memory_ranges.is_empty());
    for (auto& current_range : m_reserved_memory_ranges) {
        if (!(current_range.start <= start_address))
            continue;
        if (!(current_range.start.offset(current_range.length) > start_address))
            continue;
        if (current_range.length < range.size())
            return false;
        return true;
    }
    return false;
}

UNMAP_AFTER_INIT void MemoryManager::parse_memory_map()
{

    m_used_memory_ranges.ensure_capacity(4);
    m_used_memory_ranges.append(UsedMemoryRange { UsedMemoryRangeType::LowMemory, PhysicalAddress(0x00000000), PhysicalAddress(1 * MiB) });
    m_used_memory_ranges.append(UsedMemoryRange { UsedMemoryRangeType::Prekernel, start_of_prekernel_image, end_of_prekernel_image });
    m_used_memory_ranges.append(UsedMemoryRange { UsedMemoryRangeType::Kernel, PhysicalAddress(virtual_to_low_physical((FlatPtr)start_of_kernel_image)), PhysicalAddress(page_round_up(virtual_to_low_physical((FlatPtr)end_of_kernel_image))) });

    if (multiboot_info_ptr->flags & 0x4) {
        auto* bootmods_start = multiboot_copy_boot_modules_array;
        auto* bootmods_end = bootmods_start + multiboot_copy_boot_modules_count;

        for (auto* bootmod = bootmods_start; bootmod < bootmods_end; bootmod++) {
            m_used_memory_ranges.append(UsedMemoryRange { UsedMemoryRangeType::BootModule, PhysicalAddress(bootmod->start), PhysicalAddress(bootmod->end) });
        }
    }

    auto* mmap_begin = reinterpret_cast<multiboot_memory_map_t*>(low_physical_to_virtual(multiboot_info_ptr->mmap_addr));
    auto* mmap_end = reinterpret_cast<multiboot_memory_map_t*>(low_physical_to_virtual(multiboot_info_ptr->mmap_addr) + multiboot_info_ptr->mmap_length);

    struct ContiguousPhysicalRange {
        PhysicalAddress lower;
        PhysicalAddress upper;
    };

    Vector<ContiguousPhysicalRange> contiguous_physical_ranges;

    for (auto* mmap = mmap_begin; mmap < mmap_end; mmap++) {
        dmesgln("MM: Multiboot mmap: address={:p}, length={}, type={}", mmap->addr, mmap->len, mmap->type);

        auto start_address = PhysicalAddress(mmap->addr);
        auto length = mmap->len;
        switch (mmap->type) {
        case (MULTIBOOT_MEMORY_AVAILABLE):
            m_physical_memory_ranges.append(PhysicalMemoryRange { PhysicalMemoryRangeType::Usable, start_address, length });
            break;
        case (MULTIBOOT_MEMORY_RESERVED):
            m_physical_memory_ranges.append(PhysicalMemoryRange { PhysicalMemoryRangeType::Reserved, start_address, length });
            break;
        case (MULTIBOOT_MEMORY_ACPI_RECLAIMABLE):
            m_physical_memory_ranges.append(PhysicalMemoryRange { PhysicalMemoryRangeType::ACPI_Reclaimable, start_address, length });
            break;
        case (MULTIBOOT_MEMORY_NVS):
            m_physical_memory_ranges.append(PhysicalMemoryRange { PhysicalMemoryRangeType::ACPI_NVS, start_address, length });
            break;
        case (MULTIBOOT_MEMORY_BADRAM):
            dmesgln("MM: Warning, detected bad memory range!");
            m_physical_memory_ranges.append(PhysicalMemoryRange { PhysicalMemoryRangeType::BadMemory, start_address, length });
            break;
        default:
            dbgln("MM: Unknown range!");
            m_physical_memory_ranges.append(PhysicalMemoryRange { PhysicalMemoryRangeType::Unknown, start_address, length });
            break;
        }

        if (mmap->type != MULTIBOOT_MEMORY_AVAILABLE)
            continue;

        auto diff = (FlatPtr)mmap->addr % PAGE_SIZE;
        if (diff != 0) {
            dmesgln("MM: Got an unaligned physical_region from the bootloader; correcting {:p} by {} bytes", mmap->addr, diff);
            diff = PAGE_SIZE - diff;
            mmap->addr += diff;
            mmap->len -= diff;
        }
        if ((mmap->len % PAGE_SIZE) != 0) {
            dmesgln("MM: Got an unaligned physical_region from the bootloader; correcting length {} by {} bytes", mmap->len, mmap->len % PAGE_SIZE);
            mmap->len -= mmap->len % PAGE_SIZE;
        }
        if (mmap->len < PAGE_SIZE) {
            dmesgln("MM: Memory physical_region from bootloader is too small; we want >= {} bytes, but got {} bytes", PAGE_SIZE, mmap->len);
            continue;
        }

        for (PhysicalSize page_base = mmap->addr; page_base <= (mmap->addr + mmap->len); page_base += PAGE_SIZE) {
            auto addr = PhysicalAddress(page_base);

            bool should_skip = false;
            for (auto& used_range : m_used_memory_ranges) {
                if (addr.get() >= used_range.start.get() && addr.get() <= used_range.end.get()) {
                    should_skip = true;
                    break;
                }
            }
            if (should_skip)
                continue;

            if (contiguous_physical_ranges.is_empty() || contiguous_physical_ranges.last().upper.offset(PAGE_SIZE) != addr) {
                contiguous_physical_ranges.append(ContiguousPhysicalRange {
                    .lower = addr,
                    .upper = addr,
                });
            } else {
                contiguous_physical_ranges.last().upper = addr;
            }
        }
    }

    for (auto& range : contiguous_physical_ranges) {
        m_user_physical_regions.append(PhysicalRegion::try_create(range.lower, range.upper).release_nonnull());
    }

    VERIFY(virtual_to_low_physical((FlatPtr)super_pages) + sizeof(super_pages) < 0x1000000);


    m_super_physical_regions.append(PhysicalRegion::try_create(
        PhysicalAddress(virtual_to_low_physical(FlatPtr(super_pages))),
        PhysicalAddress(virtual_to_low_physical(FlatPtr(super_pages + sizeof(super_pages)))))
                                        .release_nonnull());

    for (auto& region : m_super_physical_regions)
        m_system_memory_info.super_physical_pages += region.size();

    for (auto& region : m_user_physical_regions)
        m_system_memory_info.user_physical_pages += region.size();

    register_reserved_ranges();
    for (auto& range : m_reserved_memory_ranges) {
        dmesgln("MM: Contiguous reserved range from {}, length is {}", range.start, range.length);
    }

    initialize_physical_pages();

    VERIFY(m_system_memory_info.super_physical_pages > 0);
    VERIFY(m_system_memory_info.user_physical_pages > 0);

    
    m_system_memory_info.user_physical_pages_uncommitted = m_system_memory_info.user_physical_pages;

    for (auto& used_range : m_used_memory_ranges) {
        dmesgln("MM: {} range @ {} - {} (size {:#x})", UserMemoryRangeTypeNames[to_underlying(used_range.type)], used_range.start, used_range.end.offset(-1), used_range.end.as_ptr() - used_range.start.as_ptr());
    }

    for (auto& region : m_super_physical_regions) {
        dmesgln("MM: Super physical region: {} - {} (size {:#x})", region.lower(), region.upper().offset(-1), PAGE_SIZE * region.size());
        region.initialize_zones();
    }

    for (auto& region : m_user_physical_regions) {
        dmesgln("MM: User physical region: {} - {} (size {:#x})", region.lower(), region.upper().offset(-1), PAGE_SIZE * region.size());
        region.initialize_zones();
    }
}

UNMAP_AFTER_INIT void MemoryManager::initialize_physical_pages()
{
    PhysicalAddress highest_physical_address;
    for (auto& range : m_used_memory_ranges) {
        if (range.end.get() > highest_physical_address.get())
            highest_physical_address = range.end;
    }
    for (auto& region : m_physical_memory_ranges) {
        auto range_end = PhysicalAddress(region.start).offset(region.length);
        if (range_end.get() > highest_physical_address.get())
            highest_physical_address = range_end;
    }


    m_physical_page_entries_count = PhysicalAddress::physical_page_index(highest_physical_address.get()) + 1;
    VERIFY(m_physical_page_entries_count != 0);
    VERIFY(!Checked<decltype(m_physical_page_entries_count)>::multiplication_would_overflow(m_physical_page_entries_count, sizeof(PhysicalPageEntry)));

    auto physical_page_array_size = m_physical_page_entries_count * sizeof(PhysicalPageEntry);
    auto physical_page_array_pages = page_round_up(physical_page_array_size) / PAGE_SIZE;
    VERIFY(physical_page_array_pages * PAGE_SIZE >= physical_page_array_size);

    auto needed_page_table_count = (physical_page_array_pages + 512 - 1) / 512;

    auto physical_page_array_pages_and_page_tables_count = physical_page_array_pages + needed_page_table_count;

    PhysicalRegion* found_region { nullptr };
    Optional<size_t> found_region_index;
    for (size_t i = 0; i < m_user_physical_regions.size(); ++i) {
        auto& region = m_user_physical_regions[i];
        if (region.size() >= physical_page_array_pages_and_page_tables_count) {
            found_region = &region;
            found_region_index = i;
            break;
        }
    }

    if (!found_region) {
        dmesgln("MM: Need {} bytes for physical page management, but no memory region is large enough!", physical_page_array_pages_and_page_tables_count);
        VERIFY_NOT_REACHED();
    }

    VERIFY(m_system_memory_info.user_physical_pages >= physical_page_array_pages_and_page_tables_count);
    m_system_memory_info.user_physical_pages -= physical_page_array_pages_and_page_tables_count;

    if (found_region->size() == physical_page_array_pages_and_page_tables_count) {

        m_physical_pages_region = m_user_physical_regions.take(*found_region_index);
    } else {
        m_physical_pages_region = found_region->try_take_pages_from_beginning(physical_page_array_pages_and_page_tables_count);
    }
    m_used_memory_ranges.append({ UsedMemoryRangeType::PhysicalPages, m_physical_pages_region->lower(), m_physical_pages_region->upper() });

    m_kernel_page_directory = PageDirectory::create_kernel_page_directory();

    auto range = m_kernel_page_directory->range_allocator().allocate_anywhere(physical_page_array_pages * PAGE_SIZE);
    if (!range.has_value()) {
        dmesgln("MM: Could not allocate {} bytes to map physical page array!", physical_page_array_pages * PAGE_SIZE);
        VERIFY_NOT_REACHED();
    }

    ScopedSpinLock lock(s_mm_lock);


    auto page_tables_base = m_physical_pages_region->lower();
    auto physical_page_array_base = page_tables_base.offset(needed_page_table_count * PAGE_SIZE);
    auto physical_page_array_current_page = physical_page_array_base.get();
    auto virtual_page_array_base = range.value().base().get();
    auto virtual_page_array_current_page = virtual_page_array_base;
    for (size_t pt_index = 0; pt_index < needed_page_table_count; pt_index++) {
        auto virtual_page_base_for_this_pt = virtual_page_array_current_page;
        auto pt_paddr = page_tables_base.offset(pt_index * PAGE_SIZE);
        auto* pt = reinterpret_cast<PageTableEntry*>(quickmap_page(pt_paddr));
        __builtin_memset(pt, 0, PAGE_SIZE);
        for (size_t pte_index = 0; pte_index < PAGE_SIZE / sizeof(PageTableEntry); pte_index++) {
            auto& pte = pt[pte_index];
            pte.set_physical_page_base(physical_page_array_current_page);
            pte.set_user_allowed(false);
            pte.set_writable(true);
            if (Processor::current().has_feature(CPUFeature::NX))
                pte.set_execute_disabled(false);
            pte.set_global(true);
            pte.set_present(true);

            physical_page_array_current_page += PAGE_SIZE;
            virtual_page_array_current_page += PAGE_SIZE;
        }
        unquickmap_page();

        u32 page_directory_index = (virtual_page_base_for_this_pt >> 21) & 0x1ff;
        auto* pd = reinterpret_cast<PageDirectoryEntry*>(quickmap_page(boot_pd_kernel));
        PageDirectoryEntry& pde = pd[page_directory_index];

        VERIFY(!pde.is_present()); 

        pde.set_page_table_base(pt_paddr.get());
        pde.set_user_allowed(false);
        pde.set_present(true);
        pde.set_writable(true);
        pde.set_global(true);

        unquickmap_page();

        flush_tlb_local(VirtualAddress(virtual_page_base_for_this_pt));
    }

    m_physical_page_entries = (PhysicalPageEntry*)range.value().base().get();
    for (size_t i = 0; i < m_physical_page_entries_count; i++)
        new (&m_physical_page_entries[i]) PageTableEntry();


    m_kernel_page_directory->allocate_kernel_directory();

    auto& kernel_page_tables = kernel_page_directory().m_page_tables;
    virtual_page_array_current_page = virtual_page_array_base;
    for (size_t pt_index = 0; pt_index < needed_page_table_count; pt_index++) {
        VERIFY(virtual_page_array_current_page <= range.value().end().get());
        auto pt_paddr = page_tables_base.offset(pt_index * PAGE_SIZE);
        auto physical_page_index = PhysicalAddress::physical_page_index(pt_paddr.get());
        auto& physical_page_entry = m_physical_page_entries[physical_page_index];
        auto physical_page = adopt_ref(*new (&physical_page_entry.allocated.physical_page) PhysicalPage(MayReturnToFreeList::No));
        auto result = kernel_page_tables.set(virtual_page_array_current_page & ~0x1fffff, move(physical_page));
        VERIFY(result == Base::HashSetResult::InsertedNewEntry);

        virtual_page_array_current_page += (PAGE_SIZE / sizeof(PageTableEntry)) * PAGE_SIZE;
    }

    dmesgln("MM: Physical page entries: {}", range.value());
}

PhysicalPageEntry& MemoryManager::get_physical_page_entry(PhysicalAddress physical_address)
{
    VERIFY(m_physical_page_entries);
    auto physical_page_entry_index = PhysicalAddress::physical_page_index(physical_address.get());
    VERIFY(physical_page_entry_index < m_physical_page_entries_count);
    return m_physical_page_entries[physical_page_entry_index];
}

PhysicalAddress MemoryManager::get_physical_address(PhysicalPage const& physical_page)
{
    PhysicalPageEntry const& physical_page_entry = *reinterpret_cast<PhysicalPageEntry const*>((u8 const*)&physical_page - __builtin_offsetof(PhysicalPageEntry, allocated.physical_page));
    VERIFY(m_physical_page_entries);
    size_t physical_page_entry_index = &physical_page_entry - m_physical_page_entries;
    VERIFY(physical_page_entry_index < m_physical_page_entries_count);
    return PhysicalAddress((PhysicalPtr)physical_page_entry_index * PAGE_SIZE);
}

PageTableEntry* MemoryManager::pte(PageDirectory& page_directory, VirtualAddress vaddr)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(s_mm_lock.own_lock());
    VERIFY(page_directory.get_lock().own_lock());
    u32 page_directory_table_index = (vaddr.get() >> 30) & 0x1ff;
    u32 page_directory_index = (vaddr.get() >> 21) & 0x1ff;
    u32 page_table_index = (vaddr.get() >> 12) & 0x1ff;

    auto* pd = quickmap_pd(const_cast<PageDirectory&>(page_directory), page_directory_table_index);
    PageDirectoryEntry const& pde = pd[page_directory_index];
    if (!pde.is_present())
        return nullptr;

    return &quickmap_pt(PhysicalAddress((FlatPtr)pde.page_table_base()))[page_table_index];
}

PageTableEntry* MemoryManager::ensure_pte(PageDirectory& page_directory, VirtualAddress vaddr)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(s_mm_lock.own_lock());
    VERIFY(page_directory.get_lock().own_lock());
    u32 page_directory_table_index = (vaddr.get() >> 30) & 0x1ff;
    u32 page_directory_index = (vaddr.get() >> 21) & 0x1ff;
    u32 page_table_index = (vaddr.get() >> 12) & 0x1ff;

    auto* pd = quickmap_pd(page_directory, page_directory_table_index);
    PageDirectoryEntry& pde = pd[page_directory_index];
    if (!pde.is_present()) {
        bool did_purge = false;
        auto page_table = allocate_user_physical_page(ShouldZeroFill::Yes, &did_purge);
        if (!page_table) {
            dbgln("MM: Unable to allocate page table to map {}", vaddr);
            return nullptr;
        }
        if (did_purge) {
            
            pd = quickmap_pd(page_directory, page_directory_table_index);
            VERIFY(&pde == &pd[page_directory_index]); 

            VERIFY(!pde.is_present()); 
        }
        pde.set_page_table_base(page_table->paddr().get());
        pde.set_user_allowed(true);
        pde.set_present(true);
        pde.set_writable(true);
        pde.set_global(&page_directory == m_kernel_page_directory.ptr());

        auto result = page_directory.m_page_tables.set(vaddr.get() & ~(FlatPtr)0x1fffff, move(page_table));

        VERIFY(result == Base::HashSetResult::InsertedNewEntry);
    }

    return &quickmap_pt(PhysicalAddress((FlatPtr)pde.page_table_base()))[page_table_index];
}

void MemoryManager::release_pte(PageDirectory& page_directory, VirtualAddress vaddr, bool is_last_release)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(s_mm_lock.own_lock());
    VERIFY(page_directory.get_lock().own_lock());
    u32 page_directory_table_index = (vaddr.get() >> 30) & 0x1ff;
    u32 page_directory_index = (vaddr.get() >> 21) & 0x1ff;
    u32 page_table_index = (vaddr.get() >> 12) & 0x1ff;

    auto* pd = quickmap_pd(page_directory, page_directory_table_index);
    PageDirectoryEntry& pde = pd[page_directory_index];
    if (pde.is_present()) {
        auto* page_table = quickmap_pt(PhysicalAddress((FlatPtr)pde.page_table_base()));
        auto& pte = page_table[page_table_index];
        pte.clear();

        if (is_last_release || page_table_index == 0x1ff) {

            bool all_clear = true;
            for (u32 i = 0; i <= 0x1ff; i++) {
                if (!page_table[i].is_null()) {
                    all_clear = false;
                    break;
                }
            }
            if (all_clear) {
                pde.clear();

                auto result = page_directory.m_page_tables.remove(vaddr.get() & ~0x1fffff);
                VERIFY(result);
            }
        }
    }
}

UNMAP_AFTER_INIT void MemoryManager::initialize(u32 cpu)
{
    auto mm_data = new MemoryManagerData;
    Processor::current().set_mm_data(*mm_data);

    if (cpu == 0) {
        new MemoryManager;
        kmalloc_enable_expand();
    }
}

Region* MemoryManager::kernel_region_from_vaddr(VirtualAddress vaddr)
{
    ScopedSpinLock lock(s_mm_lock);
    for (auto& region : MM.m_kernel_regions) {
        if (region.contains(vaddr))
            return &region;
    }
    return nullptr;
}

Region* MemoryManager::find_user_region_from_vaddr_no_lock(Space& space, VirtualAddress vaddr)
{
    VERIFY(space.get_lock().own_lock());
    return space.find_region_containing({ vaddr, 1 });
}

Region* MemoryManager::find_user_region_from_vaddr(Space& space, VirtualAddress vaddr)
{
    ScopedSpinLock lock(space.get_lock());
    return find_user_region_from_vaddr_no_lock(space, vaddr);
}

void MemoryManager::validate_syscall_preconditions(Space& space, RegisterState const& regs)
{

    ScopedSpinLock lock(space.get_lock());

    auto unlock_and_handle_crash = [&lock, &regs](const char* description, int signal) {
        lock.unlock();
        handle_crash(regs, description, signal);
    };

    {
        VirtualAddress userspace_sp = VirtualAddress { regs.userspace_sp() };
        if (!MM.validate_user_stack_no_lock(space, userspace_sp)) {
            dbgln("Invalid stack pointer: {:p}", userspace_sp);
            unlock_and_handle_crash("Bad stack on syscall entry", SIGSTKFLT);
        }
    }

    {
        VirtualAddress ip = VirtualAddress { regs.ip() };
        auto* calling_region = MM.find_user_region_from_vaddr_no_lock(space, ip);
        if (!calling_region) {
            dbgln("Syscall from {:p} which has no associated region", ip);
            unlock_and_handle_crash("Syscall from unknown region", SIGSEGV);
        }

        if (calling_region->is_writable()) {
            dbgln("Syscall from writable memory at {:p}", ip);
            unlock_and_handle_crash("Syscall from writable memory", SIGSEGV);
        }

        if (space.enforces_syscall_regions() && !calling_region->is_syscall_region()) {
            dbgln("Syscall from non-syscall region");
            unlock_and_handle_crash("Syscall from non-syscall region", SIGSEGV);
        }
    }
}

Region* MemoryManager::find_region_from_vaddr(VirtualAddress vaddr)
{
    ScopedSpinLock lock(s_mm_lock);
    if (auto* region = kernel_region_from_vaddr(vaddr))
        return region;
    auto page_directory = PageDirectory::find_by_cr3(read_cr3());
    if (!page_directory)
        return nullptr;
    VERIFY(page_directory->space());
    return find_user_region_from_vaddr(*page_directory->space(), vaddr);
}

PageFaultResponse MemoryManager::handle_page_fault(PageFault const& fault)
{
    VERIFY_INTERRUPTS_DISABLED();
    if (Processor::current().in_irq()) {
        dbgln("CPU[{}] BUG! Page fault while handling IRQ! code={}, vaddr={}, irq level: {}",
            Processor::id(), fault.code(), fault.vaddr(), Processor::current().in_irq());
        dump_kernel_regions();
        return PageFaultResponse::ShouldCrash;
    }
    dbgln_if(PAGE_FAULT_DEBUG, "MM: CPU[{}] handle_page_fault({:#04x}) at {}", Processor::id(), fault.code(), fault.vaddr());
    auto* region = find_region_from_vaddr(fault.vaddr());
    if (!region) {
        return PageFaultResponse::ShouldCrash;
    }
    return region->handle_fault(fault);
}

OwnPtr<Region> MemoryManager::allocate_contiguous_kernel_region(size_t size, StringView name, Region::Access access, Region::Cacheable cacheable)
{
    VERIFY(!(size % PAGE_SIZE));
    ScopedSpinLock lock(s_mm_lock);
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.has_value())
        return {};
    auto vmobject = ContiguousVMObject::try_create_with_size(size);
    if (!vmobject) {
        kernel_page_directory().range_allocator().deallocate(range.value());
        return {};
    }
    return allocate_kernel_region_with_vmobject(range.value(), *vmobject, name, access, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region(size_t size, StringView name, Region::Access access, AllocationStrategy strategy, Region::Cacheable cacheable)
{
    VERIFY(!(size % PAGE_SIZE));
    auto vm_object = AnonymousVMObject::try_create_with_size(size, strategy);
    if (!vm_object)
        return {};
    ScopedSpinLock lock(s_mm_lock);
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.has_value())
        return {};
    return allocate_kernel_region_with_vmobject(range.value(), vm_object.release_nonnull(), name, access, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region(PhysicalAddress paddr, size_t size, StringView name, Region::Access access, Region::Cacheable cacheable)
{
    auto vm_object = AnonymousVMObject::try_create_for_physical_range(paddr, size);
    if (!vm_object)
        return {};
    VERIFY(!(size % PAGE_SIZE));
    ScopedSpinLock lock(s_mm_lock);
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.has_value())
        return {};
    return allocate_kernel_region_with_vmobject(range.value(), *vm_object, name, access, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region_identity(PhysicalAddress paddr, size_t size, StringView name, Region::Access access, Region::Cacheable cacheable)
{
    auto vm_object = AnonymousVMObject::try_create_for_physical_range(paddr, size);
    if (!vm_object)
        return {};
    VERIFY(!(size % PAGE_SIZE));
    ScopedSpinLock lock(s_mm_lock);
    auto range = kernel_page_directory().identity_range_allocator().allocate_specific(VirtualAddress(paddr.get()), size);
    if (!range.has_value())
        return {};
    return allocate_kernel_region_with_vmobject(range.value(), *vm_object, name, access, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region_with_vmobject(Range const& range, VMObject& vmobject, StringView name, Region::Access access, Region::Cacheable cacheable)
{
    ScopedSpinLock lock(s_mm_lock);
    auto region = Region::try_create_kernel_only(range, vmobject, 0, KString::try_create(name), access, cacheable);
    if (region)
        region->map(kernel_page_directory());
    return region;
}

OwnPtr<Region> MemoryManager::allocate_kernel_region_with_vmobject(VMObject& vmobject, size_t size, StringView name, Region::Access access, Region::Cacheable cacheable)
{
    VERIFY(!(size % PAGE_SIZE));
    ScopedSpinLock lock(s_mm_lock);
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    if (!range.has_value())
        return {};
    return allocate_kernel_region_with_vmobject(range.value(), vmobject, name, access, cacheable);
}

bool MemoryManager::commit_user_physical_pages(size_t page_count)
{
    VERIFY(page_count > 0);
    ScopedSpinLock lock(s_mm_lock);
    if (m_system_memory_info.user_physical_pages_uncommitted < page_count)
        return false;

    m_system_memory_info.user_physical_pages_uncommitted -= page_count;
    m_system_memory_info.user_physical_pages_committed += page_count;
    return true;
}

void MemoryManager::uncommit_user_physical_pages(size_t page_count)
{
    VERIFY(page_count > 0);
    ScopedSpinLock lock(s_mm_lock);
    VERIFY(m_system_memory_info.user_physical_pages_committed >= page_count);

    m_system_memory_info.user_physical_pages_uncommitted += page_count;
    m_system_memory_info.user_physical_pages_committed -= page_count;
}

void MemoryManager::deallocate_physical_page(PhysicalAddress paddr)
{
    ScopedSpinLock lock(s_mm_lock);

    for (auto& region : m_user_physical_regions) {
        if (!region.contains(paddr))
            continue;

        region.return_page(paddr);
        --m_system_memory_info.user_physical_pages_used;

        ++m_system_memory_info.user_physical_pages_uncommitted;
        return;
    }

    for (auto& region : m_super_physical_regions) {
        if (!region.contains(paddr)) {
            dbgln("MM: deallocate_supervisor_physical_page: {} not in {} - {}", paddr, region.lower(), region.upper());
            continue;
        }

        region.return_page(paddr);
        --m_system_memory_info.super_physical_pages_used;
        return;
    }

    PANIC("MM: deallocate_user_physical_page couldn't figure out region for page @ {}", paddr);
}

RefPtr<PhysicalPage> MemoryManager::find_free_user_physical_page(bool committed)
{
    VERIFY(s_mm_lock.is_locked());
    RefPtr<PhysicalPage> page;
    if (committed) {
        VERIFY(m_system_memory_info.user_physical_pages_committed > 0);
        m_system_memory_info.user_physical_pages_committed--;
    } else {
        if (m_system_memory_info.user_physical_pages_uncommitted == 0)
            return {};
        m_system_memory_info.user_physical_pages_uncommitted--;
    }
    for (auto& region : m_user_physical_regions) {
        page = region.take_free_page();
        if (!page.is_null()) {
            ++m_system_memory_info.user_physical_pages_used;
            break;
        }
    }
    VERIFY(!committed || !page.is_null());
    return page;
}

NonnullRefPtr<PhysicalPage> MemoryManager::allocate_committed_user_physical_page(ShouldZeroFill should_zero_fill)
{
    ScopedSpinLock lock(s_mm_lock);
    auto page = find_free_user_physical_page(true);
    if (should_zero_fill == ShouldZeroFill::Yes) {
        auto* ptr = quickmap_page(*page);
        memset(ptr, 0, PAGE_SIZE);
        unquickmap_page();
    }
    return page.release_nonnull();
}

RefPtr<PhysicalPage> MemoryManager::allocate_user_physical_page(ShouldZeroFill should_zero_fill, bool* did_purge)
{
    ScopedSpinLock lock(s_mm_lock);
    auto page = find_free_user_physical_page(false);
    bool purged_pages = false;

    if (!page) {
        for_each_vmobject([&](auto& vmobject) {
            if (!vmobject.is_anonymous())
                return IterationDecision::Continue;
            int purged_page_count = static_cast<AnonymousVMObject&>(vmobject).purge();
            if (purged_page_count) {
                dbgln("MM: Purge saved the day! Purged {} pages from AnonymousVMObject", purged_page_count);
                page = find_free_user_physical_page(false);
                purged_pages = true;
                VERIFY(page);
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });
        if (!page) {
            dmesgln("MM: no user physical pages available");
            return {};
        }
    }

    if (should_zero_fill == ShouldZeroFill::Yes) {
        auto* ptr = quickmap_page(*page);
        memset(ptr, 0, PAGE_SIZE);
        unquickmap_page();
    }

    if (did_purge)
        *did_purge = purged_pages;
    return page;
}

NonnullRefPtrVector<PhysicalPage> MemoryManager::allocate_contiguous_supervisor_physical_pages(size_t size)
{
    VERIFY(!(size % PAGE_SIZE));
    ScopedSpinLock lock(s_mm_lock);
    size_t count = ceil_div(size, static_cast<size_t>(PAGE_SIZE));
    NonnullRefPtrVector<PhysicalPage> physical_pages;

    for (auto& region : m_super_physical_regions) {
        physical_pages = region.take_contiguous_free_pages(count);
        if (!physical_pages.is_empty())
            continue;
    }

    if (physical_pages.is_empty()) {
        if (m_super_physical_regions.is_empty()) {
            dmesgln("MM: no super physical regions available (?)");
        }

        dmesgln("MM: no super physical pages available");
        VERIFY_NOT_REACHED();
        return {};
    }

    auto cleanup_region = MM.allocate_kernel_region(physical_pages[0].paddr(), PAGE_SIZE * count, "MemoryManager Allocation Sanitization", Region::Access::Read | Region::Access::Write);
    fast_u32_fill((u32*)cleanup_region->vaddr().as_ptr(), 0, (PAGE_SIZE * count) / sizeof(u32));
    m_system_memory_info.super_physical_pages_used += count;
    return physical_pages;
}

RefPtr<PhysicalPage> MemoryManager::allocate_supervisor_physical_page()
{
    ScopedSpinLock lock(s_mm_lock);
    RefPtr<PhysicalPage> page;

    for (auto& region : m_super_physical_regions) {
        page = region.take_free_page();
        if (!page.is_null())
            break;
    }

    if (!page) {
        if (m_super_physical_regions.is_empty()) {
            dmesgln("MM: no super physical regions available (?)");
        }

        dmesgln("MM: no super physical pages available");
        VERIFY_NOT_REACHED();
        return {};
    }

    fast_u32_fill((u32*)page->paddr().offset(kernel_base).as_ptr(), 0, PAGE_SIZE / sizeof(u32));
    ++m_system_memory_info.super_physical_pages_used;
    return page;
}

void MemoryManager::enter_process_paging_scope(Process& process)
{
    enter_space(process.space());
}

void MemoryManager::enter_space(Space& space)
{
    auto current_thread = Thread::current();
    VERIFY(current_thread != nullptr);
    ScopedSpinLock lock(s_mm_lock);

    current_thread->regs().cr3 = space.page_directory().cr3();
    write_cr3(space.page_directory().cr3());
}

void MemoryManager::flush_tlb_local(VirtualAddress vaddr, size_t page_count)
{
    Processor::flush_tlb_local(vaddr, page_count);
}

void MemoryManager::flush_tlb(PageDirectory const* page_directory, VirtualAddress vaddr, size_t page_count)
{
    Processor::flush_tlb(page_directory, vaddr, page_count);
}

PageDirectoryEntry* MemoryManager::quickmap_pd(PageDirectory& directory, size_t pdpt_index)
{
    VERIFY(s_mm_lock.own_lock());
    auto& mm_data = get_data();
    auto& pte = boot_pd_kernel_pt1023[(KERNEL_QUICKMAP_PD - KERNEL_PT1024_BASE) / PAGE_SIZE];
    auto pd_paddr = directory.m_directory_pages[pdpt_index]->paddr();
    if (pte.physical_page_base() != pd_paddr.get()) {
        pte.set_physical_page_base(pd_paddr.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);

        flush_tlb_local(VirtualAddress(KERNEL_QUICKMAP_PD));
    } else {

        if (mm_data.m_last_quickmap_pd != pd_paddr)
            flush_tlb_local(VirtualAddress(KERNEL_QUICKMAP_PD));
    }
    mm_data.m_last_quickmap_pd = pd_paddr;
    return (PageDirectoryEntry*)KERNEL_QUICKMAP_PD;
}

PageTableEntry* MemoryManager::quickmap_pt(PhysicalAddress pt_paddr)
{
    VERIFY(s_mm_lock.own_lock());
    auto& mm_data = get_data();
    auto& pte = ((PageTableEntry*)boot_pd_kernel_pt1023)[(KERNEL_QUICKMAP_PT - KERNEL_PT1024_BASE) / PAGE_SIZE];
    if (pte.physical_page_base() != pt_paddr.get()) {
        pte.set_physical_page_base(pt_paddr.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);

        flush_tlb_local(VirtualAddress(KERNEL_QUICKMAP_PT));
    } else {

        if (mm_data.m_last_quickmap_pt != pt_paddr)
            flush_tlb_local(VirtualAddress(KERNEL_QUICKMAP_PT));
    }
    mm_data.m_last_quickmap_pt = pt_paddr;
    return (PageTableEntry*)KERNEL_QUICKMAP_PT;
}

u8* MemoryManager::quickmap_page(PhysicalAddress const& physical_address)
{
    VERIFY_INTERRUPTS_DISABLED();
    auto& mm_data = get_data();
    mm_data.m_quickmap_prev_flags = mm_data.m_quickmap_in_use.lock();
    ScopedSpinLock lock(s_mm_lock);

    VirtualAddress vaddr(KERNEL_QUICKMAP_PER_CPU_BASE + Processor::id() * PAGE_SIZE);
    u32 pte_idx = (vaddr.get() - KERNEL_PT1024_BASE) / PAGE_SIZE;

    auto& pte = ((PageTableEntry*)boot_pd_kernel_pt1023)[pte_idx];
    if (pte.physical_page_base() != physical_address.get()) {
        pte.set_physical_page_base(physical_address.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);
        flush_tlb_local(vaddr);
    }
    return vaddr.as_ptr();
}

void MemoryManager::unquickmap_page()
{
    VERIFY_INTERRUPTS_DISABLED();
    ScopedSpinLock lock(s_mm_lock);
    auto& mm_data = get_data();
    VERIFY(mm_data.m_quickmap_in_use.is_locked());
    VirtualAddress vaddr(KERNEL_QUICKMAP_PER_CPU_BASE + Processor::id() * PAGE_SIZE);
    u32 pte_idx = (vaddr.get() - KERNEL_PT1024_BASE) / PAGE_SIZE;
    auto& pte = ((PageTableEntry*)boot_pd_kernel_pt1023)[pte_idx];
    pte.clear();
    flush_tlb_local(vaddr);
    mm_data.m_quickmap_in_use.unlock(mm_data.m_quickmap_prev_flags);
}

bool MemoryManager::validate_user_stack_no_lock(Space& space, VirtualAddress vaddr) const
{
    VERIFY(space.get_lock().own_lock());

    if (!is_user_address(vaddr))
        return false;

    auto* region = find_user_region_from_vaddr_no_lock(space, vaddr);
    return region && region->is_user() && region->is_stack();
}

bool MemoryManager::validate_user_stack(Space& space, VirtualAddress vaddr) const
{
    ScopedSpinLock lock(space.get_lock());
    return validate_user_stack_no_lock(space, vaddr);
}

void MemoryManager::register_vmobject(VMObject& vmobject)
{
    ScopedSpinLock lock(s_mm_lock);
    m_vmobjects.append(vmobject);
}

void MemoryManager::unregister_vmobject(VMObject& vmobject)
{
    ScopedSpinLock lock(s_mm_lock);
    m_vmobjects.remove(vmobject);
}

void MemoryManager::register_region(Region& region)
{
    ScopedSpinLock lock(s_mm_lock);
    if (region.is_kernel())
        m_kernel_regions.append(region);
    else
        m_user_regions.append(region);
}

void MemoryManager::unregister_region(Region& region)
{
    ScopedSpinLock lock(s_mm_lock);
    if (region.is_kernel())
        m_kernel_regions.remove(region);
    else
        m_user_regions.remove(region);
}

void MemoryManager::dump_kernel_regions()
{
    dbgln("Kernel regions:");
#if ARCH(I386)
    auto addr_padding = "";
#else
    auto addr_padding = "        ";
#endif
    dbgln("BEGIN{}         END{}        SIZE{}       ACCESS NAME",
        addr_padding, addr_padding, addr_padding);
    ScopedSpinLock lock(s_mm_lock);
    for (auto& region : m_kernel_regions) {
        dbgln("{:p} -- {:p} {:p} {:c}{:c}{:c}{:c}{:c}{:c} {}",
            region.vaddr().get(),
            region.vaddr().offset(region.size() - 1).get(),
            region.size(),
            region.is_readable() ? 'R' : ' ',
            region.is_writable() ? 'W' : ' ',
            region.is_executable() ? 'X' : ' ',
            region.is_shared() ? 'S' : ' ',
            region.is_stack() ? 'T' : ' ',
            region.is_syscall_region() ? 'C' : ' ',
            region.name());
    }
}

void MemoryManager::set_page_writable_direct(VirtualAddress vaddr, bool writable)
{
    ScopedSpinLock lock(s_mm_lock);
    ScopedSpinLock page_lock(kernel_page_directory().get_lock());
    auto* pte = ensure_pte(kernel_page_directory(), vaddr);
    VERIFY(pte);
    if (pte->is_writable() == writable)
        return;
    pte->set_writable(writable);
    flush_tlb(&kernel_page_directory(), vaddr);
}

}