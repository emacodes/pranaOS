/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <base/NonnullRefPtr.h>
#include <kernel/Forward.h>

namespace Kernel {

class Mount {
public:
    Mount(FileSystem&, Custody* host_custody, int flags);
    Mount(Inode& source, Custody& host_custody, int flags);

    Inode const* host() const;
    Inode* host();

    Inode const& guest() const { return *m_guest; }
    Inode& guest() { return *m_guest; }

    FileSystem const& guest_fs() const { return *m_guest_fs; }
    FileSystem& guest_fs() { return *m_guest_fs; }

    String absolute_path() const;

    int flags() const { return m_flags; }
    void set_flags(int flags) { m_flags = flags; }

private:
    NonnullRefPtr<Inode> m_guest;
    NonnullRefPtr<FileSystem> m_guest_fs;
    RefPtr<Custody> m_host_custody;
    int m_flags;
};

}
