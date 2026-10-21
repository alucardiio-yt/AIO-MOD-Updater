#include "mtp_install_stream.hpp"

#include <algorithm>
#include <cstring>

namespace mtp {

InstallStream::InstallStream(size_t capacityBytes)
    : capacity(std::max<size_t>(capacityBytes, 64 * 1024)), buffer(capacity)
{
}

bool InstallStream::push(const void* data, size_t size)
{
    const auto* src = static_cast<const uint8_t*>(data);
    size_t done = 0;

    while (done < size) {
        std::unique_lock lock(mutex);
        canWrite.wait(lock, [&] {
            return cancelledFlag || consumerDone || used < capacity;
        });

        if (cancelledFlag)
            return false;

        // Installer can reject/finish before Windows has sent the entire object.
        // Drain the host transaction by accepting the remaining bytes without
        // retaining them in RAM.
        if (consumerDone) {
            pushed += static_cast<uint64_t>(size - done);
            return true;
        }

        const size_t room = capacity - used;
        const size_t contiguous = std::min(capacity - writePos, room);
        const size_t chunk = std::min(size - done, contiguous);
        std::memcpy(buffer.data() + writePos, src + done, chunk);
        writePos = (writePos + chunk) % capacity;
        used += chunk;
        pushed += chunk;
        done += chunk;
        lock.unlock();
        canRead.notify_one();
    }
    return true;
}

bool InstallStream::readExact(void* out, size_t size)
{
    auto* dst = static_cast<uint8_t*>(out);
    size_t done = 0;

    while (done < size) {
        std::unique_lock lock(mutex);
        canRead.wait(lock, [&] {
            return cancelledFlag || used > 0 || producerDone;
        });

        if (cancelledFlag)
            return false;
        if (used == 0 && producerDone)
            return false;

        const size_t contiguous = std::min(capacity - readPos, used);
        const size_t chunk = std::min(size - done, contiguous);
        std::memcpy(dst + done, buffer.data() + readPos, chunk);
        readPos = (readPos + chunk) % capacity;
        used -= chunk;
        consumed += chunk;
        done += chunk;
        lock.unlock();
        canWrite.notify_one();
    }
    return true;
}

void InstallStream::closeProducer()
{
    {
        std::scoped_lock lock(mutex);
        producerDone = true;
    }
    canRead.notify_all();
    canWrite.notify_all();
}

void InstallStream::cancel()
{
    {
        std::scoped_lock lock(mutex);
        cancelledFlag = true;
    }
    canRead.notify_all();
    canWrite.notify_all();
}

void InstallStream::finishConsumer()
{
    {
        std::scoped_lock lock(mutex);
        consumerDone = true;
        // Bytes no longer needed by an installer that has already returned.
        used = 0;
        readPos = writePos;
    }
    canRead.notify_all();
    canWrite.notify_all();
}

bool InstallStream::producerClosed() const { std::scoped_lock lock(mutex); return producerDone; }
bool InstallStream::consumerFinished() const { std::scoped_lock lock(mutex); return consumerDone; }
bool InstallStream::cancelled() const { std::scoped_lock lock(mutex); return cancelledFlag; }
uint64_t InstallStream::pushedBytes() const { std::scoped_lock lock(mutex); return pushed; }
uint64_t InstallStream::consumedBytes() const { std::scoped_lock lock(mutex); return consumed; }

} // namespace mtp
