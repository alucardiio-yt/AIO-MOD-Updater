#include "nsp_transaction_installer.hpp"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "package_reader.hpp"
#include "progress_event.hpp"

namespace installer {
namespace {

constexpr const char JOURNAL_PATH[] = "/switch/aio-switch-updater-mod/config/install_work/current_install.json";
constexpr const char INSTALL_WORK_DIR[] = "/switch/aio-switch-updater-mod/config/install_work";
constexpr size_t COPY_BUFFER_SIZE = 1024 * 1024;

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

struct ContentStorageRecord {
    NcmContentMetaKey key;
    uint8_t storage_id;
    uint8_t padding[7];
};

struct PlannedContent {
    const NspEntryInfo* entry = nullptr;
    NcmContentInfo info{};
    NcmPlaceHolderId placeholder{};
    bool placeholderCreated = false;
    bool registered = false;
    bool preExisting = false;
};

struct ParsedMeta {
    Diagnostic diagnostic;
    PackagedCnmtHeader header{};
    std::vector<uint8_t> extendedHeader;
    std::vector<NcmContentInfo> contentInfos; // excludes meta until caller adds it
    std::vector<NcmContentMetaInfo> metaInfos;
    uint64_t appId = 0;
    bool ok() const { return diagnostic.ok(); }
};

std::string hex(const void* ptr, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(ptr);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) ss << std::setw(2) << static_cast<unsigned>(p[i]);
    return ss.str();
}

int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fromHex(const std::string& s, void* ptr, size_t size)
{
    if (s.size() != size * 2) return false;
    auto* p = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        int hi = nibble(s[i * 2]), lo = nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        p[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

Diagnostic rcDiag(Result rc, ErrorCode code, const std::string& where)
{
    if (R_SUCCEEDED(rc)) return makeDiagnostic(ErrorCode::None);
    std::ostringstream ss;
    ss << where << " rc=0x" << std::uppercase << std::hex << static_cast<uint32_t>(rc);
    return makeDiagnostic(code, ss.str(), rc);
}

const NspEntryInfo* findEntry(const NspManifest& manifest, const NcmContentId& id)
{
    for (const auto& e : manifest.entries)
        if (e.isNca && e.hasContentId && std::memcmp(&e.contentId, &id, sizeof(id)) == 0) return &e;
    return nullptr;
}

const NspEntryInfo* findCnmt(const NspManifest& manifest)
{
    for (const auto& e : manifest.entries) if (e.isCnmtNca && e.hasContentId) return &e;
    return nullptr;
}

uint64_t applicationIdFor(uint64_t titleId, uint8_t type, const std::vector<uint8_t>& ext)
{
    switch (type) {
        case NcmContentMetaType_Application: return titleId;
        case NcmContentMetaType_Patch:
            if (ext.size() >= sizeof(NcmPatchMetaExtendedHeader)) {
                NcmPatchMetaExtendedHeader h{}; std::memcpy(&h, ext.data(), sizeof(h)); return h.application_id;
            }
            return titleId & ~0x800ULL;
        case NcmContentMetaType_AddOnContent:
            if (hosversionAtLeast(15,0,0) && ext.size() >= sizeof(NcmAddOnContentMetaExtendedHeader)) {
                NcmAddOnContentMetaExtendedHeader h{}; std::memcpy(&h, ext.data(), sizeof(h)); return h.application_id;
            }
            if (ext.size() >= sizeof(NcmLegacyAddOnContentMetaExtendedHeader)) {
                NcmLegacyAddOnContentMetaExtendedHeader h{}; std::memcpy(&h, ext.data(), sizeof(h)); return h.application_id;
            }
            return titleId;
        default: return titleId;
    }
}

struct NcmContext {
    bool init = false;
    NcmContentStorage storage{};
    NcmContentMetaDatabase db{};
    bool storageOpen = false;
    bool dbOpen = false;
    Diagnostic open()
    {
        Result rc = ncmInitialize();
        if (R_FAILED(rc)) return rcDiag(rc, ErrorCode::InternalError, "ncmInitialize");
        init = true;
        rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard);
        if (R_FAILED(rc)) return rcDiag(rc, ErrorCode::PlaceholderError, "ncmOpenContentStorage(SD)");
        storageOpen = true;
        rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
        if (R_FAILED(rc)) return rcDiag(rc, ErrorCode::RegistrationError, "ncmOpenContentMetaDatabase(SD)");
        dbOpen = true;
        return makeDiagnostic(ErrorCode::None);
    }
    ~NcmContext() {
        if (dbOpen) ncmContentMetaDatabaseClose(&db);
        if (storageOpen) ncmContentStorageClose(&storage);
        if (init) ncmExit();
    }
};

void saveJournal(const std::string& package, uint64_t appId, const NcmContentMetaKey& key,
                 const std::vector<PlannedContent>& contents, bool metaSet, bool recordPushed,
                 bool ticketImported, const std::string& stage)
{
    std::error_code ec; std::filesystem::create_directories(INSTALL_WORK_DIR, ec);
    nlohmann::json j;
    j["owner"] = "aio-switch-updater-mod";
    j["kind"] = "real-install";
    j["package"] = package;
    j["stage"] = stage;
    j["application_id"] = appId;
    j["meta_key"] = { {"id", key.id}, {"version", key.version}, {"type", key.type}, {"install_type", key.install_type} };
    j["meta_set"] = metaSet;
    j["record_pushed"] = recordPushed;
    j["ticket_imported"] = ticketImported;
    j["contents"] = nlohmann::json::array();
    for (const auto& c : contents) {
        nlohmann::json x;
        x["content_id"] = hex(&c.info.content_id, sizeof(c.info.content_id));
        x["placeholder_id"] = hex(&c.placeholder, sizeof(c.placeholder));
        x["placeholder_created"] = c.placeholderCreated;
        x["registered"] = c.registered;
        x["pre_existing"] = c.preExisting;
        j["contents"].push_back(x);
    }
    const std::string tmp = std::string(JOURNAL_PATH) + ".tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); if (out) out << j.dump(2); }
    std::filesystem::rename(tmp, JOURNAL_PATH, ec);
    if (ec) { std::filesystem::remove(JOURNAL_PATH, ec); ec.clear(); std::filesystem::rename(tmp, JOURNAL_PATH, ec); }
}

bool removeJournal(std::string* detail = nullptr)
{
    // fsdev/FAT may refuse to unlink a file that is still open.  Recovery callers
    // therefore close the input stream before reaching this helper.  Be strict:
    // success means the active journal path is actually gone.
    std::error_code ec;
    const std::string tmp = std::string(JOURNAL_PATH) + ".tmp";
    const std::string recovered = std::string(JOURNAL_PATH) + ".recovered";

    std::filesystem::remove(tmp, ec);
    ec.clear();
    std::filesystem::remove(recovered, ec);
    ec.clear();

    std::filesystem::remove(JOURNAL_PATH, ec);
    ec.clear();
    if (!std::filesystem::exists(JOURNAL_PATH, ec) && !ec) {
        if (detail) *detail = "Install journal cleared.";
        return true;
    }

    // Some FAT/fsdev paths are more reliable when the active filename is first
    // moved away and then removed.  The installer only treats JOURNAL_PATH as
    // an active transaction, so a successful rename is already enough to unblock
    // the next install; we still remove the tombstone when possible.
    ec.clear();
    std::filesystem::rename(JOURNAL_PATH, recovered, ec);
    if (!ec) {
        std::error_code removeEc;
        std::filesystem::remove(recovered, removeEc);
        std::error_code existsEc;
        if (!std::filesystem::exists(JOURNAL_PATH, existsEc) && !existsEc) {
            if (detail) *detail = "Install journal cleared after rename fallback.";
            return true;
        }
    }

    if (detail) *detail = "Could not remove the active install journal.";
    return false;
}

Result pushApplicationRecord(uint64_t appId, const ContentStorageRecord* records, uint32_t count)
{
    Service srv{};
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return rc;
    rc = nsGetApplicationManagerInterface(&srv);
    if (R_SUCCEEDED(rc)) {
        const struct { uint8_t event; uint8_t pad[7]; uint64_t tid; } in = {3,{0},appId};
        rc = serviceDispatchIn(&srv, 16, in,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
            .buffers = {{records, sizeof(*records) * count}});
        serviceClose(&srv);
    }
    nsExit();
    return rc;
}

Result deleteApplicationRecord(uint64_t appId)
{
    Service srv{};
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return rc;
    rc = nsGetApplicationManagerInterface(&srv);
    if (R_SUCCEEDED(rc)) { rc = serviceDispatchIn(&srv, 27, appId); serviceClose(&srv); }
    nsExit();
    return rc;
}

Result importTicket(const void* tik, size_t tikSize, const void* cert, size_t certSize)
{
    Service es{};
    Result rc = smGetService(&es, "es");
    if (R_FAILED(rc)) return rc;
    rc = serviceDispatch(&es, 1,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In, SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{tik, tikSize}, {cert, certSize}});
    serviceClose(&es);
    return rc;
}

std::vector<uint8_t> readNspEntry(const std::string& path, const NspEntryInfo& entry)
{
    if (entry.size > static_cast<uint64_t>(SIZE_MAX)) return {};
    std::vector<uint8_t> out(static_cast<size_t>(entry.size));
    PackageReader f(path);
    if (!f.isOpen() || !f.seek(entry.offset) || !f.read(out.data(), out.size())) return {};
    return out;
}

const NspEntryInfo* findRightsFile(const NspManifest& manifest, const FsRightsId& rights, bool ticket)
{
    const std::string wanted = hex(&rights, sizeof(rights));
    for (const auto& e : manifest.entries) {
        if ((ticket && !e.isTicket) || (!ticket && !e.isCert)) continue;
        if (e.name.size() >= wanted.size() && std::equal(wanted.begin(), wanted.end(), e.name.begin(),
            [](char a,char b){ return std::tolower((unsigned char)a)==std::tolower((unsigned char)b); })) return &e;
    }
    return nullptr;
}

ParsedMeta parseInstalledMeta(NcmContext& ctx, const NspManifest& manifest, const NspEntryInfo& cnmtEntry)
{
    ParsedMeta out;
    char contentPath[FS_MAX_PATH]{};
    Result rc = ncmContentStorageGetPath(&ctx.storage, contentPath, sizeof(contentPath), &cnmtEntry.contentId);
    if (R_FAILED(rc)) { out.diagnostic = rcDiag(rc, ErrorCode::RegistrationError, "GetPath(CNMT)"); return out; }
    FsFileSystem fs{};
    rc = fsOpenFileSystem(&fs, FsFileSystemType_ContentMeta, contentPath);
    if (R_FAILED(rc)) { out.diagnostic = rcDiag(rc, ErrorCode::AuthorizationRejected, "fsOpenFileSystem(ContentMeta)"); return out; }
    FsDir dir{};
    rc = fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) { fsFsClose(&fs); out.diagnostic = rcDiag(rc, ErrorCode::InvalidNca, "Open CNMT dir"); return out; }
    s64 cnt=0;
    rc = fsDirGetEntryCount(&dir,&cnt);
    if (R_FAILED(rc) || cnt <= 0 || cnt > 32) {
        fsDirClose(&dir); fsFsClose(&fs);
        out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT file missing or directory count invalid",rc); return out;
    }
    std::vector<FsDirectoryEntry> ents(static_cast<size_t>(cnt)); s64 got=0;
    rc = fsDirRead(&dir,&got,cnt,ents.data());
    fsDirClose(&dir);
    if (R_FAILED(rc)||got<=0) { fsFsClose(&fs); out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT file missing",rc); return out; }
    std::string p = "/" + std::string(ents[0].name);
    FsFile f{}; rc=fsFsOpenFile(&fs,p.c_str(),FsOpenMode_Read,&f);
    if (R_FAILED(rc)) { fsFsClose(&fs); out.diagnostic=rcDiag(rc,ErrorCode::InvalidNca,"Open CNMT file"); return out; }
    auto close=[&](){fsFileClose(&f);fsFsClose(&fs);};
    uint64_t br=0; rc=fsFileRead(&f,0,&out.header,sizeof(out.header),FsReadOption_None,&br);
    if (R_FAILED(rc)||br!=sizeof(out.header)) {close();out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT header incomplete",rc);return out;}
    if (out.header.metaHeader.extended_header_size>0x10000||out.header.metaHeader.content_count>0x4000||out.header.metaHeader.content_meta_count>0x4000){close();out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT counts out of range");return out;}
    uint64_t off=sizeof(out.header);
    out.extendedHeader.resize(out.header.metaHeader.extended_header_size);
    if(!out.extendedHeader.empty()){br=0;rc=fsFileRead(&f,off,out.extendedHeader.data(),out.extendedHeader.size(),FsReadOption_None,&br);if(R_FAILED(rc)||br!=out.extendedHeader.size()){close();out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT ext incomplete",rc);return out;}off+=out.extendedHeader.size();}
    for(uint16_t i=0;i<out.header.metaHeader.content_count;i++){
        NcmPackagedContentInfo pi{};br=0;rc=fsFileRead(&f,off,&pi,sizeof(pi),FsReadOption_None,&br);if(R_FAILED(rc)||br!=sizeof(pi)){close();out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT contents incomplete",rc);return out;}off+=sizeof(pi);
        if(pi.info.content_type!=NcmContentType_DeltaFragment) out.contentInfos.push_back(pi.info);
    }
    out.metaInfos.resize(out.header.metaHeader.content_meta_count);
    if(!out.metaInfos.empty()){const uint64_t sz=out.metaInfos.size()*sizeof(NcmContentMetaInfo);br=0;rc=fsFileRead(&f,off,out.metaInfos.data(),sz,FsReadOption_None,&br);if(R_FAILED(rc)||br!=sz){close();out.diagnostic=makeDiagnostic(ErrorCode::InvalidNca,"CNMT meta refs incomplete",rc);return out;}}
    close();
    out.appId=applicationIdFor(out.header.titleId,out.header.metaType,out.extendedHeader);
    out.diagnostic=makeDiagnostic(ErrorCode::None);
    return out;
}

std::vector<uint8_t> buildMetaDbBlob(const ParsedMeta& meta, const NspEntryInfo& cnmt)
{
    NcmContentMetaHeader h = meta.header.metaHeader;
    h.content_count = static_cast<uint16_t>(meta.contentInfos.size() + 1);
    h.content_meta_count = static_cast<uint16_t>(meta.metaInfos.size());
    h.storage_id = 0;
    NcmContentInfo metaInfo{};
    metaInfo.content_id = cnmt.contentId;
    metaInfo.content_type = NcmContentType_Meta;
    ncmU64ToContentInfoSize(cnmt.size, &metaInfo);
    const size_t total = sizeof(h)+meta.extendedHeader.size()+sizeof(metaInfo)+meta.contentInfos.size()*sizeof(NcmContentInfo)+meta.metaInfos.size()*sizeof(NcmContentMetaInfo);
    std::vector<uint8_t> blob(total); size_t o=0;
    auto put=[&](const void* p,size_t n){std::memcpy(blob.data()+o,p,n);o+=n;};
    put(&h,sizeof(h)); if(!meta.extendedHeader.empty())put(meta.extendedHeader.data(),meta.extendedHeader.size()); put(&metaInfo,sizeof(metaInfo));
    if(!meta.contentInfos.empty())put(meta.contentInfos.data(),meta.contentInfos.size()*sizeof(NcmContentInfo));
    if(!meta.metaInfos.empty())put(meta.metaInfos.data(),meta.metaInfos.size()*sizeof(NcmContentMetaInfo));
    return blob;
}

} // namespace

bool recoverStaleRealInstall(std::string* detail)
{
    if (!std::filesystem::exists(JOURNAL_PATH)) return true;
    try {
        nlohmann::json j;
        {
            // IMPORTANT: close current_install.json before any rollback or unlink.
            // fsdev/FAT can keep an open file undeletable, which previously caused
            // recovery to succeed logically while JOURNAL_PATH remained on disk.
            std::ifstream in(JOURNAL_PATH);
            if (!in) {
                if (detail) *detail = "Could not open install journal for recovery.";
                return false;
            }
            in >> j;
        }

        if (j.value("owner", "") != "aio-switch-updater-mod") {
            if (detail) *detail = "Install journal belongs to an unknown owner; it was preserved.";
            return false;
        }

        const std::string kind = j.value("kind", "");

        // Migration/recovery for journals left by the old MTP diagnostics that
        // existed before the direct-stream installer.  All three phases used
        // the same current_install.json path.  The old streaming code treated
        // these known AIO journals as an active foreign transaction and returned
        // AIO-INSTALL-INT-001 even though the object upload itself had succeeded.
        if (kind == "ncm-probe") {
            NcmPlaceHolderId placeholder{};
            if (!fromHex(j.value("placeholder_id", ""), &placeholder, sizeof(placeholder))) {
                if (detail) *detail = "Legacy ncm-probe journal has an invalid placeholder id.";
                return false;
            }
            NcmContext ctx;
            Diagnostic d = ctx.open();
            if (!d.ok()) { if (detail) *detail = d.technical; return false; }
            bool exists = false;
            Result rc = ncmContentStorageHasPlaceHolder(&ctx.storage, &exists, &placeholder);
            if (R_FAILED(rc)) { if (detail) *detail = "Could not inspect legacy NCM placeholder."; return false; }
            if (exists) {
                rc = ncmContentStorageDeletePlaceHolder(&ctx.storage, &placeholder);
                if (R_FAILED(rc)) { if (detail) *detail = "Could not remove legacy NCM placeholder."; return false; }
            }
            std::string clearDetail;
            if (!removeJournal(&clearDetail)) { if (detail) *detail = clearDetail; return false; }
            if (detail) *detail = "Legacy ncm-probe transaction recovered.";
            return true;
        }

        if (kind == "cnmt-analysis") {
            NcmContentId content{};
            if (!fromHex(j.value("content_id", ""), &content, sizeof(content))) {
                if (detail) *detail = "Legacy cnmt-analysis journal has an invalid ContentId.";
                return false;
            }
            if (j.value("content_created", false)) {
                NcmContext ctx;
                Diagnostic d = ctx.open();
                if (!d.ok()) { if (detail) *detail = d.technical; return false; }
                bool exists = false;
                Result rc = ncmContentStorageHas(&ctx.storage, &exists, &content);
                if (R_FAILED(rc)) { if (detail) *detail = "Could not inspect legacy CNMT content."; return false; }
                if (exists) {
                    rc = ncmContentStorageDelete(&ctx.storage, &content);
                    if (R_FAILED(rc)) { if (detail) *detail = "Could not remove legacy CNMT content."; return false; }
                }
            }
            std::string clearDetail;
            if (!removeJournal(&clearDetail)) { if (detail) *detail = clearDetail; return false; }
            if (detail) *detail = "Legacy cnmt-analysis transaction recovered.";
            return true;
        }

        if (kind != "real-install") {
            if (detail) *detail = "Unknown AIO install journal kind: " + kind;
            return false;
        }

        // ApplicationRecord is the final mutating step. If it was successfully pushed,
        // the installation itself is complete even if power was lost before journal cleanup.
        const std::string stage = j.value("stage", "");
        if (stage == "application-record" || stage == "complete") {
            std::string clearDetail;
            if (!removeJournal(&clearDetail)) { if (detail) *detail = clearDetail; return false; }
            if (detail) *detail = "Completed install journal cleared.";
            return true;
        }

        NcmContext ctx;
        Diagnostic d = ctx.open();
        if (!d.ok()) { if (detail) *detail = d.technical; return false; }
        if (j.value("meta_set", false)) {
            NcmContentMetaKey key{};
            auto k = j["meta_key"];
            key.id = k.value("id", 0ULL);
            key.version = k.value("version", 0U);
            key.type = k.value("type", 0);
            key.install_type = k.value("install_type", 0);
            ncmContentMetaDatabaseRemove(&ctx.db, &key);
            ncmContentMetaDatabaseCommit(&ctx.db);
        }
        if (j.contains("contents")) {
            for (auto it = j["contents"].rbegin(); it != j["contents"].rend(); ++it) {
                NcmContentId cid{};
                NcmPlaceHolderId ph{};
                fromHex((*it).value("content_id", ""), &cid, sizeof(cid));
                fromHex((*it).value("placeholder_id", ""), &ph, sizeof(ph));
                if ((*it).value("registered", false) && !(*it).value("pre_existing", false)) {
                    bool exists = false;
                    if (R_SUCCEEDED(ncmContentStorageHas(&ctx.storage, &exists, &cid)) && exists)
                        ncmContentStorageDelete(&ctx.storage, &cid);
                } else if ((*it).value("placeholder_created", false)) {
                    bool exists = false;
                    if (R_SUCCEEDED(ncmContentStorageHasPlaceHolder(&ctx.storage, &exists, &ph)) && exists)
                        ncmContentStorageDeletePlaceHolder(&ctx.storage, &ph);
                }
            }
        }
        std::string clearDetail;
        if (!removeJournal(&clearDetail)) {
            if (detail) *detail = "Rollback completed, but " + clearDetail;
            return false;
        }
        if (detail) *detail = "Interrupted AIO install rolled back and journal cleared.";
        return true;
    } catch (const std::exception& e) {
        if (detail) *detail = e.what();
        return false;
    }
}

RealInstallResult installNspToSdTransactional(const std::string& nspPath, InstallProgressCallback progress)
{
    RealInstallResult out;
    std::string recovery; if(!recoverStaleRealInstall(&recovery)){out.diagnostic=makeDiagnostic(ErrorCode::RegistrationError,"Recovery failed: "+recovery);return out;}
    if(std::filesystem::exists(JOURNAL_PATH)){out.diagnostic=makeDiagnostic(ErrorCode::RegistrationError,"Install journal remained after recovery: "+recovery);return out;}
    const auto analysis=analyzeCnmtRequirements(nspPath); if(!analysis.ok()){out.diagnostic=analysis.diagnostic;return out;}
    const auto manifest=inspectNspManifest(nspPath); if(!manifest.ok()){out.diagnostic=manifest.diagnostic;return out;}
    if (analysis.metaType != NcmContentMetaType_Application &&
        analysis.metaType != NcmContentMetaType_Patch &&
        analysis.metaType != NcmContentMetaType_AddOnContent) {
        out.diagnostic = makeDiagnostic(ErrorCode::UnsupportedFormat,
            "Only Application, Patch and AddOnContent CNMT types are supported.");
        return out;
    }
    if(manifest.cnmtNcaCount!=1){out.diagnostic=makeDiagnostic(ErrorCode::UnsupportedFormat,"Multi-title bundle NSPs are intentionally refused by the transactional installer; use separate Base/Update/DLC NSPs.");return out;}
    const auto* cnmt=findCnmt(manifest); if(!cnmt){out.diagnostic=makeDiagnostic(ErrorCode::MissingContent,"CNMT missing");return out;}

    NcmContext ctx; out.diagnostic=ctx.open(); if(!out.diagnostic.ok())return out;
    NcmContentMetaKey key{}; key.id=analysis.titleId;key.version=analysis.titleVersion;key.type=analysis.metaType;key.install_type=NcmContentInstallType_Full;std::memset(key.padding,0,sizeof(key.padding));
    bool metaExists=false; Result rc=ncmContentMetaDatabaseHas(&ctx.db,&metaExists,&key); if(R_FAILED(rc)){out.diagnostic=rcDiag(rc,ErrorCode::RegistrationError,"ContentMetaDatabaseHas");return out;}
    if(metaExists){out.diagnostic=makeDiagnostic(ErrorCode::AlreadyInstalled,"This exact title/version is already installed.");return out;}

    // Updates/DLC require the base application metadata to already exist on the SD.
    if (analysis.metaType == NcmContentMetaType_Patch || analysis.metaType == NcmContentMetaType_AddOnContent) {
        bool baseFound=false; NcmContentMetaKey baseKey{};
        rc=ncmContentMetaDatabaseGetLatestContentMetaKey(&ctx.db,&baseKey,analysis.applicationId);
        baseFound=R_SUCCEEDED(rc) && baseKey.type==NcmContentMetaType_Application;
        if(!baseFound){
            NcmContentMetaDatabase userDb{};
            Result urc=ncmOpenContentMetaDatabase(&userDb,NcmStorageId_BuiltInUser);
            if(R_SUCCEEDED(urc)){
                NcmContentMetaKey userKey{};
                urc=ncmContentMetaDatabaseGetLatestContentMetaKey(&userDb,&userKey,analysis.applicationId);
                baseFound=R_SUCCEEDED(urc) && userKey.type==NcmContentMetaType_Application;
                ncmContentMetaDatabaseClose(&userDb);
            }
        }
        if(!baseFound){
            out.diagnostic=makeDiagnostic(ErrorCode::MissingContent,
                "The base application is not installed. Install the base game before its update/DLC.");
            return out;
        }
    }

    // Refuse downgrades/same-version metadata across both SD and internal user storage.
    // Installing a duplicate base/update on another storage can leave ambiguous records,
    // so the destination choice remains deterministic: this installer writes new content to SD.
    bool matchingMetaFound = false;
    uint32_t latestMatchingVersion = 0;
    auto inspectLatest = [&](NcmContentMetaDatabase* db) {
        NcmContentMetaKey candidate{};
        const Result r = ncmContentMetaDatabaseGetLatestContentMetaKey(db, &candidate, analysis.titleId);
        if (R_SUCCEEDED(r) && candidate.type == analysis.metaType) {
            matchingMetaFound = true;
            latestMatchingVersion = std::max(latestMatchingVersion, candidate.version);
        }
    };
    inspectLatest(&ctx.db);
    NcmContentMetaDatabase userMetaDb{};
    Result userMetaRc = ncmOpenContentMetaDatabase(&userMetaDb, NcmStorageId_BuiltInUser);
    if (R_SUCCEEDED(userMetaRc)) {
        inspectLatest(&userMetaDb);
        ncmContentMetaDatabaseClose(&userMetaDb);
    }

    if (matchingMetaFound && latestMatchingVersion >= analysis.titleVersion) {
        out.diagnostic = makeDiagnostic(ErrorCode::AlreadyInstalled,
            "The same or a newer version of this content is already installed.");
        return out;
    }
    if (analysis.metaType == NcmContentMetaType_Application && matchingMetaFound) {
        out.diagnostic = makeDiagnostic(ErrorCode::AlreadyInstalled,
            "Base application replacement is intentionally disabled. Install updates as Patch NSPs instead.");
        return out;
    }

    // Build the content plan. ContentIds already present in ContentStorage are shared/reused.
    // Rollback will never delete those pre-existing objects.
    std::vector<PlannedContent> plan; plan.reserve(manifest.ncaCount);
    uint64_t total = 0;
    for(const auto& e:manifest.entries) if(e.isNca&&e.hasContentId){
        bool exists=false;rc=ncmContentStorageHas(&ctx.storage,&exists,&e.contentId);if(R_FAILED(rc)){out.diagnostic=rcDiag(rc,ErrorCode::RegistrationError,"ContentStorageHas");return out;}
        PlannedContent p; p.entry=&e;p.info.content_id=e.contentId;p.info.content_type=e.isCnmtNca?NcmContentType_Meta:NcmContentType_Data;ncmU64ToContentInfoSize(e.size,&p.info);p.preExisting=exists;plan.push_back(p);
        if(!exists) total += e.size;
    }

    // The NSP staging copy already occupies SD space. Check the exact amount that NCM
    // still needs after accounting for ContentIds already present/shared on the SD.
    constexpr uint64_t INSTALL_SPACE_RESERVE = 64ULL * 1024ULL * 1024ULL;
    const auto space = getSystemSnapshot();
    if (space.sdFreeBytes >= 0) {
        const uint64_t freeBytes = static_cast<uint64_t>(space.sdFreeBytes);
        const uint64_t requiredBytes = total > std::numeric_limits<uint64_t>::max() - INSTALL_SPACE_RESERVE
            ? std::numeric_limits<uint64_t>::max()
            : total + INSTALL_SPACE_RESERVE;
        if (freeBytes < requiredBytes) {
            out.diagnostic = makeDiagnostic(ErrorCode::NotEnoughSpace,
                "install_required=" + std::to_string(requiredBytes) +
                ", free=" + std::to_string(freeBytes));
            return out;
        }
    }

    saveJournal(nspPath,analysis.applicationId,key,plan,false,false,false,"prepared");
    uint64_t overall=0; PackageReader nsp(nspPath); if(!nsp.isOpen()){removeJournal();out.diagnostic=makeDiagnostic(ErrorCode::InternalError,"Cannot open staged NSP");return out;}
    std::vector<uint8_t> buf(COPY_BUFFER_SIZE);
    auto failRollback=[&](Diagnostic d){out.diagnostic=d;std::string x;recoverStaleRealInstall(&x);return out;};
    for(size_t i=0;i<plan.size();++i){ auto& p=plan[i];
        if(ProgressEvent::instance().getInterupt()) return failRollback(makeDiagnostic(ErrorCode::Cancelled,"User cancelled install"));
        if(p.preExisting){
            if(progress)progress(overall,std::max<uint64_t>(total,1),p.entry->name+" (already present)",(uint32_t)i+1,(uint32_t)plan.size());
            continue;
        }
        rc=ncmContentStorageGeneratePlaceHolderId(&ctx.storage,&p.placeholder);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::PlaceholderError,"GeneratePlaceHolderId"));
        rc=ncmContentStorageCreatePlaceHolder(&ctx.storage,&p.entry->contentId,&p.placeholder,p.entry->size);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::PlaceholderError,"CreatePlaceHolder"));
        p.placeholderCreated=true;saveJournal(nspPath,analysis.applicationId,key,plan,false,false,false,"writing-content");
        if(!nsp.seek(p.entry->offset))return failRollback(makeDiagnostic(ErrorCode::PackageTruncated,"Seek failed: "+p.entry->name));
        uint64_t local=0;while(local<p.entry->size){size_t want=(size_t)std::min<uint64_t>(buf.size(),p.entry->size-local);if(!nsp.read(buf.data(),want))return failRollback(makeDiagnostic(ErrorCode::PackageTruncated,"NSP ended while reading "+p.entry->name));
            rc=ncmContentStorageWritePlaceHolder(&ctx.storage,&p.placeholder,local,buf.data(),want);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::WriteError,"WritePlaceHolder"));local+=want;overall+=want;
            if(progress)progress(overall,std::max<uint64_t>(total,1),p.entry->name,(uint32_t)i+1,(uint32_t)plan.size());
            if(ProgressEvent::instance().getInterupt())return failRollback(makeDiagnostic(ErrorCode::Cancelled,"User cancelled install"));
        }
        rc=ncmContentStorageRegister(&ctx.storage,&p.entry->contentId,&p.placeholder);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"Register content"));
        p.placeholderCreated=false;p.registered=true;saveJournal(nspPath,analysis.applicationId,key,plan,false,false,false,"registered-content");out.installedBytes=overall;out.installedContents=(uint32_t)i+1;
    }

    ParsedMeta meta=parseInstalledMeta(ctx,manifest,*cnmt);if(!meta.ok())return failRollback(meta.diagnostic);
    auto blob=buildMetaDbBlob(meta,*cnmt);
    // Record rollback intent before the first metadata mutation. Removing a non-existent
    // key is harmless, while missing a successful Set after a crash would not be.
    saveJournal(nspPath,meta.appId,key,plan,true,false,false,"metadata-pending");
    rc=ncmContentMetaDatabaseSet(&ctx.db,&key,blob.data(),blob.size());if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"ContentMetaDatabaseSet"));
    rc=ncmContentMetaDatabaseCommit(&ctx.db);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"ContentMetaDatabaseCommit"));
    bool metaSet=true;saveJournal(nspPath,meta.appId,key,plan,metaSet,false,false,"metadata-committed");

    // Import the ticket (when required) before the final ApplicationRecord mutation.
    // If later registration fails, content/meta can still be rolled back; an imported ticket
    // may remain orphaned, which is harmless and avoids tearing down a pre-existing app record.
    NcmRightsId rights{}; bool rightsFound=false;
    for(const auto& p:plan){ if(p.entry->isCnmtNca)continue; Result rr=ncmContentStorageGetRightsIdFromContentId(&ctx.storage,&rights,&p.info.content_id,FsContentAttributes_None);if(R_SUCCEEDED(rr)){std::array<uint8_t,sizeof(FsRightsId)> z{};if(std::memcmp(&rights.rights_id,z.data(),z.size())!=0){rightsFound=true;break;}} }
    if(rightsFound){
        const auto* tik=findRightsFile(manifest,rights.rights_id,true);const auto* cert=findRightsFile(manifest,rights.rights_id,false);
        if(!tik||!cert)return failRollback(makeDiagnostic(ErrorCode::MissingContent,"Rights-managed title requires matching .tik and .cert"));
        auto tikData=readNspEntry(nspPath,*tik), certData=readNspEntry(nspPath,*cert);if(tikData.empty()||certData.empty())return failRollback(makeDiagnostic(ErrorCode::PackageTruncated,"Ticket/cert data truncated"));
        rc=importTicket(tikData.data(),tikData.size(),certData.data(),certData.size());if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::AuthorizationRejected,"es ImportTicket"));
        out.ticketImported=true;
        saveJournal(nspPath,meta.appId,key,plan,metaSet,false,out.ticketImported,"ticket-imported");
    }

    // Push the ApplicationRecord as the final mutation. Reuse the current record set and
    // append this meta key, so Patch/DLC installs don't discard the base application's records.
    std::vector<ContentStorageRecord> records;
    {
        Service srv{};
        Result rr=nsInitialize();
        if(R_SUCCEEDED(rr)) rr=nsGetApplicationManagerInterface(&srv);
        if(R_SUCCEEDED(rr)){
            constexpr uint32_t MAX_RECORDS=64;
            std::array<ContentStorageRecord,MAX_RECORDS> existing{};
            s32 entries=0;
            const struct { u64 offset; u64 tid; } in={0,meta.appId};
            rr=serviceDispatchInOut(&srv,17,in,entries,
                .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_Out},
                .buffers={{existing.data(),sizeof(existing)}});
            if(R_SUCCEEDED(rr) && entries>0){
                entries=std::min<s32>(entries,MAX_RECORDS);
                records.insert(records.end(),existing.begin(),existing.begin()+entries);
            }
            serviceClose(&srv);
        }
        nsExit();
        if(R_FAILED(rr) && analysis.metaType!=NcmContentMetaType_Application)
            return failRollback(rcDiag(rr,ErrorCode::RegistrationError,"ListApplicationRecordContentMeta"));
    }
    bool haveKey=false;
    for(const auto& r:records) if(std::memcmp(&r.key,&key,sizeof(key))==0){haveKey=true;break;}
    if(!haveKey){ContentStorageRecord rec{};rec.key=key;rec.storage_id=NcmStorageId_SdCard;records.push_back(rec);}
    if(records.empty()){ContentStorageRecord rec{};rec.key=key;rec.storage_id=NcmStorageId_SdCard;records.push_back(rec);}
    saveJournal(nspPath,meta.appId,key,plan,metaSet,false,out.ticketImported,"application-record-pending");
    rc=pushApplicationRecord(meta.appId,records.data(),(uint32_t)records.size());if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"PushApplicationRecord"));
    bool recordPushed=true;saveJournal(nspPath,meta.appId,key,plan,metaSet,recordPushed,out.ticketImported,"application-record");out.applicationRecordPushed=true;

    saveJournal(nspPath,meta.appId,key,plan,metaSet,recordPushed,out.ticketImported,"complete");
    removeJournal();
    out.titleId=analysis.titleId;out.applicationId=meta.appId;out.diagnostic=makeDiagnostic(ErrorCode::None,"Transactional Application/Patch/DLC install completed");
    if(progress)progress(std::max<uint64_t>(total,1),std::max<uint64_t>(total,1),"Installation complete",(uint32_t)plan.size(),(uint32_t)plan.size());
    return out;
}


namespace {

constexpr uint32_t STREAM_MAX_PFS0_FILES = 0x10000;
constexpr uint32_t STREAM_MAX_STRING_TABLE = 32 * 1024 * 1024;
constexpr uint64_t STREAM_INSTALL_SPACE_RESERVE = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t STREAM_MAX_AUX_FILE = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t STREAM_MAX_AUX_TOTAL = 32ULL * 1024ULL * 1024ULL;

#pragma pack(push, 1)
struct StreamPfs0Header {
    char magic[4];
    uint32_t fileCount;
    uint32_t stringTableSize;
    uint32_t reserved;
};
struct StreamPfs0Entry {
    uint64_t offset;
    uint64_t size;
    uint32_t stringOffset;
    uint32_t reserved;
};
#pragma pack(pop)

bool streamCheckedAdd(uint64_t a, uint64_t b, uint64_t& out)
{
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}

bool streamEndsWithCi(const std::string& value, const std::string& suffix)
{
    if (value.size() < suffix.size()) return false;
    const size_t start = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[start + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
    }
    return true;
}

bool streamParseContentId(const std::string& name, NcmContentId& out)
{
    const size_t dot = name.find('.');
    if (dot != 32) return false;
    std::array<uint8_t, 16> bytes{};
    for (size_t i = 0; i < 16; ++i) {
        const int hi = nibble(name[i * 2]);
        const int lo = nibble(name[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    static_assert(sizeof(NcmContentId) == 16);
    std::memcpy(&out, bytes.data(), bytes.size());
    return true;
}

NspManifest parseStreamManifest(uint64_t packageSize, const StreamReadExactCallback& readExact,
                                uint64_t& consumed)
{
    NspManifest out;
    out.packageSize = packageSize;
    consumed = 0;

    StreamPfs0Header header{};
    if (!readExact(&header, sizeof(header))) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "MTP stream ended before PFS0 header");
        return out;
    }
    consumed += sizeof(header);
    if (std::memcmp(header.magic, "PFS0", 4) != 0) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "MTP object is not a PFS0 NSP");
        return out;
    }
    if (!header.fileCount || header.fileCount > STREAM_MAX_PFS0_FILES ||
        header.stringTableSize > STREAM_MAX_STRING_TABLE) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0,
            "PFS0 table limits invalid; files=" + std::to_string(header.fileCount) +
            ", strings=" + std::to_string(header.stringTableSize));
        return out;
    }

    const uint64_t entriesBytes64 = static_cast<uint64_t>(header.fileCount) * sizeof(StreamPfs0Entry);
    if (entriesBytes64 > static_cast<uint64_t>(SIZE_MAX)) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "PFS0 entry table is too large");
        return out;
    }
    std::vector<StreamPfs0Entry> raw(header.fileCount);
    if (!readExact(raw.data(), static_cast<size_t>(entriesBytes64))) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "MTP stream ended in PFS0 entry table");
        return out;
    }
    consumed += entriesBytes64;

    std::vector<char> strings(static_cast<size_t>(header.stringTableSize) + 1, '\0');
    if (header.stringTableSize && !readExact(strings.data(), header.stringTableSize)) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "MTP stream ended in PFS0 string table");
        return out;
    }
    consumed += header.stringTableSize;
    const uint64_t dataStart = consumed;

    out.entries.reserve(header.fileCount);
    uint64_t minimumPackageSize = dataStart;
    for (uint32_t i = 0; i < header.fileCount; ++i) {
        const auto& e = raw[i];
        if (e.stringOffset >= header.stringTableSize) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "PFS0 filename offset outside string table");
            return out;
        }
        const char* cName = strings.data() + e.stringOffset;
        const size_t remaining = header.stringTableSize - e.stringOffset;
        if (!std::memchr(cName, '\0', remaining)) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "PFS0 filename is not terminated");
            return out;
        }

        uint64_t absoluteOffset = 0, absoluteEnd = 0;
        if (!streamCheckedAdd(dataStart, e.offset, absoluteOffset) ||
            !streamCheckedAdd(absoluteOffset, e.size, absoluteEnd)) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "PFS0 entry integer overflow");
            return out;
        }
        if (packageSize && absoluteEnd > packageSize) {
            out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated,
                std::string("PFS0 entry outside MTP object: ") + cName);
            return out;
        }
        minimumPackageSize = std::max(minimumPackageSize, absoluteEnd);

        NspEntryInfo info;
        info.name = cName;
        info.offset = absoluteOffset;
        info.size = e.size;
        info.isCnmtNca = streamEndsWithCi(info.name, ".cnmt.nca");
        info.isNca = info.isCnmtNca || streamEndsWithCi(info.name, ".nca");
        info.isTicket = streamEndsWithCi(info.name, ".tik");
        info.isCert = streamEndsWithCi(info.name, ".cert");
        if (info.isNca) info.hasContentId = streamParseContentId(info.name, info.contentId);
        if (info.isNca) { ++out.ncaCount; out.totalNcaBytes += info.size; }
        if (info.isCnmtNca) ++out.cnmtNcaCount;
        if (info.isTicket) ++out.ticketCount;
        if (info.isCert) ++out.certCount;
        out.entries.push_back(std::move(info));
    }

    if (!out.packageSize) out.packageSize = minimumPackageSize;
    if (!out.ncaCount) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, "NSP contains no NCA entries");
        return out;
    }
    if (out.cnmtNcaCount != 1) {
        out.diagnostic = makeDiagnostic(ErrorCode::UnsupportedFormat,
            "Streaming installer requires exactly one CNMT NCA; count=" + std::to_string(out.cnmtNcaCount));
        return out;
    }
    if (out.ticketCount != out.certCount) {
        out.diagnostic = makeDiagnostic(ErrorCode::MissingContent,
            "Ticket/cert count mismatch; tik=" + std::to_string(out.ticketCount) +
            ", cert=" + std::to_string(out.certCount));
        return out;
    }
    for (const auto& e : out.entries) {
        if (e.isNca && !e.hasContentId) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidNca, "Invalid NCA ContentId filename: " + e.name);
            return out;
        }
    }
    out.diagnostic = makeDiagnostic(ErrorCode::None);
    return out;
}

uint32_t streamCurrentFirmwarePacked()
{
    SetSysFirmwareVersion fw{};
    if (R_FAILED(setsysGetFirmwareVersion(&fw))) return 0;
    unsigned major = 0, minor = 0, micro = 0;
    if (std::sscanf(fw.display_version, "%u.%u.%u", &major, &minor, &micro) < 2) return 0;
    major = std::min(major, 0x3Fu);
    minor = std::min(minor, 0x3Fu);
    micro = std::min(micro, 0x0Fu);
    return (major << 26) | (minor << 20) | (micro << 16);
}

uint32_t streamRequiredSystemVersion(uint8_t metaType, const std::vector<uint8_t>& ext)
{
    if (metaType == NcmContentMetaType_Application && ext.size() >= sizeof(NcmApplicationMetaExtendedHeader)) {
        NcmApplicationMetaExtendedHeader h{}; std::memcpy(&h, ext.data(), sizeof(h)); return h.required_system_version;
    }
    if (metaType == NcmContentMetaType_Patch && ext.size() >= sizeof(NcmPatchMetaExtendedHeader)) {
        NcmPatchMetaExtendedHeader h{}; std::memcpy(&h, ext.data(), sizeof(h)); return h.required_system_version;
    }
    return 0;
}

bool streamValidateCnmtReferences(const ParsedMeta& meta, const NspManifest& manifest, std::string& technical)
{
    for (const auto& info : meta.contentInfos) {
        const auto* e = findEntry(manifest, info.content_id);
        if (!e) {
            technical = "CNMT references ContentId missing from NSP: " + hex(&info.content_id, sizeof(info.content_id));
            return false;
        }
        uint64_t expected = 0;
        ncmContentInfoSizeToU64(&info, &expected);
        if (e->size != expected) {
            technical = "CNMT/NCA size mismatch for " + e->name +
                        "; cnmt=" + std::to_string(expected) + ", pfs0=" + std::to_string(e->size);
            return false;
        }
    }
    return true;
}

struct BufferedAuxFile {
    const NspEntryInfo* entry = nullptr;
    std::vector<uint8_t> bytes;
};

const BufferedAuxFile* findBufferedRights(const std::vector<BufferedAuxFile>& aux,
                                          const FsRightsId& rights, bool ticket)
{
    const std::string wanted = hex(&rights, sizeof(rights));
    for (const auto& f : aux) {
        if (!f.entry) continue;
        if ((ticket && !f.entry->isTicket) || (!ticket && !f.entry->isCert)) continue;
        if (f.entry->name.size() >= wanted.size() &&
            std::equal(wanted.begin(), wanted.end(), f.entry->name.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) return &f;
    }
    return nullptr;
}

Diagnostic validateStreamMetaAgainstInstalled(NcmContext& ctx, const ParsedMeta& meta,
                                              NcmContentMetaKey& key)
{
    if (meta.header.metaType != NcmContentMetaType_Application &&
        meta.header.metaType != NcmContentMetaType_Patch &&
        meta.header.metaType != NcmContentMetaType_AddOnContent) {
        return makeDiagnostic(ErrorCode::UnsupportedFormat,
            "Only Application, Patch and AddOnContent CNMT types are supported.");
    }

    key = {};
    key.id = meta.header.titleId;
    key.version = meta.header.titleVersion;
    key.type = meta.header.metaType;
    key.install_type = NcmContentInstallType_Full;
    std::memset(key.padding, 0, sizeof(key.padding));

    bool exact = false;
    Result rc = ncmContentMetaDatabaseHas(&ctx.db, &exact, &key);
    if (R_FAILED(rc)) return rcDiag(rc, ErrorCode::RegistrationError, "ContentMetaDatabaseHas");
    if (exact) return makeDiagnostic(ErrorCode::AlreadyInstalled, "This exact title/version is already installed.");

    if (meta.header.metaType == NcmContentMetaType_Patch ||
        meta.header.metaType == NcmContentMetaType_AddOnContent) {
        bool baseFound = false;
        NcmContentMetaKey baseKey{};
        rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&ctx.db, &baseKey, meta.appId);
        baseFound = R_SUCCEEDED(rc) && baseKey.type == NcmContentMetaType_Application;
        if (!baseFound) {
            NcmContentMetaDatabase userDb{};
            Result urc = ncmOpenContentMetaDatabase(&userDb, NcmStorageId_BuiltInUser);
            if (R_SUCCEEDED(urc)) {
                NcmContentMetaKey userKey{};
                urc = ncmContentMetaDatabaseGetLatestContentMetaKey(&userDb, &userKey, meta.appId);
                baseFound = R_SUCCEEDED(urc) && userKey.type == NcmContentMetaType_Application;
                ncmContentMetaDatabaseClose(&userDb);
            }
        }
        if (!baseFound) return makeDiagnostic(ErrorCode::MissingContent,
            "The base application is not installed. Install the base game before its update/DLC.");
    }

    bool matching = false;
    uint32_t latest = 0;
    auto inspect = [&](NcmContentMetaDatabase* db) {
        NcmContentMetaKey candidate{};
        Result r = ncmContentMetaDatabaseGetLatestContentMetaKey(db, &candidate, meta.header.titleId);
        if (R_SUCCEEDED(r) && candidate.type == meta.header.metaType) {
            matching = true;
            latest = std::max(latest, candidate.version);
        }
    };
    inspect(&ctx.db);
    NcmContentMetaDatabase userDb{};
    Result urc = ncmOpenContentMetaDatabase(&userDb, NcmStorageId_BuiltInUser);
    if (R_SUCCEEDED(urc)) { inspect(&userDb); ncmContentMetaDatabaseClose(&userDb); }

    if (matching && latest >= meta.header.titleVersion)
        return makeDiagnostic(ErrorCode::AlreadyInstalled, "The same or a newer version is already installed.");
    if (meta.header.metaType == NcmContentMetaType_Application && matching)
        return makeDiagnostic(ErrorCode::AlreadyInstalled,
            "Base application replacement is disabled; install newer content as an Update/Patch.");

    const uint32_t required = streamRequiredSystemVersion(meta.header.metaType, meta.extendedHeader);
    const uint32_t current = streamCurrentFirmwarePacked();
    if (required && current && required > current)
        return makeDiagnostic(ErrorCode::FirmwareTooOld,
            "required=" + formatPackedSystemVersion(required) +
            ", current=" + formatPackedSystemVersion(current));

    return makeDiagnostic(ErrorCode::None);
}

} // namespace

RealInstallResult installNspStreamToSdTransactional(
    const std::string& displayName,
    uint64_t packageSize,
    StreamReadExactCallback readExact,
    InstallProgressCallback progress)
{
    RealInstallResult out;
    if (!readExact) {
        out.diagnostic = makeDiagnostic(ErrorCode::InternalError, "Streaming installer received no read callback");
        return out;
    }

    std::string recovery;
    if (!recoverStaleRealInstall(&recovery)) {
        out.diagnostic = makeDiagnostic(ErrorCode::RegistrationError, "Recovery failed: " + recovery);
        return out;
    }
    if (std::filesystem::exists(JOURNAL_PATH)) {
        out.diagnostic = makeDiagnostic(ErrorCode::RegistrationError,
            "Install journal remained after recovery: " + recovery);
        return out;
    }

    if (progress) progress(0, std::max<uint64_t>(packageSize, 1), "Reading NSP manifest", 0, 0);
    uint64_t streamOffset = 0;
    NspManifest manifest = parseStreamManifest(packageSize, readExact, streamOffset);
    if (!manifest.ok()) { out.diagnostic = manifest.diagnostic; return out; }
    const auto* cnmt = findCnmt(manifest);
    if (!cnmt) { out.diagnostic = makeDiagnostic(ErrorCode::MissingContent, "CNMT NCA missing"); return out; }

    NcmContext ctx;
    out.diagnostic = ctx.open();
    if (!out.diagnostic.ok()) return out;

    std::vector<PlannedContent> plan;
    plan.reserve(manifest.ncaCount);
    uint64_t newContentBytes = 0;
    Result rc = 0;
    for (const auto& e : manifest.entries) {
        if (!e.isNca || !e.hasContentId) continue;
        bool exists = false;
        rc = ncmContentStorageHas(&ctx.storage, &exists, &e.contentId);
        if (R_FAILED(rc)) { out.diagnostic = rcDiag(rc, ErrorCode::RegistrationError, "ContentStorageHas"); return out; }
        PlannedContent p;
        p.entry = &e;
        p.info.content_id = e.contentId;
        p.info.content_type = e.isCnmtNca ? NcmContentType_Meta : NcmContentType_Data;
        ncmU64ToContentInfoSize(e.size, &p.info);
        p.preExisting = exists;
        plan.push_back(p);
        if (!exists) {
            if (newContentBytes > std::numeric_limits<uint64_t>::max() - e.size) {
                out.diagnostic = makeDiagnostic(ErrorCode::NotEnoughSpace, "Install size overflow"); return out;
            }
            newContentBytes += e.size;
        }
    }

    s64 freeRaw = -1;
    if (R_SUCCEEDED(nsGetFreeSpaceSize(NcmStorageId_SdCard, &freeRaw)) && freeRaw >= 0) {
        const uint64_t freeBytes = static_cast<uint64_t>(freeRaw);
        const uint64_t required = newContentBytes > std::numeric_limits<uint64_t>::max() - STREAM_INSTALL_SPACE_RESERVE
            ? std::numeric_limits<uint64_t>::max() : newContentBytes + STREAM_INSTALL_SPACE_RESERVE;
        if (freeBytes < required) {
            out.diagnostic = makeDiagnostic(ErrorCode::NotEnoughSpace,
                "stream_install_required=" + std::to_string(required) + ", free=" + std::to_string(freeBytes));
            return out;
        }
    }

    NcmContentMetaKey provisionalKey{};
    saveJournal(displayName, 0, provisionalKey, plan, false, false, false, "stream-prepared");
    auto failRollback = [&](Diagnostic d) {
        out.diagnostic = std::move(d);
        std::string detail;
        recoverStaleRealInstall(&detail);
        return out;
    };

    auto findPlan = [&](const NcmContentId& id) -> PlannedContent* {
        for (auto& p : plan)
            if (std::memcmp(&p.info.content_id, &id, sizeof(id)) == 0) return &p;
        return nullptr;
    };

    std::vector<const NspEntryInfo*> physical;
    physical.reserve(manifest.entries.size());
    for (const auto& e : manifest.entries) physical.push_back(&e);
    std::stable_sort(physical.begin(), physical.end(), [](const NspEntryInfo* a, const NspEntryInfo* b) {
        return a->offset < b->offset;
    });

    std::vector<uint8_t> io(COPY_BUFFER_SIZE);
    std::vector<BufferedAuxFile> aux;
    uint64_t auxTotal = 0;
    uint64_t installedNow = 0;
    uint32_t contentOrdinal = 0;

    auto consumeBytes = [&](uint64_t bytes, const std::function<Diagnostic(const uint8_t*, size_t, uint64_t)>& sink,
                            const std::string& stage, uint32_t index) -> Diagnostic {
        uint64_t local = 0;
        while (local < bytes) {
            if (ProgressEvent::instance().getInterupt())
                return makeDiagnostic(ErrorCode::Cancelled, "User cancelled streaming install");
            const size_t want = static_cast<size_t>(std::min<uint64_t>(io.size(), bytes - local));
            if (!readExact(io.data(), want))
                return makeDiagnostic(ErrorCode::PackageTruncated,
                    "MTP stream ended while reading " + stage + "; offset=" + std::to_string(streamOffset));
            if (sink) {
                Diagnostic d = sink(io.data(), want, local);
                if (!d.ok()) return d;
            }
            local += want;
            streamOffset += want;
            if (progress) progress(streamOffset, std::max<uint64_t>(manifest.packageSize, 1), stage,
                                   index, static_cast<uint32_t>(plan.size()));
        }
        return makeDiagnostic(ErrorCode::None);
    };

    for (const NspEntryInfo* entry : physical) {
        if (entry->offset < streamOffset)
            return failRollback(makeDiagnostic(ErrorCode::InvalidPfs0, "Overlapping/out-of-order PFS0 entries"));
        if (entry->offset > streamOffset) {
            Diagnostic d = consumeBytes(entry->offset - streamOffset, {}, "Skipping NSP padding", contentOrdinal);
            if (!d.ok()) return failRollback(d);
        }

        if (entry->isNca) {
            ++contentOrdinal;
            PlannedContent* p = findPlan(entry->contentId);
            if (!p) return failRollback(makeDiagnostic(ErrorCode::InternalError, "NCA missing from stream content plan"));
            if (p->preExisting) {
                Diagnostic d = consumeBytes(entry->size, {}, entry->name + " (already present)", contentOrdinal);
                if (!d.ok()) return failRollback(d);
                continue;
            }

            rc = ncmContentStorageGeneratePlaceHolderId(&ctx.storage, &p->placeholder);
            if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::PlaceholderError, "GeneratePlaceHolderId"));
            rc = ncmContentStorageCreatePlaceHolder(&ctx.storage, &entry->contentId, &p->placeholder, entry->size);
            if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::PlaceholderError, "CreatePlaceHolder"));
            p->placeholderCreated = true;
            saveJournal(displayName, 0, provisionalKey, plan, false, false, false, "stream-writing-content");

            Diagnostic d = consumeBytes(entry->size,
                [&](const uint8_t* data, size_t size, uint64_t local) -> Diagnostic {
                    Result wr = ncmContentStorageWritePlaceHolder(&ctx.storage, &p->placeholder, local, data, size);
                    if (R_FAILED(wr)) return rcDiag(wr, ErrorCode::WriteError, "WritePlaceHolder(stream)");
                    installedNow += size;
                    return makeDiagnostic(ErrorCode::None);
                }, entry->name, contentOrdinal);
            if (!d.ok()) return failRollback(d);

            rc = ncmContentStorageRegister(&ctx.storage, &entry->contentId, &p->placeholder);
            if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::RegistrationError, "Register content(stream)"));
            p->placeholderCreated = false;
            p->registered = true;
            out.installedBytes = installedNow;
            ++out.installedContents;
            saveJournal(displayName, 0, provisionalKey, plan, false, false, false, "stream-registered-content");
        } else if (entry->isTicket || entry->isCert) {
            if (entry->size > STREAM_MAX_AUX_FILE || auxTotal > STREAM_MAX_AUX_TOTAL - entry->size)
                return failRollback(makeDiagnostic(ErrorCode::UnsupportedFormat, "Ticket/cert payload is unexpectedly large"));
            BufferedAuxFile f;
            f.entry = entry;
            f.bytes.resize(static_cast<size_t>(entry->size));
            size_t copied = 0;
            Diagnostic d = consumeBytes(entry->size,
                [&](const uint8_t* data, size_t size, uint64_t) -> Diagnostic {
                    std::memcpy(f.bytes.data() + copied, data, size);
                    copied += size;
                    return makeDiagnostic(ErrorCode::None);
                }, entry->name, contentOrdinal);
            if (!d.ok()) return failRollback(d);
            auxTotal += entry->size;
            aux.push_back(std::move(f));
        } else {
            Diagnostic d = consumeBytes(entry->size, {}, "Skipping " + entry->name, contentOrdinal);
            if (!d.ok()) return failRollback(d);
        }
    }

    // Consume package padding/trailer so the consumer and host remain byte-aligned.
    if (manifest.packageSize > streamOffset) {
        Diagnostic d = consumeBytes(manifest.packageSize - streamOffset, {}, "Finishing NSP stream", contentOrdinal);
        if (!d.ok()) return failRollback(d);
    }

    ParsedMeta meta = parseInstalledMeta(ctx, manifest, *cnmt);
    if (!meta.ok()) return failRollback(meta.diagnostic);
    std::string refError;
    if (!streamValidateCnmtReferences(meta, manifest, refError))
        return failRollback(makeDiagnostic(ErrorCode::MissingContent, refError));

    NcmContentMetaKey key{};
    Diagnostic state = validateStreamMetaAgainstInstalled(ctx, meta, key);
    if (!state.ok()) return failRollback(state);

    // Replace the provisional zero-key journal with the real title identity before
    // any metadata mutation. Content rollback data remains unchanged.
    saveJournal(displayName, meta.appId, key, plan, false, false, false, "stream-content-complete");
    auto blob = buildMetaDbBlob(meta, *cnmt);
    saveJournal(displayName, meta.appId, key, plan, true, false, false, "metadata-pending");
    rc = ncmContentMetaDatabaseSet(&ctx.db, &key, blob.data(), blob.size());
    if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::RegistrationError, "ContentMetaDatabaseSet(stream)"));
    rc = ncmContentMetaDatabaseCommit(&ctx.db);
    if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::RegistrationError, "ContentMetaDatabaseCommit(stream)"));
    bool metaSet = true;
    saveJournal(displayName, meta.appId, key, plan, metaSet, false, false, "metadata-committed");

    NcmRightsId rights{};
    bool rightsFound = false;
    for (const auto& p : plan) {
        if (p.entry && p.entry->isCnmtNca) continue;
        Result rr = ncmContentStorageGetRightsIdFromContentId(&ctx.storage, &rights, &p.info.content_id,
                                                              FsContentAttributes_None);
        if (R_SUCCEEDED(rr)) {
            std::array<uint8_t, sizeof(FsRightsId)> zero{};
            if (std::memcmp(&rights.rights_id, zero.data(), zero.size()) != 0) { rightsFound = true; break; }
        }
    }
    if (rightsFound) {
        const auto* tik = findBufferedRights(aux, rights.rights_id, true);
        const auto* cert = findBufferedRights(aux, rights.rights_id, false);
        if (!tik || !cert)
            return failRollback(makeDiagnostic(ErrorCode::MissingContent,
                "Rights-managed title requires matching .tik and .cert"));
        rc = importTicket(tik->bytes.data(), tik->bytes.size(), cert->bytes.data(), cert->bytes.size());
        if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::AuthorizationRejected, "es ImportTicket(stream)"));
        out.ticketImported = true;
        saveJournal(displayName, meta.appId, key, plan, metaSet, false, true, "ticket-imported");
    }

    std::vector<ContentStorageRecord> records;
    {
        Service srv{};
        Result rr = nsInitialize();
        if (R_SUCCEEDED(rr)) rr = nsGetApplicationManagerInterface(&srv);
        if (R_SUCCEEDED(rr)) {
            constexpr uint32_t MAX_RECORDS = 64;
            std::array<ContentStorageRecord, MAX_RECORDS> existing{};
            s32 entries = 0;
            const struct { u64 offset; u64 tid; } in = {0, meta.appId};
            rr = serviceDispatchInOut(&srv, 17, in, entries,
                .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                .buffers = {{existing.data(), sizeof(existing)}});
            if (R_SUCCEEDED(rr) && entries > 0) {
                entries = std::min<s32>(entries, MAX_RECORDS);
                records.insert(records.end(), existing.begin(), existing.begin() + entries);
            }
            serviceClose(&srv);
        }
        nsExit();
        if (R_FAILED(rr) && meta.header.metaType != NcmContentMetaType_Application)
            return failRollback(rcDiag(rr, ErrorCode::RegistrationError, "ListApplicationRecordContentMeta(stream)"));
    }
    bool haveKey = false;
    for (const auto& r : records) if (std::memcmp(&r.key, &key, sizeof(key)) == 0) { haveKey = true; break; }
    if (!haveKey) { ContentStorageRecord rec{}; rec.key = key; rec.storage_id = NcmStorageId_SdCard; records.push_back(rec); }
    if (records.empty()) { ContentStorageRecord rec{}; rec.key = key; rec.storage_id = NcmStorageId_SdCard; records.push_back(rec); }

    saveJournal(displayName, meta.appId, key, plan, metaSet, false, out.ticketImported, "application-record-pending");
    rc = pushApplicationRecord(meta.appId, records.data(), static_cast<uint32_t>(records.size()));
    if (R_FAILED(rc)) return failRollback(rcDiag(rc, ErrorCode::RegistrationError, "PushApplicationRecord(stream)"));
    out.applicationRecordPushed = true;
    saveJournal(displayName, meta.appId, key, plan, metaSet, true, out.ticketImported, "application-record");
    saveJournal(displayName, meta.appId, key, plan, metaSet, true, out.ticketImported, "complete");
    removeJournal();

    out.titleId = meta.header.titleId;
    out.applicationId = meta.appId;
    out.diagnostic = makeDiagnostic(ErrorCode::None, "Direct MTP streaming install completed");
    if (progress) progress(std::max<uint64_t>(manifest.packageSize, 1), std::max<uint64_t>(manifest.packageSize, 1),
                           "Installation complete", static_cast<uint32_t>(plan.size()), static_cast<uint32_t>(plan.size()));
    return out;
}

} // namespace installer
