#include "hb_store.hpp"
#include "hb_store_file.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

#include "fs.hpp"
#include "progress_event.hpp"
#include "storage_error.hpp"
#include "utils.hpp"

using namespace brls::i18n::literals;
namespace hb_store {
namespace {
namespace disk = std::filesystem;
constexpr char base[] = "https://switch.cdn.fortheusers.org/";
const disk::path cache = "/config/aio-switch-updater-mod/hb-store";
constexpr uint64_t MiB = 1024 * 1024;

struct Temporary {
    disk::path path;
    ~Temporary() { std::error_code ec; disk::remove(path, ec); }
};

bool validName(const std::string& name)
{
    return !name.empty() && name.size() <= 120 && name.front() != '.' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-+") == std::string::npos;
}

struct Sink {
    FILE* file;
    uint64_t limit;
    uint64_t written = 0;
    const std::atomic<bool>* stop;
    bool package;
    mbedtls_md_context_t* digest;
    int error = 0;
};

size_t writeChunk(char* data, size_t size, size_t count, void* opaque)
{
    auto& sink = *static_cast<Sink*>(opaque);
    const size_t bytes = size * count;
    if (bytes > sink.limit - sink.written)
        return 0;
    const size_t saved = fwrite(data, 1, bytes, sink.file);
    sink.written += saved;
    if (saved != bytes) sink.error = errno ? errno : EIO;
    if (sink.digest && mbedtls_md_update(sink.digest,reinterpret_cast<const unsigned char*>(data),saved))
        return 0;
    return saved;
}

int transferProgress(void* opaque, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
{
    auto& sink = *static_cast<Sink*>(opaque);
    if (sink.package) {
        auto& progress = ProgressEvent::instance();
        progress.setDownloadProgress(now, total);
        return progress.getInterupt();
    }
    return sink.stop && sink.stop->load();
}

void fetch(const std::string& relative, const disk::path& path, uint64_t limit,
           long timeout, const std::atomic<bool>* stop = nullptr, mbedtls_md_context_t* digest = nullptr)
{
    disk::create_directories(path.parent_path());
    const std::string url = base + relative;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("menus/errors/download_init"_i18n);
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "wb"), fclose);
    if (!file) throw std::runtime_error(storage::describeError(path.string(), {errno, std::generic_category()}));
    Sink sink{file.get(), limit, 0, stop, digest != nullptr, digest};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, "romfs:/hb-store-ca.pem");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AIO-MOD/HB-App-Store");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 25L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeChunk);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, transferProgress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    const auto result = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (fclose(file.release()) != 0 && !sink.error) sink.error = errno ? errno : EIO;
    if (sink.error) throw std::runtime_error(storage::describeError(path.string(), {sink.error, std::generic_category()}));
    if (status >= 400) throw std::runtime_error(util::getErrorMessage(status));
    if (result == CURLE_COULDNT_RESOLVE_HOST) throw std::runtime_error("menus/errors/network_dns"_i18n);
    if (result == CURLE_COULDNT_CONNECT) throw std::runtime_error("menus/errors/network_connect"_i18n);
    if (result == CURLE_OPERATION_TIMEDOUT) throw std::runtime_error("menus/errors/network_timeout"_i18n);
    if (result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_CACERT_BADFILE)
        throw std::runtime_error("menus/errors/network_tls"_i18n);
    if (result != CURLE_OK || status != 200 || !sink.written)
        throw std::runtime_error("menus/errors/network_interrupted"_i18n);
}

std::vector<Package> parse(const disk::path& path)
{
    if (disk::file_size(path) > 8 * MiB) throw std::runtime_error("catalog size");
    std::ifstream input(path);
    auto doc = nlohmann::json::parse(input);
    const auto& items = doc.at("packages");
    if (!items.is_array() || items.size() > 10000) throw std::runtime_error("catalog format");
    std::vector<Package> out;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        try {
            Package p;
            p.name = item.value("name", "");
            if (!validName(p.name)) continue;
            p.title = item.value("title", p.name);
            p.author = item.value("author", "");
            p.version = item.value("version", "");
            p.description = item.value("description", "");
            p.details = item.value("details", "");
            p.category = item.value("category", "");
            if (p.category == "_misc") p.category = "misc";
            p.updated = item.value("updated", "");
            p.binary = item.value("binary", "");
            p.changelog = item.value("changelog", "");
            p.downloads = item.value("app_dls", uint64_t{0});
            p.md5 = item.value("md5", "");
            p.downloadKiB = item.value("filesize", uint64_t{0});
            p.installedKiB = item.value("extracted", uint64_t{0});
            out.push_back(std::move(p));
        } catch (const nlohmann::json::exception&) { continue; }
    }
    if (out.empty()) throw std::runtime_error("empty catalog");
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
    return out;
}

// Decode only PNG artwork within the texture budget for this page.
bool validImage(const disk::path& path)
{
    std::array<unsigned char, 24> h{};
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(h.data()), h.size()) ||
        memcmp(h.data(), "\x89PNG\r\n\x1a\n", 8) || memcmp(h.data() + 12, "IHDR", 4)) return false;
    auto dimension = [&h](size_t i) -> uint32_t {
        return uint32_t(h[i]) << 24 | uint32_t(h[i+1]) << 16 | uint32_t(h[i+2]) << 8 | h[i+3];
    };
    auto w = dimension(16), height = dimension(20);
    return w && height && w <= 2048 && height <= 1024 && uint64_t(w)*height <= 1280*720;
}
} // namespace

Catalog load()
{
    const auto local = cache / "repo.json";
    Temporary pending{cache / "repo.part"};
    try {
        fetch("repo.json", pending.path, 8 * MiB, 25);
        auto packages = parse(pending.path);
        try { replaceFile(pending.path,local); }
        catch (const std::exception&) { /* A valid network catalog is usable even on a full SD. */ }
        return {std::move(packages), false};
    } catch (const std::exception&) {
        const auto backup = local.string() + ".aio-hb-backup";
        return {parse(disk::exists(local) ? local : disk::path(backup)), true};
    }
}

std::string artwork(const std::string& name, bool banner, const std::atomic<bool>& stop)
{
    const auto leaf = banner ? "screen.png" : "icon.png";
    const auto path = cache / name / leaf;
    if (validImage(path)) return path.string();
    Temporary pending{path.string() + ".part"};
    fetch("packages/" + name + "/" + leaf, pending.path, 3 * MiB, 8, &stop);
    if (!validImage(pending.path)) return {};
    replaceFile(pending.path, path);
    return path.string();
}

void installArchive(const std::string& zip, const Package& package, const std::filesystem::path& root);

void install(const Package& package)
{
    auto& progress = ProgressEvent::instance();
    if (package.md5.size() != 32 || package.md5.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw std::runtime_error("menus/hb_store/checksum"_i18n);
    s64 free = 0;
    constexpr uint64_t maxZip = 0xffffffffULL;
    if (package.downloadKiB > maxZip / 1024 || package.installedKiB > UINT32_MAX)
        throw std::runtime_error("menus/errors/archive_too_large"_i18n);
    if (R_SUCCEEDED(fs::getFreeStorageSD(free)) &&
        (package.downloadKiB + package.installedKiB) * 1024 > static_cast<uint64_t>(free))
        throw std::runtime_error("menus/errors/insufficient_storage"_i18n);
    Temporary zip{cache / "install.zip.part"};
    progress.setOperation(ProgressOperation::DOWNLOAD);
    progress.setCurrentFile(package.title);
    progress.setTotalSteps(1000);
    mbedtls_md_context_t hash;
    mbedtls_md_init(&hash);
    const std::unique_ptr<mbedtls_md_context_t,decltype(&mbedtls_md_free)> cleanup(&hash,mbedtls_md_free);
    const auto* md = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!md || mbedtls_md_setup(&hash,md,0) || mbedtls_md_starts(&hash))
        throw std::runtime_error("menus/hb_store/checksum"_i18n);
    try {
        fetch("zips/" + package.name + ".zip", zip.path, maxZip, 0, nullptr, &hash);
    } catch (...) { if (progress.getInterupt()) return; throw; }
    if (progress.getInterupt()) return;
    std::array<unsigned char, 16> digest{};
    if (mbedtls_md_finish(&hash,digest.data()))
        throw std::runtime_error("menus/hb_store/checksum"_i18n);
    char hex[33]{};
    for (size_t i = 0; i < digest.size(); ++i) snprintf(hex + i*2, 3, "%02x", digest[i]);
    if (strcasecmp(hex, package.md5.c_str())) throw std::runtime_error("menus/hb_store/checksum"_i18n);
    if (!progress.getInterupt()) installArchive(zip.path.string(), package, "/");
}
} // namespace hb_store
