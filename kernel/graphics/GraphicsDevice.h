/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <base/String.h>
#include <base/Types.h>
#include <kernel/bus/pci/Definitions.h>
#include <kernel/devices/BlockDevice.h>
#include <kernel/PhysicalAddress.h>

namespace Kernel {
class GraphicsDevice : public RefCounted<GraphicsDevice> {
public:
    enum class Type {
        VGACompatible,
        Bochs,
        SVGA,
        Raw
    };
    virtual ~GraphicsDevice() = default;
    virtual void initialize_framebuffer_devices() = 0;
    virtual Type type() const = 0;
    virtual void enable_consoles() = 0;
    virtual void disable_consoles() = 0;
    bool consoles_enabled() const { return m_consoles_enabled; }
    virtual bool framebuffer_devices_initialized() const = 0;

    virtual bool modesetting_capable() const = 0;
    virtual bool double_framebuffering_capable() const = 0;

    virtual bool try_to_set_resolution(size_t output_port_index, size_t width, size_t height) = 0;
    virtual bool set_y_offset(size_t output_port_index, size_t y) = 0;

protected:
    GraphicsDevice() = default;

    bool m_consoles_enabled { false };
};

}
