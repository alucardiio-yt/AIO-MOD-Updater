#include "forwarders_tab.hpp"

#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "forwarder_installer.hpp"
#include "fs.hpp"
#include "progress_event.hpp"
#include "worker_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
constexpr const char SWITCH_ROOT[] = "/switch";
constexpr size_t MAX_ICON_SIZE = 1024 * 1024;
constexpr int FORWARDER_ROW_HEIGHT = 64;

struct NroData {
    NroStart start{};
    NroHeader header{};
};

struct ParsedNro {
    std::string path;
    std::string name;
    std::string author;
    std::string version;
    NacpStruct nacp{};
    std::vector<u8> icon;
    bool validNacp = false;
};

std::string trimCString(const char* value, size_t maxSize)
{
    if (!value || maxSize == 0)
        return {};

    const size_t length = strnlen(value, maxSize);
    return std::string(value, length);
}

std::string fileStem(const std::string& path)
{
    std::filesystem::path fsPath(path);
    std::string stem = fsPath.stem().string();
    return stem.empty() ? "Homebrew" : stem;
}

std::string normalizeDisplayPath(const std::filesystem::path& path)
{
    std::string value = path.generic_string();
    if (value.empty() || value.front() != '/')
        value.insert(value.begin(), '/');
    return value;
}

bool hasNroExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".nro";
}

bool readAt(std::ifstream& file, std::streamoff offset, void* output, size_t size)
{
    file.clear();
    file.seekg(offset, std::ios::beg);
    if (!file.good())
        return false;

    file.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(size));
    return file.good() || static_cast<size_t>(file.gcount()) == size;
}

std::vector<u8> readBinaryFile(const std::string& path)
{
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

bool parseNro(const std::string& path, ParsedNro& out, bool loadIcon)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    NroData data{};
    if (!readAt(file, 0, &data, sizeof(data)) || data.header.magic != NROHEADER_MAGIC)
        return false;

    out = {};
    out.path = path;
    out.name = fileStem(path);
    out.author = "Unknown";
    out.version = "Unknown";

    NroAssetHeader asset{};
    if (!readAt(file, static_cast<std::streamoff>(data.header.size), &asset, sizeof(asset)) ||
        asset.magic != NROASSETHEADER_MAGIC ||
        asset.nacp.offset == 0 ||
        asset.nacp.size != sizeof(NacpStruct)) {
        
        std::memset(&out.nacp, 0, sizeof(out.nacp));
        std::strncpy(out.nacp.lang[0].name, out.name.c_str(), sizeof(out.nacp.lang[0].name) - 1);
        std::strncpy(out.nacp.lang[0].author, out.author.c_str(), sizeof(out.nacp.lang[0].author) - 1);
        std::strncpy(out.nacp.display_version, out.version.c_str(), sizeof(out.nacp.display_version) - 1);
        return true;
    }

    if (!readAt(file,
                static_cast<std::streamoff>(data.header.size + asset.nacp.offset),
                &out.nacp,
                sizeof(out.nacp))) {
        return false;
    }
    out.validNacp = true;

    
    for (const auto& language : out.nacp.lang) {
        const std::string name = trimCString(language.name, sizeof(language.name));
        if (!name.empty()) {
            out.name = name;
            const std::string author = trimCString(language.author, sizeof(language.author));
            if (!author.empty())
                out.author = author;
            break;
        }
    }

    const std::string version = trimCString(out.nacp.display_version, sizeof(out.nacp.display_version));
    if (!version.empty())
        out.version = version;

    if (loadIcon && asset.icon.offset != 0 && asset.icon.size > 0 && asset.icon.size <= MAX_ICON_SIZE) {
        out.icon.resize(static_cast<size_t>(asset.icon.size));
        if (!readAt(file,
                    static_cast<std::streamoff>(data.header.size + asset.icon.offset),
                    out.icon.data(),
                    out.icon.size())) {
            out.icon.clear();
        }
    }

    return true;
}

std::vector<u8> readNroIcon(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};

    NroData data{};
    if (!readAt(file, 0, &data, sizeof(data)) || data.header.magic != NROHEADER_MAGIC)
        return {};

    NroAssetHeader asset{};
    if (!readAt(file, static_cast<std::streamoff>(data.header.size), &asset, sizeof(asset)) ||
        asset.magic != NROASSETHEADER_MAGIC || asset.icon.offset == 0 ||
        asset.icon.size == 0 || asset.icon.size > MAX_ICON_SIZE) {
        return {};
    }

    std::vector<u8> icon(static_cast<size_t>(asset.icon.size));
    if (!readAt(file,
                static_cast<std::streamoff>(data.header.size + asset.icon.offset),
                icon.data(),
                icon.size())) {
        return {};
    }
    return icon;
}

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string iconCachePath(const std::string& nroPath)
{
    return fmt::format("{}{:016X}.jpg", FORWARDER_ICONS_CACHE_PATH, fnv1a64(nroPath));
}

bool iconCacheExists(const std::string& cachePath)
{
    std::error_code error;
    if (!std::filesystem::exists(cachePath, error) || error)
        return false;

    const auto size = std::filesystem::file_size(cachePath, error);
    return !error && size > 0;
}

bool writeIconCache(const std::string& nroPath, const std::vector<u8>& icon, std::string& outputPath)
{
    if (icon.empty())
        return false;

    fs::createTree(FORWARDER_ICONS_CACHE_PATH);
    outputPath = iconCachePath(nroPath);

    if (iconCacheExists(outputPath))
        return true;

    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(icon.data()), static_cast<std::streamsize>(icon.size()));
    return file.good();
}

std::string ensureCachedIcon(const std::string& nroPath)
{
    const std::string cachePath = iconCachePath(nroPath);
    if (iconCacheExists(cachePath))
        return cachePath;

    const auto icon = readNroIcon(nroPath);
    std::string output;
    return writeIconCache(nroPath, icon, output) ? output : std::string{};
}

void scanDirectoryOneLevel(std::vector<ParsedNro>& out)
{
    std::error_code error;
    const std::filesystem::path root(SWITCH_ROOT);
    if (!std::filesystem::exists(root, error) || error)
        return;

    auto addNro = [&out](const std::filesystem::path& path) {
        if (!hasNroExtension(path))
            return;
        ParsedNro parsed;
        if (parseNro(normalizeDisplayPath(path), parsed, false))
            out.emplace_back(std::move(parsed));
    };

    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error)
            break;
        const std::string filename = entry.path().filename().string();
        if (!filename.empty() && filename.front() == '.')
            continue;

        if (entry.is_regular_file(error)) {
            if (!error)
                addNro(entry.path());
            error.clear();
            continue;
        }

        if (!entry.is_directory(error)) {
            error.clear();
            continue;
        }
        error.clear();

        std::error_code nestedError;
        for (const auto& child : std::filesystem::directory_iterator(entry.path(), nestedError)) {
            if (nestedError)
                break;
            const std::string childName = child.path().filename().string();
            if (!childName.empty() && childName.front() == '.')
                continue;
            if (child.is_regular_file(nestedError) && !nestedError)
                addNro(child.path());
            nestedError.clear();
        }
    }

    std::sort(out.begin(), out.end(), [](const ParsedNro& lhs, const ParsedNro& rhs) {
        std::string a = lhs.name;
        std::string b = rhs.name;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (a == b)
            return lhs.path < rhs.path;
        return a < b;
    });
}

std::vector<u8> defaultIcon()
{
    return readBinaryFile("romfs:/forwarder/default_icon.jpg");
}

std::string resultText(Result result)
{
    return fmt::format("Error al crear el forwarder (Result 0x{:08X}).", static_cast<u32>(result));
}
} 

ForwardersTab::ForwardersTab() : brls::List()
{
    auto* description = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        "menus/forwarders/description"_i18n,
        true);
    this->addView(description);
    this->createList();
}

void ForwardersTab::createList()
{
    std::vector<ParsedNro> entries;
    scanDirectoryOneLevel(entries);

    if (entries.empty()) {
        this->displayNotFound();
        return;
    }

    for (auto& entry : entries) {
        const std::string cachedIcon = ensureCachedIcon(entry.path);

        std::string meta;
        if (!entry.author.empty() && !entry.version.empty())
            meta = entry.author + "  •  " + entry.version;
        else if (!entry.author.empty())
            meta = entry.author;
        else
            meta = entry.version;

        

        auto* item = new brls::ListItem(entry.name, "", meta);
        item->setHeight(FORWARDER_ROW_HEIGHT);
        if (!cachedIcon.empty())
            item->setThumbnail(cachedIcon);
        else
            item->setThumbnail("romfs:/forwarder/default_icon.jpg");

        const std::string path = entry.path;
        const std::string name = entry.name;
        const std::string author = entry.author;
        const std::string version = entry.version;

        item->getClickEvent()->subscribe([path, name, author, version](brls::View*) {
            const std::string confirmText = fmt::format(
                "menus/forwarders/confirm"_i18n,
                name,
                author,
                version,
                path);

            auto* stagedFrame = new brls::StagedAppletFrame();
            stagedFrame->setTitle("menus/forwarders/popup_title"_i18n);
            stagedFrame->addStage(new ConfirmPage(stagedFrame, confirmText));

            stagedFrame->addStage(new WorkerPage(
                stagedFrame,
                "menus/forwarders/installing"_i18n,
                [path, name, author]() {
                    auto& progress = ProgressEvent::instance();
                    progress.setOperation(ProgressOperation::FORWARDER);
                    progress.setTotalSteps(10); 
                    progress.setCurrentFile("Preparando forwarder...");

                    ParsedNro parsed;
                    if (!parseNro(path, parsed, true)) {
                        progress.setErrorMessage(fmt::format("No se pudo leer el NRO: {}", path));
                        progress.setStep(10);
                        return;
                    }

                    if (parsed.icon.empty())
                        parsed.icon = defaultIcon();
                    if (parsed.icon.empty()) {
                        progress.setErrorMessage("No se pudo cargar un icono para el forwarder.");
                        progress.setStep(10);
                        return;
                    }

                    aio::forwarder::Config config;
                    config.nro_path = path;
                    config.name = name;
                    config.author = author;
                    config.nacp = parsed.nacp;
                    config.icon = std::move(parsed.icon);

                    const Result rc = aio::forwarder::install(
                        std::move(config),
                        NcmStorageId_SdCard,
                        [&progress](int current, int total, const std::string& message) {
                            progress.setForwarderProgress(current, total, message);
                        });

                    if (R_FAILED(rc))
                        progress.setErrorMessage(resultText(rc));

                    
                    progress.setStep(progress.getMax());
                }));

            stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "menus/forwarders/done"_i18n));
            brls::Application::pushView(stagedFrame);
            return true;
        });

        this->addView(item);
    }
}

void ForwardersTab::displayNotFound()
{
    auto* notFound = new brls::Label(
        brls::LabelStyle::SMALL,
        "menus/forwarders/load_failed"_i18n,
        true);
    notFound->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->addView(notFound);
}
