/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <base/Array.h>
#include <base/Concepts.h>
#include <base/Function.h>
#include <base/Types.h>
#include <kernel/arch/x86/ASM_wrapper.h>
#include <kernel/arch/x86/CPUID.h>
#include <kernel/arch/x86/DescriptorTable.h>
#include <kernel/arch/x86/PageDirectory.h>
#include <kernel/arch/x86/TSS.h>
#include <kernel/Forward.h>

namespace Kernel {

class ProcessorInfo;
struct ProcessorMessageEntry;

enum class ProcessorSpecificDataID {
    MemoryManager,
    Scheduler,
    __Count,
};

#if ARCH(X86_64)
#    define MSR_FS_BASE 0xc0000100
#    define MSR_GS_BASE 0xc0000101
#endif

extern "C" void thread_context_first_enter(void);
extern "C" void exit_kernel_thread(void);
extern "C" void do_assume_context(Thread* thread, u32 flags);

struct [[gnu::aligned(16)]] FPUState
{
    u8 buffer[512];
};

struct ProcessorMessage {
    using CallbackFunction = Function<void()>;

    enum Type {
        FlushTlb,
        Callback,
    };
    Type type;
    Atomic<u32> refs;
    union {
        ProcessorMessage* next; 
        alignas(CallbackFunction) u8 callback_storage[sizeof(CallbackFunction)];
        struct {
            Memory::PageDirectory const* page_directory;
            u8* ptr;
            size_t page_count;
        } flush_tlb;
    };

    volatile bool async;

    ProcessorMessageEntry* per_proc_entries;

    CallbackFunction& callback_value()
    {
        return *bit_cast<CallbackFunction*>(&callback_storage);
    }

    void invoke_callback()
    {
        VERIFY(type == Type::Callback);
        callback_value()();
    }
};

struct ProcessorMessageEntry {
    ProcessorMessageEntry* next;
    ProcessorMessage* msg;
};

struct DeferredCallEntry {
    using HandlerFunction = Function<void()>;

    DeferredCallEntry* next;
    alignas(HandlerFunction) u8 handler_storage[sizeof(HandlerFunction)];
    bool was_allocated;

    HandlerFunction& handler_value()
    {
        return *bit_cast<HandlerFunction*>(&handler_storage);
    }

    void invoke_handler()
    {
        handler_value()();
    }
};

class Processor;

using ProcessorContainer = Array<Processor*, 8>;

class Processor {
    friend class ProcessorInfo;

    BASE_MAKE_NONCOPYABLE(Processor);
    BASE_MAKE_NONMOVABLE(Processor);

    Processor* m_self;

    DescriptorTablePointer m_gdtr;
    Descriptor m_gdt[256];
    u32 m_gdt_length;

    u32 m_cpu;
    u32 m_in_irq;
    volatile u32 m_in_critical {};
    static Atomic<u32> s_idle_cpu_mask;

    TSS m_tss;
    static FPUState s_clean_fpu_state;
    CPUFeature m_features;
    static Atomic<u32> g_total_processors;
    u8 m_physical_address_bit_width;

    ProcessorInfo* m_info;
    Thread* m_current_thread;
    Thread* m_idle_thread;

    Atomic<ProcessorMessageEntry*> m_message_queue;

    bool m_invoke_scheduler_async;
    bool m_scheduler_initialized;
    Atomic<bool> m_halt_requested;

    DeferredCallEntry* m_pending_deferred_calls; 
    DeferredCallEntry* m_free_deferred_call_pool_entry;
    DeferredCallEntry m_deferred_call_pool[5];

    void* m_processor_specific_data[(size_t)ProcessorSpecificDataID::__Count];

    void gdt_init();
    void write_raw_gdt_entry(u16 selector, u32 low, u32 high);
    void write_gdt_entry(u16 selector, Descriptor& descriptor);
    static ProcessorContainer& processors();

    static void smp_return_to_pool(ProcessorMessage& msg);
    static ProcessorMessage& smp_get_from_pool();
    static void smp_cleanup_message(ProcessorMessage& msg);
    bool smp_enqueue_message(ProcessorMessage&);
    static void smp_unicast_message(u32 cpu, ProcessorMessage& msg, bool async);
    static void smp_broadcast_message(ProcessorMessage& msg);
    static void smp_broadcast_wait_sync(ProcessorMessage& msg);
    static void smp_broadcast_halt();

    void deferred_call_pool_init();
    void deferred_call_execute_pending();
    DeferredCallEntry* deferred_call_get_free();
    void deferred_call_return_to_pool(DeferredCallEntry*);
    void deferred_call_queue_entry(DeferredCallEntry*);

    void cpu_detect();
    void cpu_setup();

    String features_string() const;

public:
    Processor() = default;

    void early_initialize(u32 cpu);
    void initialize(u32 cpu);

    void detect_hypervisor();
    void detect_hypervisor_hyperv(CPUID const& hypervisor_leaf_range);

    void idle_begin()
    {
        s_idle_cpu_mask.fetch_or(1u << m_cpu, Base::MemoryOrder::memory_order_relaxed);
    }

    void idle_end()
    {
        s_idle_cpu_mask.fetch_and(~(1u << m_cpu), Base::MemoryOrder::memory_order_relaxed);
    }

    static u32 count()
    {
        return *g_total_processors.ptr();
    }

    ALWAYS_INLINE static void pause()
    {
        asm volatile("pause");
    }

    ALWAYS_INLINE static void wait_check()
    {
        Processor::pause();
        if (Processor::is_smp_enabled())
            Processor::current().smp_process_pending_messages();
    }

    [[noreturn]] static void halt();

    static void flush_entire_tlb_local()
    {
        write_cr3(read_cr3());
    }

    static void flush_tlb_local(VirtualAddress vaddr, size_t page_count);
    static void flush_tlb(Memory::PageDirectory const*, VirtualAddress, size_t);

    Descriptor& get_gdt_entry(u16 selector);
    void flush_gdt();
    const DescriptorTablePointer& get_gdtr();

    static size_t processor_count() { return processors().size(); }

    template<IteratorFunction<Processor&> Callback>
    static inline IterationDecision for_each(Callback callback)
    {
        auto& procs = processors();
        size_t count = procs.size();
        for (size_t i = 0; i < count; i++) {
            if (callback(*procs[i]) == IterationDecision::Break)
                return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    }

    template<VoidFunction<Processor&> Callback>
    static inline IterationDecision for_each(Callback callback)
    {
        auto& procs = processors();
        size_t count = procs.size();
        for (size_t i = 0; i < count; i++) {
            if (procs[i] != nullptr)
                callback(*procs[i]);
        }
        return IterationDecision::Continue;
    }

    ALWAYS_INLINE u8 physical_address_bit_width() const { return m_physical_address_bit_width; }

    ALWAYS_INLINE ProcessorInfo& info() { return *m_info; }

    static bool is_smp_enabled();

    ALWAYS_INLINE static Processor& current()
    {
        return *(Processor*)read_gs_ptr(__builtin_offsetof(Processor, m_self));
    }

    ALWAYS_INLINE static bool is_initialized()
    {
        return
#if ARCH(I386)
            get_gs() == GDT_SELECTOR_PROC &&
#endif
            read_gs_ptr(__builtin_offsetof(Processor, m_self)) != 0;
    }

    template<typename T>
    T* get_specific()
    {
        return static_cast<T*>(m_processor_specific_data[static_cast<size_t>(T::processor_specific_data_id())]);
    }

    void set_specific(ProcessorSpecificDataID specific_id, void* ptr)
    {
        m_processor_specific_data[static_cast<size_t>(specific_id)] = ptr;
    }

    ALWAYS_INLINE void set_idle_thread(Thread& idle_thread)
    {
        m_idle_thread = &idle_thread;
    }

    ALWAYS_INLINE static Thread* current_thread()
    {
        return (Thread*)read_gs_ptr(__builtin_offsetof(Processor, m_current_thread));
    }

    ALWAYS_INLINE static void set_current_thread(Thread& current_thread)
    {
        write_gs_ptr(__builtin_offsetof(Processor, m_current_thread), FlatPtr(&current_thread));
    }

    ALWAYS_INLINE static Thread* idle_thread()
    {
        return (Thread*)read_gs_ptr(__builtin_offsetof(Processor, m_idle_thread));
    }

    ALWAYS_INLINE u32 get_id() const
    {
        return m_cpu;
    }

    ALWAYS_INLINE static u32 id()
    {
        return read_gs_ptr(__builtin_offsetof(Processor, m_cpu));
    }

    ALWAYS_INLINE static bool is_bootstrap_processor()
    {
        return Processor::id() == 0;
    }

    ALWAYS_INLINE u32& in_irq()
    {
        return m_in_irq;
    }

    ALWAYS_INLINE static void restore_in_critical(u32 critical)
    {
        write_gs_ptr(__builtin_offsetof(Processor, m_in_critical), critical);
    }

    ALWAYS_INLINE static void enter_critical()
    {
        write_gs_ptr(__builtin_offsetof(Processor, m_in_critical), in_critical() + 1);
    }

private:
    ALWAYS_INLINE void do_leave_critical()
    {
        VERIFY(m_in_critical > 0);
        if (m_in_critical == 1) {
            if (!m_in_irq) {
                deferred_call_execute_pending();
                VERIFY(m_in_critical == 1);
            }
            m_in_critical = 0;
            if (!m_in_irq)
                check_invoke_scheduler();
        } else {
            m_in_critical = m_in_critical - 1;
        }
    }

public:
    ALWAYS_INLINE static void leave_critical()
    {
        current().do_leave_critical();
    }

    ALWAYS_INLINE static u32 clear_critical()
    {
        auto prev_critical = in_critical();
        write_gs_ptr(__builtin_offsetof(Processor, m_in_critical), 0);
        auto& proc = current();
        if (!proc.m_in_irq)
            proc.check_invoke_scheduler();
        return prev_critical;
    }

    ALWAYS_INLINE static void restore_critical(u32 prev_critical)
    {
        write_gs_ptr(__builtin_offsetof(Processor, m_in_critical), prev_critical);
    }

    ALWAYS_INLINE static u32 in_critical()
    {
        return read_gs_ptr(__builtin_offsetof(Processor, m_in_critical));
    }

    ALWAYS_INLINE const FPUState& clean_fpu_state() const
    {
        return s_clean_fpu_state;
    }

    static void smp_enable();
    bool smp_process_pending_messages();

    static void smp_unicast(u32 cpu, Function<void()>, bool async);
    static void smp_broadcast_flush_tlb(Memory::PageDirectory const*, VirtualAddress, size_t);
    static u32 smp_wake_n_idle_processors(u32 wake_count);

    static void deferred_call_queue(Function<void()> callback);

    ALWAYS_INLINE bool has_feature(CPUFeature f) const
    {
        return (static_cast<u32>(m_features) & static_cast<u32>(f)) != 0;
    }

    void check_invoke_scheduler();
    void invoke_scheduler_async() { m_invoke_scheduler_async = true; }

    void enter_trap(TrapFrame& trap, bool raise_irq);

    void exit_trap(TrapFrame& trap);

    [[noreturn]] void initialize_context_switching(Thread& initial_thread);
    NEVER_INLINE void switch_context(Thread*& from_thread, Thread*& to_thread);
    [[noreturn]] static void assume_context(Thread& thread, FlatPtr flags);
    FlatPtr init_context(Thread& thread, bool leave_crit);
    static Vector<FlatPtr> capture_stack_trace(Thread& thread, size_t max_frames = 0);

    String platform_string() const;
};

template<typename T>
class ProcessorSpecific {
public:
    static void initialize()
    {
        Processor::current().set_specific(T::processor_specific_data_id(), new T);
    }
    static T& get()
    {
        return *Processor::current().get_specific<T>();
    }
};

}
