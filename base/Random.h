/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <base/Platform.h>
#include <base/Types.h>

#if defined(__pranaos__)
#    include <stdlib.h>
#endif

#if defined(__unix__)
#    include <unistd.h>
#endif

#if defined(__APPLE__)
#    include <sys/random.h>
#endif

namespace Base {

inline void fill_with_random([[maybe_unused]] void* buffer, [[maybe_unused]] size_t length)
{
#if defined(__pranaos__)
    arc4random_buf(buffer, length);
#elif defined(OSS_FUZZ)
#elif defined(__unix__) or defined(__APPLE__)
    [[maybe_unused]] int rc = getentropy(buffer, length);
#endif
}

template<typename T>
inline T get_random()
{
    T t;
    fill_with_random(&t, sizeof(T));
    return t;
}

u32 get_random_uniform(u32 max_bounds);

}

using Base::fill_with_random;
using Base::get_random;
using Base::get_random_uniform;
