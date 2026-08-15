#include "ports_tab.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "worker_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
constexpr const char PORTS_JSON_URL[] = "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/ports.json";

std::string stripUrlParameters(const std::string& url)
{
    const size_t end = url.find_first_of("?#");
    return url.substr(0, end);
}

std::string sanitizeFilename(std::string value)
{
    for (char& character : value) {
        const unsigned char current = static_cast<unsigned char>(character);
        if (!std::isalnum(current) && character != '.' && character != '-' && character != '_')
            character = '_';
    }

    while (!value.empty() && value.front() == '.')
        value.erase(value.begin());

    return value;
}

std::string filenameFromUrl(const std::string& url, const std::string& fallback)
{
    const std::string cleanUrl = stripUrlParameters(url);
    const size_t slash = cleanUrl.find_last_of('/');
    std::string filename = slash == std::string::npos ? cleanUrl : cleanUrl.substr(slash + 1);
    filename = sanitizeFilename(filename);

    return filename.empty() ? fallback : filename;
}

std::string iconCachePath(const std::string& label, const std::string& iconUrl)
{
    std::string safeLabel = sanitizeFilename(label);
    if (safeLabel.empty())
        safeLabel = "port";

    std::string iconFilename = filenameFromUrl(iconUrl, "icon.png");
    return std::string(PORTS_ICONS_CACHE_PATH) + safeLabel + "_" + iconFilename;
}

bool isUsableFile(const std::string& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error)
        return false;

    const auto size = std::filesystem::file_size(path, error);
    return !error && size > 0;
}

std::string cacheIcon(const std::string& label, const std::string& iconUrl)
{
    if (iconUrl.empty())
        return "";

    fs::createTree(PORTS_ICONS_CACHE_PATH);
    const std::string path = iconCachePath(label, iconUrl);

    if (isUsableFile(path))
        return path;

    const long status = download::downloadFile(iconUrl, path, ON);
    if (status < 200 || status >= 300 || !isUsableFile(path)) {
        std::error_code error;
        std::filesystem::remove(path, error);
        return "";
    }

    return path;
}

bool isZipUrl(const std::string& url)
{
    std::string cleanUrl = stripUrlParameters(url);
    std::transform(cleanUrl.begin(), cleanUrl.end(), cleanUrl.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    return cleanUrl.size() >= 4 && cleanUrl.substr(cleanUrl.size() - 4) == ".zip";
}
}  

PortsTab::PortsTab() : brls::List()
{
    brls::Label* description = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        "menus/ports/description"_i18n,
        true);
    this->addView(description);

    this->createList();
}

void PortsTab::createList()
{
    json catalog;
    const long status = download::getRequest(PORTS_JSON_URL, catalog);

    if (status < 200 || status >= 300 || !catalog.is_object() || catalog.empty()) {
        this->displayNotFound();
        return;
    }

    bool addedPort = false;

    for (auto entry = catalog.begin(); entry != catalog.end(); ++entry) {
        const std::string label = entry.key();
        std::string url;
        std::string iconUrl;

        if (entry.value().is_string()) {
            
            url = entry.value().get<std::string>();
        }
        else if (entry.value().is_object()) {
            const auto urlEntry = entry.value().find("url");
            if (urlEntry != entry.value().end() && urlEntry->is_string())
                url = urlEntry->get<std::string>();

            const auto iconEntry = entry.value().find("icon");
            if (iconEntry != entry.value().end() && iconEntry->is_string())
                iconUrl = iconEntry->get<std::string>();
        }

        if (url.empty())
            continue;

        brls::ListItem* item = new brls::ListItem(label);
        item->setHeight(72);

        const std::string cachedIcon = cacheIcon(label, iconUrl);
        if (!cachedIcon.empty())
            item->setThumbnail(cachedIcon);

        item->getClickEvent()->subscribe([label, url](brls::View* view) {
            const bool isZip = isZipUrl(url);

            const std::string confirmText = isZip
                ? fmt::format("menus/ports/confirm_zip"_i18n, label)
                : fmt::format("menus/ports/confirm_nro"_i18n, label);

            brls::StagedAppletFrame* stagedFrame = new brls::StagedAppletFrame();
            stagedFrame->setTitle("menus/ports/popup_title"_i18n);
            stagedFrame->addStage(new ConfirmPage(stagedFrame, confirmText));

            if (isZip) {
                stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/ports/downloading"_i18n, [url]() {
                    fs::createTree(PORTS_CONFIG_PATH);
                    download::downloadFile(url, PORTS_TEMP_PATH, OFF);
                }));

                stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/ports/extracting"_i18n, []() {
                    extract::extract(PORTS_TEMP_PATH, "/", false);
                    if (std::filesystem::exists(PORTS_TEMP_PATH))
                        std::filesystem::remove(PORTS_TEMP_PATH);
                }));
            }
            else {
                const std::string fallbackFilename = sanitizeFilename(label) + ".nro";
                const std::string filename = filenameFromUrl(url, fallbackFilename);
                const std::string destination = std::string("/switch/") + filename;

                stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/ports/downloading"_i18n, [url, destination]() {
                    fs::createTree("/switch/");
                    download::downloadFile(url, destination, OFF);
                }));
            }

            stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "menus/ports/done"_i18n));
            brls::Application::pushView(stagedFrame);
            return true;
        });

        this->addView(item);
        addedPort = true;
    }

    if (!addedPort)
        this->displayNotFound();
}

void PortsTab::displayNotFound()
{
    brls::Label* notFound = new brls::Label(
        brls::LabelStyle::SMALL,
        "menus/ports/load_failed"_i18n,
        true);
    notFound->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->addView(notFound);
}
