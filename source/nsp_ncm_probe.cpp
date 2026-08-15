#include "nsp_ncm_probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace installer {
namespace {

constexpr const char INSTALL_WORK_DIR[] = "/switch/aio-switch-updater-mod/config/install_work";
constexpr const char JOURNAL_PATH[] = "/switch/aio-switch-updater-mod/config/install_work/current_install.json";
constexpr size_t COPY_BUFFER_SIZE = 1024 * 1024;
constexpr uint32_t MAX_PFS0_FILES = 0x10000;
constexpr uint32_t MAX_STRING_TABLE = 32 * 1024 * 1024;

#pragma pack(push, 1)
struct Pfs0Header {
    char magic[4];
    uint32_t fileCount;
    uint32_t stringTableSize;
    uint32_t reserved;
};
struct Pfs0Entry {
    uint64_t offset;
    uint64_t size;
    uint32_t stringOffset;
    uint32_t reserved;
};
#pragma pack(pop)

bool checkedAdd(uint64_t a, uint64_t b, uint64_t& out)
{
    if (a > std::numeric_limits<uint64_t>::max() - b)
        return false;
    out = a + b;
    return true;
}

bool endsWithCaseInsensitive(const std::string& value, const std::string& suffix)
{
    if (value.size() < suffix.size())
        return false;
    const size_t start = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[start + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseContentIdFromName(const std::string& name, NcmContentId& out)
{

    const size_t dot = name.find('.');
    if (dot != 32)
        return false;
    std::array<uint8_t, 16> bytes{};
    for (size_t i = 0; i < 16; ++i) {
        const int hi = hexNibble(name[i * 2]);
        const int lo = hexNibble(name[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    static_assert(sizeof(NcmContentId) == 16, "Unexpected NcmContentId size");
    std::memcpy(&out, bytes.data(), bytes.size());
    return true;
}

std::string bytesToHex(const void* ptr, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(ptr);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i)
        ss << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return ss.str();
}

bool hexToBytes(const std::string& text, void* ptr, size_t size)
{
    if (text.size() != size * 2)
        return false;
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        const int hi = hexNibble(text[i * 2]);
        const int lo = hexNibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void writeJournal(const std::string& package,
                  const NspEntryInfo& entry,
                  const NcmPlaceHolderId& placeholder,
                  const std::string& stage)
{
    std::error_code ec;
    std::filesystem::create_directories(INSTALL_WORK_DIR, ec);
    nlohmann::json j;
    j["owner"] = "aio-switch-updater-mod";
    j["kind"] = "ncm-probe";
    j["stage"] = stage;
    j["package"] = package;
    j["content_name"] = entry.name;
    j["content_size"] = entry.size;
    j["content_id"] = bytesToHex(&entry.contentId, sizeof(entry.contentId));
    j["placeholder_id"] = bytesToHex(&placeholder, sizeof(placeholder));
    std::ofstream out(JOURNAL_PATH, std::ios::binary | std::ios::trunc);
    if (out)
        out << j.dump(2);
}

void removeJournal()
{
    std::error_code ec;
    std::filesystem::remove(JOURNAL_PATH, ec);
}

Diagnostic resultToDiagnostic(Result rc, ErrorCode fallback, const std::string& where)
{
    if (R_SUCCEEDED(rc))
        return makeDiagnostic(ErrorCode::None);
    std::ostringstream tech;
    tech << where << " rc=0x" << std::uppercase << std::hex << static_cast<uint32_t>(rc);
    return makeDiagnostic(fallback, tech.str(), rc);
}

struct NcmSession {
    bool initialized = false;
    NcmContentStorage storage{};
    bool storageOpen = false;

    Diagnostic open()
    {
        Result rc = ncmInitialize();
        if (R_FAILED(rc))
            return resultToDiagnostic(rc, ErrorCode::InternalError, "ncmInitialize");
        initialized = true;
        rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
        if (R_FAILED(rc))
            return resultToDiagnostic(rc, ErrorCode::PlaceholderError, "ncmOpenContentStorage(SD)");
        storageOpen = true;
        return makeDiagnostic(ErrorCode::None);
    }

    ~NcmSession()
    {
        if (storageOpen)
            ncmContentStorageClose(&storage);
        if (initialized)
            ncmExit();
    }
};

} 

std::string getInstallWorkDirectory() { return INSTALL_WORK_DIR; }
std::string getInstallJournalPath() { return JOURNAL_PATH; }

NspManifest inspectNspManifest(const std::string& path)
{
    NspManifest out;
    std::error_code ec;
    out.packageSize = std::filesystem::file_size(path, ec);
    if (ec || out.packageSize < sizeof(Pfs0Header)) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "No se pudo leer el NSP");
        return out;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out.diagnostic = makeDiagnostic(ErrorCode::InternalError, "No se pudo abrir: " + path);
        return out;
    }

    Pfs0Header header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.magic, "PFS0", 4) != 0) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Magic PFS0 ausente");
        return out;
    }
    if (!header.fileCount || header.fileCount > MAX_PFS0_FILES || header.stringTableSize > MAX_STRING_TABLE) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Cabecera PFS0 fuera de limites");
        return out;
    }

    const uint64_t entriesBytes = static_cast<uint64_t>(header.fileCount) * sizeof(Pfs0Entry);
    uint64_t stringStart = 0, dataStart = 0;
    if (!checkedAdd(sizeof(Pfs0Header), entriesBytes, stringStart) ||
        !checkedAdd(stringStart, header.stringTableSize, dataStart) || dataStart > out.packageSize) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "Tablas PFS0 fuera del archivo");
        return out;
    }

    std::vector<Pfs0Entry> entries(header.fileCount);
    file.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entriesBytes));
    std::vector<char> strings(header.stringTableSize + 1, '\0');
    if (header.stringTableSize)
        file.read(strings.data(), static_cast<std::streamsize>(header.stringTableSize));
    if (!file) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "Tablas PFS0 incompletas");
        return out;
    }

    out.entries.reserve(header.fileCount);
    for (uint32_t i = 0; i < header.fileCount; ++i) {
        const auto& e = entries[i];
        if (e.stringOffset >= header.stringTableSize) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Nombre fuera de rango");
            return out;
        }
        const char* cName = strings.data() + e.stringOffset;
        const size_t remaining = header.stringTableSize - e.stringOffset;
        if (!std::memchr(cName, '\0', remaining)) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Nombre sin terminador");
            return out;
        }

        uint64_t entryEnd = 0;
        if (!checkedAdd(e.offset, e.size, entryEnd) || entryEnd > out.packageSize - dataStart) {
            out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, std::string("Entrada fuera del NSP: ") + cName);
            return out;
        }

        NspEntryInfo info;
        info.name = cName;
        info.offset = dataStart + e.offset;
        info.size = e.size;
        info.isCnmtNca = endsWithCaseInsensitive(info.name, ".cnmt.nca");
        info.isNca = info.isCnmtNca || endsWithCaseInsensitive(info.name, ".nca");
        info.isTicket = endsWithCaseInsensitive(info.name, ".tik");
        info.isCert = endsWithCaseInsensitive(info.name, ".cert");
        if (info.isNca)
            info.hasContentId = parseContentIdFromName(info.name, info.contentId);

        if (info.isNca) {
            ++out.ncaCount;
            out.totalNcaBytes += info.size;
        }
        if (info.isCnmtNca) ++out.cnmtNcaCount;
        if (info.isTicket) ++out.ticketCount;
        if (info.isCert) ++out.certCount;
        out.entries.push_back(info);
    }

    if (out.ncaCount == 0) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, "El NSP no contiene entradas NCA");
        return out;
    }
    if (out.cnmtNcaCount == 0) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, "El NSP no contiene .cnmt.nca");
        return out;
    }
    if (out.ticketCount != out.certCount) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent,
            "Ticket/cert desparejados: tik=" + std::to_string(out.ticketCount) +
            " cert=" + std::to_string(out.certCount));
        return out;
    }
    for (const auto& e : out.entries) {
        if (e.isNca && !e.hasContentId) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "Nombre NCA sin Content ID valido: " + e.name);
            return out;
        }
    }

    out.diagnostic = makeDiagnostic(ErrorCode::None,
        "manifest OK; nca=" + std::to_string(out.ncaCount) +
        ", cnmt=" + std::to_string(out.cnmtNcaCount) +
        ", tik=" + std::to_string(out.ticketCount));
    return out;
}

bool recoverStaleNcmProbe(std::string* detail)
{
    if (!std::filesystem::exists(JOURNAL_PATH))
        return true;

    try {
        std::ifstream in(JOURNAL_PATH, std::ios::binary);
        nlohmann::json j;
        in >> j;
        if (j.value("owner", "") != "aio-switch-updater-mod" || j.value("kind", "") != "ncm-probe") {
            if (detail) *detail = "Journal no pertenece al NCM probe de AIO; no se modifico.";
            return false;
        }

        NcmPlaceHolderId placeholder{};
        if (!hexToBytes(j.value("placeholder_id", ""), &placeholder, sizeof(placeholder))) {
            if (detail) *detail = "Placeholder ID invalido en journal.";
            return false;
        }

        NcmSession session;
        Diagnostic d = session.open();
        if (!d.ok()) {
            if (detail) *detail = d.technical;
            return false;
        }

        bool exists = false;
        Result rc = ncmContentStorageHasPlaceHolder(&session.storage, &exists, &placeholder);
        if (R_FAILED(rc)) {
            if (detail) *detail = "No se pudo consultar placeholder antiguo.";
            return false;
        }
        if (exists) {
            rc = ncmContentStorageDeletePlaceHolder(&session.storage, &placeholder);
            if (R_FAILED(rc)) {
                if (detail) *detail = "No se pudo eliminar placeholder antiguo.";
                return false;
            }
        }
        removeJournal();
        if (detail) *detail = exists ? "Placeholder incompleto anterior eliminado." : "Journal antiguo limpiado; placeholder ya no existia.";
        return true;
    } catch (const std::exception& e) {
        if (detail) *detail = e.what();
        return false;
    }
}

NcmProbeResult runSafeNcmProbe(const std::string& nspPath)
{
    NcmProbeResult out;
    std::string recovery;
    if (std::filesystem::exists(JOURNAL_PATH)) {
        if (!recoverStaleNcmProbe(&recovery)) {
            out.diagnostic = makeDiagnostic(ErrorCode::PlaceholderError, "No se pudo recuperar transaccion anterior: " + recovery);
            return out;
        }
        out.staleTransactionRecovered = true;
    }

    const auto manifest = inspectNspManifest(nspPath);
    if (!manifest.ok()) {
        out.diagnostic = manifest.diagnostic;
        return out;
    }

    const NspEntryInfo* candidate = nullptr;
    for (const auto& e : manifest.entries) {
        if (!e.isNca || !e.hasContentId || e.size == 0)
            continue;
        if (!candidate || e.size < candidate->size)
            candidate = &e;
    }
    if (!candidate) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, "No hay NCA valida para probar ContentStorage");
        return out;
    }

    out.testedContentName = candidate->name;
    out.testedBytes = candidate->size;

    const auto system = getSystemSnapshot();
    if (system.sdFreeBytes >= 0 && candidate->size > static_cast<uint64_t>(system.sdFreeBytes)) {
        out.diagnostic = makeDiagnostic(ErrorCode::NotEnoughSpace,
            "probe_required=" + std::to_string(candidate->size) + ", free=" + std::to_string(system.sdFreeBytes));
        return out;
    }

    std::ifstream nsp(nspPath, std::ios::binary);
    if (!nsp) {
        out.diagnostic = makeDiagnostic(ErrorCode::InternalError, "No se pudo reabrir NSP para NCM probe");
        return out;
    }

    NcmSession session;
    out.diagnostic = session.open();
    if (!out.diagnostic.ok())
        return out;

    NcmPlaceHolderId placeholder{};
    Result rc = ncmContentStorageGeneratePlaceHolderId(&session.storage, &placeholder);
    if (R_FAILED(rc)) {
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::PlaceholderError, "GeneratePlaceHolderId");
        return out;
    }

    bool exists = false;
    rc = ncmContentStorageHasPlaceHolder(&session.storage, &exists, &placeholder);
    if (R_FAILED(rc)) {
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::PlaceholderError, "HasPlaceHolder");
        return out;
    }
    if (exists)
        ncmContentStorageDeletePlaceHolder(&session.storage, &placeholder);

    rc = ncmContentStorageCreatePlaceHolder(&session.storage, &candidate->contentId, &placeholder, candidate->size);
    if (R_FAILED(rc)) {
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::PlaceholderError, "CreatePlaceHolder");
        return out;
    }

    writeJournal(nspPath, *candidate, placeholder, "placeholder-created");

    bool placeholderAlive = true;
    auto cleanup = [&]() {
        if (placeholderAlive) {
            ncmContentStorageDeletePlaceHolder(&session.storage, &placeholder);
            placeholderAlive = false;
        }
        removeJournal();
    };

    nsp.seekg(static_cast<std::streamoff>(candidate->offset), std::ios::beg);
    if (!nsp) {
        cleanup();
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "No se pudo posicionar en NCA seleccionada");
        return out;
    }

    std::vector<uint8_t> buffer(COPY_BUFFER_SIZE);
    uint64_t copied = 0;
    writeJournal(nspPath, *candidate, placeholder, "writing-placeholder");
    while (copied < candidate->size) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), candidate->size - copied));
        nsp.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
        if (static_cast<size_t>(nsp.gcount()) != want) {
            cleanup();
            out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated,
                "NSP termino durante copia NCM en offset=" + std::to_string(copied));
            return out;
        }
        rc = ncmContentStorageWritePlaceHolder(&session.storage, &placeholder, copied, buffer.data(), want);
        if (R_FAILED(rc)) {
            cleanup();
            out.diagnostic = resultToDiagnostic(rc, ErrorCode::WriteError, "WritePlaceHolder");
            return out;
        }
        copied += want;
    }

    writeJournal(nspPath, *candidate, placeholder, "write-complete-rolling-back");
    rc = ncmContentStorageDeletePlaceHolder(&session.storage, &placeholder);
    placeholderAlive = false;
    if (R_FAILED(rc)) {
        
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::PlaceholderError, "DeletePlaceHolder after probe");
        return out;
    }
    removeJournal();

    out.diagnostic = makeDiagnostic(ErrorCode::None,
        "NCM ContentStorage probe OK; copied_and_rolled_back=" + std::to_string(copied) +
        "; content=" + candidate->name);
    return out;
}

namespace {

#pragma pack(push, 1)
struct PackagedCnmtHeader {
    uint64_t titleId;
    uint32_t titleVersion;
    uint8_t metaType;
    uint8_t platformOrReserved;
    NcmContentMetaHeader metaHeader;
    uint8_t installType;
    uint8_t reserved17;
    uint32_t requiredDownloadSystemVersion;
    uint32_t reserved1C;
};
#pragma pack(pop)
static_assert(sizeof(PackagedCnmtHeader) == 0x20, "Unexpected packaged CNMT header size");

uint32_t parseCurrentFirmwarePacked()
{
    SetSysFirmwareVersion fw{};
    if (R_FAILED(setsysGetFirmwareVersion(&fw)))
        return 0;
    unsigned major = 0, minor = 0, micro = 0;
    if (std::sscanf(fw.display_version, "%u.%u.%u", &major, &minor, &micro) < 2)
        return 0;
    major = std::min(major, 0x3Fu);
    minor = std::min(minor, 0x3Fu);
    micro = std::min(micro, 0x0Fu);
    return (major << 26) | (minor << 20) | (micro << 16);
}

uint64_t getApplicationIdForMeta(uint64_t titleId, uint8_t metaType, const std::vector<uint8_t>& ext)
{
    switch (metaType) {
        case NcmContentMetaType_Application:
            return titleId;
        case NcmContentMetaType_Patch:
            if (ext.size() >= sizeof(NcmPatchMetaExtendedHeader)) {
                NcmPatchMetaExtendedHeader h{};
                std::memcpy(&h, ext.data(), sizeof(h));
                return h.application_id;
            }
            return titleId ^ 0x800ULL;
        case NcmContentMetaType_AddOnContent:
            if (ext.size() >= sizeof(NcmLegacyAddOnContentMetaExtendedHeader)) {
                uint64_t appId = 0;
                std::memcpy(&appId, ext.data(), sizeof(appId));
                return appId;
            }
            return (titleId ^ 0x1000ULL) & ~0xFFFULL;
        case NcmContentMetaType_DataPatch:
            if (ext.size() >= sizeof(NcmDataPatchMetaExtendedHeader)) {
                NcmDataPatchMetaExtendedHeader h{};
                std::memcpy(&h, ext.data(), sizeof(h));
                return h.application_id;
            }
            return (titleId ^ 0x1000ULL) & ~0xFFFULL;
        default:
            return titleId;
    }
}

uint32_t getRequiredSystemVersion(uint8_t metaType, const std::vector<uint8_t>& ext)
{
    switch (metaType) {
        case NcmContentMetaType_Application:
            if (ext.size() >= sizeof(NcmApplicationMetaExtendedHeader)) {
                NcmApplicationMetaExtendedHeader h{};
                std::memcpy(&h, ext.data(), sizeof(h));
                return h.required_system_version;
            }
            break;
        case NcmContentMetaType_Patch:
            if (ext.size() >= sizeof(NcmPatchMetaExtendedHeader)) {
                NcmPatchMetaExtendedHeader h{};
                std::memcpy(&h, ext.data(), sizeof(h));
                return h.required_system_version;
            }
            break;
        default:
            break;
    }
    return 0;
}

const NspEntryInfo* findManifestContent(const NspManifest& manifest, const NcmContentId& id)
{
    for (const auto& entry : manifest.entries) {
        if (entry.isNca && entry.hasContentId && std::memcmp(&entry.contentId, &id, sizeof(id)) == 0)
            return &entry;
    }
    return nullptr;
}

void writeCnmtAnalysisJournal(const NspEntryInfo& entry, bool contentCreated)
{
    std::error_code ec;
    std::filesystem::create_directories(INSTALL_WORK_DIR, ec);
    nlohmann::json j;
    j["owner"] = "aio-switch-updater-mod";
    j["kind"] = "cnmt-analysis";
    j["content_name"] = entry.name;
    j["content_id"] = bytesToHex(&entry.contentId, sizeof(entry.contentId));
    j["content_created"] = contentCreated;
    std::ofstream out(JOURNAL_PATH, std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

bool recoverStaleCnmtAnalysis(std::string* detail)
{
    if (!std::filesystem::exists(JOURNAL_PATH))
        return true;
    try {
        std::ifstream in(JOURNAL_PATH, std::ios::binary);
        nlohmann::json j;
        in >> j;
        if (j.value("owner", "") != "aio-switch-updater-mod" || j.value("kind", "") != "cnmt-analysis")
            return true; 
        NcmContentId content{};
        if (!hexToBytes(j.value("content_id", ""), &content, sizeof(content))) {
            if (detail) *detail = "Content ID invalido en journal CNMT.";
            return false;
        }
        if (j.value("content_created", false)) {
            NcmSession session;
            Diagnostic d = session.open();
            if (!d.ok()) {
                if (detail) *detail = d.technical;
                return false;
            }
            bool exists = false;
            Result rc = ncmContentStorageHas(&session.storage, &exists, &content);
            if (R_FAILED(rc)) {
                if (detail) *detail = "No se pudo consultar contenido CNMT temporal.";
                return false;
            }
            if (exists) {
                rc = ncmContentStorageDelete(&session.storage, &content);
                if (R_FAILED(rc)) {
                    if (detail) *detail = "No se pudo eliminar contenido CNMT temporal.";
                    return false;
                }
            }
        }
        removeJournal();
        if (detail) *detail = "Transaccion CNMT temporal recuperada.";
        return true;
    } catch (const std::exception& e) {
        if (detail) *detail = e.what();
        return false;
    }
}

} 

std::string formatTitleId(uint64_t titleId)
{
    std::ostringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << titleId;
    return ss.str();
}

std::string formatPackedSystemVersion(uint32_t version)
{
    if (!version) return "0.0.0";
    const uint32_t major = (version >> 26) & 0x3F;
    const uint32_t minor = (version >> 20) & 0x3F;
    const uint32_t micro = (version >> 16) & 0x0F;
    std::ostringstream ss;
    ss << major << '.' << minor << '.' << micro;
    return ss.str();
}

CnmtAnalysisResult analyzeCnmtRequirements(const std::string& nspPath)
{
    CnmtAnalysisResult out;

    std::string recovery;
    if (!recoverStaleCnmtAnalysis(&recovery)) {
        out.diagnostic = makeDiagnostic(ErrorCode::RegistrationError,
            "No se pudo recuperar analisis CNMT anterior: " + recovery);
        return out;
    }
    if (std::filesystem::exists(JOURNAL_PATH)) {
        out.diagnostic = makeDiagnostic(ErrorCode::InternalError,
            "Existe otra transaccion AIO activa: " + std::string(JOURNAL_PATH));
        return out;
    }

    const auto manifest = inspectNspManifest(nspPath);
    if (!manifest.ok()) {
        out.diagnostic = manifest.diagnostic;
        return out;
    }

    const NspEntryInfo* cnmtEntry = nullptr;
    for (const auto& entry : manifest.entries) {
        if (entry.isCnmtNca && entry.hasContentId) {
            cnmtEntry = &entry;
            break;
        }
    }
    if (!cnmtEntry) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, "No se encontro CNMT NCA utilizable");
        return out;
    }
    out.cnmtEntryName = cnmtEntry->name;

    NcmSession session;
    out.diagnostic = session.open();
    if (!out.diagnostic.ok())
        return out;

    bool contentAlreadyExisted = false;
    Result rc = ncmContentStorageHas(&session.storage, &contentAlreadyExisted, &cnmtEntry->contentId);
    if (R_FAILED(rc)) {
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::RegistrationError, "ContentStorageHas(CNMT)");
        return out;
    }

    bool createdContent = false;
    NcmPlaceHolderId placeholder{};
    if (!contentAlreadyExisted) {
        rc = ncmContentStorageGeneratePlaceHolderId(&session.storage, &placeholder);
        if (R_FAILED(rc)) {
            out.diagnostic = resultToDiagnostic(rc, ErrorCode::PlaceholderError, "GeneratePlaceHolderId(CNMT)");
            return out;
        }
        rc = ncmContentStorageCreatePlaceHolder(&session.storage, &cnmtEntry->contentId, &placeholder, cnmtEntry->size);
        if (R_FAILED(rc)) {
            out.diagnostic = resultToDiagnostic(rc, ErrorCode::PlaceholderError, "CreatePlaceHolder(CNMT)");
            return out;
        }

        bool placeholderAlive = true;
        auto deletePlaceholder = [&]() {
            if (placeholderAlive) {
                ncmContentStorageDeletePlaceHolder(&session.storage, &placeholder);
                placeholderAlive = false;
            }
        };

        std::ifstream nsp(nspPath, std::ios::binary);
        if (!nsp) {
            deletePlaceholder();
            out.diagnostic = makeDiagnostic(ErrorCode::InternalError, "No se pudo abrir NSP para copiar CNMT");
            return out;
        }
        nsp.seekg(static_cast<std::streamoff>(cnmtEntry->offset), std::ios::beg);
        if (!nsp) {
            deletePlaceholder();
            out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "No se pudo posicionar en CNMT NCA");
            return out;
        }

        std::vector<uint8_t> buffer(COPY_BUFFER_SIZE);
        uint64_t copied = 0;
        while (copied < cnmtEntry->size) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), cnmtEntry->size - copied));
            nsp.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
            if (static_cast<size_t>(nsp.gcount()) != want) {
                deletePlaceholder();
                out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "NSP termino durante copia CNMT");
                return out;
            }
            rc = ncmContentStorageWritePlaceHolder(&session.storage, &placeholder, copied, buffer.data(), want);
            if (R_FAILED(rc)) {
                deletePlaceholder();
                out.diagnostic = resultToDiagnostic(rc, ErrorCode::WriteError, "WritePlaceHolder(CNMT)");
                return out;
            }
            copied += want;
        }

        rc = ncmContentStorageRegister(&session.storage, &cnmtEntry->contentId, &placeholder);
        if (R_FAILED(rc)) {
            deletePlaceholder();
            out.diagnostic = resultToDiagnostic(rc, ErrorCode::RegistrationError, "Register(CNMT)");
            return out;
        }
        placeholderAlive = false; 
        createdContent = true;
        writeCnmtAnalysisJournal(*cnmtEntry, true);
    } else {
        writeCnmtAnalysisJournal(*cnmtEntry, false);
    }

    auto cleanup = [&]() {
        if (createdContent) {
            ncmContentStorageDelete(&session.storage, &cnmtEntry->contentId);
            createdContent = false;
        }
        removeJournal();
    };

    char contentPath[FS_MAX_PATH]{};
    rc = ncmContentStorageGetPath(&session.storage, contentPath, sizeof(contentPath), &cnmtEntry->contentId);
    if (R_FAILED(rc)) {
        cleanup();
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::RegistrationError, "GetPath(CNMT)");
        return out;
    }

    FsFileSystem cnmtFs{};
    rc = fsOpenFileSystem(&cnmtFs, FsFileSystemType_ContentMeta, contentPath);
    if (R_FAILED(rc)) {
        cleanup();
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::AuthorizationRejected, "fsOpenFileSystem(ContentMeta)");
        return out;
    }

    FsDir dir{};
    rc = fsFsOpenDirectory(&cnmtFs, "/", FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &dir);
    if (R_FAILED(rc)) {
        fsFsClose(&cnmtFs);
        cleanup();
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::InvalidNca, "Open CNMT directory");
        return out;
    }
    s64 count = 0;
    rc = fsDirGetEntryCount(&dir, &count);
    if (R_FAILED(rc) || count <= 0 || count > 16) {
        fsDirClose(&dir);
        fsFsClose(&cnmtFs);
        cleanup();
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "CNMT filesystem sin archivo metadata valido", rc);
        return out;
    }
    std::vector<FsDirectoryEntry> dirEntries(static_cast<size_t>(count));
    s64 readCount = 0;
    rc = fsDirRead(&dir, &readCount, count, dirEntries.data());
    fsDirClose(&dir);
    if (R_FAILED(rc) || readCount <= 0) {
        fsFsClose(&cnmtFs);
        cleanup();
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::InvalidNca, "Read CNMT directory");
        return out;
    }

    const FsDirectoryEntry* cnmtFileEntry = nullptr;
    for (s64 i = 0; i < readCount; ++i) {
        if (dirEntries[static_cast<size_t>(i)].type == FsDirEntryType_File) {
            cnmtFileEntry = &dirEntries[static_cast<size_t>(i)];
            break;
        }
    }
    if (!cnmtFileEntry) {
        fsFsClose(&cnmtFs);
        cleanup();
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "ContentMeta no contiene archivo CNMT");
        return out;
    }

    std::string cnmtInternalPath = "/" + std::string(cnmtFileEntry->name);
    FsFile cnmtFile{};
    rc = fsFsOpenFile(&cnmtFs, cnmtInternalPath.c_str(), FsOpenMode_Read, &cnmtFile);
    if (R_FAILED(rc)) {
        fsFsClose(&cnmtFs);
        cleanup();
        out.diagnostic = resultToDiagnostic(rc, ErrorCode::InvalidNca, "Open CNMT file");
        return out;
    }

    auto closeFs = [&]() {
        fsFileClose(&cnmtFile);
        fsFsClose(&cnmtFs);
    };

    PackagedCnmtHeader header{};
    uint64_t bytesRead = 0;
    rc = fsFileRead(&cnmtFile, 0, &header, sizeof(header), FsReadOption_None, &bytesRead);
    if (R_FAILED(rc) || bytesRead != sizeof(header)) {
        closeFs();
        cleanup();
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "CNMT header incompleto", rc);
        return out;
    }

    out.titleId = header.titleId;
    out.titleVersion = header.titleVersion;
    out.metaType = header.metaType;
    out.expectedContentCount = header.metaHeader.content_count;

    if (header.metaHeader.extended_header_size > 0x10000 || header.metaHeader.content_count > 0x4000) {
        closeFs();
        cleanup();
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "CNMT contiene contadores fuera de limite");
        return out;
    }

    std::vector<uint8_t> ext(header.metaHeader.extended_header_size);
    uint64_t offset = sizeof(header);
    if (!ext.empty()) {
        bytesRead = 0;
        rc = fsFileRead(&cnmtFile, offset, ext.data(), ext.size(), FsReadOption_None, &bytesRead);
        if (R_FAILED(rc) || bytesRead != ext.size()) {
            closeFs();
            cleanup();
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "Extended header CNMT incompleto", rc);
            return out;
        }
        offset += ext.size();
    }

    out.applicationId = getApplicationIdForMeta(header.titleId, header.metaType, ext);
    out.requiredSystemVersion = getRequiredSystemVersion(header.metaType, ext);

    for (uint16_t i = 0; i < header.metaHeader.content_count; ++i) {
        NcmPackagedContentInfo packed{};
        bytesRead = 0;
        rc = fsFileRead(&cnmtFile, offset, &packed, sizeof(packed), FsReadOption_None, &bytesRead);
        if (R_FAILED(rc) || bytesRead != sizeof(packed)) {
            closeFs();
            cleanup();
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "Lista de contenidos CNMT incompleta", rc);
            return out;
        }
        offset += sizeof(packed);
        if (packed.info.content_type == NcmContentType_DeltaFragment)
            continue;

        uint64_t expectedSize = 0;
        ncmContentInfoSizeToU64(&packed.info, &expectedSize);
        out.expectedInstallBytes += expectedSize;
        const auto* actual = findManifestContent(manifest, packed.info.content_id);
        const std::string id = bytesToHex(&packed.info.content_id, sizeof(packed.info.content_id));
        if (!actual) {
            out.missingContentIds.push_back(id);
            continue;
        }
        if (actual->size != expectedSize) {
            out.sizeMismatchContentIds.push_back(id);
            continue;
        }
        ++out.matchedContentCount;
    }

    out.expectedInstallBytes += cnmtEntry->size;
    closeFs();
    cleanup();

    if (!out.missingContentIds.empty() || !out.sizeMismatchContentIds.empty()) {
        std::ostringstream tech;
        tech << "missing=" << out.missingContentIds.size()
             << ", size_mismatch=" << out.sizeMismatchContentIds.size();
        if (!out.missingContentIds.empty()) tech << ", first_missing=" << out.missingContentIds.front();
        if (!out.sizeMismatchContentIds.empty()) tech << ", first_size_mismatch=" << out.sizeMismatchContentIds.front();
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, tech.str());
        return out;
    }

    const uint32_t currentPacked = parseCurrentFirmwarePacked();
    if (out.requiredSystemVersion && currentPacked && out.requiredSystemVersion > currentPacked) {
        out.diagnostic = makeDiagnostic(ErrorCode::FirmwareTooOld,
            "required=" + formatPackedSystemVersion(out.requiredSystemVersion) +
            ", current=" + formatPackedSystemVersion(currentPacked));
        return out;
    }

    const auto system = getSystemSnapshot();
    if (system.sdFreeBytes >= 0 && out.expectedInstallBytes > static_cast<uint64_t>(system.sdFreeBytes)) {
        out.diagnostic = makeDiagnostic(ErrorCode::NotEnoughSpace,
            "install_required=" + std::to_string(out.expectedInstallBytes) +
            ", free=" + std::to_string(system.sdFreeBytes));
        return out;
    }

    std::ostringstream tech;
    tech << "CNMT OK title=" << formatTitleId(out.titleId)
         << " type=0x" << std::hex << static_cast<unsigned>(out.metaType)
         << std::dec << " contents=" << out.matchedContentCount
         << " install_bytes=" << out.expectedInstallBytes
         << " required_fw=" << formatPackedSystemVersion(out.requiredSystemVersion);
    out.diagnostic = makeDiagnostic(ErrorCode::None, tech.str());
    return out;
}

} 
