/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

// includes
#include <kernel/devices/PCISerialDevice.h>
#include <kernel/Sections.h>

namespace Kernel {

static SerialDevice* s_the = nullptr;

UNMAP_AFTER_INIT void PCISerialDevice::detect()
{
    size_t current_device_minor = 68;
    PCI::enumerate([&](const PCI::Address& address, PCI::ID id) {
        if (address.is_null())
            return;

        for (auto& board_definition : board_definitions) {
            if (board_definition.device_id != id)
                continue;

            auto bar_base = PCI::get_BAR(address, board_definition.pci_bar) & ~1;
            auto port_base = IOAddress(bar_base + board_definition.first_offset);
            for (size_t i = 0; i < board_definition.port_count; i++) {
                auto serial_device = new SerialDevice(port_base.offset(board_definition.port_size * i), current_device_minor++);
                if (board_definition.baud_rate != SerialDevice::Baud::Baud38400) 
                    serial_device->set_baud(board_definition.baud_rate);

                if (!is_available())
                    s_the = serial_device;

            }

            dmesgln("PCISerialDevice: Found {} @ {}", board_definition.name, address);
            return;
        }
    });
}

SerialDevice& PCISerialDevice::the()
{
    VERIFY(s_the);
    return *s_the;
}

bool PCISerialDevice::is_available()
{
    return s_the;
}

}
