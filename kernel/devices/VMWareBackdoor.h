/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <base/Optional.h>
#include <base/Types.h>
#include <base/kmalloc.h>
#include <kernel/api/MousePacket.h>

namespace Kernel {

#define VMMOUSE_GETVERSION 10
#define VMMOUSE_DATA 39
#define VMMOUSE_STATUS 40
#define VMMOUSE_COMMAND 41

struct VMWareCommand {
    union {
        u32 ax;
        u32 magic;
    };
    union {
        u32 bx;
        u32 size;
    };
    union {
        u32 cx;
        u32 command;
    };
    union {
        u32 dx;
        u32 port;
    };
    u32 si;
    u32 di;
};

class VMWareBackdoor {
    BASE_MAKE_ETERNAL;

public:
    VMWareBackdoor();
    static VMWareBackdoor* the();

    bool vmmouse_is_absolute() const;
    void enable_absolute_vmmouse();
    void disable_absolute_vmmouse();
    void send(VMWareCommand& command);

    Optional<MousePacket> receive_mouse_packet();

private:
    void send_high_bandwidth(VMWareCommand& command);
    void get_high_bandwidth(VMWareCommand& command);
    bool detect_vmmouse();
    bool m_vmmouse_absolute { false };
};

}
