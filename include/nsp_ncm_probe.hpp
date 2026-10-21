#pragma once

#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

#include "installer_diagnostics.hpp"

namespace installer {

struct NspEntryInfo {
    std::string name;
    uint64_t offset = 0;      // absolute offset inside NSP
    uint64_t size = 0;
    bool isNca = false;
    bool isCnmtNca = false;
    bool isTicket = false;
    bool isCert = false;
    bool hasContentId = false;
    NcmContentId contentId{};
};

struct NspManifest {
    Diagnostic diagnostic;
    uint64_t packageSize = 0;
    uint64_t totalNcaBytes = 0;
    uint32_t ncaCount = 0;
    uint32_t cnmtNcaCount = 0;
    uint32_t ticketCount = 0;
    uint32_t certCount = 0;
    std::vector<NspEntryInfo> entries;

    bool ok() const { return diagnostic.ok(); }
};

struct NcmProbeResult {
    Diagnostic diagnostic;
    std::string testedContentName;
    uint64_t testedBytes = 0;
    bool staleTransactionRecovered = false;

    bool ok() const { return diagnostic.ok(); }
};

// Parses the full PFS0 manifest and classifies NCA/CNMT/ticket/cert entries.
// This does not decrypt or bypass any content validation.
NspManifest inspectNspManifest(const std::string& path);

// Safe ContentStorage smoke test. It selects the smallest NCA in the package,
// creates a real NCM placeholder on the SD ContentStorage, copies the complete
// NCA into that placeholder, then ALWAYS deletes the placeholder instead of
// registering it. This proves the low-level NCM write path without installing
// or registering a title.
NcmProbeResult runSafeNcmProbe(const std::string& nspPath);

// Removes only a stale placeholder previously created by AIO's own NCM probe.
// Never scans/deletes unrelated ContentStorage entries.
bool recoverStaleNcmProbe(std::string* detail = nullptr);

std::string getInstallWorkDirectory();
std::string getInstallJournalPath();

struct CnmtAnalysisResult {
    Diagnostic diagnostic;
    uint64_t titleId = 0;
    uint64_t applicationId = 0;
    uint32_t titleVersion = 0;
    uint32_t requiredSystemVersion = 0;
    uint8_t metaType = 0;
    uint32_t expectedContentCount = 0;
    uint32_t matchedContentCount = 0;
    uint64_t expectedInstallBytes = 0;
    std::string cnmtEntryName;
    std::vector<std::string> missingContentIds;
    std::vector<std::string> sizeMismatchContentIds;

    bool ok() const { return diagnostic.ok(); }
};

// Temporarily registers only the CNMT NCA, mounts its ContentMeta filesystem,
// parses PackagedContentMeta, validates every referenced content against the
// staged NSP, checks the RequiredSystemVersion, and then removes the temporary
// CNMT content. No title/database/application record is installed by this call.
CnmtAnalysisResult analyzeCnmtRequirements(const std::string& nspPath);

std::string formatTitleId(uint64_t titleId);
std::string formatPackedSystemVersion(uint32_t version);

} // namespace installer
