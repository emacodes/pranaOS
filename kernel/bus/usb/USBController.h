/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <base/RefCounted.h>
#include <kernel/bus/usb/USBDevice.h>
#include <kernel/bus/usb/USBTransfer.h>
#include <kernel/KResult.h>

namespace Kernel::USB {

class USBController : public RefCounted<USBController> {
public:
    virtual ~USBController() = default;

    virtual KResult initialize() = 0;

    virtual KResult reset() = 0;
    virtual KResult stop() = 0;
    virtual KResult start() = 0;

    virtual KResultOr<size_t> submit_control_transfer(Transfer&) = 0;

    virtual RefPtr<USB::Device> const get_device_at_port(USB::Device::PortNumber) = 0;
    virtual RefPtr<USB::Device> const get_device_from_address(u8) = 0;

    u8 allocate_address();

private:
    u8 m_next_device_index { 1 };

    IntrusiveListNode<USBController, NonnullRefPtr<USBController>> m_controller_list_node;

public:
    using List = IntrusiveList<USBController, NonnullRefPtr<USBController>, &USBController::m_controller_list_node>;
};

}
