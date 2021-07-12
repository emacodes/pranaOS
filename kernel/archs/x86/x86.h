/*
 * Copyright (c) 2021, krishpranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include "archs/Arch.h"

namespace Arch::x86
{

#ifdef __x86_64__
using CRRegister = uint64_t;
#else
using CRRegister = uint32_t;
#endif

static inline CRRegister CR0()
{
    CRRegister r;
    asm volatile("mov %%cr0, %0"
                 : "=r"(r));
    return r;
}
