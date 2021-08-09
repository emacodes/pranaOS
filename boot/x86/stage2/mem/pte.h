/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#ifndef _BOOT_X86_STAGE2_MEM_PTE_H
#define _BOOT_X86_STAGE2_MEM_PTE_H

// includes
#include "../types.h"

#define page_desc_t uint32_t
#define pte_t uint32_t

enum PAGE_DESC_PAGE_FLAGS {
    PAGE_DESC_PRESENT = 0,
    PAGE_DESC_WRITABLE,
    PAGE_DESC_USER,
    PAGE_DESC_WRITETHOUGH,
    PAGE_DESC_NOT_CACHEABLE,
    PAGE_DESC_ACCESSED,
    PAGE_DESC_DIRTY,
    PAGE_DESC_PAT,
    PAGE_DESC_CPU_GLOBAL,
    PAGE_DESC_LV4_GLOBAL,
    PAGE_DESC_COPY_ON_WRITE,
    PAGE_DESC_ZEROING_ON_DEMAND,
    PAGE_DESC_FRAME = 12
};

#endif 