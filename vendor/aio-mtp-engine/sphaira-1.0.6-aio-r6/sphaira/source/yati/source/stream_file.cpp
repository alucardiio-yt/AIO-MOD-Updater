#include "yati/source/stream_file.hpp"
#include "log.hpp"

namespace sphaira::yati::source {

StreamFile::StreamFile(fs::Fs* fs, const fs::FsPath& path) : m_fs{fs} {
    m_open_result = m_fs->OpenFile(path, FsOpenMode_Read, std::addressof(m_file));
}

Result StreamFile::ReadChunk(void* buf, s64 size, u64* bytes_read) {
    R_TRY(GetOpenResult());
    const auto rc = m_file.Read(m_offset, buf, size, 0, bytes_read);
    m_offset += *bytes_read;
    return rc;
}

} // namespace sphaira::yati::source
