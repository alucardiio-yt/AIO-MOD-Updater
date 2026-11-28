#include "hb_store.hpp"
#include "hb_store_file.hpp"

#include <borealis.hpp>
#include <minizip/unzip.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "fs.hpp"
#include "progress_event.hpp"
#include "storage_error.hpp"
#include "utils.hpp"

using namespace brls::i18n::literals;
namespace hb_store {
namespace {
namespace disk = std::filesystem;

[[noreturn]] void invalid() { throw std::runtime_error("menus/hb_store/manifest"_i18n); }

struct Zip {
    unzFile handle;
    ~Zip() { if (handle) unzClose(handle); }
};
struct Current {
    unzFile zip;
    bool open = true;
    explicit Current(unzFile z) : zip(z) { if (unzOpenCurrentFile(zip) != UNZ_OK) invalid(); }
    ~Current() { if (open) unzCloseCurrentFile(zip); }
    void finish() { open = false; if (unzCloseCurrentFile(zip) != UNZ_OK) invalid(); }
};
struct RemoveFile {
    disk::path path;
    ~RemoveFile() { std::error_code ec; disk::remove(path, ec); }
};
std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    return s;
}
bool safePath(const std::string& path)
{
    if (path.empty() || path.size() > 700 || path.front() == '/' || path.back() == '/' ||
        path.find_first_of("\\:*?\"<>|") != path.npos) return false;
    for (unsigned char c : path) if (c < 32 || c == 127) return false;
    std::istringstream parts(path);
    for (std::string part; std::getline(parts, part, '/');)
        if (part.empty() || part.back()=='.' || part.back()==' ') return false;
    const auto key = lower(path) + "/";
    return !key.starts_with("config/aio-switch-updater-mod/") &&
           !key.starts_with("switch/appstore/.get/") && !key.starts_with("switch/aio-switch-updater-mod/");
}

struct Entry {
    std::string destination;
    char rule;
    unz64_file_pos pos{};
    uint64_t size = 0;
    bool found = false;
};

std::map<std::string,Entry> readPlan(unzFile z, const std::string& name)
{
    unz_file_info64 info{};
    if (unzLocateFile(z, "manifest.install", 1) != UNZ_OK ||
        unzGetCurrentFileInfo64(z, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
        !info.uncompressed_size || info.uncompressed_size > 2*1024*1024) invalid();
    std::string manifest(info.uncompressed_size, '\0');
    {
        Current file(z);
        if (unzReadCurrentFile(z, manifest.data(), manifest.size()) != static_cast<int>(manifest.size())) invalid();
        file.finish();
    }

    std::map<std::string,Entry> plan;
    std::istringstream lines(manifest);
    std::string active = lower(util::getAppPath());
    if (active.starts_with("sdmc:")) active.erase(0,5);
    for (std::string line; std::getline(lines,line);) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size()<4 || line.substr(1,2)!=": " || std::string("EUG").find(line[0])==line.npos) invalid();
        auto path = line.substr(3);
        if (!safePath(path) || lower(path).ends_with(".aio-hb-backup") ||
            "/" + lower(path) == active || plan.size() >= 32768) invalid();
        if (!plan.emplace(lower(path), Entry{"/"+path,line[0]}).second) invalid();
    }
    if (plan.empty()) invalid();
    const std::string receipt = "/switch/appstore/.get/packages/" + name + "/";
    for (const std::string meta : {"info.json","manifest.install"})
        if (!plan.emplace(meta,Entry{receipt+meta,'U'}).second) invalid();

    if (unzGoToFirstFile(z) != UNZ_OK) invalid();
    int rc;
    do {
        std::array<char, 768> key{};
        if (unzGetCurrentFileInfo64(z, &info, key.data(), key.size(), nullptr,0,nullptr,0)!=UNZ_OK ||
            info.size_filename >= key.size() || strlen(key.data()) != info.size_filename) invalid();
        const auto it = plan.find(lower(key.data()));
        if (it != plan.end()) {
            auto& entry = it->second;
            if (entry.found || info.uncompressed_size > UINT32_MAX || unzGetFilePos64(z,&entry.pos)!=UNZ_OK) invalid();
            entry.found = true;
            entry.size = info.uncompressed_size;
        }
        rc = unzGoToNextFile(z);
    } while (rc == UNZ_OK);
    if (rc != UNZ_END_OF_LIST_OF_FILE) invalid();
    for (const auto& [key,entry] : plan) if (!entry.found) invalid();
    return plan;
}

void writeEntry(unzFile z, const Entry& entry, const disk::path& root, uint64_t& done, uint64_t total)
{
    if (unzGoToFilePos64(z,&entry.pos)!=UNZ_OK) invalid();
    auto& progress = ProgressEvent::instance();
    progress.setCurrentFile(entry.destination);
    const auto target = root / entry.destination.substr(1);
    disk::create_directories(target.parent_path());
    // Stage on the same volume. The previous file stays readable until CRC and close succeed.
    const auto temp = root / "config/aio-switch-updater-mod/hb-store/entry.part";
    RemoveFile cleanup{temp};
    Current input(z);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    auto checkWrite = [&] {
        if (!out) throw std::runtime_error(storage::describeError(entry.destination,{errno ? errno : EIO,std::generic_category()}));
    };
    checkWrite();
    std::array<char,32*1024> chunk;
    uint64_t written = 0;
    while (!progress.getInterupt()) {
        int count = unzReadCurrentFile(z, chunk.data(), chunk.size());
        if (count < 0) invalid();
        if (!count) break;
        written += count;
        if (written > entry.size) invalid();
        out.write(chunk.data(), count);
        checkWrite();
        progress.setExtractionProgress(done + written,total,written,entry.size);
    }
    if (progress.getInterupt()) return;
    input.finish();
    out.close();
    checkWrite();
    if (written != entry.size) invalid();
    replaceFile(temp,target);
    done += written;
}
} // namespace

void installArchive(const std::string& zip, const Package& package, const disk::path& root)
{
    Zip archive{unzOpen64(zip.c_str())};
    if (!archive.handle) invalid();
    const auto plan = readPlan(archive.handle, package.name);
    uint64_t total = 0;
    for (const auto& [key,entry] : plan)
        if (entry.rule != 'G' || !disk::exists(root / entry.destination.substr(1))) total += entry.size;
    s64 free = 0;
    if (R_SUCCEEDED(fs::getFreeStorageSD(free)) && total > static_cast<uint64_t>(free))
        throw std::runtime_error("menus/errors/insufficient_storage"_i18n);
    auto& progress = ProgressEvent::instance();
    progress.setOperation(ProgressOperation::EXTRACT);
    progress.setTotalSteps(1000);
    uint64_t done = 0;
    try {
        for (const auto& [key,entry] : plan) {
            if (progress.getInterupt()) return;
            if (key=="info.json" || key=="manifest.install" || (entry.rule=='G' && disk::exists(root / entry.destination.substr(1)))) continue;
            writeEntry(archive.handle,entry,root,done,total);
        }
        for (const std::string key : {"manifest.install","info.json"}) {
            if (progress.getInterupt()) return;
            writeEntry(archive.handle,plan.at(key),root,done,total);
        }
    } catch (const disk::filesystem_error& e) {
        throw std::runtime_error(storage::describeError(e.path1().string(),e.code()));
    }
}
} // namespace hb_store
