#pragma once

#include <vector>
#include <string>
#include <memory>
#include <switch.h>

namespace sphaira::usb {

struct Base {
    Base(u64 transfer_timeout);
    virtual ~Base();

    // sets up usb.
    virtual Result Init() = 0;

    // returns 0 if usb is connected to a device.
    virtual Result IsUsbConnected(u64 timeout) = 0;

    // transfers a chunk of data, check out_size_transferred for how much was transferred.
    Result TransferPacketImpl(bool read, void *page, u32 remaining, u32 size, u32 *out_size_transferred, u64 timeout);
    Result TransferPacketImpl(bool read, void *page, u32 remaining, u32 size, u32 *out_size_transferred) {
        return TransferPacketImpl(read, page, remaining, size, out_size_transferred, m_transfer_timeout);
    }

    // transfers all data.
    Result TransferAll(bool read, void *data, u32 size, u64 timeout);
    Result TransferAll(bool read, void *data, u32 size) {
        return TransferAll(read, data, size, m_transfer_timeout);
    }

    // returns the cancel event.
    auto GetCancelEvent() {
        return &m_uevent;
    }

    // cancels an in progress transfer.
    void Cancel() {
        ueventSignal(GetCancelEvent());
    }

    auto GetTransferTimeout() const {
        return m_transfer_timeout;
    }

protected:
    enum UsbSessionEndpoint {
        UsbSessionEndpoint_In = 0,
        UsbSessionEndpoint_Out = 1,
    };

    virtual Event *GetCompletionEvent(UsbSessionEndpoint ep) = 0;
    virtual Result WaitTransferCompletion(UsbSessionEndpoint ep, u64 timeout) = 0;
    virtual Result TransferAsync(UsbSessionEndpoint ep, void *buffer, u32 remaining, u32 size, u32 *out_xfer_id) = 0;
    virtual Result GetTransferResult(UsbSessionEndpoint ep, u32 xfer_id, u32 *out_requested_size, u32 *out_transferred_size) = 0;

private:
    u64 m_transfer_timeout{};
    UEvent m_uevent{};
    std::unique_ptr<u8*> m_aligned{};
};

} // namespace sphaira::usb
