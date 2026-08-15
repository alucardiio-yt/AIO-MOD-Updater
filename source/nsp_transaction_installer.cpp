#include "nsp_transaction_installer.hpp"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

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
    std::vector<NcmContentInfo> contentInfos; 
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

void removeJournal()
{
    std::error_code ec; std::filesystem::remove(JOURNAL_PATH, ec); std::filesystem::remove(std::string(JOURNAL_PATH)+".tmp", ec);
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
    std::vector<uint8_t> out(static_cast<size_t>(entry.size));
    std::ifstream f(path, std::ios::binary);
    if (!f || entry.size > static_cast<uint64_t>(SIZE_MAX)) return {};
    f.seekg(static_cast<std::streamoff>(entry.offset));
    if (!f) return {};
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (static_cast<size_t>(f.gcount()) != out.size()) return {};
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

} 

bool recoverStaleRealInstall(std::string* detail)
{
    if (!std::filesystem::exists(JOURNAL_PATH)) return true;
    try {
        std::ifstream in(JOURNAL_PATH); nlohmann::json j; in>>j;
        if(j.value("owner","")!="aio-switch-updater-mod"||j.value("kind","")!="real-install") return true;

        const std::string stage=j.value("stage","");
        if(stage=="application-record" || stage=="complete") { removeJournal(); if(detail)*detail="Completed install journal cleared."; return true; }
        NcmContext ctx; Diagnostic d=ctx.open(); if(!d.ok()){if(detail)*detail=d.technical;return false;}
        if(j.value("meta_set",false)) {
            NcmContentMetaKey key{}; auto k=j["meta_key"]; key.id=k.value("id",0ULL);key.version=k.value("version",0U);key.type=k.value("type",0);key.install_type=k.value("install_type",0);
            ncmContentMetaDatabaseRemove(&ctx.db,&key); ncmContentMetaDatabaseCommit(&ctx.db);
        }
        if(j.contains("contents")) for(auto it=j["contents"].rbegin();it!=j["contents"].rend();++it){
            NcmContentId cid{}; NcmPlaceHolderId ph{}; fromHex((*it).value("content_id",""),&cid,sizeof(cid)); fromHex((*it).value("placeholder_id",""),&ph,sizeof(ph));
            if((*it).value("registered",false) && !(*it).value("pre_existing",false)){bool exists=false;if(R_SUCCEEDED(ncmContentStorageHas(&ctx.storage,&exists,&cid))&&exists)ncmContentStorageDelete(&ctx.storage,&cid);}
            else if((*it).value("placeholder_created",false)){bool exists=false;if(R_SUCCEEDED(ncmContentStorageHasPlaceHolder(&ctx.storage,&exists,&ph))&&exists)ncmContentStorageDeletePlaceHolder(&ctx.storage,&ph);}
        }
        removeJournal(); if(detail)*detail="Interrupted AIO install rolled back."; return true;
    } catch(const std::exception& e){if(detail)*detail=e.what();return false;}
}

RealInstallResult installNspToSdTransactional(const std::string& nspPath, InstallProgressCallback progress)
{
    RealInstallResult out;
    std::string recovery; if(!recoverStaleRealInstall(&recovery)){out.diagnostic=makeDiagnostic(ErrorCode::RegistrationError,"Recovery failed: "+recovery);return out;}
    if(std::filesystem::exists(JOURNAL_PATH)){out.diagnostic=makeDiagnostic(ErrorCode::InternalError,"Another AIO transaction owns current_install.json");return out;}
    const auto analysis=analyzeCnmtRequirements(nspPath); if(!analysis.ok()){out.diagnostic=analysis.diagnostic;return out;}
    const auto manifest=inspectNspManifest(nspPath); if(!manifest.ok()){out.diagnostic=manifest.diagnostic;return out;}
    if (analysis.metaType != NcmContentMetaType_Application &&
        analysis.metaType != NcmContentMetaType_Patch &&
        analysis.metaType != NcmContentMetaType_AddOnContent) {
        out.diagnostic = makeDiagnostic(ErrorCode::UnsupportedFormat,
            "Only Application, Patch and AddOnContent CNMT types are supported.");
        return out;
    }
    if(manifest.cnmtNcaCount!=1){out.diagnostic=makeDiagnostic(ErrorCode::UnsupportedFormat,"Exactly one CNMT NCA is required (multi-title bundles are not supported).");return out;}
    const auto* cnmt=findCnmt(manifest); if(!cnmt){out.diagnostic=makeDiagnostic(ErrorCode::MissingContent,"CNMT missing");return out;}

    NcmContext ctx; out.diagnostic=ctx.open(); if(!out.diagnostic.ok())return out;
    NcmContentMetaKey key{}; key.id=analysis.titleId;key.version=analysis.titleVersion;key.type=analysis.metaType;key.install_type=NcmContentInstallType_Full;std::memset(key.padding,0,sizeof(key.padding));
    bool metaExists=false; Result rc=ncmContentMetaDatabaseHas(&ctx.db,&metaExists,&key); if(R_FAILED(rc)){out.diagnostic=rcDiag(rc,ErrorCode::RegistrationError,"ContentMetaDatabaseHas");return out;}
    if(metaExists){out.diagnostic=makeDiagnostic(ErrorCode::AlreadyInstalled,"This exact title/version is already installed.");return out;}

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

    NcmContentMetaKey latestKey{};
    rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&ctx.db, &latestKey, analysis.titleId);
    if (R_SUCCEEDED(rc) && latestKey.type == analysis.metaType && latestKey.version >= analysis.titleVersion) {
        out.diagnostic = makeDiagnostic(ErrorCode::AlreadyInstalled,
            "The same or a newer version of this content is already installed.");
        return out;
    }
    if (analysis.metaType == NcmContentMetaType_Application && R_SUCCEEDED(rc)) {
        out.diagnostic = makeDiagnostic(ErrorCode::AlreadyInstalled,
            "Base application replacement is intentionally disabled. Install updates as Patch NSPs instead.");
        return out;
    }

    
    std::vector<PlannedContent> plan; plan.reserve(manifest.ncaCount);
    uint64_t total = 0;
    for(const auto& e:manifest.entries) if(e.isNca&&e.hasContentId){
        bool exists=false;rc=ncmContentStorageHas(&ctx.storage,&exists,&e.contentId);if(R_FAILED(rc)){out.diagnostic=rcDiag(rc,ErrorCode::RegistrationError,"ContentStorageHas");return out;}
        PlannedContent p; p.entry=&e;p.info.content_id=e.contentId;p.info.content_type=e.isCnmtNca?NcmContentType_Meta:NcmContentType_Data;ncmU64ToContentInfoSize(e.size,&p.info);p.preExisting=exists;plan.push_back(p);
        if(!exists) total += e.size;
    }
    saveJournal(nspPath,analysis.applicationId,key,plan,false,false,false,"prepared");
    uint64_t overall=0; std::ifstream nsp(nspPath,std::ios::binary); if(!nsp){removeJournal();out.diagnostic=makeDiagnostic(ErrorCode::InternalError,"Cannot open staged NSP");return out;}
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
        nsp.clear();nsp.seekg((std::streamoff)p.entry->offset);if(!nsp)return failRollback(makeDiagnostic(ErrorCode::PackageTruncated,"Seek failed: "+p.entry->name));
        uint64_t local=0;while(local<p.entry->size){size_t want=(size_t)std::min<uint64_t>(buf.size(),p.entry->size-local);nsp.read((char*)buf.data(),(std::streamsize)want);if((size_t)nsp.gcount()!=want)return failRollback(makeDiagnostic(ErrorCode::PackageTruncated,"NSP ended while reading "+p.entry->name));
            rc=ncmContentStorageWritePlaceHolder(&ctx.storage,&p.placeholder,local,buf.data(),want);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::WriteError,"WritePlaceHolder"));local+=want;overall+=want;
            if(progress)progress(overall,std::max<uint64_t>(total,1),p.entry->name,(uint32_t)i+1,(uint32_t)plan.size());
            if(ProgressEvent::instance().getInterupt())return failRollback(makeDiagnostic(ErrorCode::Cancelled,"User cancelled install"));
        }
        rc=ncmContentStorageRegister(&ctx.storage,&p.entry->contentId,&p.placeholder);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"Register content"));
        p.placeholderCreated=false;p.registered=true;saveJournal(nspPath,analysis.applicationId,key,plan,false,false,false,"registered-content");out.installedBytes=overall;out.installedContents=(uint32_t)i+1;
    }

    ParsedMeta meta=parseInstalledMeta(ctx,manifest,*cnmt);if(!meta.ok())return failRollback(meta.diagnostic);
    auto blob=buildMetaDbBlob(meta,*cnmt);

    saveJournal(nspPath,meta.appId,key,plan,true,false,false,"metadata-pending");
    rc=ncmContentMetaDatabaseSet(&ctx.db,&key,blob.data(),blob.size());if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"ContentMetaDatabaseSet"));
    rc=ncmContentMetaDatabaseCommit(&ctx.db);if(R_FAILED(rc))return failRollback(rcDiag(rc,ErrorCode::RegistrationError,"ContentMetaDatabaseCommit"));
    bool metaSet=true;saveJournal(nspPath,meta.appId,key,plan,metaSet,false,false,"metadata-committed");

    
    
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

} 
