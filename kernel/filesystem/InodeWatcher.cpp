/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

// includes
#include <base/Memory.h>
#include <kernel/filesystem/Inode.h>
#include <kernel/filesystem/InodeWatcher.h>
#include <kernel/Process.h>

namespace Kernel {

KResultOr<NonnullRefPtr<InodeWatcher>> InodeWatcher::create()
{
    auto watcher = adopt_ref_if_nonnull(new (nothrow) InodeWatcher);
    if (watcher)
        return watcher.release_nonnull();
    return ENOMEM;
}

InodeWatcher::~InodeWatcher()
{
    (void)close();
}

bool InodeWatcher::can_read(const FileDescription&, size_t) const
{
    MutexLocker locker(m_lock);
    return !m_queue.is_empty();
}

KResultOr<size_t> InodeWatcher::read(FileDescription&, u64, UserOrKernelBuffer& buffer, size_t buffer_size)
{
    MutexLocker locker(m_lock);
    if (m_queue.is_empty())

        return EAGAIN;

    auto event = m_queue.dequeue();

    size_t name_length = event.path.length() + 1;
    size_t bytes_to_write = sizeof(InodeWatcherEvent);
    if (!event.path.is_null())
        bytes_to_write += name_length;

    if (buffer_size < bytes_to_write)
        return EINVAL;

    auto result = buffer.write_buffered<MAXIMUM_EVENT_SIZE>(bytes_to_write, [&](u8* data, size_t data_bytes) {
        size_t offset = 0;

        memcpy(data + offset, &event.wd, sizeof(InodeWatcherEvent::watch_descriptor));
        offset += sizeof(InodeWatcherEvent::watch_descriptor);
        memcpy(data + offset, &event.type, sizeof(InodeWatcherEvent::type));
        offset += sizeof(InodeWatcherEvent::type);

        if (!event.path.is_null()) {
            memcpy(data + offset, &name_length, sizeof(InodeWatcherEvent::name_length));
            offset += sizeof(InodeWatcherEvent::name_length);
            memcpy(data + offset, event.path.characters(), name_length);
        } else {
            memset(data + offset, 0, sizeof(InodeWatcherEvent::name_length));
        }

        return data_bytes;
    });
    evaluate_block_conditions();
    return result;
}

KResult InodeWatcher::close()
{
    MutexLocker locker(m_lock);

    for (auto& entry : m_wd_to_watches) {
        auto& inode = const_cast<Inode&>(entry.value->inode);
        inode.unregister_watcher({}, *this);
    }

    m_wd_to_watches.clear();
    m_inode_to_watches.clear();
    return KSuccess;
}

String InodeWatcher::absolute_path(const FileDescription&) const
{
    return String::formatted("InodeWatcher:({})", m_wd_to_watches.size());
}

void InodeWatcher::notify_inode_event(Badge<Inode>, InodeIdentifier inode_id, InodeWatcherEvent::Type event_type, String const& name)
{
    MutexLocker locker(m_lock);

    auto it = m_inode_to_watches.find(inode_id);
    if (it == m_inode_to_watches.end())
        return;

    auto& watcher = *it->value;
    if (!(watcher.event_mask & static_cast<unsigned>(event_type)))
        return;

    m_queue.enqueue({ watcher.wd, event_type, name });
    evaluate_block_conditions();
}

KResultOr<int> InodeWatcher::register_inode(Inode& inode, unsigned event_mask)
{
    MutexLocker locker(m_lock);

    if (m_inode_to_watches.find(inode.identifier()) != m_inode_to_watches.end())
        return EEXIST;

    int wd;
    do {
        wd = m_wd_counter.value();

        m_wd_counter++;
        if (m_wd_counter.has_overflow())
            m_wd_counter = 1;
    } while (m_wd_to_watches.find(wd) != m_wd_to_watches.end());

    auto description_or_error = WatchDescription::create(wd, inode, event_mask);
    if (description_or_error.is_error())
        return description_or_error.error();

    auto description = description_or_error.release_value();
    m_inode_to_watches.set(inode.identifier(), description.ptr());
    m_wd_to_watches.set(wd, move(description));

    inode.register_watcher({}, *this);
    return wd;
}

KResult InodeWatcher::unregister_by_wd(int wd)
{
    MutexLocker locker(m_lock);

    auto it = m_wd_to_watches.find(wd);
    if (it == m_wd_to_watches.end())
        return ENOENT;

    auto& inode = it->value->inode;
    inode.unregister_watcher({}, *this);

    m_inode_to_watches.remove(inode.identifier());
    m_wd_to_watches.remove(it);

    return KSuccess;
}

void InodeWatcher::unregister_by_inode(Badge<Inode>, InodeIdentifier identifier)
{
    MutexLocker locker(m_lock);

    auto it = m_inode_to_watches.find(identifier);
    if (it == m_inode_to_watches.end())
        return;


    m_inode_to_watches.remove(identifier);
    m_wd_to_watches.remove(it->value->wd);
}

}
