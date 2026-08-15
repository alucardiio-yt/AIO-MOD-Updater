#pragma once

#include <switch.h>

namespace aio::forwarder::nca {

constexpr u32 NCA3_MAGIC = 0x3341434E;
constexpr u32 NCA_SECTION_TOTAL = 4;

enum DistributionType : u8 {
    DistributionType_System = 0x0,
    DistributionType_GameCard = 0x1,
};

enum ContentType : u8 {
    ContentType_Program = 0x0,
    ContentType_Meta = 0x1,
    ContentType_Control = 0x2,
    ContentType_Manual = 0x3,
    ContentType_Data = 0x4,
    ContentType_PublicData = 0x5,
};

enum FileSystemType : u8 {
    FileSystemType_RomFS = 0x0,
    FileSystemType_PFS0 = 0x1,
};

enum HashType : u8 {
    HashType_Auto = 0x0,
    HashType_HierarchicalSha256 = 0x2,
    HashType_HierarchicalIntegrity = 0x3,
};

enum EncryptionType : u8 {
    EncryptionType_Auto = 0x0,
    EncryptionType_None = 0x1,
    EncryptionType_AesXts = 0x2,
    EncryptionType_AesCtr = 0x3,
    EncryptionType_AesCtrEx = 0x4,
    EncryptionType_AesCtrSkipLayerHash = 0x5,
    EncryptionType_AesCtrExSkipLayerHash = 0x6,
};

struct SectionTableEntry {
    u32 media_start_offset;
    u32 media_end_offset;
    u8 _0x8[0x4];
    u8 _0xC[0x4];
};
static_assert(sizeof(SectionTableEntry) == 0x10);

struct LayerRegion {
    u64 offset;
    u64 size;
};

struct HierarchicalSha256Data {
    u8 master_hash[0x20];
    u32 block_size;
    u32 layer_count;
    LayerRegion hash_layer;
    LayerRegion pfs0_layer;
    LayerRegion unused_layers[3];
    u8 _0x78[0x80];
};
static_assert(sizeof(HierarchicalSha256Data) == 0xF8);

#pragma pack(push, 1)
struct HierarchicalIntegrityVerificationLevelInformation {
    u64 logical_offset;
    u64 hash_data_size;
    u32 block_size;
    u32 _0x14;
};
#pragma pack(pop)
static_assert(sizeof(HierarchicalIntegrityVerificationLevelInformation) == 0x18);

struct InfoLevelHash {
    u32 max_layers;
    HierarchicalIntegrityVerificationLevelInformation levels[6];
    u8 signature_salt[0x20];
};
static_assert(sizeof(InfoLevelHash) == 0xB4);

struct IntegrityMetaInfo {
    u32 magic;
    u32 version;
    u32 master_hash_size;
    InfoLevelHash info_level_hash;
    u8 master_hash[0x20];
    u8 _0xE0[0x18];
};
static_assert(sizeof(IntegrityMetaInfo) == 0xF8);

struct BucketTreeHeader {
    u32 magic;
    u32 version;
    u32 count;
    u8 _0xC[0x4];
};
static_assert(sizeof(BucketTreeHeader) == 0x10);

struct PatchInfo {
    u64 indirect_offset;
    u64 indirect_size;
    BucketTreeHeader indirect_header;
    u64 aes_ctr_offset;
    u64 aes_ctr_size;
    BucketTreeHeader aes_ctr_header;
};
static_assert(sizeof(PatchInfo) == 0x40);

struct CompressionInfo {
    u64 table_offset;
    u64 table_size;
    BucketTreeHeader table_header;
    u8 _0x20[0x8];
};
static_assert(sizeof(CompressionInfo) == 0x28);

struct FsHeader {
    u16 version;
    u8 fs_type;
    u8 hash_type;
    u8 encryption_type;
    u8 metadata_hash_type;
    u8 _0x6[0x2];

    union {
        HierarchicalSha256Data hierarchical_sha256_data;
        IntegrityMetaInfo integrity_meta_info;
    } hash_data;

    PatchInfo patch_info;
    u64 section_ctr;
    u8 spares_info[0x30];
    CompressionInfo compression_info;
    u8 meta_data_hash_data_info[0x30];
    u8 reserved[0x30];
};
static_assert(sizeof(FsHeader) == 0x200);

struct SectionHeaderHash {
    u8 sha256[0x20];
};

struct KeyArea {
    u8 area[0x10];
};

struct Header {
    u8 rsa_fixed_key[0x100];
    u8 rsa_npdm[0x100];
    u32 magic;
    u8 distribution_type;
    u8 content_type;
    u8 old_key_gen;
    u8 kaek_index;
    u64 size;
    u64 program_id;
    u32 context_id;
    union {
        u32 sdk_version;
        struct {
            u8 sdk_revision;
            u8 sdk_micro;
            u8 sdk_minor;
            u8 sdk_major;
        };
    };
    u8 key_gen;
    u8 sig_key_gen;
    u8 _0x222[0xE];
    u8 rights_id[0x10];
    SectionTableEntry fs_table[NCA_SECTION_TOTAL];
    SectionHeaderHash fs_header_hash[NCA_SECTION_TOTAL];
    KeyArea key_area[NCA_SECTION_TOTAL];
    u8 _0x340[0xC0];
    FsHeader fs_header[NCA_SECTION_TOTAL];
};
static_assert(sizeof(Header) == 0xC00);

} 
