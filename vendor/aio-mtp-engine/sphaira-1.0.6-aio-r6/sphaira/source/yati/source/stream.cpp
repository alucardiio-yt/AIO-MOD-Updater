#include "yati/source/stream.hpp"
#include "defines.hpp"
#include "log.hpp"
#include <algorithm>

namespace sphaira::yati::source {

namespace {
constexpr s64 SKIP_BUFFER_SIZE = 1024 * 1024;
}

Result Stream::Read(void* _buf, s64 off, s64 size, u64* bytes_read_out) {
    // streams don't allow for random access (seeking backwards).
    R_UNLESS(off >= m_offset, Result_StreamBadSeek);

    auto buf = static_cast<u8*>(_buf);
    *bytes_read_out = 0;

    // check if we already have some data in the buffer.
    while (size) {
        // while it is invalid to seek backwards, it is valid to seek forwards.
        // this can be done to skip padding, skip undeeded files etc.
        // to handle this, simply read the data into a buffer and discard it.
        if (off > m_offset) {
            const auto skip_size = std::min<s64>(off - m_offset, SKIP_BUFFER_SIZE);
            std::vector<u8> temp_buf(skip_size);
            u64 bytes_read{};
            R_TRY(ReadChunk(temp_buf.data(), temp_buf.size(), &bytes_read));
            R_UNLESS(bytes_read, Result_TransferCancelled);

            m_offset += bytes_read;
        } else {
            u64 bytes_read{};
            R_TRY(ReadChunk(buf, size, &bytes_read));
            R_UNLESS(bytes_read, Result_TransferCancelled);

            *bytes_read_out += bytes_read;
            buf += bytes_read;
            off += bytes_read;
            m_offset += bytes_read;
            size -= bytes_read;
        }
    }

    R_SUCCEED();
}

} // namespace sphaira::yati::source
