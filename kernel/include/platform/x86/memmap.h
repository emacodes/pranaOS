/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#ifndef _KERNEL_PLATFORM_X86_MEMMAP_H
#define _KERNEL_PLATFORM_X86_MEMMAP_H

#define KERNEL_PM_BASE 0x00100000
#define KERNEL_BASE 0xc0000000
#define BIOS_SETTING_PM_BASE 0x00000000
#define BIOS_SETTING_BASE 0xffc00000
#define KMALLOC_BASE (KERNEL_BASE + 0x400000)

#endif 