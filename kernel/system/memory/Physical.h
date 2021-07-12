/*
 * Copyright (c) 2021, krishpranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <libutils/Prelude.h>
#include "system/memory/MemoryRange.h"

extern size_t TOTAL_MEMORY;
extern size_t USED_MEMORY;
extern uint8_t MEMORY[1024 * 1024 / 8];

MemoryRange physical_alloc(size_t size);

