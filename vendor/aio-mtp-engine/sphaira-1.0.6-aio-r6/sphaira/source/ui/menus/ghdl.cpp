#include "ui/menus/ghdl.hpp"
#include "ui/menus/homebrew.hpp"

#include "swkbd.hpp"
#include "ui/sidebar.hpp"
#include "ui/option_box.hpp"
#include "ui/popup_list.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"
#include "ui/nvg_util.hpp"

#include "log.hpp"
#include "app.hpp"
#include "fs.hpp"
#include "defines.hpp"
#include "image.hpp"
#include "download.hpp"
#include "i18n.hpp"
#include "minizip_helper.hpp"
#include "yyjson_helper.hpp"
#include "threaded_file_transfer.hpp"

#include <minIni.h>
#include <minizip/unzip.h>
#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace sphaira::ui::menu::gh {
namespace {

constexpr auto CACHE_PATH = "/switch/sphaira/cache/github";
constexpr fs::FsPath DIRECT_LINKS_PATH{"/config/sphaira/github/direct_links.json"};
constexpr fs::FsPath REPOSITORIES_PATH{"/config/sphaira/github/repositories.json"};
constexpr fs::FsPath DIRECT_LINKS_DIRECTORY{"/config/sphaira/github"};
constexpr fs::FsPath SWITCHPORTS_CACHE_PATH{"/switch/sphaira/cache/github/switchports.md"};
constexpr auto SWITCHPORTS_README_URL = "https://raw.githubusercontent.com/robzilla10001/SwitchPorts/main/README.md";
constexpr std::size_t SWITCHPORTS_README_MAX_SIZE = 2 * 1024 * 1024;

struct SwitchPortEntry {
    std::string category;
    std::string name;
    std::string version;
    std::string source_url;
    std::string owner;
    std::string repo;
    std::string install_folder;
};

struct SwitchPortCategory {
    std::string name;
    std::vector<SwitchPortEntry> entries;
};

auto Trim(std::string_view value) -> std::string {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

auto Lower(std::string_view value) -> std::string {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](char c){
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

auto EndsWithCaseInsensitive(std::string_view value, std::string_view suffix) -> bool {
    return value.size() >= suffix.size()
        && !strncasecmp(value.data() + value.size() - suffix.size(), suffix.data(), suffix.size());
}

auto IsHttpUrl(std::string_view url) -> bool {
    const auto scheme_size = url.size() >= 8 && !strncasecmp(url.data(), "https://", 8) ? 8U
        : url.size() >= 7 && !strncasecmp(url.data(), "http://", 7) ? 7U
        : 0U;
    if (!scheme_size || url.size() == scheme_size) {
        return false;
    }

    if (std::ranges::any_of(url, [](unsigned char c){ return c <= 0x20 || c == 0x7F; })) {
        return false;
    }

    const auto authority = url.substr(scheme_size, url.find_first_of("/?#", scheme_size) - scheme_size);
    return !authority.empty() && authority != "." && authority != "..";
}

auto GetDirectLinkName(std::string_view url) -> std::string {
    if (const auto end = url.find_first_of("?#"); end != std::string_view::npos) {
        url = url.substr(0, end);
    }
    while (!url.empty() && url.back() == '/') {
        url.remove_suffix(1);
    }

    auto name = url.substr(url.find_last_of('/') == std::string_view::npos ? 0 : url.find_last_of('/') + 1);
    if (EndsWithCaseInsensitive(name, ".zip")) {
        name.remove_suffix(4);
    }
    if (name.empty()) {
        return "Direct Link"_i18n;
    }
    return std::string{name};
}

auto IsSafeInstallPath(std::string_view path) -> bool {
    if (path.empty() || path.front() != '/' || path.size() >= PATH_MAX || path.find('\\') != std::string_view::npos) {
        return false;
    }

    std::size_t offset{1};
    while (offset <= path.size()) {
        const auto slash = path.find('/', offset);
        const auto segment = path.substr(offset, slash == std::string_view::npos ? path.size() - offset : slash - offset);
        if (segment == "." || segment == ".." || segment.find(':') != std::string_view::npos
            || std::ranges::any_of(segment, [](unsigned char c){ return c < 0x20 || c == 0x7F; })) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }
    return true;
}

auto IsSafeAssetFileName(std::string_view name) -> bool {
    return !name.empty() && name != "." && name != ".."
        && name.size() + std::strlen("/switch/") < PATH_MAX
        && name.find_first_of("/\\:") == std::string_view::npos
        && !std::ranges::any_of(name, [](unsigned char c){ return c < 0x20 || c == 0x7F; });
}

auto IsSafeArchivePath(std::string_view path) -> bool {
    if (path.empty() || path.front() == '/' || path.size() >= PATH_MAX || path.find('\\') != std::string_view::npos) {
        return false;
    }

    std::size_t offset{};
    while (offset <= path.size()) {
        const auto slash = path.find('/', offset);
        const auto segment = path.substr(offset, slash == std::string_view::npos ? path.size() - offset : slash - offset);
        if (segment == "." || segment == ".." || segment.find(':') != std::string_view::npos
            || std::ranges::any_of(segment, [](unsigned char c){ return c < 0x20 || c == 0x7F; })) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }
    return true;
}

auto ValidateDirectArchive(const fs::FsPath& path, const fs::FsPath& install_path, fs::FsNativeSd& fs) -> Result {
    zlib_filefunc64_def file_func;
    mz::FileFuncStdio(&file_func);

    auto zfile = unzOpen2_64(path, &file_func);
    R_UNLESS(zfile, Result_UnzOpen2_64);
    ON_SCOPE_EXIT(unzClose(zfile));

    unz_global_info64 global_info{};
    R_UNLESS(UNZ_OK == unzGetGlobalInfo64(zfile, &global_info), Result_UnzGetGlobalInfo64);
    R_UNLESS(global_info.number_entry && UNZ_OK == unzGoToFirstFile(zfile), Result_UnzGoToFirstFile);

    u64 total_size{};
    for (u64 i = 0; i < global_info.number_entry; ++i) {
        if (i) {
            R_UNLESS(UNZ_OK == unzGoToNextFile(zfile), Result_UnzGoToNextFile);
        }

        unz_file_info64 info{};
        fs::FsPath name{};
        R_UNLESS(UNZ_OK == unzGetCurrentFileInfo64(zfile, &info, name, sizeof(name), nullptr, 0, nullptr, 0), Result_UnzGetCurrentFileInfo64);
        R_UNLESS(info.size_filename < sizeof(name) && info.size_filename == std::strlen(name)
            && install_path.size() + info.size_filename + 1 < PATH_MAX && IsSafeArchivePath(name), Result_GhdlUnsafeArchivePath);
        R_UNLESS(info.uncompressed_size <= std::numeric_limits<u64>::max() - total_size, FsError_UsableSpaceNotEnoughSdCard);
        total_size += info.uncompressed_size;
    }

    s64 free_space{};
    if (R_SUCCEEDED(fs.GetFreeSpace("/", &free_space))) {
        R_UNLESS(total_size <= static_cast<u64>(std::max<s64>(free_space, 0)), FsError_UsableSpaceNotEnoughSdCard);
    }
    R_SUCCEED();
}

auto StripMarkdown(std::string value) -> std::string {
    for (const auto marker : {"~~", "**", "__", "`"}) {
        std::size_t offset{};
        while ((offset = value.find(marker, offset)) != std::string::npos) {
            value.erase(offset, std::strlen(marker));
        }
    }
    return Trim(value);
}

auto SplitMarkdownRow(std::string_view line) -> std::vector<std::string> {
    std::vector<std::string> cells;
    std::size_t start{};
    while (start <= line.size()) {
        const auto end = line.find('|', start);
        cells.emplace_back(Trim(line.substr(start, end == std::string_view::npos ? line.size() - start : end - start)));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (!cells.empty() && cells.front().empty()) {
        cells.erase(cells.begin());
    }
    if (!cells.empty() && cells.back().empty()) {
        cells.pop_back();
    }
    return cells;
}

auto ExtractFirstMarkdownUrl(std::string_view line) -> std::string {
    std::size_t offset{};
    while ((offset = line.find("](", offset)) != std::string_view::npos) {
        const auto begin = offset + 2;
        const auto end = line.find(')', begin);
        if (end == std::string_view::npos) {
            break;
        }
        const auto url = Trim(line.substr(begin, end - begin));
        if (url.starts_with("https://") || url.starts_with("http://")) {
            return url;
        }
        offset = end + 1;
    }
    return {};
}

auto ParseGithubUrl(std::string_view url, std::string& owner, std::string& repo) -> bool {
    owner.clear();
    repo.clear();

    constexpr std::string_view prefixes[]{"https://github.com/", "http://github.com/"};
    bool found{};
    for (const auto prefix : prefixes) {
        if (url.starts_with(prefix)) {
            url.remove_prefix(prefix.size());
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

    const auto slash = url.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return false;
    }
    owner = std::string{url.substr(0, slash)};
    url.remove_prefix(slash + 1);

    const auto repo_end = url.find_first_of("/?#");
    repo = std::string{url.substr(0, repo_end)};
    if (repo.ends_with(".git")) {
        repo.resize(repo.size() - 4);
    }

    const auto valid_component = [](std::string_view value) {
        return !value.empty() && std::ranges::all_of(value, [](unsigned char c){
            return std::isalnum(c) || c == '-' || c == '_' || c == '.';
        });
    };
    if (!valid_component(owner) || !valid_component(repo)) {
        owner.clear();
        repo.clear();
        return false;
    }
    return true;
}

auto IsSameRepository(const Entry& entry, std::string_view owner, std::string_view repo) -> bool {
    return entry.direct_url.empty() && entry.owner.size() == owner.size() && entry.repo.size() == repo.size()
        && !strncasecmp(entry.owner.data(), owner.data(), owner.size())
        && !strncasecmp(entry.repo.data(), repo.data(), repo.size());
}

auto SanitizeFolder(std::string_view value) -> std::string {
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 80));
    for (const auto c : value) {
        if (result.size() == 80) {
            break;
        }
        const auto ch = static_cast<unsigned char>(c);
        result += std::isalnum(ch) || c == '-' || c == '_' || c == '.' ? c : '_';
    }
    while (!result.empty() && result.front() == '.') {
        result.erase(result.begin());
    }
    return result.empty() ? "homebrew" : result;
}

auto GenerateApiUrl(const Entry& e) {
    if (e.tag.empty()) {
        return "https://api.github.com/repos/" + e.owner + "/" + e.repo + "/releases";
    } else if (e.tag == "latest") {
        return "https://api.github.com/repos/" + e.owner + "/" + e.repo + "/releases/latest";
    } else {
        return "https://api.github.com/repos/" + e.owner + "/" + e.repo + "/releases/tags/" + e.tag;
    }
}

auto apiBuildAssetCache(const std::string& url) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%u.json", CACHE_PATH, crc32Calculate(url.data(), url.size()));
    return path;
}

void from_json(yyjson_val* json, AssetEntry& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(name);
        JSON_SET_STR(path);
        JSON_SET_STR(pre_install_message);
        JSON_SET_STR(post_install_message);
    );
}

void from_json(const fs::FsPath& path, Entry& e) {
    JSON_INIT_VEC_FILE(path, nullptr, nullptr);
    JSON_OBJ_ITR(
        JSON_SET_STR(name);
        JSON_SET_STR(url);
        JSON_SET_STR(direct_url);
        JSON_SET_STR(path);
        JSON_SET_STR(owner);
        JSON_SET_STR(repo);
        JSON_SET_STR(tag);
        JSON_SET_STR(catalog);
        JSON_SET_STR(pre_install_message);
        JSON_SET_STR(post_install_message);
        JSON_SET_ARR_OBJ(assets);
    );
}

void from_json(yyjson_val* json, GhApiAsset& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(name);
        JSON_SET_STR(content_type);
        JSON_SET_UINT(size);
        JSON_SET_UINT(download_count);
        JSON_SET_STR(updated_at);
        JSON_SET_STR(browser_download_url);
    );
}

void from_json(yyjson_val* json, GhApiEntry& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(tag_name);
        JSON_SET_STR(name);
        JSON_SET_STR(published_at);
        JSON_SET_BOOL(prerelease);
        JSON_SET_ARR_OBJ(assets);
    );
}

void from_json(const fs::FsPath& path, std::vector<GhApiEntry>& e) {
    JSON_INIT_VEC_FILE(path, nullptr, nullptr);
    if (yyjson_is_arr(json)) {
        JSON_ARR_ITR(e);
    } else {
        e.resize(1);
        from_json(json, e[0]);
    }
}

void LoadDirectLinksFile(std::vector<Entry>& entries) {
    fs::FsNativeSd fs;
    if (R_FAILED(fs.GetFsOpenResult()) || !fs.FileExists(DIRECT_LINKS_PATH)) {
        return;
    }

    std::vector<u8> data;
    if (R_FAILED(fs.read_entire_file(DIRECT_LINKS_PATH, data)) || data.empty()) {
        return;
    }

    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_ALLOW_TRAILING_COMMAS);
    if (!doc) {
        return;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        root = yyjson_obj_get(root, "links");
    }
    if (!yyjson_is_arr(root)) {
        return;
    }

    size_t index, count;
    yyjson_val* value;
    yyjson_arr_foreach(root, index, count, value) {
        if (!yyjson_is_obj(value)) {
            continue;
        }

        auto url_value = yyjson_obj_get(value, "direct_url");
        if (!yyjson_is_str(url_value)) {
            url_value = yyjson_obj_get(value, "url");
        }
        if (!yyjson_is_str(url_value)) {
            continue;
        }

        Entry entry{};
        entry.direct_url = Trim(yyjson_get_str(url_value));
        if (!IsHttpUrl(entry.direct_url)) {
            continue;
        }

        if (const auto name = yyjson_obj_get(value, "name"); yyjson_is_str(name)) {
            entry.name = Trim(yyjson_get_str(name));
        }
        if (entry.name.empty()) {
            entry.name = GetDirectLinkName(entry.direct_url);
        }

        entry.path = "/";
        if (const auto path = yyjson_obj_get(value, "path"); yyjson_is_str(path) && yyjson_get_len(path)) {
            entry.path = yyjson_get_str(path);
        }
        if (!IsSafeInstallPath(entry.path)) {
            continue;
        }

        if (std::ranges::any_of(entries, [&](const auto& existing){ return existing.direct_url == entry.direct_url; })) {
            continue;
        }

        entry.owner = "Direct";
        entry.repo = entry.name;
        entry.json_path = DIRECT_LINKS_PATH;
        entry.saved_direct_link = true;
        entries.emplace_back(std::move(entry));
    }
}

auto WriteDirectLinksFile(const std::vector<Entry>& entries) -> Result {
    auto doc = yyjson_mut_doc_new(nullptr);
    R_UNLESS(doc, Result_FsStdioFailedToWrite);
    ON_SCOPE_EXIT(yyjson_mut_doc_free(doc));

    auto root = yyjson_mut_arr(doc);
    R_UNLESS(root, Result_FsStdioFailedToWrite);
    yyjson_mut_doc_set_root(doc, root);

    for (const auto& entry : entries) {
        if (!entry.saved_direct_link || !IsHttpUrl(entry.direct_url)) {
            continue;
        }

        auto object = yyjson_mut_arr_add_obj(doc, root);
        R_UNLESS(object, Result_FsStdioFailedToWrite);
        R_UNLESS(yyjson_mut_obj_add_strcpy(doc, object, "name", entry.name.c_str()), Result_FsStdioFailedToWrite);
        R_UNLESS(yyjson_mut_obj_add_strcpy(doc, object, "direct_url", entry.direct_url.c_str()), Result_FsStdioFailedToWrite);
        if (!entry.path.empty() && entry.path != "/") {
            R_UNLESS(yyjson_mut_obj_add_strcpy(doc, object, "path", entry.path.c_str()), Result_FsStdioFailedToWrite);
        }
    }

    size_t json_size{};
    auto json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &json_size);
    R_UNLESS(json, Result_FsStdioFailedToWrite);
    ON_SCOPE_EXIT(std::free(json));

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    if (const auto rc = fs.CreateDirectoryRecursively(DIRECT_LINKS_DIRECTORY); R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
        return rc;
    }
    return fs.write_entire_file(DIRECT_LINKS_PATH, std::span<const u8>{reinterpret_cast<const u8*>(json), json_size});
}

void LoadRepositoriesFile(std::vector<Entry>& entries) {
    fs::FsNativeSd fs;
    if (R_FAILED(fs.GetFsOpenResult()) || !fs.FileExists(REPOSITORIES_PATH)) {
        return;
    }

    std::vector<u8> data;
    if (R_FAILED(fs.read_entire_file(REPOSITORIES_PATH, data)) || data.empty()) {
        return;
    }

    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_ALLOW_TRAILING_COMMAS);
    if (!doc) {
        return;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        root = yyjson_obj_get(root, "repositories");
    }
    if (!yyjson_is_arr(root)) {
        return;
    }

    size_t index, count;
    yyjson_val* value;
    yyjson_arr_foreach(root, index, count, value) {
        if (!yyjson_is_obj(value)) {
            continue;
        }

        const auto url_value = yyjson_obj_get(value, "url");
        if (!yyjson_is_str(url_value)) {
            continue;
        }

        Entry entry{};
        entry.url = Trim(yyjson_get_str(url_value));
        if (!ParseGithubUrl(entry.url, entry.owner, entry.repo)
            || std::ranges::any_of(entries, [&](const auto& existing){ return IsSameRepository(existing, entry.owner, entry.repo); })) {
            continue;
        }

        entry.url = "https://github.com/" + entry.owner + "/" + entry.repo;
        if (const auto name = yyjson_obj_get(value, "name"); yyjson_is_str(name)) {
            entry.name = Trim(yyjson_get_str(name));
        }
        if (const auto tag = yyjson_obj_get(value, "tag"); yyjson_is_str(tag)) {
            entry.tag = Trim(yyjson_get_str(tag));
        }
        entry.json_path = REPOSITORIES_PATH;
        entry.saved_repository = true;
        entries.emplace_back(std::move(entry));
    }
}

auto WriteRepositoriesFile(const std::vector<Entry>& entries) -> Result {
    auto doc = yyjson_mut_doc_new(nullptr);
    R_UNLESS(doc, Result_FsStdioFailedToWrite);
    ON_SCOPE_EXIT(yyjson_mut_doc_free(doc));

    auto root = yyjson_mut_arr(doc);
    R_UNLESS(root, Result_FsStdioFailedToWrite);
    yyjson_mut_doc_set_root(doc, root);

    for (const auto& entry : entries) {
        if (!entry.saved_repository || entry.owner.empty() || entry.repo.empty()) {
            continue;
        }

        auto object = yyjson_mut_arr_add_obj(doc, root);
        R_UNLESS(object, Result_FsStdioFailedToWrite);
        const auto url = "https://github.com/" + entry.owner + "/" + entry.repo;
        R_UNLESS(yyjson_mut_obj_add_strcpy(doc, object, "url", url.c_str()), Result_FsStdioFailedToWrite);
        if (!entry.name.empty()) {
            R_UNLESS(yyjson_mut_obj_add_strcpy(doc, object, "name", entry.name.c_str()), Result_FsStdioFailedToWrite);
        }
        if (!entry.tag.empty()) {
            R_UNLESS(yyjson_mut_obj_add_strcpy(doc, object, "tag", entry.tag.c_str()), Result_FsStdioFailedToWrite);
        }
    }

    size_t json_size{};
    auto json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &json_size);
    R_UNLESS(json, Result_FsStdioFailedToWrite);
    ON_SCOPE_EXIT(std::free(json));

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    if (const auto rc = fs.CreateDirectoryRecursively(DIRECT_LINKS_DIRECTORY); R_FAILED(rc) && rc != FsError_PathAlreadyExists) {
        return rc;
    }
    return fs.write_entire_file(REPOSITORIES_PATH, std::span<const u8>{reinterpret_cast<const u8*>(json), json_size});
}

auto ParseSwitchPortsCatalog(std::string_view markdown, std::vector<SwitchPortCategory>& catalog) -> bool {
    catalog.clear();
    SwitchPortCategory* current{};
    std::string previous_title;

    std::size_t offset{};
    while (offset <= markdown.size()) {
        const auto newline = markdown.find('\n', offset);
        auto line = markdown.substr(offset, newline == std::string_view::npos ? markdown.size() - offset : newline - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const auto trimmed = Trim(line);

        if (trimmed.starts_with("### ")) {
            auto category_name = StripMarkdown(trimmed.substr(4));
            if (!category_name.empty()) {
                catalog.push_back({.name = std::move(category_name)});
                current = &catalog.back();
                previous_title.clear();
            }
        } else if (current && trimmed.starts_with("|") && trimmed.find('|', 1) != std::string::npos) {
            const auto cells = SplitMarkdownRow(trimmed);
            const bool emulators = !strcasecmp(current->name.c_str(), "Emulators");
            const auto title_index = emulators ? 1U : 0U;
            const auto version_index = emulators ? 2U : 1U;
            if (cells.size() > version_index) {
                const auto raw_title = cells[title_index];
                const auto lowered_title = Lower(raw_title);
                if (lowered_title != "title" && lowered_title != "game" && lowered_title.find("---") == std::string::npos && !raw_title.starts_with("~~")) {
                    const auto source_url = ExtractFirstMarkdownUrl(trimmed);
                    if (!source_url.empty()) {
                        auto title = StripMarkdown(raw_title);
                        if (title.empty()) {
                            title = previous_title;
                        } else {
                            previous_title = title;
                        }

                        if (!title.empty()) {
                            SwitchPortEntry entry{
                                .category = current->name,
                                .name = std::move(title),
                                .version = StripMarkdown(cells[version_index]),
                                .source_url = source_url,
                            };
                            ParseGithubUrl(entry.source_url, entry.owner, entry.repo);
                            entry.install_folder = SanitizeFolder(entry.repo.empty() ? entry.name : entry.repo);
                            if (entry.source_url.starts_with("https://flyinghead.github.io/flycast-builds")) {
                                entry.install_folder = "flycast";
                            } else if (entry.source_url.starts_with("https://eliasoenal.com/2020/07/12/commander-keen")) {
                                entry.install_folder = "CommanderGenius";
                            }

                            if (emulators && !cells[0].empty()) {
                                entry.name = "[" + StripMarkdown(cells[0]) + "] " + entry.name;
                            }
                            // A blank title denotes an alternate port of the
                            // preceding game. Include the repository so both
                            // choices remain distinguishable.
                            if (raw_title.empty() && !entry.repo.empty()) {
                                entry.name += " (" + entry.owner + "/" + entry.repo + ")";
                            }
                            current->entries.emplace_back(std::move(entry));
                        }
                    }
                }
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }

    std::erase_if(catalog, [](const auto& category){ return category.entries.empty(); });
    return !catalog.empty();
}

auto NormalizeArchivePath(std::string_view archive_name, std::string_view install_folder, fs::FsPath& output) -> bool {
    std::string normalized{archive_name};
    std::ranges::replace(normalized, '\\', '/');
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    if (normalized.empty() || normalized.size() >= PATH_MAX - 32) {
        return false;
    }

    const bool directory = normalized.back() == '/';
    std::vector<std::string_view> segments;
    std::size_t offset{};
    while (offset < normalized.size()) {
        const auto slash = normalized.find('/', offset);
        const auto segment = std::string_view{normalized}.substr(offset, slash == std::string::npos ? normalized.size() - offset : slash - offset);
        if (!segment.empty()) {
            if (segment == "." || segment == ".." || segment.find(':') != std::string_view::npos
                || std::ranges::any_of(segment, [](unsigned char c){ return c < 0x20; })) {
                return false;
            }
            segments.push_back(segment);
        }
        if (slash == std::string::npos) {
            break;
        }
        offset = slash + 1;
    }
    if (segments.empty()) {
        return false;
    }

    const auto is_sd_root = [](std::string_view segment) {
        return !strcasecmp(std::string{segment}.c_str(), "switch")
            || !strcasecmp(std::string{segment}.c_str(), "atmosphere")
            || !strcasecmp(std::string{segment}.c_str(), "config")
            || !strcasecmp(std::string{segment}.c_str(), "themes")
            || !strcasecmp(std::string{segment}.c_str(), "bootloader");
    };

    // Release archives sometimes wrap an SD-card layout in a versioned
    // directory. Start from the first known root so switch/ and atmosphere/
    // land where the package author intended.
    std::size_t root_index = segments.size();
    if (is_sd_root(segments.front())) {
        root_index = 0;
    } else {
        // A single release wrapper around switch/ or atmosphere/ is common.
        // Do not treat an arbitrary nested "config" data folder as an SD root.
        for (std::size_t i = 1; i < std::min<std::size_t>(segments.size(), 3); ++i) {
            const auto segment = Lower(segments[i]);
            if (segment == "switch" || segment == "atmosphere" || segment == "bootloader") {
                root_index = i;
                break;
            }
        }
    }

    std::string destination;
    if (root_index != segments.size()) {
        destination = "/";
        for (std::size_t i = root_index; i < segments.size(); ++i) {
            if (i != root_index) destination += '/';
            destination += segments[i];
        }
    } else if (!strcasecmp(std::string{segments.front()}.c_str(), std::string{install_folder}.c_str())
        || EndsWithCaseInsensitive(segments.front(), "_nx")
        || EndsWithCaseInsensitive(segments.front(), "-nx")
        || (segments.size() > 1 && EndsWithCaseInsensitive(segments[1], ".nro"))) {
        destination = "/switch/";
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (i) destination += '/';
            destination += segments[i];
        }
    } else {
        destination = "/switch/" + std::string{install_folder} + "/";
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (i) destination += '/';
            destination += segments[i];
        }
    }

    if (directory) {
        destination += '/';
    }
    if (destination.size() >= sizeof(output.s)) {
        return false;
    }
    output = destination;
    return true;
}

auto DownloadApp(ProgressBox* pbox, const GhApiAsset& gh_asset, const AssetEntry* entry, bool validate_archive = false) -> Result {
    static const fs::FsPath temp_file{"/switch/sphaira/cache/github/ghdl.temp"};

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    ON_SCOPE_EXIT(fs.DeleteFile(temp_file));

    R_UNLESS(!gh_asset.browser_download_url.empty(), Result_GhdlEmptyAsset);

    // 2. download the asset
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", gh_asset.name));
        log_write("starting download: %s\n", gh_asset.browser_download_url.c_str());

        const auto result = curl::Api().ToFile(
            curl::Url{gh_asset.browser_download_url},
            curl::Path{temp_file},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        R_UNLESS(result.success, Result_GhdlFailedToDownloadAsset);
    }
    R_TRY(pbox->ShouldExitResult());

    const auto is_archive = EndsWithCaseInsensitive(gh_asset.name, ".zip")
        || gh_asset.content_type.find("zip") != gh_asset.content_type.npos;
    fs::FsPath root_path{"/"};
    if (entry && !entry->path.empty()) {
        root_path = entry->path;
    } else if (!is_archive) {
        R_UNLESS(IsSafeAssetFileName(gh_asset.name), Result_GhdlUnsafeArchivePath);
        root_path = fs::AppendPath("/switch", gh_asset.name);
    }

    // 3. extract the zip / file
    if (is_archive) {
        log_write("found zip\n");
        if (validate_archive) {
            R_TRY(ValidateDirectArchive(temp_file, root_path, fs));
        }
        R_TRY(thread::TransferUnzipAll(pbox, temp_file, &fs, root_path));
    } else {
        R_TRY(fs.CreateDirectoryRecursivelyWithPath(root_path));
        fs.DeleteFile(root_path);
        R_TRY(fs.RenameFile(temp_file, root_path));
    }

    log_write("success\n");
    R_SUCCEED();
}

auto DownloadReleaseJsonJson(ProgressBox* pbox, const std::string& url, std::vector<GhApiEntry>& out) -> Result {
    // 1. download the json
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Downloading json"_i18n);
        log_write("starting download\n");

        const auto path = apiBuildAssetCache(url);

        const auto result = curl::Api().ToFile(
            curl::Url{url},
            curl::Path{path},
            curl::OnProgress{pbox->OnDownloadProgressCallback()},
            curl::Flags{curl::Flag_Cache},
            curl::Header{
                { "Accept", "application/vnd.github+json" },
            }
        );

        R_UNLESS(result.success, Result_GhdlFailedToDownloadAssetJson);
        from_json(result.path, out);
    }

    R_UNLESS(!out.empty(), Result_GhdlEmptyAsset);
    R_SUCCEED();
}

auto DownloadSwitchPortsCatalog(ProgressBox* pbox, std::vector<SwitchPortCategory>& catalog) -> Result {
    pbox->NewTransfer("Downloading Switch Ports catalog"_i18n);
    const auto result = curl::Api().ToFile(
        curl::Url{SWITCHPORTS_README_URL},
        curl::Path{SWITCHPORTS_CACHE_PATH},
        curl::OnProgress{pbox->OnDownloadProgressCallback()},
        curl::Flags{curl::Flag_Cache}
    );
    R_UNLESS(result.success, Result_GhdlFailedToDownloadAssetJson);

    std::vector<u8> data;
    R_TRY(fs::FsNativeSd().read_entire_file(result.path, data));
    R_UNLESS(!data.empty() && data.size() <= SWITCHPORTS_README_MAX_SIZE, Result_GhdlEmptyAsset);
    R_UNLESS(ParseSwitchPortsCatalog(
        std::string_view{reinterpret_cast<const char*>(data.data()), data.size()}, catalog
    ), Result_GhdlEmptyAsset);
    R_SUCCEED();
}

auto IsInstallableSwitchPortAsset(const GhApiAsset& asset) -> bool {
    if (!EndsWithCaseInsensitive(asset.name, ".zip") && !EndsWithCaseInsensitive(asset.name, ".nro")) {
        return false;
    }
    const auto name = Lower(asset.name);
    if (name.find("symbols") != std::string::npos
        || name.find("debug") != std::string::npos
        || name.find("source-code") != std::string::npos
        || name.find("source_code") != std::string::npos
        || name.starts_with("source.")) {
        return false;
    }
    if (EndsWithCaseInsensitive(name, ".nro")) {
        return true;
    }

    const bool switch_hint = name.find("switch") != std::string::npos
        || name.find("_nx") != std::string::npos
        || name.find("-nx") != std::string::npos;
    if (!switch_hint && (name.find("windows") != std::string::npos
        || name.find("win64") != std::string::npos
        || name.find("linux") != std::string::npos
        || name.find("macos") != std::string::npos
        || name.find("mac-") != std::string::npos
        || name.find("android") != std::string::npos)) {
        return false;
    }
    return true;
}

auto IsSupportedExternalSwitchPort(const SwitchPortEntry& port) -> bool {
    return port.source_url.starts_with("https://flyinghead.github.io/flycast-builds")
        || port.source_url.starts_with("https://eliasoenal.com/2020/07/12/commander-keen");
}

auto XmlElement(std::string_view block, std::string_view name) -> std::string {
    const auto open = "<" + std::string{name} + ">";
    const auto close = "</" + std::string{name} + ">";
    const auto begin = block.find(open);
    if (begin == std::string_view::npos) return {};
    const auto value_begin = begin + open.size();
    const auto end = block.find(close, value_begin);
    if (end == std::string_view::npos) return {};
    return std::string{block.substr(value_begin, end - value_begin)};
}

auto ResolveExternalSwitchPort(ProgressBox* pbox, const SwitchPortEntry& port, GhApiAsset& asset) -> Result {
    pbox->NewTransfer("Resolving port download"_i18n);
    if (port.source_url.starts_with("https://flyinghead.github.io/flycast-builds")) {
        constexpr auto bucket_url = "https://flycast-builds.s3.fr-par.scw.cloud/";
        const auto listing_url = std::string{bucket_url} + "?prefix=switch%2Fheads%2Fmaster-";
        const auto result = curl::Api().ToMemory(
            curl::Url{listing_url},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );
        R_UNLESS(result.success && !result.data.empty(), Result_GhdlFailedToDownloadAssetJson);

        const std::string_view xml{reinterpret_cast<const char*>(result.data.data()), result.data.size()};
        std::string latest_key;
        std::string latest_date;
        std::size_t offset{};
        while ((offset = xml.find("<Contents>", offset)) != std::string_view::npos) {
            const auto end = xml.find("</Contents>", offset);
            if (end == std::string_view::npos) break;
            const auto block = xml.substr(offset, end + std::strlen("</Contents>") - offset);
            const auto key = XmlElement(block, "Key");
            const auto date = XmlElement(block, "LastModified");
            if (key.starts_with("switch/heads/master-") && EndsWithCaseInsensitive(key, ".zip")
                && (latest_date.empty() || date > latest_date)) {
                latest_key = key;
                latest_date = date;
            }
            offset = end + std::strlen("</Contents>");
        }
        R_UNLESS(!latest_key.empty(), Result_GhdlEmptyAsset);
        asset.name = latest_key.substr(latest_key.find_last_of('/') + 1);
        asset.content_type = "application/zip";
        asset.browser_download_url = std::string{bucket_url} + latest_key;
        R_SUCCEED();
    }

    if (port.source_url.starts_with("https://eliasoenal.com/2020/07/12/commander-keen")) {
        const auto result = curl::Api().ToMemory(
            curl::Url{port.source_url},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );
        R_UNLESS(result.success && !result.data.empty(), Result_GhdlFailedToDownloadAssetJson);

        const std::string html{reinterpret_cast<const char*>(result.data.data()), result.data.size()};
        const auto lowered = Lower(html);
        std::size_t zip_offset{};
        while ((zip_offset = lowered.find(".zip", zip_offset)) != std::string::npos) {
            const auto href = lowered.rfind("href=", zip_offset);
            if (href == std::string::npos || href + 5 >= html.size()) break;
            const auto quote = html[href + 5];
            if (quote != '\'' && quote != '"') {
                zip_offset += 4;
                continue;
            }
            const auto begin = href + 6;
            const auto end = html.find(quote, begin);
            if (end == std::string::npos || end < zip_offset) break;
            auto url = html.substr(begin, end - begin);
            const auto lowered_url = Lower(url);
            if ((url.starts_with("https://") || url.starts_with("http://"))
                && (lowered_url.find("nsw") != std::string::npos || lowered_url.find("switch") != std::string::npos)) {
                asset.name = url.substr(url.find_last_of('/') + 1);
                asset.content_type = "application/zip";
                asset.browser_download_url = std::move(url);
                R_SUCCEED();
            }
            zip_offset = end + 1;
        }
    }

    R_THROW(Result_GhdlEmptyAsset);
}

auto InstallSwitchPort(ProgressBox* pbox, const SwitchPortEntry& port, const GhApiAsset& asset) -> Result {
    static const fs::FsPath temp_file{"/switch/sphaira/cache/github/switchports.temp"};
    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    fs.DeleteFile(temp_file);
    ON_SCOPE_EXIT(fs.DeleteFile(temp_file));

    R_UNLESS(!asset.browser_download_url.empty(), Result_GhdlEmptyAsset);
    pbox->NewTransfer(i18n::Reorder("Downloading ", asset.name));
    const auto result = curl::Api().ToFile(
        curl::Url{asset.browser_download_url},
        curl::Path{temp_file},
        curl::OnProgress{pbox->OnDownloadProgressCallback()}
    );
    R_UNLESS(result.success, Result_GhdlFailedToDownloadAsset);
    R_TRY(pbox->ShouldExitResult());

    if (EndsWithCaseInsensitive(asset.name, ".zip")) {
        std::size_t extracted{};
        std::size_t skipped{};
        R_TRY(thread::TransferUnzipAll(pbox, temp_file, &fs, "/", [&](const fs::FsPath& name, fs::FsPath& path) {
            if (!NormalizeArchivePath(name, port.install_folder, path)) {
                ++skipped;
                log_write("[GH] skipped unsafe SwitchPorts archive path: %s\n", name.s);
                return false;
            }
            ++extracted;
            return true;
        }));
        log_write("[GH] SwitchPorts archive extracted=%zu skipped=%zu\n", extracted, skipped);
        R_UNLESS(extracted, Result_GhdlEmptyAsset);
        R_UNLESS(!skipped, Result_GhdlFailedToDownloadAsset);
    } else {
        auto file_name = asset.name;
        const auto slash = file_name.find_last_of("/\\");
        if (slash != std::string::npos) {
            file_name.erase(0, slash + 1);
        }
        R_UNLESS(EndsWithCaseInsensitive(file_name, ".nro"), Result_GhdlEmptyAsset);

        auto folder = file_name.substr(0, file_name.size() - 4);
        folder = SanitizeFolder(folder.empty() ? port.install_folder : folder);
        file_name = folder + ".nro";
        fs::FsPath folder_path;
        std::snprintf(folder_path, sizeof(folder_path), "/switch/%s", folder.c_str());
        fs.CreateDirectoryRecursively(folder_path);

        const auto output = fs::AppendPath(folder_path, fs::FsPath{file_name});
        fs::FsPath backup;
        std::snprintf(backup, sizeof(backup), "%s.sphaira.bak", output.s);
        const bool had_previous = fs.FileExists(output);
        if (had_previous) {
            fs.DeleteFile(backup);
            R_TRY(fs.RenameFile(output, backup));
        }
        const auto install_result = fs.RenameFile(temp_file, output);
        if (R_FAILED(install_result)) {
            if (had_previous) {
                fs.RenameFile(backup, output);
            }
            return install_result;
        }
        if (had_previous) {
            fs.DeleteFile(backup);
        }
        log_write("[GH] installed SwitchPorts NRO to %s\n", output.s);
    }

    R_SUCCEED();
}

void StartSwitchPortInstall(const SwitchPortEntry& port, const GhApiAsset& asset) {
    auto destination = "/switch/" + port.install_folder;
    if (EndsWithCaseInsensitive(asset.name, ".nro")) {
        auto stem = asset.name.substr(0, asset.name.size() - 4);
        destination = "/switch/" + SanitizeFolder(stem);
    }

    const auto prompt = i18n::Reorder("Install ", port.name) + "\n\n" +
        "Destination: "_i18n + destination + "\n\n" +
        "Only the homebrew port is downloaded. Original game data may still be required."_i18n;
    App::Push<OptionBox>(prompt, "Back"_i18n, "Install"_i18n, 1, [port, asset](auto op_index) {
        if (!op_index || !*op_index) {
            return;
        }
        App::Push<ProgressBox>(0, "Downloading "_i18n, port.name, [port, asset](auto pbox) -> Result {
            return InstallSwitchPort(pbox, port, asset);
        }, [port](Result rc) {
            homebrew::SignalChange();
            App::PushErrorBox(rc, "Failed to download app!"_i18n);
            if (R_SUCCEEDED(rc)) {
                App::Notify(i18n::Reorder("Downloaded ", port.name));
            }
        });
    });
}

void DownloadSwitchPort(const SwitchPortEntry& port) {
    auto releases = std::make_shared<std::vector<GhApiEntry>>();
    const auto url = "https://api.github.com/repos/" + port.owner + "/" + port.repo + "/releases";
    App::Push<ProgressBox>(0, "Downloading "_i18n, port.name, [url, releases](auto pbox) -> Result {
        return DownloadReleaseJsonJson(pbox, url, *releases);
    }, [port, releases](Result rc) {
        App::PushErrorBox(rc, "Failed to download json"_i18n);
        if (R_FAILED(rc)) {
            return;
        }

        std::vector<GhApiAsset> assets;
        std::string release_name;
        // GitHub returns releases newest-first. Choose the newest release
        // that actually publishes a Switch-installable ZIP or NRO.
        for (const auto& release : *releases) {
            for (const auto& asset : release.assets) {
                if (IsInstallableSwitchPortAsset(asset)) {
                    assets.push_back(asset);
                }
            }
            if (!assets.empty()) {
                release_name = release.tag_name.empty() ? release.name : release.tag_name;
                break;
            }
        }

        if (assets.empty()) {
            App::Push<OptionBox>(
                "No installable ZIP or NRO release asset was found for this port."_i18n +
                "\n\n" + port.source_url,
                "OK"_i18n
            );
            return;
        }
        if (assets.size() == 1) {
            StartSwitchPortInstall(port, assets.front());
            return;
        }

        PopupList::Items items;
        for (const auto& asset : assets) {
            items.push_back(asset.name);
        }
        auto title = "Select asset to download for "_i18n + port.name;
        if (!release_name.empty()) {
            title += " [" + release_name + "]";
        }
        App::Push<PopupList>(title, items, [port, assets](auto op_index) {
            if (op_index && *op_index < assets.size()) {
                StartSwitchPortInstall(port, assets[*op_index]);
            }
        });
    });
}

void DownloadExternalSwitchPort(const SwitchPortEntry& port) {
    auto asset = std::make_shared<GhApiAsset>();
    App::Push<ProgressBox>(0, "Downloading "_i18n, port.name, [port, asset](auto pbox) -> Result {
        return ResolveExternalSwitchPort(pbox, port, *asset);
    }, [port, asset](Result rc) {
        App::PushErrorBox(rc, "Failed to resolve port download"_i18n);
        if (R_SUCCEEDED(rc)) {
            StartSwitchPortInstall(port, *asset);
        }
    });
}

void ShowSwitchPortsCategory(const std::shared_ptr<std::vector<SwitchPortCategory>>& catalog, std::size_t category_index) {
    if (category_index >= catalog->size()) {
        return;
    }
    const auto& category = (*catalog)[category_index];
    auto options = std::make_unique<Sidebar>(category.name, Sidebar::Side::RIGHT);
    for (const auto& port : category.entries) {
        auto label = port.name;
        if (!port.version.empty() && port.version != "???") {
            label += " [" + port.version + "]";
        }
        auto info = port.source_url;
        if (!port.owner.empty() || IsSupportedExternalSwitchPort(port)) {
            info += "\n\n" + "Default install folder: "_i18n + "/switch/" + port.install_folder;
        }
        auto item = options->Add<SidebarEntryCallback>(label, [port] {
            if (!port.owner.empty() && !port.repo.empty()) {
                DownloadSwitchPort(port);
            } else {
                DownloadExternalSwitchPort(port);
            }
        }, info);
        if ((port.owner.empty() || port.repo.empty()) && !IsSupportedExternalSwitchPort(port)) {
            item->Depends([]{ return false; },
                "This catalog entry does not point to a GitHub release repository."_i18n,
                [url = port.source_url] {
                    App::Push<OptionBox>("No direct GitHub release is available for this entry.\n\n"_i18n + url, "OK"_i18n);
                }
            );
        }
    }
    App::Push(std::move(options));
}

void ShowSwitchPortsCatalog(const std::shared_ptr<std::vector<SwitchPortCategory>>& catalog) {
    auto options = std::make_unique<Sidebar>("Switch Ports"_i18n, Sidebar::Side::RIGHT);
    for (std::size_t i = 0; i < catalog->size(); ++i) {
        const auto& category = (*catalog)[i];
        options->Add<SidebarEntryCallback>(
            category.name + " (" + std::to_string(category.entries.size()) + ")",
            [catalog, i] { ShowSwitchPortsCategory(catalog, i); },
            "Browse ports from this category."_i18n
        );
    }
    App::Push(std::move(options));
}

void OpenSwitchPortsCatalog() {
    auto catalog = std::make_shared<std::vector<SwitchPortCategory>>();
    App::Push<ProgressBox>(0, "Downloading "_i18n, "Switch Ports", [catalog](auto pbox) -> Result {
        return DownloadSwitchPortsCatalog(pbox, *catalog);
    }, [catalog](Result rc) {
        App::PushErrorBox(rc, "Failed to download Switch Ports catalog"_i18n);
        if (R_SUCCEEDED(rc)) {
            ShowSwitchPortsCatalog(catalog);
        }
    });
}

void DownloadDirectEntry(const Entry& entry, const std::function<void()>& on_success = {}) {
    const auto start_download = [entry, on_success]() {
        GhApiAsset asset{};
        asset.name = entry.name + ".zip";
        asset.content_type = "application/zip";
        asset.browser_download_url = entry.direct_url;

        AssetEntry install{};
        install.path = entry.path.empty() ? "/" : entry.path;

        App::Push<ProgressBox>(0, "Downloading "_i18n, entry.name, [asset, install](auto pbox) -> Result {
            return DownloadApp(pbox, asset, &install, true);
        }, [entry, on_success](Result rc) {
            auto error = "Failed to download app!"_i18n;
            if (rc == Result_UnzOpen2_64) {
                error = "The downloaded file is not a valid ZIP archive."_i18n;
            } else if (rc == Result_GhdlUnsafeArchivePath) {
                error = "The ZIP contains an unsafe file path."_i18n;
            } else if (rc == FsError_UsableSpaceNotEnoughSdCard) {
                error = "There is not enough free space on the SD card."_i18n;
            }
            App::PushErrorBox(rc, error);
            if (R_FAILED(rc)) {
                return;
            }

            homebrew::SignalChange();
            App::Notify(i18n::Reorder("Downloaded ", entry.name));

            if (!entry.post_install_message.empty()) {
                App::Push<OptionBox>(entry.post_install_message, "OK"_i18n, [on_success](auto) {
                    if (on_success) {
                        on_success();
                    }
                });
            } else if (on_success) {
                on_success();
            }
        });
    };

    if (!entry.pre_install_message.empty()) {
        App::Push<OptionBox>(
            entry.pre_install_message,
            "Back"_i18n,
            "Download"_i18n,
            0,
            [start_download](auto index) {
                if (index && *index == 1) {
                    start_download();
                }
            }
        );
    } else {
        start_download();
    }
}

} // namespace

Menu::Menu(u32 flags) : MenuBase{"GitHub"_i18n, flags} {
    fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);

    this->SetActions(
        std::make_pair(Button::A, Action{"Download"_i18n, [this](){
            if (m_entries.empty()) {
                return;
            }

            if (GetEntry().catalog == "switchports") {
                OpenSwitchPortsCatalog();
            } else {
                DownloadEntries(GetEntry());
            }
        }}),

        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),

        std::make_pair(Button::Y, Action{"Add"_i18n, [this](){
            PromptAdd();
        }})
    );

    const Vec4 v{75, GetY() + 1.f + 42.f, 1220.f-45.f*2, 60};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
}

Menu::~Menu() {
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);
    m_list->OnUpdate(controller, touch, m_index, m_entries.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_entries.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty..."_i18n.c_str());
        return;
    }

    constexpr float text_xoffset{15.f};

    m_list->Draw(vg, theme, m_entries.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        auto& e = m_entries[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_entries.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        nvgSave(vg);
        nvgIntersectScissor(vg, x + text_xoffset, y, w-(x+text_xoffset+50), h);
            if (e.name.empty()) {
                gfx::drawTextArgs(vg, x + text_xoffset, y + (h / 2.f), 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), "%s By %s", e.repo.c_str(), e.owner.c_str());
            } else {
                gfx::drawText(vg, x + text_xoffset, y + (h / 2.f), 20.f, theme->GetColour(text_id), e.name.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            }
        nvgRestore(vg);

        if (!e.tag.empty()) {
            gfx::drawTextArgs(vg, x + w - text_xoffset, y + (h / 2.f), 16.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "version: %s"_i18n.c_str(), e.tag.c_str());
        }
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    if (m_entries.empty()) {
        Scan();
    }
}

void Menu::SetIndex(s64 index) {
    if (m_entries.empty()) {
        m_index = 0;
        SetTitleSubHeading("");
        RemoveAction(Button::X);
        UpdateSubheading();
        return;
    }
    index = std::clamp<s64>(index, 0, m_entries.size() - 1);
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    const auto& entry = m_entries[m_index];
    SetTitleSubHeading(!entry.direct_url.empty() ? entry.direct_url : entry.saved_repository ? entry.url : entry.json_path.toString());
    if (entry.saved_direct_link || entry.saved_repository) {
        SetAction(Button::X, Action{"Remove"_i18n, [this](){
            if (GetEntry().saved_direct_link) {
                RemoveSelectedDirectLink();
            } else {
                RemoveSelectedRepository();
            }
        }});
    } else {
        RemoveAction(Button::X);
    }
    UpdateSubheading();
}

void Menu::Scan() {
    m_entries.clear();

    // load from romfs first
    if (R_SUCCEEDED(romfsInit())) {
        LoadEntriesFromPath("romfs:/github/");
        romfsExit();
    }

    // then load custom entries
    LoadEntriesFromPath("/config/sphaira/github/");
    LoadDirectLinks();
    LoadRepositories();
    Sort();
    SetIndex(0);
}

void Menu::LoadEntriesFromPath(const fs::FsPath& path) {
    auto dir = opendir(path);
    if (!dir) {
        return;
    }
    ON_SCOPE_EXIT(closedir(dir));

    while (auto d = readdir(dir)) {
        if (d->d_name[0] == '.') {
            continue;
        }

        if (d->d_type != DT_REG) {
            continue;
        }

        const auto ext = std::strrchr(d->d_name, '.');
        if (!ext || strcasecmp(ext, ".json")) {
            continue;
        }

        Entry entry{};
        const auto full_path = fs::AppendPath(path, d->d_name);
        from_json(full_path, entry);

        // parse owner and author from url (if needed).
        if (!entry.url.empty() && (entry.owner.empty() || entry.repo.empty())) {
            ParseGithubUrl(entry.url, entry.owner, entry.repo);
        }

        if (!entry.direct_url.empty()) {
            entry.direct_url = Trim(entry.direct_url);
            if (!IsHttpUrl(entry.direct_url)) {
                continue;
            }
            if (entry.name.empty()) {
                entry.name = GetDirectLinkName(entry.direct_url);
            }
            if (entry.path.empty()) {
                entry.path = "/";
            }
            if (!IsSafeInstallPath(entry.path)) {
                continue;
            }
            entry.owner = "Direct";
            entry.repo = entry.name;
        } else if (entry.owner.empty() || entry.repo.empty()) {
            continue;
        }

        entry.json_path = full_path;
        m_entries.emplace_back(entry);
    }
}

void Menu::LoadDirectLinks() {
    LoadDirectLinksFile(m_entries);
}

void Menu::LoadRepositories() {
    LoadRepositoriesFile(m_entries);
}

void Menu::PromptAdd() {
    PopupList::Items items{
        "GitHub Repository"_i18n,
        "Direct Link"_i18n,
    };
    App::Push<PopupList>("Add"_i18n, items, [this](auto index){
        if (!index) {
            return;
        }
        if (*index == 0) {
            PromptRepository();
        } else if (*index == 1) {
            PromptDirectLink();
        }
    });
}

void Menu::PromptDirectLink() {
    std::string url;
    if (R_FAILED(swkbd::ShowText(
        url,
        "Direct Link"_i18n.c_str(),
        "Enter a direct ZIP URL"_i18n.c_str(),
        "https://",
        8,
        PATH_MAX - 1
    ))) {
        return;
    }

    url = Trim(url);
    if (!IsHttpUrl(url)) {
        App::Notify("Enter a valid HTTP or HTTPS URL"_i18n);
        return;
    }

    Entry entry{};
    entry.name = GetDirectLinkName(url);
    entry.direct_url = std::move(url);
    entry.path = "/";
    entry.owner = "Direct";
    entry.repo = entry.name;

    DownloadDirectEntry(entry, [this, entry]() {
        if (std::ranges::any_of(m_entries, [&](const auto& existing){ return existing.direct_url == entry.direct_url; })) {
            return;
        }

        App::Push<OptionBox>(
            "Save this direct link?"_i18n,
            "No"_i18n,
            "Yes"_i18n,
            0,
            [this, entry](auto index) {
                if (index && *index == 1) {
                    SaveDirectLink(entry);
                }
            }
        );
    });
}

void Menu::PromptRepository() {
    std::string url;
    if (R_FAILED(swkbd::ShowText(
        url,
        "GitHub Repository"_i18n.c_str(),
        "Enter a GitHub repository URL"_i18n.c_str(),
        "https://github.com/",
        19,
        PATH_MAX - 1
    ))) {
        return;
    }

    url = Trim(url);
    Entry entry{};
    if (!ParseGithubUrl(url, entry.owner, entry.repo)) {
        App::Notify("Enter a valid GitHub repository URL"_i18n);
        return;
    }

    entry.url = "https://github.com/" + entry.owner + "/" + entry.repo;
    SaveRepository(entry);
}

void Menu::SaveDirectLink(const Entry& direct_entry) {
    if (std::ranges::any_of(m_entries, [&](const auto& entry){ return entry.direct_url == direct_entry.direct_url; })) {
        App::Notify("Direct link is already saved"_i18n);
        return;
    }

    auto entry = direct_entry;
    entry.json_path = DIRECT_LINKS_PATH;
    entry.saved_direct_link = true;
    m_entries.emplace_back(std::move(entry));

    if (const auto rc = WriteDirectLinksFile(m_entries); R_FAILED(rc)) {
        m_entries.pop_back();
        App::PushErrorBox(rc, "Failed to save direct link"_i18n);
        return;
    }

    Sort();
    const auto found = std::ranges::find(m_entries, direct_entry.direct_url, &Entry::direct_url);
    SetIndex(found == m_entries.end() ? 0 : std::distance(m_entries.begin(), found));
    App::Notify("Direct link saved"_i18n);
}

void Menu::SaveRepository(const Entry& repository) {
    if (std::ranges::any_of(m_entries, [&](const auto& entry){ return IsSameRepository(entry, repository.owner, repository.repo); })) {
        App::Notify("GitHub repository is already saved"_i18n);
        return;
    }

    auto entry = repository;
    entry.json_path = REPOSITORIES_PATH;
    entry.saved_repository = true;
    m_entries.emplace_back(std::move(entry));

    if (const auto rc = WriteRepositoriesFile(m_entries); R_FAILED(rc)) {
        m_entries.pop_back();
        App::PushErrorBox(rc, "Failed to save GitHub repository"_i18n);
        return;
    }

    Sort();
    const auto found = std::ranges::find_if(m_entries, [&](const auto& saved){
        return saved.saved_repository && IsSameRepository(saved, repository.owner, repository.repo);
    });
    SetIndex(found == m_entries.end() ? 0 : std::distance(m_entries.begin(), found));
    App::Notify("GitHub repository saved"_i18n);
}

void Menu::RemoveSelectedDirectLink() {
    if (m_entries.empty() || !GetEntry().saved_direct_link) {
        return;
    }

    const auto name = GetEntry().name;
    App::Push<OptionBox>(
        "Remove saved direct link?"_i18n,
        "Back"_i18n,
        "Remove"_i18n,
        0,
        [this, name](auto index) {
            if (!index || *index != 1 || m_entries.empty() || !GetEntry().saved_direct_link) {
                return;
            }

            const auto removed = m_entries[m_index];
            m_entries.erase(m_entries.begin() + m_index);
            if (const auto rc = WriteDirectLinksFile(m_entries); R_FAILED(rc)) {
                m_entries.emplace_back(removed);
                Sort();
                const auto restored = std::ranges::find(m_entries, removed.direct_url, &Entry::direct_url);
                SetIndex(restored == m_entries.end() ? 0 : std::distance(m_entries.begin(), restored));
                App::PushErrorBox(rc, "Failed to remove direct link"_i18n);
                return;
            }

            SetIndex(m_entries.empty() ? 0 : std::min<s64>(m_index, m_entries.size() - 1));
            App::Notify(i18n::Reorder("Removed ", name));
        }
    );
}

void Menu::RemoveSelectedRepository() {
    if (m_entries.empty() || !GetEntry().saved_repository) {
        return;
    }

    const auto owner = GetEntry().owner;
    const auto repo = GetEntry().repo;
    App::Push<OptionBox>(
        "Remove saved GitHub repository?"_i18n,
        "Back"_i18n,
        "Remove"_i18n,
        0,
        [this, owner, repo](auto index) {
            if (!index || *index != 1 || m_entries.empty() || !GetEntry().saved_repository) {
                return;
            }

            const auto removed = m_entries[m_index];
            m_entries.erase(m_entries.begin() + m_index);
            if (const auto rc = WriteRepositoriesFile(m_entries); R_FAILED(rc)) {
                m_entries.emplace_back(removed);
                Sort();
                const auto restored = std::ranges::find_if(m_entries, [&](const auto& entry){
                    return entry.saved_repository && IsSameRepository(entry, owner, repo);
                });
                SetIndex(restored == m_entries.end() ? 0 : std::distance(m_entries.begin(), restored));
                App::PushErrorBox(rc, "Failed to remove GitHub repository"_i18n);
                return;
            }

            SetIndex(m_entries.empty() ? 0 : std::min<s64>(m_index, m_entries.size() - 1));
            App::Notify(i18n::Reorder("Removed ", repo));
        }
    );
}

void Menu::Sort() {
    const auto sorter = [this](Entry& lhs, Entry& rhs) -> bool {
        // handle fallback if multiple entries are added with the same name
        // used for forks of a project.
        // in the rare case of the user adding the same owner and repo,
        // fallback to the filepath, which *is* unqiue
        auto r = strcasecmp(lhs.repo.c_str(), rhs.repo.c_str());
        if (!r) {
            r = strcasecmp(lhs.owner.c_str(), rhs.owner.c_str());
            if (!r) {
                r = strcasecmp(lhs.json_path, rhs.json_path);
            }
        }
        return r < 0;
    };

    std::sort(m_entries.begin(), m_entries.end(), sorter);
}

void Menu::UpdateSubheading() {
    const auto index = m_entries.empty() ? 0 : m_index + 1;
    this->SetSubHeading(std::to_string(index) + " / " + std::to_string(m_entries.size()));
}

void DownloadEntries(const Entry& entry) {
    if (!entry.direct_url.empty()) {
        DownloadDirectEntry(entry);
        return;
    }

    auto gh_entries = std::make_shared<std::vector<GhApiEntry>>();

    App::Push<ProgressBox>(0, "Downloading "_i18n, entry.repo, [entry, gh_entries](auto pbox) -> Result {
        return DownloadReleaseJsonJson(pbox, GenerateApiUrl(entry), *gh_entries);
    }, [entry, gh_entries](Result rc){
        App::PushErrorBox(rc, "Failed to download json"_i18n);
        if (R_FAILED(rc) || gh_entries->empty()) {
            return;
        }

        PopupList::Items entry_items;
        for (const auto& e : *gh_entries) {
            const auto date = e.published_at.substr(0, std::min<std::size_t>(10, e.published_at.size()));
            std::string str = date.empty() ? "" : " [" + date + "]";

            if (!e.name.empty()) {
                str += " " + e.name;
            } else {
                str += " " + e.tag_name;
            }
            if (e.prerelease) {
                str += " (Pre-Release)";
            }

            entry_items.emplace_back(str);
        }

        App::Push<PopupList>("Select release to download for "_i18n + entry.repo, entry_items, [entry, gh_entries](auto op_index){
            if (!op_index || *op_index >= gh_entries->size()) {
                return;
            }

            const auto& gh_entry = (*gh_entries)[*op_index];
            const auto& assets = entry.assets;
            PopupList::Items asset_items;
            std::vector<std::optional<AssetEntry>> asset_configs;
            std::vector<GhApiAsset> api_assets;
            const bool using_name = std::ranges::any_of(assets, [](const auto& asset){ return !asset.name.empty(); });

            for (const auto& api_asset : gh_entry.assets) {
                std::optional<AssetEntry> matched;

                for (const auto& asset : assets) {
                    if (!asset.name.empty() && api_asset.name.find(asset.name) != api_asset.name.npos) {
                        matched = asset;
                        break;
                    }
                }

                if (!using_name || matched) {
                    const auto date = api_asset.updated_at.substr(0, std::min<std::size_t>(10, api_asset.updated_at.size()));
                    std::string str = date.empty() ? api_asset.name : " [" + date + "] " + api_asset.name;

                    asset_items.emplace_back(str);
                    api_assets.emplace_back(api_asset);
                    asset_configs.emplace_back(std::move(matched));
                }
            }

            if (api_assets.empty()) {
                App::Push<OptionBox>("No matching release assets were found."_i18n, "OK"_i18n);
                return;
            }

            App::Push<PopupList>("Select asset to download for "_i18n + entry.repo, asset_items, [entry, api_assets, asset_configs](auto op_index){
                if (!op_index || *op_index >= api_assets.size()) {
                    return;
                }

                const auto index = *op_index;
                const auto asset_entry = api_assets[index];
                const auto asset_config = asset_configs[index];
                auto pre_install_message = entry.pre_install_message;
                if (asset_config && !asset_config->pre_install_message.empty()) {
                    pre_install_message = asset_config->pre_install_message;
                }

                const auto func = [entry, asset_entry, asset_config](){
                    App::Push<ProgressBox>(0, "Downloading "_i18n, entry.repo, [asset_entry, asset_config](auto pbox) -> Result {
                        return DownloadApp(pbox, asset_entry, asset_config ? &*asset_config : nullptr);
                    }, [entry, asset_config](Result rc){
                        homebrew::SignalChange();
                        App::PushErrorBox(rc, "Failed to download app!"_i18n);

                        if (R_SUCCEEDED(rc)) {
                            App::Notify(i18n::Reorder("Downloaded ", entry.repo));
                            auto post_install_message = entry.post_install_message;
                            if (asset_config && !asset_config->post_install_message.empty()) {
                                post_install_message = asset_config->post_install_message;
                            }

                            if (!post_install_message.empty()) {
                                App::Push<OptionBox>(post_install_message, "OK"_i18n);
                            }
                        }
                    });
                };

                if (!pre_install_message.empty()) {
                    App::Push<OptionBox>(
                        pre_install_message,
                        "Back"_i18n, "Download"_i18n, 1, [func](auto op_index){
                            if (op_index && *op_index) {
                                func();
                            }
                        }
                    );
                } else {
                    func();
                }
            });
        });
    });
}

bool Download(const std::string& url, const std::vector<AssetEntry>& assets, const std::string& tag, const std::string& pre_install_message, const std::string& post_install_message) {
    Entry entry{};
    entry.url = url;
    entry.tag = tag;
    entry.assets = assets;
    entry.pre_install_message = pre_install_message;
    entry.post_install_message = post_install_message;

    // parse owner and author from url (if needed).
    if (!entry.url.empty()) {
        ParseGithubUrl(entry.url, entry.owner, entry.repo);
    }

    // check that we have a owner and repo
    if (entry.owner.empty() || entry.repo.empty()) {
        return false;
    }

    DownloadEntries(entry);
    return true;
}

} // namespace sphaira::ui::menu::gh
