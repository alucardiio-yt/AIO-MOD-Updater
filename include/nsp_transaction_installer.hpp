#pragma once

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

RealInstallResult installNspToSdTransactional(const std::string& nspPath,
                                              InstallProgressCallback progress = {});

inline RealInstallResult installFreshNspToSd(const std::string& nspPath,
                                             InstallProgressCallback progress = {})
{
    return installNspToSdTransactional(nspPath, std::move(progress));
}

bool recoverStaleRealInstall(std::string* detail = nullptr);

} 
