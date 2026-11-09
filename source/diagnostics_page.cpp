#include "diagnostics_page.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.hpp"
#include "diagnostic_log.hpp"
#include "utils.hpp"

using namespace brls::i18n::literals;
namespace path = std::filesystem;

namespace {
std::string sizeText(std::uint64_t bytes)
{
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double MiB = 1024.0 * 1024.0;
    if (bytes >= static_cast<std::uint64_t>(GiB))
        return fmt::format("{:.2f} GiB", bytes / GiB);
    return fmt::format("{:.1f} MiB", bytes / MiB);
}

brls::ListItem* row(brls::List* list, const std::string& label, const std::string& value = "")
{
    auto* item = new brls::ListItem(label);
    item->setHeight(LISTITEM_HEIGHT);
    if (!value.empty()) item->setValue(value, false, false);
    list->addView(item);
    return item;
}

void setValue(brls::ListItem* item, const std::string& value)
{
    if (item->getValue() != value) item->setValue(value, false, false);
}

class ErrorLogPage final : public brls::AppletFrame {
public:
    ErrorLogPage() : brls::AppletFrame(true, true)
    {
        setTitle("menus/diagnostics/errors_title"_i18n);
        auto* list = new brls::List();
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "menus/diagnostics/errors_help"_i18n, true));
        auto* exportItem = row(list, "menus/diagnostics/export"_i18n);
        exportItem->getClickEvent()->subscribe([](brls::View*) {
            std::string error;
            if (diagnostic_log::exportFile(error))
                util::showDialogBoxInfo(fmt::format("menus/diagnostics/export_done"_i18n,
                    diagnostic_log::ExportPath));
            else
                util::showDialogBoxInfo(error.empty()
                    ? "menus/diagnostics/errors_empty"_i18n
                    : fmt::format("menus/diagnostics/export_failed"_i18n, error));
        });
        const auto entries = diagnostic_log::recent();
        if (entries.empty()) {
            row(list, "menus/diagnostics/errors_empty"_i18n);
        }
        for (const auto& entry : entries) {
            const auto first = entry.find(" | ");
            const auto second = first == std::string::npos ? first : entry.find(" | ", first + 3);
            const std::string title = second == std::string::npos ? entry : entry.substr(0, second);
            const std::string message = second == std::string::npos ? "" : entry.substr(second + 3);
            const std::string preview = message.size() > 96 ? message.substr(0, 93) + "..." : message;
            auto* item = new brls::ListItem(title, preview);
            item->setHeight(84);
            item->getClickEvent()->subscribe([entry](brls::View*) {
                util::showDialogBoxInfo(entry);
            });
            list->addView(item);
        }
        setContentView(list);
    }
};

struct Endpoint {
    const char* label;
    const char* url;
    bool package = false;
};

constexpr std::array<Endpoint, 5> Endpoints{{
    {"menus/diagnostics/server_apps", "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/apps.json"},
    {"menus/diagnostics/server_ports", "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/ports.json"},
    {"menus/diagnostics/server_cfw", NXLINKS_URL},
    {"menus/diagnostics/server_update", TAGS_INFO},
    {"menus/diagnostics/server_package", APP_URL, true},
}};

int stopCurl(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<std::atomic_bool*>(opaque)->load(std::memory_order_relaxed) ? 1 : 0;
}

struct PackageProbe {
    size_t accepted = 0;
    bool sawBytes = false;
    bool stopped = false;
};

size_t receivePackageProbe(char*, size_t size, size_t count, void* opaque)
{
    auto& probe = *static_cast<PackageProbe*>(opaque);
    constexpr size_t limit = 4096;
    if (size && count) probe.sawBytes = true;
    if (size && count > (limit - probe.accepted) / size) {
        probe.stopped = true;
        return 0;
    }
    const size_t bytes = size * count;
    probe.accepted += bytes;
    return bytes;
}

std::string checkEndpoint(const Endpoint& endpoint, std::atomic_bool& cancelled)
{
    CURL* curl = curl_easy_init();
    if (!curl) return "menus/diagnostics/network_init_failed"_i18n;
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.url);
    PackageProbe probe;
    if (endpoint.package) {
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receivePackageProbe);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &probe);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, endpoint.package ? 6L : 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, endpoint.package ? 20L : 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AIO-MOD-Diagnostics");
    // Match the current transfer setup so this test answers whether AIO MOD
    // can reach the same endpoints with its existing network configuration.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopCurl);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (cancelled.load()) return "menus/diagnostics/cancelled"_i18n;
    const bool receivedEnough = endpoint.package && result == CURLE_WRITE_ERROR && probe.stopped;
    if ((result == CURLE_OK || receivedEnough) && status >= 200 && status < 300 &&
        (!endpoint.package || probe.sawBytes))
        return "menus/diagnostics/available"_i18n;
    if (result == CURLE_COULDNT_RESOLVE_HOST)
        return "menus/errors/network_dns"_i18n;
    if (result == CURLE_COULDNT_CONNECT)
        return "menus/errors/network_connect"_i18n;
    if (result == CURLE_OPERATION_TIMEDOUT)
        return "menus/errors/network_timeout"_i18n;
    if (result == CURLE_RECV_ERROR || result == CURLE_SEND_ERROR)
        return "menus/errors/network_interrupted"_i18n;
    if (result == CURLE_SSL_CONNECT_ERROR || result == CURLE_PEER_FAILED_VERIFICATION)
        return "menus/errors/network_tls"_i18n;
    if (result != CURLE_OK && !receivedEnough)
        return fmt::format("{} ({})", "menus/errors/network_other"_i18n, static_cast<int>(result));
    return fmt::format("HTTP {}: {}", status,
        status == 404 ? "menus/errors/http_missing"_i18n :
        status == 403 ? "menus/errors/http_access"_i18n :
        status == 429 ? "menus/errors/http_limited"_i18n :
        status >= 500 ? "menus/errors/http_server"_i18n :
        "menus/errors/http_other"_i18n);
}

class ConnectionPage final : public brls::AppletFrame {
    brls::ListItem* action = nullptr;
    std::array<brls::ListItem*, Endpoints.size()> items{};
    std::array<std::string, Endpoints.size()> results{};
    std::mutex mutex;
    std::thread worker;
    std::atomic_bool cancelled{false};
    std::atomic_bool running{false};

public:
    ConnectionPage() : brls::AppletFrame(true, true)
    {
        setTitle("menus/diagnostics/connection_title"_i18n);
        auto* list = new brls::List();
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "menus/diagnostics/connection_help"_i18n, true));
        action = row(list, "menus/diagnostics/test_connection"_i18n);
        action->getClickEvent()->subscribe([this](brls::View*) { start(); });
        for (size_t i = 0; i < Endpoints.size(); ++i) {
            results[i] = "menus/diagnostics/pending"_i18n;
            items[i] = row(list, brls::i18n::getStr(Endpoints[i].label), results[i]);
        }
        setContentView(list);
    }

    ~ConnectionPage() override
    {
        cancelled.store(true);
        if (worker.joinable()) worker.join();
    }

    bool onCancel() override
    {
        if (running.load()) {
            cancelled.store(true);
            return true;
        }
        return brls::AppletFrame::onCancel();
    }

    void start()
    {
        if (running.load()) return;
        if (worker.joinable()) worker.join();
        cancelled.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.fill("menus/diagnostics/pending"_i18n);
        }
        running.store(true);
        action->setLabel("menus/diagnostics/testing"_i18n);
        try {
            worker = std::thread([this] {
                try {
                    for (size_t i = 0; i < Endpoints.size() && !cancelled.load(); ++i) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            results[i] = "menus/diagnostics/checking"_i18n;
                        }
                        const std::string result = checkEndpoint(Endpoints[i], cancelled);
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            results[i] = result;
                        }
                        if (result != "menus/diagnostics/available"_i18n && !cancelled.load())
                            diagnostic_log::record("menus/diagnostics/area_connection"_i18n,
                                std::string(Endpoints[i].url) + ": " + result);
                    }
                    if (cancelled.load()) {
                        std::lock_guard<std::mutex> lock(mutex);
                        for (auto& result : results)
                            if (result == "menus/diagnostics/pending"_i18n ||
                                result == "menus/diagnostics/checking"_i18n)
                                result = "menus/diagnostics/cancelled"_i18n;
                    }
                }
                catch (...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (auto& result : results)
                        if (result == "menus/diagnostics/checking"_i18n)
                            result = "menus/diagnostics/network_init_failed"_i18n;
                }
                running.store(false);
            });
        }
        catch (...) {
            running.store(false);
            std::lock_guard<std::mutex> lock(mutex);
            results[0] = "menus/diagnostics/network_init_failed"_i18n;
        }
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = 0; i < Endpoints.size(); ++i)
                setValue(items[i], results[i]);
        }
        if (!running.load() && action->getLabel() != "menus/diagnostics/test_connection"_i18n)
            action->setLabel("menus/diagnostics/test_connection"_i18n);
        brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);
    }
};

struct SdScan {
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    double readMiBPerSecond = 0.0;
    bool sampleAvailable = false;
    bool complete = false;
    bool stopped = false;
};

class SdCardPage final : public brls::AppletFrame {
    brls::ListItem* action = nullptr;
    brls::ListItem* capacity = nullptr;
    brls::ListItem* freeSpace = nullptr;
    brls::ListItem* manufacturer = nullptr;
    brls::ListItem* product = nullptr;
    brls::ListItem* interfaceMode = nullptr;
    brls::ListItem* fileCount = nullptr;
    brls::ListItem* folderCount = nullptr;
    brls::ListItem* countedBytes = nullptr;
    brls::ListItem* scanErrors = nullptr;
    brls::ListItem* readSpeed = nullptr;
    brls::ListItem* scanState = nullptr;
    std::mutex mutex;
    SdScan scan{};
    std::thread worker;
    std::atomic_bool cancelled{false};
    std::atomic_bool running{false};
    bool started = false;

public:
    SdCardPage() : brls::AppletFrame(true, true)
    {
        setTitle("menus/diagnostics/sd_title"_i18n);
        auto* list = new brls::List();
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "menus/diagnostics/sd_help"_i18n, true));
        list->addView(new brls::Header("menus/diagnostics/sd_card"_i18n));
        capacity = row(list, "menus/diagnostics/capacity"_i18n);
        freeSpace = row(list, "menus/diagnostics/free"_i18n);
        manufacturer = row(list, "menus/diagnostics/manufacturer"_i18n);
        product = row(list, "menus/diagnostics/product"_i18n);
        interfaceMode = row(list, "menus/diagnostics/interface"_i18n);
        readMetadata();
        list->addView(new brls::Header("menus/diagnostics/sd_scan"_i18n));
        action = row(list, "menus/diagnostics/start_scan"_i18n);
        action->getClickEvent()->subscribe([this](brls::View*) {
            if (running.load()) {
                cancelled.store(true);
                action->setLabel("menus/diagnostics/stopping"_i18n);
            }
            else startScan();
        });
        scanState = row(list, "menus/diagnostics/scan_status"_i18n,
            "menus/diagnostics/not_started"_i18n);
        fileCount = row(list, "menus/diagnostics/files"_i18n, "0");
        folderCount = row(list, "menus/diagnostics/folders"_i18n, "0");
        countedBytes = row(list, "menus/diagnostics/file_bytes"_i18n, "0");
        scanErrors = row(list, "menus/diagnostics/scan_errors"_i18n, "0");
        readSpeed = row(list, "menus/diagnostics/read_speed"_i18n,
            "menus/diagnostics/not_available"_i18n);
        row(list, "menus/diagnostics/bad_sectors"_i18n,
            "menus/diagnostics/not_verifiable"_i18n);
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "menus/diagnostics/sd_errors_help"_i18n, true));
        setContentView(list);
    }

    ~SdCardPage() override
    {
        cancelled.store(true);
        if (worker.joinable()) worker.join();
    }

    bool onCancel() override
    {
        if (running.load()) {
            cancelled.store(true);
            action->setLabel("menus/diagnostics/stopping"_i18n);
            return true;
        }
        return brls::AppletFrame::onCancel();
    }
    void readMetadata()
    {
        const std::string unknown = "menus/diagnostics/not_available"_i18n;
        setValue(capacity, unknown);
        setValue(freeSpace, unknown);
        setValue(manufacturer, unknown);
        setValue(product, unknown);
        setValue(interfaceMode, unknown);
        FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
        s64 total = 0, free = 0;
        bool totalKnown = false, freeKnown = false;
        if (fs) {
            totalKnown = R_SUCCEEDED(fsFsGetTotalSpace(fs, "/", &total)) && total >= 0;
            freeKnown = R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free)) && free >= 0;
        }
        if (!totalKnown) totalKnown = R_SUCCEEDED(nsGetTotalSpaceSize(NcmStorageId_SdCard, &total)) && total >= 0;
        if (!freeKnown) freeKnown = R_SUCCEEDED(nsGetFreeSpaceSize(NcmStorageId_SdCard, &free)) && free >= 0;
        if (totalKnown) setValue(capacity, sizeText(static_cast<std::uint64_t>(total)));
        if (freeKnown) setValue(freeSpace, sizeText(static_cast<std::uint64_t>(free)));
        FsDeviceOperator device{};
        if (R_FAILED(fsOpenDeviceOperator(&device))) return;
        std::array<unsigned char, 16> cid{};
        if (R_SUCCEEDED(fsDeviceOperatorGetSdCardCid(&device, cid.data(), cid.size(), cid.size()))) {
            setValue(manufacturer, fmt::format("MID 0x{:02X}", cid[0]));
            std::string name;
            for (size_t i = 3; i < 8; ++i)
                if (cid[i] >= 32 && cid[i] <= 126) name += static_cast<char>(cid[i]);
            if (!name.empty()) setValue(product, name);
        }
        s64 mode = 0;
        if (R_SUCCEEDED(fsDeviceOperatorGetSdCardSpeedMode(&device, &mode)))
            setValue(interfaceMode, fmt::format("{}", mode));
        fsDeviceOperatorClose(&device);
    }

    void startScan()
    {
        if (worker.joinable()) worker.join();
        cancelled.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            scan = {};
        }
        started = true;
        running.store(true);
        action->setLabel("menus/diagnostics/stop_scan"_i18n);
        try {
            worker = std::thread([this] { scanSd(); });
        }
        catch (...) {
            running.store(false);
            std::lock_guard<std::mutex> lock(mutex);
            scan.errors = 1;
            scan.complete = false;
        }
    }

    void scanSd()
    {
        SdScan result{};
        path::path samplePath;
        std::uintmax_t sampleSize = 0;
        std::uint64_t visited = 0;
        try {
            std::vector<path::path> pending{path::path("/")};
            while (!pending.empty() && !cancelled.load()) {
                const path::path directory = std::move(pending.back());
                pending.pop_back();
                std::error_code ec;
                path::directory_iterator it(directory, ec), end;
                if (ec) {
                    ++result.errors;
                    continue;
                }
                while (it != end && !cancelled.load()) {
                    const path::directory_entry entry = *it;
                    ++visited;
                    ec.clear();
                    const auto status = entry.symlink_status(ec);
                    if (ec) {
                        // Keep scanning other entries and folders.
                    }
                    else if (path::is_directory(status)) {
                        ++result.folders;
                        pending.push_back(entry.path());
                    }
                    else if (path::is_regular_file(status)) {
                        ++result.files;
                        const auto size = entry.file_size(ec);
                        if (!ec) {
                            result.bytes += size;
                            if (size > sampleSize) {
                                sampleSize = size;
                                samplePath = entry.path();
                            }
                        }
                    }
                    if (ec) ++result.errors;
                    it.increment(ec);
                    if (ec) {
                        ++result.errors;
                        break;
                    }
                    if (visited % 64 == 0) {
                        std::lock_guard<std::mutex> lock(mutex);
                        scan = result;
                    }
                }
            }
            if (!cancelled.load() && sampleSize >= 4 * 1024 * 1024) {
                std::ifstream file(samplePath, std::ios::binary);
                if (file) {
                    std::vector<char> buffer(64 * 1024);
                    std::uint64_t read = 0;
                    const auto startedAt = std::chrono::steady_clock::now();
                    while (read < 16 * 1024 * 1024 && !cancelled.load()) {
                        const auto request = static_cast<std::streamsize>(
                            std::min<std::uint64_t>(buffer.size(), 16 * 1024 * 1024 - read));
                        file.read(buffer.data(), request);
                        const auto got = file.gcount();
                        if (got <= 0) break;
                        read += static_cast<std::uint64_t>(got);
                    }
                    const auto seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - startedAt).count();
                    if (!file.bad() && read >= 4 * 1024 * 1024 && seconds > 0) {
                        result.readMiBPerSecond = (read / (1024.0 * 1024.0)) / seconds;
                        result.sampleAvailable = true;
                    }
                    else if (!cancelled.load()) ++result.errors;
                }
                else ++result.errors;
            }
            result.stopped = cancelled.load();
            result.complete = !result.stopped;
        }
        catch (...) {
            ++result.errors;
            result.complete = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            scan = result;
        }
        if (result.errors)
            diagnostic_log::record("menus/diagnostics/area_sd"_i18n,
                fmt::format("{}: {}", "menus/diagnostics/scan_errors"_i18n, result.errors));
        running.store(false);
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override
    {
        SdScan snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = scan;
        }
        setValue(fileCount, std::to_string(snapshot.files));
        setValue(folderCount, std::to_string(snapshot.folders));
        setValue(countedBytes, sizeText(snapshot.bytes));
        setValue(scanErrors, std::to_string(snapshot.errors));
        setValue(readSpeed, snapshot.sampleAvailable
            ? fmt::format("{:.1f} MiB/s", snapshot.readMiBPerSecond)
            : "menus/diagnostics/not_available"_i18n);
        const bool active = running.load();
        setValue(scanState, !started ? "menus/diagnostics/not_started"_i18n :
            active ? "menus/diagnostics/scanning"_i18n :
            snapshot.stopped ? "menus/diagnostics/cancelled"_i18n :
            snapshot.complete && snapshot.errors ? "menus/diagnostics/completed_with_errors"_i18n :
            snapshot.complete ? "menus/diagnostics/completed"_i18n :
            "menus/diagnostics/incomplete"_i18n);
        if (!active && action->getLabel() != "menus/diagnostics/start_scan"_i18n)
            action->setLabel("menus/diagnostics/start_scan"_i18n);
        brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);
    }
};
}

void openErrorLogPage() { brls::Application::pushView(new ErrorLogPage()); }
void openConnectionPage() { brls::Application::pushView(new ConnectionPage()); }
void openSdCardPage() { brls::Application::pushView(new SdCardPage()); }
