// this is used for testing that streams work, this code isn't used in normal
// release builds as it is slower / less feature complete than normal.
#pragma once

#include "stream.hpp"
#include "fs.hpp"
#include <switch.h>
#include <memory>

namespace sphaira::yati::source {

struct StreamFile final : Stream {
    StreamFile(fs::Fs* fs, const fs::FsPath& path);
    Result ReadChunk(void* buf, s64 size, u64* bytes_read) override;

private:
    fs::Fs* m_fs{};
    fs::File m_file{};
    s64 m_offset{};
};

} // namespace sphaira::yati::source
