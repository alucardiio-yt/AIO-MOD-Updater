#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "installer_diagnostics.hpp"
#include "nsp_ncm_probe.hpp"

namespace installer {

struct RealInstallResult {
    Diagnostic diagnostic;
    uint64_t titleId = 0;
    uint64_t applicationId = 0;
    uint64_t installedBytes = 0;
    uint32_t installedContents = 0;
    bool ticketImported = false;
    bool applicationRecordPushed = false;
    bool ok() const { return diagnostic.ok(); }
};

using InstallProgressCallback = std::function<void(uint64_t current, uint64_t total,
                                                   const std::string& stage,
                                                   uint32_t contentIndex,
                                                   uint32_t contentCount)>;

// Transactional NSP installer. Supports fresh base applications plus Patch and
// AddOnContent packages. Existing ContentIds are treated as shared/pre-existing
// and are never deleted by rollback. Exact/same-or-newer meta versions are refused.
RealInstallResult installNspToSdTransactional(const std::string& nspPath,
                                              InstallProgressCallback progress = {});

// Direct streaming installer used by the MTP virtual Install storage. The
// callback must provide exactly the requested bytes in package order. The NSP
// is parsed sequentially and NCA payload bytes are written directly to NCM
// placeholders, so no multi-gigabyte staging file is created on FAT32.
using StreamReadExactCallback = std::function<bool(void* out, size_t size)>;
RealInstallResult installNspStreamToSdTransactional(
    const std::string& displayName,
    uint64_t packageSize,
    StreamReadExactCallback readExact,
    InstallProgressCallback progress = {});

// Compatibility alias for callers from Phase 2D.
inline RealInstallResult installFreshNspToSd(const std::string& nspPath,
                                             InstallProgressCallback progress = {})
{
    return installNspToSdTransactional(nspPath, std::move(progress));
}

// Recovers only objects recorded in AIO's own real-install journal.
// It never scans/deletes unrelated ContentStorage entries.
bool recoverStaleRealInstall(std::string* detail = nullptr);

} // namespace installer
