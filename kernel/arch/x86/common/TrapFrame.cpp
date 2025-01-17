/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

// includes
#include <kernel/arch/x86/TrapFrame.h>

namespace Kernel {

extern "C" void enter_trap_no_irq(TrapFrame* trap)
{
    InterruptDisabler disable;
    Processor::current().enter_trap(*trap, false);
}

extern "C" void enter_trap(TrapFrame* trap)
{
    InterruptDisabler disable;
    Processor::current().enter_trap(*trap, true);
}

extern "C" void exit_trap(TrapFrame* trap)
{
    InterruptDisabler disable;
    return Processor::current().exit_trap(*trap);
}

}
