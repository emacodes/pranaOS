/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <base/Assertions.h>
#include <base/Atomic.h>
#include <base/Noncopyable.h>
#ifdef KERNEL
#    include <Kernel/Arch/x86/Processor.h>
#    include <Kernel/Arch/x86/ScopedCritical.h>
#endif

#ifndef __pranaos__
#    include <new>
#endif

namespace Base {

template<typename T>
struct SingletonInstanceCreator {
    static T* create()
    {
        return new T();
    }
};

template<typename T, T* (*InitFunction)() = SingletonInstanceCreator<T>::create>
class Singleton {
    BASE_MAKE_NONCOPYABLE(Singleton);
    BASE_MAKE_NONMOVABLE(Singleton);

public:
    Singleton() = default;

    template<bool allow_create = true>
    static T* get(Atomic<T*>& obj_var)
    {
        T* obj = obj_var.load(Base::memory_order_acquire);
        if (FlatPtr(obj) <= 0x1) {
            // If this is the first time, see if we get to initialize it
#ifdef KERNEL
            Kernel::ScopedCritical critical;
#endif
            if constexpr (allow_create) {
                if (obj == nullptr && obj_var.compare_exchange_strong(obj, (T*)0x1, Base::memory_order_acq_rel)) {
                    // We're the first one
                    obj = InitFunction();
                    obj_var.store(obj, Base::memory_order_release);
                    return obj;
                }
            }
            // Someone else was faster, wait until they're done
            while (obj == (T*)0x1) {
#ifdef KERNEL
                Kernel::Processor::wait_check();
#else
                // TODO: yield
#endif
                obj = obj_var.load(Base::memory_order_acquire);
            }
            if constexpr (allow_create) {
                // We should always return an instance if we allow creating one
                VERIFY(obj != nullptr);
            }
            VERIFY(obj != (T*)0x1);
        }
        return obj;
    }

    T* ptr() const
    {
        return get(m_obj);
    }

    T* operator->() const
    {
        return ptr();
    }

    T& operator*() const
    {
        return *ptr();
    }

    operator T*() const
    {
        return ptr();
    }

    operator T&() const
    {
        return *ptr();
    }

    bool is_initialized() const
    {
        T* obj = m_obj.load(Base::MemoryOrder::memory_order_consume);
        return FlatPtr(obj) > 0x1;
    }

    void ensure_instance()
    {
        ptr();
    }

private:
    mutable Atomic<T*> m_obj { nullptr };
};

}

using Base::Singleton;
