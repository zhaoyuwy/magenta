// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/io_port_dispatcher.h>

#include <assert.h>
#include <err.h>
#include <new.h>

#include <arch/user_copy.h>
#include <kernel/auto_lock.h>
#include <lib/user_copy.h>

#include <magenta/state_tracker.h>
#include <magenta/user_copy.h>

constexpr mx_rights_t kDefaultIOPortRights =
    MX_RIGHT_DUPLICATE | MX_RIGHT_TRANSFER | MX_RIGHT_READ | MX_RIGHT_WRITE;

IOP_Packet* IOP_Packet::Alloc(mx_size_t size) {
    AllocChecker ac;
    auto mem = new (&ac) char [sizeof(IOP_Packet) + size];
    if (!ac.check())
        return nullptr;
    return new (mem) IOP_Packet(size);
}

IOP_Packet* IOP_Packet::Make(const void* data, mx_size_t size) {
    auto pk = Alloc(size);
    if (!pk)
        return nullptr;
    memcpy(reinterpret_cast<char*>(pk) + sizeof(IOP_Packet), data, size);
    return pk;
}

IOP_Packet* IOP_Packet::MakeFromUser(const void* data, mx_size_t size) {
    auto pk = Alloc(size);
    if (!pk)
        return nullptr;

    auto header = reinterpret_cast<mx_packet_header_t*>(
        reinterpret_cast<char*>(pk) + sizeof(IOP_Packet));

    auto status = magenta_copy_from_user(data, header, size);
    header->type = MX_IO_PORT_PKT_TYPE_USER;

    return (status == NO_ERROR) ? pk : nullptr;
}

void IOP_Packet::Delete(IOP_Packet* packet) {
    packet->~IOP_Packet();
    delete [] reinterpret_cast<char*>(packet);
}

bool IOP_Packet::CopyToUser(void* data, mx_size_t* size) {
    if (*size < data_size)
        return ERR_NOT_ENOUGH_BUFFER;
    *size = data_size;
    return copy_to_user(
        data, reinterpret_cast<char*>(this) + sizeof(IOP_Packet), data_size) == NO_ERROR;
}

mx_status_t IOPortDispatcher::Create(uint32_t options,
                                     utils::RefPtr<Dispatcher>* dispatcher,
                                     mx_rights_t* rights) {
    AllocChecker ac;
    auto disp = new (&ac) IOPortDispatcher(options);
    if (!ac.check())
        return ERR_NO_MEMORY;

    *rights = kDefaultIOPortRights;
    *dispatcher = utils::AdoptRef<Dispatcher>(disp);
    return NO_ERROR;
}

IOPortDispatcher::IOPortDispatcher(uint32_t options)
    : options_(options),
      no_clients_(false) {
    mutex_init(&lock_);
    event_init(&event_, false, EVENT_FLAG_AUTOUNSIGNAL);
}

IOPortDispatcher::~IOPortDispatcher() {
    // The observers hold a ref to the dispatcher so if the dispatcher
    // is being destroyed there should be no registered observers.
    FreePackets_NoLock();

    DEBUG_ASSERT(observers_.is_empty());
    DEBUG_ASSERT(packets_.is_empty());

    event_destroy(&event_);
    mutex_destroy(&lock_);
}

void IOPortDispatcher::FreePackets_NoLock() {
    while (!packets_.is_empty()) {
        IOP_Packet::Delete(packets_.pop_front());
    }
}

void IOPortDispatcher::on_zero_handles() {
    AutoLock al(&lock_);
    no_clients_ = true;
    FreePackets_NoLock();
}

mx_status_t IOPortDispatcher::Queue(IOP_Packet* packet) {
    int wake_count = 0;
    mx_status_t status = NO_ERROR;
    {
        AutoLock al(&lock_);
        if (no_clients_) {
            status = ERR_NOT_AVAILABLE;
        } else {
            packets_.push_back(packet);
            wake_count = event_signal_etc(&event_, false, status);
        }
    }

    if (status != NO_ERROR) {
        IOP_Packet::Delete(packet);
        return status;
    }

    if (wake_count)
        thread_yield();

    return NO_ERROR;
}

mx_status_t IOPortDispatcher::Wait(IOP_Packet** packet) {
    while (true) {
        {
            AutoLock al(&lock_);
            if (!packets_.is_empty()) {
                *packet = packets_.pop_front();
                return NO_ERROR;
            }
        }
        status_t st = event_wait_timeout(&event_, INFINITE_TIME, true);
        if (st != NO_ERROR)
            return st;
    }
}

mx_status_t IOPortDispatcher::Bind(Handle* handle, mx_signals_t signals, uint64_t key) {
    // This method is called under the handle table lock.
    auto state_tracker = handle->dispatcher()->get_state_tracker();
    if (!state_tracker || !state_tracker->is_waitable())
        return ERR_NOT_SUPPORTED;

    AllocChecker ac;
    auto observer  =
        new (&ac) IOPortObserver(utils::RefPtr<IOPortDispatcher>(this), handle, signals, key);
    if (!ac.check())
        return ERR_NO_MEMORY;

    {
        // TODO(cpu) : Currently we allow duplicated handle / key. This is bug MG-227.
        AutoLock al(&lock_);
        observers_.push_front(observer);
    }

    mx_status_t result = state_tracker->AddObserver(observer);
    if (result != NO_ERROR) {
        CancelObserver(observer);
        return result;
    }

    return NO_ERROR;
}

mx_status_t IOPortDispatcher::Unbind(Handle* handle, uint64_t key) {
    // This method is called under the handle table lock.
    IOPortObserver* observer = nullptr;
    {
        AutoLock al(&lock_);

        observer = observers_.find_if([handle, key](const IOPortObserver& ob) {
            return ((handle == ob.get_handle()) && (key == ob.get_key()));
        });

        if (!observer)
            return ERR_BAD_HANDLE;

        // This code could 'race' with IOPortObserver::OnCancel() so the atomic
        // SetState() ensures that either the rest of this function executes
        // or the OnDidCancel() + CancelObserver() executes.
        if (observer->SetState(IOPortObserver::UNBOUND) != IOPortObserver::NEW)
            return NO_ERROR;

        observers_.erase(*observer);
    }

    auto dispatcher = handle->dispatcher();
    dispatcher->get_state_tracker()->RemoveObserver(observer);
    delete observer;
    return NO_ERROR;
}

void IOPortDispatcher::CancelObserver(IOPortObserver* observer) {
    {
        AutoLock al(&lock_);
        observers_.erase(*observer);
    }

    delete observer;
}
