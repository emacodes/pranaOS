/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

#if defined(KERNEL)
#    include <kernel/Assertions.h>
#else
#    include <assert.h>
#    define VERIFY assert
#    define VERIFY_NOT_REACHED() assert(false)
static constexpr bool TODO = false;
#    define TODO() VERIFY(TODO)
#endif
