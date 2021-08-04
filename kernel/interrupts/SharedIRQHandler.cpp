/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

// includes
#include <kernel/Assertions.h>
#include <kernel/Debug.h>
#include <kernel/interrupts/IRQHandler.h>
#include <kernel/interrupts/InterruptManagement.h>
#include <kernel/interrupts/PIC.h>
#include <kernel/interrupts/SharedIRQHandler.h>
#include <kernel/Sections.h>

namespace Kernel {

UNMAP_AFTER_INIT void SharedIRQHandler::initialize(u8 interrupt_number)
{
    auto* handler = new SharedIRQHandler(interrupt_number);
    handler->register_interrupt_handler();
    handler->disable_interrupt_vector();
}

void SharedIRQHandler::register_handler(GenericInterruptHandler& handler)
{
    dbgln_if(INTERRUPT_DEBUG, "Interrupt Handler registered @ Shared Interrupt Handler {}", interrupt_number());
    m_handlers.append(handler);
    enable_interrupt_vector();
}
void SharedIRQHandler::unregister_handler(GenericInterruptHandler& handler)
{
    dbgln_if(INTERRUPT_DEBUG, "Interrupt Handler unregistered @ Shared Interrupt Handler {}", interrupt_number());
    m_handlers.remove(handler);
    if (m_handlers.is_empty())
        disable_interrupt_vector();
}

bool SharedIRQHandler::eoi()
{
    dbgln_if(INTERRUPT_DEBUG, "EOI IRQ {}", interrupt_number());
    m_responsible_irq_controller->eoi(*this);
    return true;
}

void SharedIRQHandler::enumerate_handlers(Function<void(GenericInterruptHandler&)>& callback)
{
    for (auto& handler : m_handlers) {
        callback(handler);
    }
}

SharedIRQHandler::SharedIRQHandler(u8 irq)
    : GenericInterruptHandler(irq)
    , m_responsible_irq_controller(InterruptManagement::the().get_responsible_irq_controller(irq))
{
    dbgln_if(INTERRUPT_DEBUG, "Shared Interrupt Handler registered @ {}", interrupt_number());
}

SharedIRQHandler::~SharedIRQHandler()
{
    dbgln_if(INTERRUPT_DEBUG, "Shared Interrupt Handler unregistered @ {}", interrupt_number());
    disable_interrupt_vector();
}

bool SharedIRQHandler::handle_interrupt(const RegisterState& regs)
{
    VERIFY_INTERRUPTS_DISABLED();

    if constexpr (INTERRUPT_DEBUG) {
        dbgln("Interrupt @ {}", interrupt_number());
        dbgln("Interrupt Handlers registered - {}", m_handlers.size_slow());
    }
    int i = 0;
    bool was_handled = false;
    for (auto& handler : m_handlers) {
        dbgln_if(INTERRUPT_DEBUG, "Going for Interrupt Handling @ {}, Shared Interrupt {}", i, interrupt_number());
        if (handler.handle_interrupt(regs)) {
            handler.increment_invoking_counter();
            was_handled = true;
        }
        dbgln_if(INTERRUPT_DEBUG, "Going for Interrupt Handling @ {}, Shared Interrupt {} - End", i, interrupt_number());
        i++;
    }
    return was_handled;
}

void SharedIRQHandler::enable_interrupt_vector()
{
    if (m_enabled)
        return;
    m_enabled = true;
    m_responsible_irq_controller->enable(*this);
}

void SharedIRQHandler::disable_interrupt_vector()
{
    if (!m_enabled)
        return;
    m_enabled = false;
    m_responsible_irq_controller->disable(*this);
}

}