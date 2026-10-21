#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mtp {

// Bounded producer/consumer byte stream used by the MTP USB thread (producer)
// and the NCM installer worker (consumer). No NSP is staged to the FAT32 SD.
class InstallStream {
public:
    explicit InstallStream(size_t capacityBytes = 4 * 1024 * 1024);

    InstallStream(const InstallStream&) = delete;
    InstallStream& operator=(const InstallStream&) = delete;

    // Pushes all bytes, blocking while the ring is full. If the installer has
    // already finished, incoming bytes are deliberately discarded so the MTP
    // host can finish its SendObject transaction cleanly.
    bool push(const void* data, size_t size);

    // Reads exactly size bytes unless producer EOF/cancel occurs first.
    bool readExact(void* out, size_t size);

    void closeProducer();
    void cancel();
    void finishConsumer();

    bool producerClosed() const;
    bool consumerFinished() const;
    bool cancelled() const;
    uint64_t pushedBytes() const;
    uint64_t consumedBytes() const;

private:
    const size_t capacity;
    std::vector<uint8_t> buffer;
    size_t readPos = 0;
    size_t writePos = 0;
    size_t used = 0;

    bool producerDone = false;
    bool consumerDone = false;
    bool cancelledFlag = false;
    uint64_t pushed = 0;
    uint64_t consumed = 0;

    mutable std::mutex mutex;
    std::condition_variable canRead;
    std::condition_variable canWrite;
};

} // namespace mtp
