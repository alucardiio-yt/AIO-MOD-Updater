/* Forwarder implementation derived in part from Sphaira (GPL-3.0). See THIRD_PARTY.md. */

#include "forwarder_installer.hpp"
#include "forwarder_nca.hpp"

#include <switch.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aio::forwarder {
namespace {

constexpr u32 IVFC_MAX_LEVEL = 6;
constexpr u32 IVFC_HASH_BLOCK_SIZE = 0x4000;
constexpr u32 PFS0_EXEFS_HASH_BLOCK_SIZE = 0x10000;
constexpr u32 PFS0_META_HASH_BLOCK_SIZE = 0x1000;
constexpr u32 PFS0_PADDING_SIZE = 0x200;
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;
constexpr u32 ROMFS_FILEPARTITION_OFS = 0x200;
constexpr Result ResultBadArgs = MAKERESULT(345, 1);
constexpr Result ResultLoaderMissing = MAKERESULT(345, 2);
constexpr Result ResultIconMissing = MAKERESULT(345, 3);

constexpr u8 HEADER_KEK_SRC[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D,
    0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A
};
constexpr u8 HEADER_KEY_SRC[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26,
    0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D,
    0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2
};

struct Keys {
    u8 header_key[0x20]{};
};

struct BufHelper {
    std::vector<u8> buf;
    u64 offset{};

    void write(const void* data, u64 size) {
        if (offset + size > buf.size())
            buf.resize(offset + size);
        if (size)
            std::memcpy(buf.data() + offset, data, size);
        offset += size;
    }

    void write(std::span<const u8> data) {
        write(data.data(), data.size());
    }

    void seek(u64 where) { offset = where; }
    u64 tell() const { return offset; }
};

struct NcaEntry {
    NcaEntry(const BufHelper& source, NcmContentType contentType)
        : data(source.buf), type(contentType) {
        sha256CalculateHash(hash, data.data(), data.size());
    }

    std::vector<u8> data;
    NcmContentType type;
    u8 hash[SHA256_HASH_SIZE]{};
};

struct CnmtHeader {
    u64 title_id;
    u32 title_version;
    u8 meta_type;
    u8 _0xD;
    NcmContentMetaHeader meta_header;
    u8 install_type;
    u8 _0x17;
    u32 required_sys_version;
    u8 _0x1C[0x4];
};
static_assert(sizeof(CnmtHeader) == 0x20);

struct ContentStorageRecord {
    NcmContentMetaKey key;
    u8 storage_id;
    u8 padding[0x7];
};

struct NcmContentMetaData {
    NcmContentMetaHeader header;
    NcmApplicationMetaExtendedHeader extended;
    NcmContentInfo infos[3];
};

struct NcaMetaEntry {
    NcaMetaEntry(const BufHelper& source, NcmContentType contentType)
        : nca_entry(source, contentType) {}

    NcaEntry nca_entry;
    NcmContentMetaHeader content_meta_header{};
    NcmContentMetaKey content_meta_key{};
    ContentStorageRecord content_storage_record{};
    NcmContentMetaData content_meta_data{};
};

struct Pfs0Header {
    u32 magic;
    u32 total_files;
    u32 string_table_size;
    u32 padding;
};

struct Pfs0FileTable {
    u64 data_offset;
    u64 data_size;
    u32 name_offset;
    u32 padding;
};

struct FileEntry {
    std::string name;
    std::vector<u8> data;
};
using FileEntries = std::vector<FileEntry>;

struct NpdmMeta {
    u32 magic;
    u32 signature_key_generation;
    u32 _0x8;
    u8 flags;
    u8 _0xD;
    u8 main_thread_priority;
    u8 main_thread_core_num;
    u32 _0x10;
    u32 sys_resource_size;
    u32 version;
    u32 main_thread_stack_size;
    char title_name[0x10];
    char product_code[0x10];
    u8 _0x40[0x30];
    u32 aci0_offset;
    u32 aci0_size;
    u32 acid_offset;
    u32 acid_size;
};

struct NpdmAcid {
    u8 rsa_sig[0x100];
    u8 rsa_pub[0x100];
    u32 magic;
    u32 size;
    u8 version;
    u8 _0x209[0x1];
    u8 _0x20A[0x2];
    u32 flags;
    u64 program_id_min;
    u64 program_id_max;
    u32 fac_offset;
    u32 fac_size;
    u32 sac_offset;
    u32 sac_size;
    u32 kac_offset;
    u32 kac_size;
    u8 _0x238[0x8];
};

struct NpdmAci0 {
    u32 magic;
    u8 _0x4[0xC];
    u64 program_id;
    u8 _0x18[0x8];
    u32 fac_offset;
    u32 fac_size;
    u32 sac_offset;
    u32 sac_size;
    u32 kac_offset;
    u32 kac_size;
    u8 _0x38[0x8];
};

struct NpdmPatch {
    char title_name[0x10]{"Application"};
    char product_code[0x10]{};
    u64 tid{};
};

struct NacpPatch {
    std::string name;
    std::string author;
    u64 tid{};
};

struct RomfsDirCtx {
    u32 entry_offset;
    RomfsDirCtx* parent;
    RomfsDirCtx* child;
    RomfsDirCtx* sibling;
    struct RomfsFileCtx* file;
    RomfsDirCtx* next;
};

struct RomfsFileCtx {
    u32 entry_offset;
    u64 offset;
    u64 size;
    RomfsDirCtx* parent;
    RomfsFileCtx* sibling;
    RomfsFileCtx* next;
};

struct RomfsCtx {
    RomfsFileCtx* files;
    u64 num_dirs;
    u64 num_files;
    u64 dir_table_size;
    u64 file_table_size;
    u64 dir_hash_table_size;
    u64 file_hash_table_size;
    u64 file_partition_size;
};

Service g_nsAppSrv{};
bool g_nsInitialized = false;

void report(const ProgressCallback& cb, int current, int total, const std::string& text) {
    if (cb)
        cb(current, total, text);
}

std::vector<u8> readBinary(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize size = file.tellg();
    if (size <= 0)
        return {};
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size))
        return {};
    return data;
}

Result deriveHeaderKey(Keys& out) {
    u8 header_kek[0x10]{};
    Result rc = splCryptoGenerateAesKek(HEADER_KEK_SRC, 0, 0, header_kek);
    if (R_FAILED(rc)) return rc;
    rc = splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC, out.header_key);
    if (R_FAILED(rc)) return rc;
    return splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC + 0x10, out.header_key + 0x10);
}

Result nsExInitialize() {
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return rc;

    if (hosversionAtLeast(3, 0, 0)) {
        rc = nsGetApplicationManagerInterface(&g_nsAppSrv);
        if (R_FAILED(rc)) {
            nsExit();
            return rc;
        }
    } else {
        g_nsAppSrv = *nsGetServiceSession_ApplicationManagerInterface();
    }
    g_nsInitialized = true;
    return 0;
}

void nsExExit() {
    if (!g_nsInitialized)
        return;
    serviceClose(&g_nsAppSrv);
    nsExit();
    g_nsInitialized = false;
}

Result pushApplicationRecord(u64 tid, const ContentStorageRecord* records, u32 count) {
    const struct {
        u8 last_modified_event;
        u8 padding[0x7];
        u64 tid;
    } in = {3, {0}, tid};

    return serviceDispatchIn(&g_nsAppSrv, 16, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{records, sizeof(*records) * count}});
}

Result invalidateApplicationControlCache(u64 tid) {
    return serviceDispatchIn(&g_nsAppSrv, 404, tid);
}

u64 writePadding(BufHelper& buf, u64 off, u64 block) {
    const u64 size = block - (off % block);
    if (size) {
        std::vector<u8> padding(size);
        buf.write(padding.data(), padding.size());
    }
    return size;
}

u32 align32(u32 offset, u32 alignment) {
    const u32 mask = ~(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}

u64 align64(u64 offset, u64 alignment) {
    const u64 mask = ~(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}

romfs_dir* romfsGetDirEntry(romfs_dir* directories, u32 offset) {
    return reinterpret_cast<romfs_dir*>(reinterpret_cast<u8*>(directories) + offset);
}

romfs_file* romfsGetFileEntry(romfs_file* files, u32 offset) {
    return reinterpret_cast<romfs_file*>(reinterpret_cast<u8*>(files) + offset);
}

u32 calcPathHash(u32 parent, const u8* path, u32 start, u32 path_len) {
    u32 hash = parent ^ 123456789;
    for (u32 i = 0; i < path_len; i++) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= path[start + i];
    }
    return hash;
}

u32 romfsGetHashTableCount(u32 num_entries) {
    if (num_entries < 3)
        return 3;
    if (num_entries < 19)
        return num_entries | 1;

    u32 count = num_entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 ||
           count % 11 == 0 || count % 13 == 0 || count % 17 == 0) {
        count++;
    }
    return count;
}

void romfsVisitDir(const FileEntries& entries, RomfsDirCtx* parent, RomfsCtx* ctx) {
    RomfsFileCtx* child_file_tree = nullptr;
    RomfsFileCtx* cur_file = nullptr;

    for (const auto& entry : entries) {
        (void)entry;
        cur_file = static_cast<RomfsFileCtx*>(std::calloc(1, sizeof(RomfsFileCtx)));
        ctx->num_files++;
        cur_file->parent = parent;
        cur_file->size = entry.data.size();
        ctx->file_table_size += sizeof(romfs_file) + align32(entry.name.length() - 1, 4);

        if (!child_file_tree) {
            cur_file->sibling = child_file_tree;
            child_file_tree = cur_file;
        } else {
            RomfsFileCtx* prev = child_file_tree;
            RomfsFileCtx* child = child_file_tree->sibling;
            prev->sibling = cur_file;
            cur_file->sibling = child;
        }

        if (!ctx->files) {
            cur_file->next = ctx->files;
            ctx->files = cur_file;
        } else {
            RomfsFileCtx* prev = ctx->files;
            RomfsFileCtx* child = ctx->files->next;
            prev->next = cur_file;
            cur_file->next = child;
        }
        cur_file = nullptr;
    }

    parent->child = nullptr;
    parent->file = child_file_tree;
}

void buildRomfsIntoFile(const FileEntries& entries, BufHelper& buf) {
    auto* root_ctx = static_cast<RomfsDirCtx*>(std::calloc(1, sizeof(RomfsDirCtx)));
    root_ctx->parent = root_ctx;

    RomfsCtx ctx{};
    ctx.dir_table_size = sizeof(romfs_dir);
    ctx.num_dirs = 1;
    romfsVisitDir(entries, root_ctx, &ctx);

    const u32 dir_hash_count = romfsGetHashTableCount(ctx.num_dirs);
    const u32 file_hash_count = romfsGetHashTableCount(ctx.num_files);
    ctx.dir_hash_table_size = 4 * dir_hash_count;
    ctx.file_hash_table_size = 4 * file_hash_count;

    romfs_header header{};
    std::vector<u32> dir_hash_table(dir_hash_count, ROMFS_ENTRY_EMPTY);
    std::vector<u32> file_hash_table(file_hash_count, ROMFS_ENTRY_EMPTY);
    auto* dir_table = static_cast<romfs_dir*>(std::calloc(1, ctx.dir_table_size));
    auto* file_table = static_cast<romfs_file*>(std::calloc(1, ctx.file_table_size));

    RomfsFileCtx* cur_file = ctx.files;
    u32 entry_offset = 0;
    for (const auto& entry : entries) {
        ctx.file_partition_size = align64(ctx.file_partition_size, 0x10);
        cur_file->offset = ctx.file_partition_size;
        ctx.file_partition_size += cur_file->size;
        cur_file->entry_offset = entry_offset;
        entry_offset += sizeof(romfs_file) + align32(entry.name.length() - 1, 4);
        cur_file = cur_file->next;
    }

    root_ctx->entry_offset = 0;
    cur_file = ctx.files;
    for (const auto& entry : entries) {
        auto* cur_entry = romfsGetFileEntry(file_table, cur_file->entry_offset);
        cur_entry->parent = cur_file->parent->entry_offset;
        cur_entry->sibling = cur_file->sibling ? cur_file->sibling->entry_offset : ROMFS_ENTRY_EMPTY;
        cur_entry->dataOff = cur_file->offset;
        cur_entry->dataSize = cur_file->size;
        const u32 name_size = entry.name.length() - 1;
        const u32 hash = calcPathHash(cur_file->parent->entry_offset,
                                      reinterpret_cast<const u8*>(entry.name.c_str()), 1, name_size);
        cur_entry->nextHash = file_hash_table[hash % file_hash_count];
        file_hash_table[hash % file_hash_count] = cur_file->entry_offset;
        cur_entry->nameLen = name_size;
        std::memcpy(cur_entry->name, entry.name.c_str() + 1, name_size);
        cur_file = cur_file->next;
    }

    auto* cur_dir = root_ctx;
    while (cur_dir) {
        auto* cur_entry = romfsGetDirEntry(dir_table, cur_dir->entry_offset);
        cur_entry->parent = cur_dir->parent->entry_offset;
        cur_entry->sibling = cur_dir->sibling ? cur_dir->sibling->entry_offset : ROMFS_ENTRY_EMPTY;
        cur_entry->childDir = cur_dir->child ? cur_dir->child->entry_offset : ROMFS_ENTRY_EMPTY;
        cur_entry->childFile = cur_dir->file ? cur_dir->file->entry_offset : ROMFS_ENTRY_EMPTY;
        const u32 hash = calcPathHash(0, nullptr, 0, 0);
        cur_entry->nextHash = dir_hash_table[hash % dir_hash_count];
        dir_hash_table[hash % dir_hash_count] = cur_dir->entry_offset;
        cur_entry->nameLen = 0;

        auto* old = cur_dir;
        cur_dir = cur_dir->next;
        std::free(old);
    }

    header.headerSize = sizeof(header);
    header.fileHashTableSize = ctx.file_hash_table_size;
    header.fileTableSize = ctx.file_table_size;
    header.dirHashTableSize = ctx.dir_hash_table_size;
    header.dirTableSize = ctx.dir_table_size;
    header.fileDataOff = ROMFS_FILEPARTITION_OFS;
    header.dirHashTableOff = align64(ctx.file_partition_size + ROMFS_FILEPARTITION_OFS, 4);
    header.dirTableOff = header.dirHashTableOff + ctx.dir_hash_table_size;
    header.fileHashTableOff = header.dirTableOff + ctx.dir_table_size;
    header.fileTableOff = header.fileHashTableOff + ctx.file_hash_table_size;

    buf.write(&header, sizeof(header));

    cur_file = ctx.files;
    for (const auto& entry : entries) {
        buf.seek(cur_file->offset + ROMFS_FILEPARTITION_OFS);
        buf.write(entry.data.data(), entry.data.size());
        auto* old = cur_file;
        cur_file = cur_file->next;
        std::free(old);
    }

    buf.seek(header.dirHashTableOff);
    buf.write(dir_hash_table.data(), ctx.dir_hash_table_size);
    buf.write(dir_table, ctx.dir_table_size);
    std::free(dir_table);
    buf.write(file_hash_table.data(), ctx.file_hash_table_size);
    buf.write(file_table, ctx.file_table_size);
    std::free(file_table);
}

std::vector<u8> romfsBuild(const FileEntries& entries, u64* out_size) {
    BufHelper buf;
    buildRomfsIntoFile(entries, buf);
    buf.seek(buf.buf.size());
    *out_size = buf.tell();
    writePadding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

bool npdmPatchKc(std::vector<u8>& npdm, u32 off, u32 size, u32 bitmask, u32 value) {
    const u32 pattern = BIT(bitmask) - 1;
    const u32 mask = BIT(bitmask) | pattern;
    for (u32 i = 0; i < size; i += 4) {
        u32 current{};
        std::memcpy(&current, npdm.data() + off + i, sizeof(current));
        if ((current & mask) == pattern) {
            current = value | pattern;
            std::memcpy(npdm.data() + off + i, &current, sizeof(current));
            return true;
        }
    }
    return false;
}

void patchNpdm(std::vector<u8>& npdm, const NpdmPatch& patch) {
    if (npdm.size() < sizeof(NpdmMeta))
        return;

    NpdmMeta meta{};
    std::memcpy(&meta, npdm.data(), sizeof(meta));
    if (meta.aci0_offset + sizeof(NpdmAci0) > npdm.size() ||
        meta.acid_offset + sizeof(NpdmAcid) > npdm.size())
        return;

    NpdmAci0 aci0{};
    NpdmAcid acid{};
    std::memcpy(&aci0, npdm.data() + meta.aci0_offset, sizeof(aci0));
    std::memcpy(&acid, npdm.data() + meta.acid_offset, sizeof(acid));

    std::memcpy(meta.title_name, patch.title_name, sizeof(meta.title_name));
    std::memcpy(meta.product_code, patch.product_code, sizeof(meta.product_code));
    aci0.program_id = patch.tid;
    acid.program_id_min = patch.tid;
    acid.program_id_max = patch.tid;

    if (R_SUCCEEDED(splInitialize())) {
        u64 ver{};
        const auto exosphereVersion = static_cast<SplConfigItem>(65000);
        splGetConfig(exosphereVersion, &ver);
        ver >>= 40;
        if (ver >= MAKEHOSVERSION(1, 8, 0)) {
            npdmPatchKc(npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, BIT(19));
            npdmPatchKc(npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 16, BIT(19));
        }
        splExit();
    }

    std::memcpy(npdm.data(), &meta, sizeof(meta));
    std::memcpy(npdm.data() + meta.aci0_offset, &aci0, sizeof(aci0));
    std::memcpy(npdm.data() + meta.acid_offset, &acid, sizeof(acid));
}

void patchNacp(NacpStruct& nacp, const NacpPatch& patch) {
    if (!patch.name.empty()) {
        for (auto& lang : nacp.lang) {
            std::memset(lang.name, 0, sizeof(lang.name));
            std::strncpy(lang.name, patch.name.c_str(), sizeof(lang.name) - 1);
        }
    }
    if (!patch.author.empty()) {
        for (auto& lang : nacp.lang) {
            std::memset(lang.author, 0, sizeof(lang.author));
            std::strncpy(lang.author, patch.author.c_str(), sizeof(lang.author) - 1);
        }
    }

    nacp.startup_user_account = 0x00;
    nacp.user_account_switch_lock = 0x00;
    nacp.add_on_content_registration_type = 0x01;
    nacp.screenshot = 0;
    nacp.video_capture = 0x2;
    nacp.logo_type = 0x2;
    nacp.logo_handling = 0x0;
    nacp.data_loss_confirmation = 0x0;
    nacp.required_network_service_license_on_launch = 0x0;

    const char error_code[] = "aiomod";
    nacp.application_error_code_category = 0;
    std::memcpy(&nacp.application_error_code_category, error_code,
                std::min(sizeof(error_code) - 1, sizeof(nacp.application_error_code_category)));

    nacp.presence_group_id = patch.tid;
    nacp.save_data_owner_id = patch.tid;
    nacp.pseudo_device_id_seed = patch.tid;
    nacp.add_on_content_base_id = patch.tid ^ 0x1000;
    for (auto& id : nacp.local_communication_id)
        id = patch.tid;

    nacp.play_log_policy = 0x0;
    nacp.play_log_query_capability = 0x0;
    nacp.user_account_save_data_size = 0;
    nacp.user_account_save_data_journal_size = 0;
    nacp.device_save_data_size = 0;
    nacp.device_save_data_journal_size = 0;
    nacp.user_account_save_data_size_max = 0;
    nacp.user_account_save_data_journal_size_max = 0;
    nacp.device_save_data_size_max = 0;
    nacp.device_save_data_journal_size_max = 0;
}

void addFileEntry(FileEntries& entries, const char* name, const void* data, u64 size) {
    FileEntry entry;
    entry.name = name;
    entry.data.resize(size);
    if (size)
        std::memcpy(entry.data.data(), data, size);
    entries.emplace_back(std::move(entry));
}

void addFileEntry(FileEntries& entries, const char* name, std::span<const u8> data) {
    addFileEntry(entries, name, data.data(), data.size());
}

std::vector<u8> buildIvfcMasterHash(std::span<const u8> level1) {
    std::vector<u8> hash(SHA256_HASH_SIZE);
    sha256CalculateHash(hash.data(), level1.data(), level1.size());
    return hash;
}

std::vector<u8> buildPfs0(const FileEntries& entries) {
    BufHelper buf;
    Pfs0Header header{};
    std::vector<Pfs0FileTable> file_table(entries.size());
    std::vector<char> string_table;

    u64 string_offset{};
    u64 data_offset{};
    for (u32 i = 0; i < entries.size(); i++) {
        file_table[i].data_offset = data_offset;
        file_table[i].data_size = entries[i].data.size();
        file_table[i].name_offset = string_offset;
        file_table[i].padding = 0;
        string_table.resize(string_offset + entries[i].name.length() + 1);
        std::memcpy(string_table.data() + string_offset, entries[i].name.c_str(), entries[i].name.length() + 1);
        data_offset += entries[i].data.size();
        string_offset += entries[i].name.length() + 1;
    }

    string_table.resize((string_table.size() + 0x1F) & ~0x1F);
    header.magic = 0x30534650; 
    header.total_files = entries.size();
    header.string_table_size = string_table.size();
    buf.write(&header, sizeof(header));
    buf.write(file_table.data(), sizeof(Pfs0FileTable) * file_table.size());
    buf.write(string_table.data(), string_table.size());
    for (const auto& entry : entries)
        buf.write(entry.data.data(), entry.data.size());
    return buf.buf;
}

std::vector<u8> buildPfs0HashTable(const std::vector<u8>& pfs0, u32 block_size) {
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    for (u64 pos = 0; pos < pfs0.size();) {
        const u64 read_size = std::min<u64>(block_size, pfs0.size() - pos);
        sha256CalculateHash(hash, pfs0.data() + pos, read_size);
        buf.write(hash, sizeof(hash));
        pos += read_size;
    }
    return buf.buf;
}

std::vector<u8> buildPfs0MasterHash(const std::vector<u8>& hash_table) {
    std::vector<u8> hash(SHA256_HASH_SIZE);
    sha256CalculateHash(hash.data(), hash_table.data(), hash_table.size());
    return hash;
}

void writeNcaPadding(BufHelper& buf) {
    writePadding(buf, buf.tell(), 0x200);
}

void ncaEncryptHeader(nca::Header* header, std::span<const u8> key) {
    Aes128XtsContext ctx{};
    aes128XtsContextCreate(&ctx, key.data(), key.data() + 0x10, true);
    u8 sector{};
    for (u64 pos = 0; pos < sizeof(nca::Header); pos += 0x200) {
        aes128XtsContextResetSector(&ctx, sector++, true);
        aes128XtsEncrypt(&ctx, reinterpret_cast<u8*>(header) + pos,
                         reinterpret_cast<const u8*>(header) + pos, 0x200);
    }
}

void writeNcaSection(nca::Header& header, u8 index, u64 start, u64 end) {
    auto& section = header.fs_table[index];
    section.media_start_offset = start / 0x200;
    section.media_end_offset = end / 0x200;
    section._0x8[0] = 0x1;
}

void writeNcaFsHeaderPfs0(nca::Header& header, u8 index, const std::vector<u8>& master_hash,
                           u64 hash_table_size, u32 block_size) {
    auto& fs_header = header.fs_header[index];
    fs_header.hash_type = nca::HashType_HierarchicalSha256;
    fs_header.fs_type = nca::FileSystemType_PFS0;
    fs_header.version = 0x2;
    fs_header.hash_data.hierarchical_sha256_data.layer_count = 0x2;
    fs_header.hash_data.hierarchical_sha256_data.block_size = block_size;
    fs_header.encryption_type = nca::EncryptionType_None;
    fs_header.hash_data.hierarchical_sha256_data.hash_layer.size = hash_table_size;
    std::memcpy(fs_header.hash_data.hierarchical_sha256_data.master_hash, master_hash.data(), master_hash.size());
    sha256CalculateHash(header.fs_header_hash[index].sha256, &fs_header, sizeof(fs_header));
}

void writeNcaFsHeaderRomfs(nca::Header& header, u8 index) {
    auto& fs_header = header.fs_header[index];
    fs_header.hash_type = nca::HashType_HierarchicalIntegrity;
    fs_header.fs_type = nca::FileSystemType_RomFS;
    fs_header.version = 0x2;
    fs_header.hash_data.integrity_meta_info.magic = 0x43465649; 
    fs_header.hash_data.integrity_meta_info.version = 0x20000;
    fs_header.hash_data.integrity_meta_info.master_hash_size = SHA256_HASH_SIZE;
    fs_header.hash_data.integrity_meta_info.info_level_hash.max_layers = 0x7;
    fs_header.encryption_type = nca::EncryptionType_None;
    fs_header.hash_data.integrity_meta_info.info_level_hash.levels[5].block_size = 0x0E;
    sha256CalculateHash(header.fs_header_hash[index].sha256, &fs_header, sizeof(fs_header));
}

void writeNcaPfs0(nca::Header& header, u8 index, const FileEntries& entries, u32 block_size, BufHelper& buf) {
    const auto pfs0 = buildPfs0(entries);
    const auto hash_table = buildPfs0HashTable(pfs0, block_size);
    const auto master_hash = buildPfs0MasterHash(hash_table);

    buf.write(hash_table.data(), hash_table.size());
    const auto padding_size = writePadding(buf, hash_table.size(), PFS0_PADDING_SIZE);
    header.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.offset = hash_table.size() + padding_size;
    header.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.size = pfs0.size();
    buf.write(pfs0.data(), pfs0.size());
    writeNcaPadding(buf);

    const auto section_start = index == 0 ? sizeof(header) : header.fs_table[index - 1].media_end_offset * 0x200ULL;
    writeNcaSection(header, index, section_start, buf.tell());
    writeNcaFsHeaderPfs0(header, index, master_hash, hash_table.size(), block_size);
}

std::vector<u8> ivfcCreateLevel(const std::vector<u8>& src) {
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    for (u64 pos = 0; pos < src.size();) {
        const u64 read_size = std::min<u64>(IVFC_HASH_BLOCK_SIZE, src.size() - pos);
        sha256CalculateHash(hash, src.data() + pos, read_size);
        buf.write(hash, sizeof(hash));
        pos += read_size;
    }
    writePadding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

void writeNcaRomfs(nca::Header& header, u8 index, const FileEntries& entries, BufHelper& buf) {
    auto& fs_header = header.fs_header[index];
    auto& meta_info = fs_header.hash_data.integrity_meta_info;
    auto& info_level_hash = meta_info.info_level_hash;

    std::vector<u8> ivfc[IVFC_MAX_LEVEL];
    ivfc[5] = romfsBuild(entries, &info_level_hash.levels[5].hash_data_size);
    for (int level = 4; level >= 0; level--) {
        ivfc[level] = ivfcCreateLevel(ivfc[level + 1]);
        info_level_hash.levels[level].hash_data_size = ivfc[level].size();
        info_level_hash.levels[level].block_size = 0x0E;
    }
    info_level_hash.levels[0].logical_offset = 0;
    for (int level = 1; level <= 5; level++) {
        info_level_hash.levels[level].logical_offset = info_level_hash.levels[level - 1].logical_offset +
                                                        info_level_hash.levels[level - 1].hash_data_size;
    }
    for (const auto& level : ivfc)
        buf.write(level.data(), level.size());
    writeNcaPadding(buf);

    const auto master_hash = buildIvfcMasterHash(ivfc[0]);
    std::memcpy(meta_info.master_hash, master_hash.data(), sizeof(meta_info.master_hash));
    const auto section_start = index == 0 ? sizeof(header) : header.fs_table[index - 1].media_end_offset * 0x200ULL;
    writeNcaSection(header, index, section_start, buf.tell());
    writeNcaFsHeaderRomfs(header, index);
}

void writeNcaHeaderEncrypted(nca::Header& header, u64 tid, const Keys& keys, nca::ContentType type, BufHelper& buf) {
    header.magic = nca::NCA3_MAGIC;
    header.distribution_type = nca::DistributionType_System;
    header.content_type = type;
    header.program_id = tid;
    header.sdk_version = 0x000C1100;
    header.size = buf.tell();
    ncaEncryptHeader(&header, std::span<const u8>(keys.header_key, sizeof(keys.header_key)));
    buf.seek(0);
    buf.write(&header, sizeof(header));
}

NcaEntry createProgramNca(u64 tid, const Keys& keys, const FileEntries& exefs, const FileEntries& romfs) {
    BufHelper buf;
    nca::Header header{};
    buf.write(&header, sizeof(header));
    writeNcaPfs0(header, 0, exefs, PFS0_EXEFS_HASH_BLOCK_SIZE, buf);
    writeNcaRomfs(header, 1, romfs, buf);
    writeNcaHeaderEncrypted(header, tid, keys, nca::ContentType_Program, buf);
    return {buf, NcmContentType_Program};
}

NcaEntry createControlNca(u64 tid, const Keys& keys, const FileEntries& romfs) {
    BufHelper buf;
    nca::Header header{};
    buf.write(&header, sizeof(header));
    writeNcaRomfs(header, 0, romfs, buf);
    writeNcaHeaderEncrypted(header, tid, keys, nca::ContentType_Control, buf);
    return {buf, NcmContentType_Control};
}

NcaMetaEntry createMetaNca(u64 tid, const Keys& keys, NcmStorageId storage_id, const std::vector<NcaEntry>& ncas) {
    CnmtHeader cnmt_header{};
    NcmApplicationMetaExtendedHeader cnmt_extended{};
    NcmPackagedContentInfo packaged_content_info[2]{};
    u8 digest[0x20]{};
    BufHelper buf;

    cnmt_header.title_id = tid;
    cnmt_header.title_version = 0;
    cnmt_header.meta_type = NcmContentMetaType_Application;
    cnmt_header.meta_header.extended_header_size = sizeof(cnmt_extended);
    cnmt_header.meta_header.content_count = 0x2;
    cnmt_header.meta_header.content_meta_count = 0x1;
    cnmt_header.meta_header.attributes = 0x0;
    cnmt_header.meta_header.storage_id = storage_id;
    cnmt_extended.patch_id = cnmt_header.title_id | 0x800;

    for (u32 i = 0; i < ncas.size() && i < 2; i++) {
        std::memcpy(packaged_content_info[i].hash, ncas[i].hash, sizeof(packaged_content_info[i].hash));
        std::memcpy(&packaged_content_info[i].info.content_id, ncas[i].hash,
                    sizeof(packaged_content_info[i].info.content_id));
        packaged_content_info[i].info.content_type = ncas[i].type;
        ncmU64ToContentInfoSize(ncas[i].data.size(), &packaged_content_info[i].info);
    }

    BufHelper cnmt_buf;
    cnmt_buf.write(&cnmt_header, sizeof(cnmt_header));
    cnmt_buf.write(&cnmt_extended, sizeof(cnmt_extended));
    cnmt_buf.write(&packaged_content_info, sizeof(packaged_content_info));
    cnmt_buf.write(digest, sizeof(digest));

    FileEntries cnmt;
    char cnmt_name[34];
    std::snprintf(cnmt_name, sizeof(cnmt_name), "Application_%016lX.cnmt", tid);
    addFileEntry(cnmt, cnmt_name, cnmt_buf.buf.data(), cnmt_buf.buf.size());

    nca::Header header{};
    buf.write(&header, sizeof(header));
    writeNcaPfs0(header, 0, cnmt, PFS0_META_HASH_BLOCK_SIZE, buf);
    writeNcaHeaderEncrypted(header, tid, keys, nca::ContentType_Meta, buf);

    NcaMetaEntry entry{buf, NcmContentType_Meta};
    entry.content_meta_header = cnmt_header.meta_header;
    entry.content_meta_header.content_count++;
    entry.content_meta_header.storage_id = 0;

    entry.content_meta_key.id = cnmt_header.title_id;
    entry.content_meta_key.version = cnmt_header.title_version;
    entry.content_meta_key.type = cnmt_header.meta_type;
    entry.content_meta_key.install_type = NcmContentInstallType_Full;
    std::memset(entry.content_meta_key.padding, 0, sizeof(entry.content_meta_key.padding));

    entry.content_storage_record.key = entry.content_meta_key;
    entry.content_storage_record.storage_id = storage_id;
    std::memset(entry.content_storage_record.padding, 0, sizeof(entry.content_storage_record.padding));

    entry.content_meta_data.header = entry.content_meta_header;
    entry.content_meta_data.extended = cnmt_extended;
    std::memcpy(&entry.content_meta_data.infos[0].content_id, entry.nca_entry.hash,
                sizeof(entry.content_meta_data.infos[0].content_id));
    entry.content_meta_data.infos[0].content_type = entry.nca_entry.type;
    entry.content_meta_data.infos[0].attr = 0;
    ncmU64ToContentInfoSize(cnmt_buf.buf.size(), &entry.content_meta_data.infos[0]);
    entry.content_meta_data.infos[0].id_offset = 0;
    entry.content_meta_data.infos[1] = packaged_content_info[0].info;
    entry.content_meta_data.infos[2] = packaged_content_info[1].info;
    return entry;
}

Result writeNcaToStorage(NcmContentStorage& cs, const NcaEntry& nca) {
    NcmContentId content_id{};
    NcmPlaceHolderId placeholder_id{};
    std::memcpy(&content_id, nca.hash, sizeof(content_id));

    Result rc = ncmContentStorageGeneratePlaceHolderId(&cs, &placeholder_id);
    if (R_FAILED(rc)) return rc;
    ncmContentStorageDeletePlaceHolder(&cs, &placeholder_id);

    rc = ncmContentStorageCreatePlaceHolder(&cs, &content_id, &placeholder_id, nca.data.size());
    if (R_FAILED(rc)) return rc;
    rc = ncmContentStorageWritePlaceHolder(&cs, &placeholder_id, 0, nca.data.data(), nca.data.size());
    if (R_FAILED(rc)) {
        ncmContentStorageDeletePlaceHolder(&cs, &placeholder_id);
        return rc;
    }

    ncmContentStorageDelete(&cs, &content_id);
    rc = ncmContentStorageRegister(&cs, &content_id, &placeholder_id);
    if (R_FAILED(rc))
        ncmContentStorageDeletePlaceHolder(&cs, &placeholder_id);
    return rc;
}

} 

Result install(Config config, NcmStorageId storage_id, ProgressCallback progress) {
    constexpr int TOTAL_STEPS = 9;
    if (config.nro_path.empty())
        return ResultBadArgs;
    if (config.icon.empty())
        return ResultIconMissing;

    report(progress, 0, TOTAL_STEPS, "Preparando forwarder...");

    Result rc = splCryptoInitialize();
    if (R_FAILED(rc))
        return rc;

    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        splCryptoExit();
        return rc;
    }

    rc = nsExInitialize();
    if (R_FAILED(rc)) {
        ncmExit();
        splCryptoExit();
        return rc;
    }

    
    rc = [&]() -> Result {
        report(progress, 1, TOTAL_STEPS, "Derivando claves de cabecera...");
        Keys keys{};
        Result inner_rc = deriveHeaderKey(keys);
        if (R_FAILED(inner_rc))
            return inner_rc;

        if (config.args.empty())
            config.args = config.nro_path;
        else
            config.args = config.nro_path + ' ' + config.args;

        u64 hash_data[SHA256_HASH_SIZE / sizeof(u64)]{};
        const auto hash_path = config.nro_path + config.args;
        sha256CalculateHash(hash_data, hash_path.data(), hash_path.length());
        const u64 old_tid = 0x0100000000000000ULL | (hash_data[0] & 0x00FFFFFFFFFFF000ULL);
        const u64 tid = 0x0500000000000000ULL | (hash_data[0] & 0x00FFFFFFFFFFF000ULL);

        report(progress, 2, TOTAL_STEPS, "Creando Program NCA...");
        auto hbl_main = readBinary("romfs:/forwarder/main");
        auto hbl_npdm = readBinary("romfs:/forwarder/main.npdm");
        if (hbl_main.empty() || hbl_npdm.empty())
            return ResultLoaderMissing;

        FileEntries exefs;
        addFileEntry(exefs, "main", std::span<const u8>(hbl_main.data(), hbl_main.size()));
        addFileEntry(exefs, "main.npdm", std::span<const u8>(hbl_npdm.data(), hbl_npdm.size()));

        NpdmPatch npdm_patch{};
        npdm_patch.tid = tid;
        patchNpdm(exefs[1].data, npdm_patch);

        FileEntries program_romfs;
        addFileEntry(program_romfs, "/nextArgv", config.args.data(), config.args.length());
        addFileEntry(program_romfs, "/nextNroPath", config.nro_path.data(), config.nro_path.length());

        std::vector<NcaEntry> nca_entries;
        nca_entries.emplace_back(createProgramNca(tid, keys, exefs, program_romfs));

        report(progress, 3, TOTAL_STEPS, "Creando Control NCA...");
        NacpPatch nacp_patch{};
        nacp_patch.tid = tid;
        nacp_patch.name = config.name;
        nacp_patch.author = config.author;
        patchNacp(config.nacp, nacp_patch);

        FileEntries control_romfs;
        addFileEntry(control_romfs, "/control.nacp", &config.nacp, sizeof(config.nacp));
        addFileEntry(control_romfs, "/icon_AmericanEnglish.dat", config.icon.data(), config.icon.size());
        nca_entries.emplace_back(createControlNca(tid, keys, control_romfs));

        report(progress, 4, TOTAL_STEPS, "Creando Meta NCA...");
        const auto meta_entry = createMetaNca(tid, keys, storage_id, nca_entries);
        nca_entries.emplace_back(meta_entry.nca_entry);

        NcmContentStorage cs{};
        inner_rc = ncmOpenContentStorage(&cs, storage_id);
        if (R_FAILED(inner_rc))
            return inner_rc;

        for (size_t i = 0; i < nca_entries.size(); i++) {
            if (i == 0) report(progress, 5, TOTAL_STEPS, "Escribiendo Program NCA...");
            if (i == 1) report(progress, 6, TOTAL_STEPS, "Escribiendo Control NCA...");
            if (i == 2) report(progress, 7, TOTAL_STEPS, "Escribiendo Meta NCA...");

            inner_rc = writeNcaToStorage(cs, nca_entries[i]);
            if (R_FAILED(inner_rc)) {
                ncmContentStorageClose(&cs);
                return inner_rc;
            }
        }
        ncmContentStorageClose(&cs);

        report(progress, 8, TOTAL_STEPS, "Actualizando base de datos y HOME Menu...");
        NcmContentMetaDatabase db{};
        inner_rc = ncmOpenContentMetaDatabase(&db, storage_id);
        if (R_FAILED(inner_rc))
            return inner_rc;

        inner_rc = ncmContentMetaDatabaseSet(&db, &meta_entry.content_meta_key,
                                              &meta_entry.content_meta_data,
                                              sizeof(meta_entry.content_meta_data));
        if (R_SUCCEEDED(inner_rc))
            inner_rc = ncmContentMetaDatabaseCommit(&db);
        ncmContentMetaDatabaseClose(&db);
        if (R_FAILED(inner_rc))
            return inner_rc;

        const Result old_delete_rc = nsDeleteApplicationCompletely(old_tid);
        (void)old_delete_rc; 

        nsDeleteApplicationEntity(tid);
        inner_rc = pushApplicationRecord(tid, &meta_entry.content_storage_record, 1);
        if (R_FAILED(inner_rc))
            return inner_rc;

        invalidateApplicationControlCache(tid);
        report(progress, 9, TOTAL_STEPS, "Forwarder instalado correctamente.");
        return 0;
    }();

    nsExExit();
    ncmExit();
    splCryptoExit();
    return rc;
}

} 
