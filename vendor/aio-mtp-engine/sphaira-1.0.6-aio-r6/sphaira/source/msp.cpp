#include "msp.hpp"

#include "defines.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "threaded_file_transfer.hpp"
#include "title_info.hpp"
#include "ui/progress_box.hpp"
#include "yati/container/nsp.hpp"
#include "yati/source/base.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace sphaira::msp {
namespace {

constexpr std::size_t MAX_MSP_ENTRIES = 0x1000;
constexpr s64 MAX_MANIFEST_SIZE = 64 * 1024;
constexpr std::size_t MAX_PATCHSET_SIZE = 128;

enum class EntryKind {
    Unknown,
    Manifest,
    Romfs,
    ExefsNsp,
    LooseExefs,
    Config,
    Icon,
    Ips,
};

struct PackageEntry {
    yati::container::CollectionEntry source{};
    EntryKind kind{EntryKind::Unknown};
    std::string output_name{};
    fs::FsPath staged_path{};
    fs::FsPath destination{};
};

struct Manifest {
    u64 title_id{};
    std::optional<u32> version{};
    std::string patchset{};
};

struct Backup {
    fs::FsPath original{};
    fs::FsPath staged{};
    bool directory{};
};

auto LowerAscii(std::string_view value) -> std::string {
    std::string out{value};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return out;
}

auto Trim(std::string_view value) -> std::string_view {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool IsSafeComponent(std::string_view name, std::size_t max_size) {
    if (name.empty() || name.size() > max_size || name == "." || name == "..") {
        return false;
    }

    for (const unsigned char c : name) {
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

auto ClassifyEntry(std::string_view name, std::string& output_name) -> EntryKind {
    const auto lower = LowerAscii(name);
    if (lower == "manifest") {
        return EntryKind::Manifest;
    }
    if (lower == "romfs.bin") {
        output_name = "romfs.bin";
        return EntryKind::Romfs;
    }
    if (lower == "exefs.nsp") {
        output_name = "exefs.nsp";
        return EntryKind::ExefsNsp;
    }
    if (lower == "config.ini") {
        output_name = "config.ini";
        return EntryKind::Config;
    }
    if (lower == "icon.jpg") {
        output_name = "icon.jpg";
        return EntryKind::Icon;
    }

    const bool numbered_exefs =
        (lower.size() == 7 && lower.starts_with("compat") && lower.back() >= '0' && lower.back() <= '9') ||
        (lower.size() == 7 && lower.starts_with("subsdk") && lower.back() >= '0' && lower.back() <= '9');
    if (lower == "rtld" || lower == "main" || lower == "main.npdm" || lower == "sdk" || numbered_exefs) {
        output_name = lower;
        return EntryKind::LooseExefs;
    }
    if (lower.size() > 4 && lower.ends_with(".ips")) {
        output_name = std::string{name};
        return EntryKind::Ips;
    }
    return EntryKind::Unknown;
}

Result ReadExact(yati::source::Base* source, void* data, s64 offset, s64 size) {
    if (!size) {
        R_SUCCEED();
    }

    u64 bytes_read{};
    R_TRY(source->Read(data, offset, size, &bytes_read));
    R_UNLESS(bytes_read == static_cast<u64>(size), Result_NspInvalidHeader);
    R_SUCCEED();
}

Result ParseManifest(std::span<const u8> data, Manifest& out) {
    R_UNLESS(!data.empty() && data.size() <= MAX_MANIFEST_SIZE, Result_MspInvalidManifest);
    R_UNLESS(std::find(data.begin(), data.end(), '\0') == data.end(), Result_MspInvalidManifest);

    const std::string_view text{reinterpret_cast<const char*>(data.data()), data.size()};
    bool has_title_id{};
    bool has_version{};
    bool has_patchset{};

    for (std::size_t pos = 0; pos <= text.size();) {
        const auto end = text.find('\n', pos);
        auto line = Trim(text.substr(pos, end == std::string_view::npos ? text.size() - pos : end - pos));
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            const auto equals = line.find('=');
            R_UNLESS(equals != std::string_view::npos, Result_MspInvalidManifest);

            const auto key = LowerAscii(Trim(line.substr(0, equals)));
            const auto value = Trim(line.substr(equals + 1));
            R_UNLESS(!key.empty(), Result_MspInvalidManifest);

            if (key == "titleid") {
                R_UNLESS(!has_title_id && value.size() == 16, Result_MspInvalidTitleId);
                const auto result = std::from_chars(value.data(), value.data() + value.size(), out.title_id, 16);
                R_UNLESS(result.ec == std::errc{} && result.ptr == value.data() + value.size() && out.title_id,
                    Result_MspInvalidTitleId);
                has_title_id = true;
            } else if (key == "version") {
                R_UNLESS(!has_version && !value.empty(), Result_MspInvalidVersion);
                u32 version{};
                const auto result = std::from_chars(value.data(), value.data() + value.size(), version, 10);
                R_UNLESS(result.ec == std::errc{} && result.ptr == value.data() + value.size(), Result_MspInvalidVersion);
                out.version = version;
                has_version = true;
            } else if (key == "patchset") {
                R_UNLESS(!has_patchset, Result_MspInvalidPatchset);
                R_UNLESS(value.empty() || IsSafeComponent(value, MAX_PATCHSET_SIZE), Result_MspInvalidPatchset);
                out.patchset = value;
                has_patchset = true;
            }
        }

        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }

    R_UNLESS(has_title_id, Result_MspInvalidTitleId);
    R_SUCCEED();
}

Result StageFile(ui::ProgressBox* pbox, yati::source::Base* source, fs::FsNativeSd& sd, PackageEntry& entry) {
    R_TRY(sd.CreateFile(entry.staged_path, entry.source.size));

    fs::File file;
    R_TRY(sd.OpenFile(entry.staged_path, FsOpenMode_Write, &file));
    R_TRY(file.SetSize(entry.source.size));

    pbox->NewTransfer(entry.source.name);
    if (entry.source.size) {
        R_TRY(thread::Transfer(pbox, entry.source.size,
            [&](void* data, s64 offset, s64 size, u64* bytes_read) -> Result {
                R_TRY(source->Read(data, entry.source.offset + offset, size, bytes_read));
                R_UNLESS(*bytes_read == static_cast<u64>(size), Result_NspInvalidHeader);
                R_SUCCEED();
            },
            [&](const void* data, s64 offset, s64 size) -> Result {
                return file.Write(offset, data, size, 0);
            }
        ));
    }
    R_SUCCEED();
}

void AddUniquePath(std::vector<fs::FsPath>& paths, const fs::FsPath& path) {
    if (std::ranges::find(paths, path) == paths.end()) {
        paths.emplace_back(path);
    }
}

Result RemovePath(fs::FsNativeSd& sd, const fs::FsPath& path) {
    if (sd.FileExists(path)) {
        return sd.DeleteFile(path);
    }
    if (sd.DirExists(path)) {
        return sd.DeleteDirectoryRecursively(path);
    }
    R_SUCCEED();
}

Result Rollback(fs::FsNativeSd& sd, const std::vector<fs::FsPath>& installed, const std::vector<Backup>& backups) {
    Result first_error{};
    for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
        const auto rc = RemovePath(sd, *it);
        if (R_FAILED(rc) && R_SUCCEEDED(first_error)) {
            first_error = rc;
        }
    }

    for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
        if (sd.FileExists(it->original)) {
            const auto rc = sd.DeleteFile(it->original);
            if (R_FAILED(rc)) {
                if (R_SUCCEEDED(first_error)) first_error = rc;
                continue;
            }
        } else if (sd.DirExists(it->original)) {
            const auto rc = sd.DeleteDirectory(it->original);
            if (R_FAILED(rc)) {
                if (R_SUCCEEDED(first_error)) first_error = rc;
                continue;
            }
        }

        const auto create_result = sd.CreateDirectoryRecursivelyWithPath(it->original);
        if (R_FAILED(create_result)) {
            if (R_SUCCEEDED(first_error)) first_error = create_result;
            continue;
        }

        const auto restore_result = it->directory
            ? sd.RenameDirectory(it->staged, it->original)
            : sd.RenameFile(it->staged, it->original);
        if (R_FAILED(restore_result) && R_SUCCEEDED(first_error)) {
            first_error = restore_result;
        }
    }
    return first_error;
}

} // namespace

Result Install(ui::ProgressBox* pbox, yati::source::Base* source, s64 source_size) {
    R_TRY(source->GetOpenResult());

    yati::container::Nsp pfs0{source};
    yati::container::Collections collections;
    R_TRY(pfs0.GetCollections(collections, 0, source_size));
    R_UNLESS(!collections.empty() && collections.size() <= MAX_MSP_ENTRIES, Result_MspMissingManifest);

    std::ranges::sort(collections, {}, &yati::container::CollectionEntry::offset);
    std::vector<PackageEntry> entries;
    entries.reserve(collections.size());
    std::unordered_set<std::string> names;

    s64 expected_offset = pfs0.GetDataOffset();
    s64 payload_size{};
    std::size_t manifest_count{};
    std::size_t payload_count{};

    for (auto& collection : collections) {
        R_UNLESS(collection.offset >= expected_offset && collection.size >= 0, Result_NspInvalidHeader);
        R_UNLESS(collection.size <= std::numeric_limits<s64>::max() - collection.offset, Result_NspInvalidHeader);
        expected_offset = collection.offset + collection.size;

        R_UNLESS(IsSafeComponent(collection.name, 255), Result_MspInvalidEntry);
        const auto canonical_name = LowerAscii(collection.name);
        R_UNLESS(names.emplace(canonical_name).second, Result_MspDuplicateEntry);

        auto& entry = entries.emplace_back();
        entry.source = std::move(collection);
        entry.kind = ClassifyEntry(entry.source.name, entry.output_name);
        R_UNLESS(entry.kind != EntryKind::Unknown, Result_MspInvalidEntry);
        if (entry.kind == EntryKind::Manifest) {
            R_UNLESS(entry.source.size <= MAX_MANIFEST_SIZE, Result_MspInvalidManifest);
            ++manifest_count;
        } else {
            R_UNLESS(entry.source.size <= std::numeric_limits<s64>::max() - payload_size, Result_MspInvalidEntry);
            payload_size += entry.source.size;
            ++payload_count;
        }
    }

    R_UNLESS(source_size < 0 || expected_offset <= source_size, Result_NspInvalidHeader);
    R_UNLESS(manifest_count == 1, Result_MspMissingManifest);
    R_UNLESS(payload_count, Result_MspNoPayload);
    fs::FsNativeSd sd;
    R_TRY(sd.GetFsOpenResult());
    s64 free_space{};
    R_TRY(sd.GetFreeSpace("/", &free_space));
    R_UNLESS(payload_size <= free_space, FsError_UsableSpaceNotEnoughSdCard);

    fs::FsPath stage_root;
    bool found_stage_path{};
    const auto stage_id = armGetSystemTick();
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        std::snprintf(stage_root, sizeof(stage_root), "/atmosphere/.sphaira_msp_%016lX", stage_id + attempt);
        if (!sd.FileExists(stage_root) && !sd.DirExists(stage_root)) {
            found_stage_path = true;
            break;
        }
    }
    R_UNLESS(found_stage_path, FsError_PathAlreadyExists);
    R_TRY(sd.CreateDirectoryRecursively(stage_root));

    bool cleanup_stage = true;
    ON_SCOPE_EXIT(
        if (cleanup_stage && sd.DirExists(stage_root)) {
            const auto rc = sd.DeleteDirectoryRecursively(stage_root);
            if (R_FAILED(rc)) {
                log_write("[MSP] failed to remove staging directory: 0x%X\n", R_VALUE(rc));
            }
        }
    );

    std::vector<u8> manifest_data;
    u32 staged_index{};
    for (auto& entry : entries) {
        if (entry.kind == EntryKind::Manifest) {
            manifest_data.resize(entry.source.size);
            R_TRY(ReadExact(source, manifest_data.data(), entry.source.offset, entry.source.size));
            continue;
        }

        std::snprintf(entry.staged_path, sizeof(entry.staged_path), "%s/%04u", stage_root.s, staged_index++);
        R_TRY(StageFile(pbox, source, sd, entry));
    }

    Manifest manifest;
    R_TRY(ParseManifest(manifest_data, manifest));

    title::MetaEntries title_entries;
    const auto title_result = title::GetMetaEntries(manifest.title_id, title_entries,
        title::ContentFlag_Application | title::ContentFlag_Patch);
    if (R_FAILED(title_result)) {
        log_write("[MSP] unable to query installed version for %016lX: 0x%X\n",
            manifest.title_id, R_VALUE(title_result));
    } else if (manifest.version && !title_entries.empty()) {
        u32 installed_version{};
        for (const auto& entry : title_entries) {
            installed_version = std::max(installed_version, entry.version);
        }
        if (installed_version != *manifest.version) {
            log_write("[MSP] recommended title version %u, installed version %u\n", *manifest.version, installed_version);
        }
    }

    fs::FsPath contents_path;
    std::snprintf(contents_path, sizeof(contents_path), "/atmosphere/contents/%016lX", manifest.title_id);
    fs::FsPath patch_path;
    if (manifest.patchset.empty()) {
        std::snprintf(patch_path, sizeof(patch_path), "/atmosphere/exefs_patches/%016lX", manifest.title_id);
    } else {
        std::snprintf(patch_path, sizeof(patch_path), "/atmosphere/exefs_patches/%s_%016lX",
            manifest.patchset.c_str(), manifest.title_id);
    }

    std::vector<fs::FsPath> conflicts;
    for (auto& entry : entries) {
        switch (entry.kind) {
            case EntryKind::Romfs:
                entry.destination = fs::AppendPath(contents_path, entry.output_name);
                break;
            case EntryKind::ExefsNsp:
                entry.destination = fs::AppendPath(contents_path, entry.output_name);
                break;
            case EntryKind::LooseExefs:
                entry.destination = fs::AppendPath(fs::AppendPath(contents_path, "exefs"), entry.output_name);
                break;
            case EntryKind::Config:
            case EntryKind::Icon:
                entry.destination = fs::AppendPath(contents_path, entry.output_name);
                AddUniquePath(conflicts, entry.destination);
                break;
            case EntryKind::Ips:
                entry.destination = fs::AppendPath(patch_path, entry.output_name);
                AddUniquePath(conflicts, entry.destination);
                break;
            default:
                break;
        }
    }

    AddUniquePath(conflicts, fs::AppendPath(contents_path, "romfs"));
    AddUniquePath(conflicts, fs::AppendPath(contents_path, "romfs.bin"));
    AddUniquePath(conflicts, fs::AppendPath(contents_path, "exefs"));
    AddUniquePath(conflicts, fs::AppendPath(contents_path, "exefs.nsp"));

    std::vector<Backup> backups;
    std::vector<fs::FsPath> installed;
    const auto fail_and_rollback = [&](Result original_error) -> Result {
        const auto rollback_result = Rollback(sd, installed, backups);
        if (R_FAILED(rollback_result)) {
            cleanup_stage = false;
            log_write("[MSP] rollback failed: 0x%X (original error: 0x%X), preserving %s\n",
                R_VALUE(rollback_result), R_VALUE(original_error), stage_root.s);
            return rollback_result;
        }
        return original_error;
    };

    for (const auto& conflict : conflicts) {
        const bool directory = sd.DirExists(conflict);
        if (!directory && !sd.FileExists(conflict)) {
            continue;
        }

        Backup backup{conflict, {}, directory};
        std::snprintf(backup.staged, sizeof(backup.staged), "%s/backup%04zu", stage_root.s, backups.size());
        const auto rc = directory
            ? sd.RenameDirectory(backup.original, backup.staged)
            : sd.RenameFile(backup.original, backup.staged);
        if (R_FAILED(rc)) {
            return fail_and_rollback(rc);
        }
        backups.emplace_back(backup);
    }

    for (auto& entry : entries) {
        if (entry.kind == EntryKind::Manifest) {
            continue;
        }

        auto rc = sd.CreateDirectoryRecursivelyWithPath(entry.destination);
        if (R_FAILED(rc)) {
            return fail_and_rollback(rc);
        }
        rc = sd.RenameFile(entry.staged_path, entry.destination);
        if (R_FAILED(rc)) {
            return fail_and_rollback(rc);
        }
        installed.emplace_back(entry.destination);
    }

    const auto cleanup_result = sd.DeleteDirectoryRecursively(stage_root);
    if (R_FAILED(cleanup_result)) {
        log_write("[MSP] installed successfully but failed to remove staging directory: 0x%X\n", R_VALUE(cleanup_result));
    }
    title::Clear();
    log_write("[MSP] installed mod for %016lX\n", manifest.title_id);
    R_SUCCEED();
}

} // namespace sphaira::msp
