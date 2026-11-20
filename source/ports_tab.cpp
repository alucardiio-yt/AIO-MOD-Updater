#include "ports_tab.hpp"
#include "catalog_news.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <curl/curl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "worker_page.hpp"
#include "progress_event.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
constexpr const char PORTS_JSON_URL[] = "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/ports.json";
constexpr const char PORTS_DETAILS_URL[] = "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/ports-details.json";
constexpr const char PORTS_VIEW_STATE_PATH[] = "/config/aio-switch-updater-mod/ports_view.json";

struct PortDetails {
    std::string author;
    std::string description;
    std::uint64_t downloadBytes = 0;
    std::uint64_t recommendedBytes = 0;
    std::uint64_t freeBytes = 0;
    bool downloadKnown = false;
    bool freeKnown = false;
};

struct RemoteSize {
    std::uint64_t bytes = 0;
    bool known = false;
};

std::string formatBytes(std::uint64_t bytes)
{
    const double amount = static_cast<double>(bytes);
    constexpr double mib = 1024.0 * 1024.0;
    constexpr double gib = mib * 1024.0;
    return amount >= gib ? fmt::format("{:.2f} GB", amount / gib)
        : amount >= mib ? fmt::format("{:.1f} MB", amount / mib)
                        : fmt::format("{:.0f} KB", amount / 1024.0);
}

std::string displayText(std::string value, size_t byteLimit)
{
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t')
            character = ' ';
    }
    if (value.size() <= byteLimit)
        return value;
    size_t end = byteLimit;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
        --end;
    return value.substr(0, end) + "…";
}

size_t appendDetails(char* contents, size_t size, size_t count, void* target)
{
    auto* output = static_cast<std::string*>(target);
    const size_t bytes = size * count;
    if (bytes > 65536 || output->size() > 65536 - bytes)
        return 0;
    output->append(contents, bytes);
    return bytes;
}

const json& detailsCatalog()
{
    static json catalog = json::object();
    static std::once_flag attempt;
    std::call_once(attempt, [&] {
        try {
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
            if (!curl)
                return;
            std::string response;
            curl_easy_setopt(curl.get(), CURLOPT_URL, PORTS_DETAILS_URL);
            curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "HamletDuFromage");
            curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
            curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 2000L);
            curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendDetails);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
            if (curl_easy_perform(curl.get()) != CURLE_OK || !json::accept(response))
                return;
            long status = 0;
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
            if (status == 200) {
                json parsed = json::parse(response);
                if (parsed.is_object())
                    catalog = std::move(parsed);
            }
        }
        catch (...) {}
    });
    return catalog;
}

void readPortDetails(const std::string& label, PortDetails& details)
{
    const json& catalog = detailsCatalog();
    if (!catalog.is_object())
        return;
    const auto entry = catalog.find(label);
    if (entry == catalog.end() || !entry->is_object())
        return;
    const auto author = entry->find("author");
    if (author != entry->end() && author->is_string())
        details.author = displayText(author->get<std::string>(), 100);
    const auto description = entry->find("description");
    if (description != entry->end()) {
        if (description->is_string())
            details.description = displayText(description->get<std::string>(), 420);
        else if (description->is_object()) {
            const json languageFile = fs::parseJsonFile(LANGUAGE_JSON);
            const auto selected = languageFile.find("language");
            const std::string locale = selected != languageFile.end() && selected->is_string()
                ? selected->get<std::string>() : i18n::getCurrentLocale();
            for (const auto& code : {locale, std::string("es"), std::string("en-US")}) {
                const auto text = description->find(code);
                if (text != description->end() && text->is_string()) {
                    details.description = displayText(text->get<std::string>(), 420);
                    break;
                }
            }
        }
    }
    const auto space = entry->find("recommended_space_mb");
    if (space != entry->end() && space->is_number_unsigned()) {
        const auto mb = space->get<std::uint64_t>();
        if (mb > 0 && mb <= 1024 * 1024)
            details.recommendedBytes = mb * 1024 * 1024;
    }
}

void removeTemporaryPortArchive()
{
    std::error_code error;
    std::filesystem::remove(PORTS_TEMP_PATH, error);
}

struct TemporaryPortArchiveCleanup {
    ~TemporaryPortArchiveCleanup() { removeTemporaryPortArchive(); }
};

bool loadGridView()
{
    const json state = fs::parseJsonFile(PORTS_VIEW_STATE_PATH);
    const auto grid = state.find("grid");
    return grid != state.end() && grid->is_boolean() && grid->get<bool>();
}

void saveGridView(bool grid)
{
    json state = {{"grid", grid}};
    fs::writeJsonToFile(state, PORTS_VIEW_STATE_PATH);
}

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

bool matchesSearch(const std::string& label, const std::string& query)
{
    return std::search(label.begin(), label.end(), query.begin(), query.end(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != label.end();
}

std::string searchPreview(const std::string& value)
{
    size_t end = 0;
    size_t characters = 0;
    while (end < value.size() && characters < 17) {
        const unsigned char first = static_cast<unsigned char>(value[end]);
        const size_t length = first < 0x80 ? 1 : (first & 0xE0) == 0xC0 ? 2
            : (first & 0xF0) == 0xE0 ? 3 : (first & 0xF8) == 0xF0 ? 4 : 1;
        end = std::min(value.size(), end + length);
        ++characters;
    }
    return value.substr(0, end) + (end < value.size() ? "..." : "");
}

class PortsHeader final : public brls::BoxLayout
{
public:
    PortsHeader() : BoxLayout(brls::BoxLayoutOrientation::HORIZONTAL) { setHeight(62); }

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override
    {
        constexpr unsigned margin = 18;
        constexpr unsigned gap = 10;
        constexpr unsigned modeWidth = 178;
        constexpr unsigned searchWidth = 220;
        const unsigned controlsWidth = modeWidth + gap + searchWidth;
        const unsigned labelWidth = getWidth() > controlsWidth + margin * 2 + gap
            ? getWidth() - controlsWidth - margin * 2 - gap : 0;
        const int controlsX = getX() + static_cast<int>(getWidth() > margin + controlsWidth
            ? getWidth() - margin - controlsWidth : 0);

        getChild(0)->setBoundaries(getX() + margin, getY(), labelWidth, getHeight());
        getChild(1)->setBoundaries(controlsX, getY() + 7, modeWidth, getHeight() - 14);
        getChild(2)->setBoundaries(controlsX + modeWidth + gap, getY() + 7,
                                   searchWidth, getHeight() - 14);
        for (size_t index = 0; index < getViewsCount(); ++index)
            getChild(index)->invalidate(true);
    }
};

class PortCard final : public brls::View
{
public:
    PortCard(const std::string& label, const std::string& iconPath, std::function<void()> open)
        : open(std::move(open))
    {
        setHeight(196);
        if (!iconPath.empty()) {
            icon = std::make_unique<brls::Image>(iconPath);
            icon->setScaleType(brls::ImageScaleType::FIT);
            icon->setParent(this);
        }
        title = std::make_unique<brls::Label>(brls::LabelStyle::SMALL, label, true);
        title->setHorizontalAlign(NVG_ALIGN_CENTER);
        title->setParent(this);
        registerAction("brls/hints/ok"_i18n, brls::Key::A, [this] {
            this->open();
            return true;
        });
    }

    brls::View* getDefaultFocus() override { return this; }

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override
    {
        if (icon) {
            icon->setBoundaries(getX() + 10, getY() + 10,
                                getWidth() > 20 ? getWidth() - 20 : 1, 130);
            icon->invalidate(true);
        }
        title->setBoundaries(getX() + 10, getY() + 145,
                             getWidth() > 20 ? getWidth() - 20 : 1, 43);
        title->invalidate(true);
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 8);
        nvgFillColor(vg, a(ctx->theme->buttonRegularBackgroundColor));
        nvgFill(vg);
        if (icon)
            icon->frame(ctx);
        title->frame(ctx);
    }

private:
    std::function<void()> open;
    std::unique_ptr<brls::Image> icon;
    std::unique_ptr<brls::Label> title;
};

class PortsItemsLayout final : public brls::BoxLayout
{
public:
    explicit PortsItemsLayout(bool grid) : BoxLayout(brls::BoxLayoutOrientation::VERTICAL), grid(grid)
    {
        setMargins(8, 20, 18, 20);
        setSpacing(0);
        setResize(!grid);
    }

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override
    {
        if (!grid) {
            BoxLayout::layout(vg, style, stash);
            return;
        }

        constexpr unsigned columns = 3;
        constexpr unsigned gap = 14;
        constexpr unsigned cardHeight = 196;
        constexpr unsigned marginX = 20;
        constexpr unsigned marginTop = 8;
        constexpr unsigned marginBottom = 18;
        const unsigned usableWidth = getWidth() > marginX * 2 + gap * 2
            ? getWidth() - marginX * 2 - gap * 2 : columns;
        const unsigned cardWidth = usableWidth / columns;

        for (size_t index = 0; index < getViewsCount(); ++index) {
            auto* child = getChild(index);
            child->setBoundaries(getX() + marginX + (index % columns) * (cardWidth + gap),
                                 getY() + marginTop + (index / columns) * (cardHeight + gap),
                                 cardWidth, cardHeight);
            child->invalidate(true);
        }

        const unsigned rows = (getViewsCount() + columns - 1) / columns;
        setHeight(marginTop + rows * cardHeight + (rows ? (rows - 1) * gap : 0) + marginBottom);
    }

    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* current) override
    {
        if (!grid)
            return BoxLayout::getNextFocus(direction, current);

        const size_t count = getViewsCount();
        const size_t index = *static_cast<size_t*>(current->getParentUserData());
        if (direction == brls::FocusDirection::LEFT && index % 3 > 0)
            return getChild(index - 1)->getDefaultFocus();
        if (direction == brls::FocusDirection::RIGHT && index % 3 < 2 && index + 1 < count)
            return getChild(index + 1)->getDefaultFocus();
        if (direction == brls::FocusDirection::UP && index >= 3)
            return getChild(index - 3)->getDefaultFocus();
        if (direction == brls::FocusDirection::DOWN && (index / 3 + 1) * 3 < count)
            return getChild(std::min(index + 3, count - 1))->getDefaultFocus();
        return nullptr;
    }

private:
    bool grid;
};

class PortDetailsPage final : public brls::View
{
public:
    PortDetailsPage(brls::StagedAppletFrame* frame, const std::string& name,
                    const std::string& iconPath, size_t partCount, PortDetails details)
    {
        if (!iconPath.empty()) {
            icon = std::make_unique<brls::Image>(iconPath);
            icon->setScaleType(brls::ImageScaleType::FIT);
            icon->setParent(this);
        }
        title = makeLabel(brls::LabelStyle::REGULAR, name, true);
        author = makeLabel(brls::LabelStyle::SMALL,
            fmt::format("menus/ports/details_author"_i18n,
                details.author.empty() ? std::string("menus/ports/details_unknown"_i18n) : details.author));
        parts = makeLabel(brls::LabelStyle::SMALL,
            fmt::format("menus/ports/details_parts"_i18n, partCount));
        size = makeLabel(brls::LabelStyle::SMALL,
            fmt::format("menus/ports/details_download"_i18n,
                details.downloadKnown ? formatBytes(details.downloadBytes)
                                      : std::string("menus/ports/details_unknown"_i18n)));
        recommended = makeLabel(brls::LabelStyle::SMALL,
            fmt::format("menus/ports/details_recommended"_i18n,
                details.recommendedBytes ? formatBytes(details.recommendedBytes)
                                         : std::string("menus/ports/details_unknown"_i18n)));
        free = makeLabel(brls::LabelStyle::SMALL,
            fmt::format("menus/ports/details_free"_i18n,
                details.freeKnown ? formatBytes(details.freeBytes)
                                  : std::string("menus/ports/details_unknown"_i18n)));
        descriptionTitle = makeLabel(brls::LabelStyle::MEDIUM, "menus/ports/details_description"_i18n);
        description = makeLabel(brls::LabelStyle::SMALL,
            details.description.empty() ? std::string("menus/ports/details_no_description"_i18n)
                                        : details.description, true);
        install = std::make_unique<brls::Button>(brls::ButtonStyle::PRIMARY);
        install->setLabel("menus/ports/details_install"_i18n);
        install->setParent(this);
        install->getClickEvent()->subscribe([frame](brls::View*) {
            frame->nextStage();
            return true;
        });
    }

    brls::View* getDefaultFocus() override { return install.get(); }

    void layout(NVGcontext*, brls::Style*, brls::FontStash*) override
    {
        const int left = getX() + 34;
        const int top = getY() + 20;
        const unsigned bodyWidth = getWidth() > 68 ? getWidth() - 68 : 1;
        const unsigned iconWidth = std::min(235u, bodyWidth / 3);
        const int infoX = left + iconWidth + 28;
        const unsigned infoWidth = bodyWidth > iconWidth + 28 ? bodyWidth - iconWidth - 28 : 1;
        if (icon) {
            icon->setBoundaries(left, top, iconWidth, 218);
            icon->invalidate(true);
        }
        title->setBoundaries(infoX, top + 4, infoWidth, 60);
        author->setBoundaries(infoX, top + 70, infoWidth, 28);
        parts->setBoundaries(infoX, top + 103, infoWidth, 28);
        size->setBoundaries(infoX, top + 136, infoWidth, 28);
        recommended->setBoundaries(infoX, top + 169, infoWidth, 28);
        free->setBoundaries(infoX, top + 202, infoWidth, 28);
        descriptionTitle->setBoundaries(left, top + 265, bodyWidth, 32);
        description->setBoundaries(left, top + 302, bodyWidth, 150);
        install->setBoundaries(getX() + static_cast<int>(getWidth() / 2) - 145,
                               getY() + static_cast<int>(getHeight()) - 74, 290, 54);
        for (auto* label : {title.get(), author.get(), parts.get(), size.get(),
                            recommended.get(), free.get(), descriptionTitle.get(), description.get()})
            label->invalidate(true);
        install->invalidate(true);
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style*, brls::FrameContext* ctx) override
    {
        if (!icon) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 34, y + 20, std::min(235u, width / 3), 218, 8);
            nvgFillColor(vg, a(ctx->theme->buttonRegularBackgroundColor));
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgRect(vg, x + 34, y + 254, width > 68 ? width - 68 : 1, 1);
        nvgFillColor(vg, a(ctx->theme->separatorColor));
        nvgFill(vg);
        if (icon) icon->frame(ctx);
        for (auto* label : {title.get(), author.get(), parts.get(), size.get(),
                            recommended.get(), free.get(), descriptionTitle.get(), description.get()})
            label->frame(ctx);
        install->frame(ctx);
    }

private:
    std::unique_ptr<brls::Label> makeLabel(brls::LabelStyle style, const std::string& value,
                                           bool multiline = false)
    {
        auto label = std::make_unique<brls::Label>(style, value, multiline);
        label->setParent(this);
        return label;
    }

    std::unique_ptr<brls::Image> icon;
    std::unique_ptr<brls::Label> title, author, parts, size, recommended, free;
    std::unique_ptr<brls::Label> descriptionTitle, description;
    std::unique_ptr<brls::Button> install;
};

class PortSpaceDialog final : public brls::Dialog
{
public:
    PortSpaceDialog(std::string label, std::vector<port_catalog::Part> parts,
                    std::function<void(PortDetails)> ready)
        : brls::Dialog("menus/common/space_checking"_i18n), label(std::move(label)),
          parts(std::move(parts)), ready(std::move(ready))
    {
        setCancelable(false);
    }

    ~PortSpaceDialog() override
    {
        if (worker.joinable())
            worker.join();
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override
    {
        brls::Dialog::draw(vg, x, y, width, height, style, ctx);
        if (!started) {
            started = true;
            try {
                worker = std::thread([this]() {
                    try {
                        std::jthread metadata;
                        try {
                            metadata = std::jthread([this] {
                                try { readPortDetails(label, details); }
                                catch (...) {}
                            });
                        }
                        catch (...) {}
                        std::vector<RemoteSize> sizes(parts.size());
                        std::atomic_size_t nextPart{0};
                        std::vector<std::jthread> sizeWorkers;
                        const size_t workerCount = std::min<size_t>(parts.size(), 3);
                        auto readSizes = [&] {
                            size_t index;
                            while ((index = nextPart.fetch_add(1)) < parts.size()) {
                                try {
                                    sizes[index].known = download::getRemoteSize(
                                        parts[index].url, sizes[index].bytes);
                                }
                                catch (...) {
                                    sizes[index].known = false;
                                }
                            }
                        };
                        try {
                            for (size_t index = 0; index < workerCount; ++index)
                                sizeWorkers.emplace_back(readSizes);
                        }
                        catch (...) {}
                        if (sizeWorkers.empty())
                            readSizes();
                        sizeWorkers.clear();
                        details.downloadKnown = true;
                        for (const auto& size : sizes) {
                            if (size.known && UINT64_MAX - details.downloadBytes >= size.bytes)
                                details.downloadBytes += size.bytes;
                            else
                                details.downloadKnown = false;
                        }
                        if (metadata.joinable())
                            metadata.join();
                        s64 freeBytes = 0;
                        details.freeKnown = R_SUCCEEDED(fs::getFreeStorageSD(freeBytes)) && freeBytes >= 0;
                        if (details.freeKnown)
                            details.freeBytes = static_cast<std::uint64_t>(freeBytes);
                        if (!details.recommendedBytes && details.downloadKnown) {
                            const bool zip = std::all_of(parts.begin(), parts.end(), [](const auto& part) {
                                return isZipUrl(part.url);
                            });
                            details.recommendedBytes = static_cast<std::uint64_t>(
                                std::ceil(static_cast<double>(details.downloadBytes) * (zip ? 2.5 : 1.0)));
                        }
                        if (details.downloadKnown)
                            details.recommendedBytes = std::max(details.recommendedBytes,
                                                                 details.downloadBytes);
                    }
                    catch (...) {
                        details.downloadKnown = false;
                    }
                    finished.store(true, std::memory_order_release);
                });
            }
            catch (...) {
                finished.store(true, std::memory_order_release);
            }
        }
        else if (finished.load(std::memory_order_acquire) && !closing) {
            closing = true;
            if (worker.joinable())
                worker.join();
            auto onReady = std::move(ready);
            PortDetails result = std::move(details);
            close([onReady = std::move(onReady), result = std::move(result)]() mutable {
                onReady(std::move(result));
            });
        }
    }

private:
    std::string label;
    std::vector<port_catalog::Part> parts;
    std::function<void(PortDetails)> ready;
    std::thread worker;
    std::atomic_bool finished{false};
    PortDetails details;
    bool started = false;
    bool closing = false;
};
}  // namespace

PortsTab::PortsTab() : brls::BoxLayout(brls::BoxLayoutOrientation::VERTICAL, 1)
{
    gridView = loadGridView();

    auto* header = new PortsHeader();
    countLabel = new brls::Label(brls::LabelStyle::DESCRIPTION, "menus/ports/description"_i18n, false);
    viewButton = new brls::Button(brls::ButtonStyle::BORDERED);
    searchButton = new brls::Button(brls::ButtonStyle::BORDERED);
    header->addView(countLabel);
    header->addView(viewButton);
    header->addView(searchButton);
    addView(header);

    catalogView = new brls::ScrollView();
    addView(catalogView, true);

    viewButton->getClickEvent()->subscribe([this](brls::View*) {
        gridView = !gridView;
        saveGridView(gridView);
        rebuildCatalog();
        return true;
    });
    searchButton->getClickEvent()->subscribe([this](brls::View*) {
        std::string entered;
        if (brls::Swkbd::openForText([&entered](std::string text) { entered = std::move(text); },
                                     "menus/ports/search_title"_i18n, "", 80, searchText, 0,
                                     "menus/ports/search_apply"_i18n,
                                     "menus/ports/search_placeholder"_i18n)) {
            const auto first = entered.find_first_not_of(" \t\r\n");
            const auto last = entered.find_last_not_of(" \t\r\n");
            entered = first == std::string::npos ? "" : entered.substr(first, last - first + 1);
            searchText = std::move(entered);
            rebuildCatalog();
        }
        return true;
    });
    searchButton->registerAction("menus/ports/clear_search"_i18n, brls::Key::X, [this] {
        searchText.clear();
        rebuildCatalog();
        return true;
    });

    loadCatalog();
    rebuildCatalog();
}

void PortsTab::loadCatalog()
{
    json catalog;
    const long status = download::getRequest(PORTS_JSON_URL, catalog);

    if (status < 200 || status >= 300 || !catalog.is_object() || catalog.empty()) {
        loadFailed = true;
        return;
    }

    std::vector<std::string> names;
    std::vector<port_catalog::Entry> rawPorts;
    for (auto entry = catalog.begin(); entry != catalog.end(); ++entry) {
        const std::string label = entry.key();
        std::string url;
        std::string iconUrl;

        if (entry.value().is_string()) {
            // Compatible con el formato simple de apps.json, aunque sin icono.
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

        rawPorts.push_back({label, iconUrl, {{label, url}}});
        names.push_back(label);
    }

    for (auto& entry : port_catalog::group(rawPorts)) {
        std::string iconPath;
        if (entry.parts.size() > 1 && !entry.iconUrl.empty()) {
            const std::string existing = iconCachePath(entry.parts.front().label, entry.iconUrl);
            if (isUsableFile(existing))
                iconPath = existing;
        }
        if (iconPath.empty())
            iconPath = cacheIcon(entry.label, entry.iconUrl);
        ports.push_back({entry.label, std::move(iconPath), std::move(entry.parts)});
    }

    loadFailed = ports.empty();
    if (!loadFailed)
        catalog_news::observe(catalog_news::Catalog::Ports, names);
}

void PortsTab::rebuildCatalog()
{
    std::vector<const Port*> visiblePorts;
    for (const auto& port : ports) {
        if (matchesSearch(port.label, searchText))
            visiblePorts.push_back(&port);
    }

    auto* content = new PortsItemsLayout(gridView && !visiblePorts.empty());
    for (const auto* entry : visiblePorts) {
        const Port port = *entry;

        if (gridView) {
            content->addView(new PortCard(port.label, port.iconPath, [this, port] {
                openPort(port);
            }));
        }
        else {
            auto* item = new brls::ListItem(port.label);
            item->setHeight(72);
            if (!port.iconPath.empty())
                item->setThumbnail(port.iconPath);
            item->getClickEvent()->subscribe([this, port](brls::View*) {
                openPort(port);
                return true;
            });
            content->addView(item);
        }
    }

    if (visiblePorts.empty()) {
        auto* message = new brls::Label(brls::LabelStyle::SMALL,
            loadFailed ? "menus/ports/load_failed"_i18n : "menus/ports/no_results"_i18n, true);
        message->setHorizontalAlign(NVG_ALIGN_CENTER);
        message->setHeight(90);
        content->addView(message);
    }

    auto* previous = catalogContent;
    catalogView->setContentView(content);
    catalogContent = content;
    if (previous) {
        previous->willDisappear(true);
        delete previous;
    }
    catalogView->willAppear(true);
    updateControls(visiblePorts.size());
}

void PortsTab::updateControls(size_t visibleCount)
{
    viewButton->setLabel(gridView ? "menus/ports/view_grid"_i18n : "menus/ports/view_list"_i18n);
    std::string searchLabel = "menus/ports/search"_i18n;
    if (!searchText.empty()) {
        searchLabel += ": ";
        searchLabel += searchPreview(searchText);
    }
    searchButton->setLabel(searchLabel);
    searchButton->setActionAvailable(brls::Key::X, !searchText.empty());
    countLabel->setText(loadFailed ? "menus/ports/tab_title"_i18n
                                   : fmt::format("menus/ports/count"_i18n, visibleCount));
    invalidate();
}

void PortsTab::openPort(const Port& port)
{
    const std::string label = port.label;
    const auto parts = port.parts;
    const std::string iconPath = port.iconPath;
    auto* loading = new PortSpaceDialog(label, parts, [label, parts, iconPath](PortDetails details) {
        const bool isZip = isZipUrl(parts.front().url);
        std::string confirmText = parts.size() > 1
            ? fmt::format("menus/ports/confirm_multipart"_i18n, label, parts.size())
            : isZip ? fmt::format("menus/ports/confirm_zip"_i18n, label)
                    : fmt::format("menus/ports/confirm_nro"_i18n, label);
        if (details.freeKnown && details.recommendedBytes > details.freeBytes)
            confirmText += "\n\n" + std::string("menus/common/space_low"_i18n);

        auto* stagedFrame = new brls::StagedAppletFrame();
        stagedFrame->setTitle("menus/ports/popup_title"_i18n);
        stagedFrame->addStage(new PortDetailsPage(stagedFrame, label, iconPath, parts.size(), details));
        stagedFrame->addStage(new ConfirmPage(stagedFrame, confirmText));

        for (size_t index = 0; index < parts.size(); ++index) {
            const auto& part = parts[index];
            const std::string url = part.url;
            const std::string partLabel = parts.size() > 1
                ? fmt::format("menus/ports/part_progress"_i18n, index + 1, parts.size()) : "";
            if (!isZipUrl(url)) {
                const std::string fallbackFilename = sanitizeFilename(part.label) + ".nro";
                const std::string filename = filenameFromUrl(url, fallbackFilename);
                const std::string destination = std::string("/switch/") + filename;
                stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/ports/downloading"_i18n,
                    [url, destination]() {
                        fs::createTree("/switch/");
                        download::downloadNro(url, destination);
                    }));
                continue;
            }
            stagedFrame->addStage(new WorkerPage(stagedFrame,
                partLabel.empty() ? std::string("menus/ports/downloading"_i18n)
                                  : fmt::format("menus/ports/downloading_part"_i18n, partLabel),
                [url, partLabel, multipart = parts.size() > 1]() {
                fs::createTree(PORTS_CONFIG_PATH);
                auto& progress = ProgressEvent::instance();
                constexpr int maxAttempts = 4;
                long status = 0;
                for (int attempt = 1; attempt <= (multipart ? maxAttempts : 1); ++attempt) {
                    if (progress.getInterupt())
                        break;
                    removeTemporaryPortArchive();
                    int transferError = CURLE_OK;
                    status = download::downloadFile(url, PORTS_TEMP_PATH, OFF, &transferError);
                    if ((status >= 200 && status < 300) || progress.getInterupt())
                        break;
                    const bool connectionLost = transferError == CURLE_COULDNT_RESOLVE_HOST ||
                        transferError == CURLE_COULDNT_CONNECT;
                    if (!multipart || !connectionLost || attempt == maxAttempts)
                        break;

                    removeTemporaryPortArchive();
                    progress.setErrorMessage("");
                    progress.setStatusCode(0);
                    progress.setStep(0);
                    progress.setFileProgress(0, 0);
                    progress.setSpeed(0);
                    progress.setCurrentFile(fmt::format("menus/ports/waiting_connection"_i18n,
                        partLabel, attempt + 1, maxAttempts));
                    const auto waitUntil = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5 * attempt);
                    while (!progress.getInterupt() && std::chrono::steady_clock::now() < waitUntil)
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                if (status < 200 || status >= 300 || ProgressEvent::instance().getInterupt()) {
                    removeTemporaryPortArchive();
                    if (!ProgressEvent::instance().getInterupt()) {
                        std::string reason = ProgressEvent::instance().getErrorMessage();
                        if (reason.empty())
                            reason = status > 0 ? util::getErrorMessage(status)
                                                : std::string("menus/errors/network_other"_i18n);
                        ProgressEvent::instance().setErrorMessage(
                            partLabel.empty() ? reason : partLabel + ": " + reason);
                    }
                }
            }));
            stagedFrame->addStage(new WorkerPage(stagedFrame,
                partLabel.empty() ? std::string("menus/ports/extracting"_i18n)
                                  : fmt::format("menus/ports/extracting_part"_i18n, partLabel), [partLabel]() {
                TemporaryPortArchiveCleanup cleanup;
                try {
                    if (extract::extract(PORTS_TEMP_PATH, "/", false) == 0 &&
                        !ProgressEvent::instance().getInterupt())
                        throw std::runtime_error("menus/errors/archive_no_files"_i18n);
                    const std::string error = ProgressEvent::instance().getErrorMessage();
                    if (!partLabel.empty() && !error.empty())
                        ProgressEvent::instance().setErrorMessage(partLabel + ": " + error);
                }
                catch (const std::exception& exception) {
                    throw std::runtime_error(partLabel.empty() ? exception.what()
                        : partLabel + ": " + exception.what());
                }
            }));
        }

        stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "menus/ports/done"_i18n));
        brls::Application::pushView(stagedFrame);
    });
    loading->open();
}
