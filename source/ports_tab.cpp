#include "ports_tab.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "worker_page.hpp"
#include "progress_event.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
constexpr const char PORTS_JSON_URL[] = "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/ports.json";
constexpr const char PORTS_VIEW_STATE_PATH[] = "/config/aio-switch-updater-mod/ports_view.json";

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

class PortSpaceDialog final : public brls::Dialog
{
public:
    PortSpaceDialog(std::string url, bool archive, std::function<void(std::string)> ready)
        : brls::Dialog("menus/common/space_checking"_i18n), url(std::move(url)),
          archive(archive), ready(std::move(ready))
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
                        sizeKnown = download::getRemoteSize(url, bytes);
                    }
                    catch (...) {
                        sizeKnown = false;
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
            std::string summary = download::spaceSummaryFromSize(bytes, sizeKnown, archive);
            auto onReady = std::move(ready);
            close([onReady = std::move(onReady), summary = std::move(summary)]() mutable {
                onReady(std::move(summary));
            });
        }
    }

private:
    std::string url;
    bool archive;
    std::function<void(std::string)> ready;
    std::thread worker;
    std::atomic_bool finished{false};
    std::uint64_t bytes = 0;
    bool sizeKnown = false;
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

        ports.push_back({label, url, cacheIcon(label, iconUrl)});
    }

    loadFailed = ports.empty();
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
    const std::string url = port.url;
    const bool isZip = isZipUrl(url);
    auto* loading = new PortSpaceDialog(url, isZip, [label, url, isZip](std::string space) {
        const std::string confirmText = (isZip
            ? fmt::format("menus/ports/confirm_zip"_i18n, label)
            : fmt::format("menus/ports/confirm_nro"_i18n, label)) + space;

        auto* stagedFrame = new brls::StagedAppletFrame();
        stagedFrame->setTitle("menus/ports/popup_title"_i18n);
        stagedFrame->addStage(new ConfirmPage(stagedFrame, confirmText));

        if (isZip) {
            stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/ports/downloading"_i18n, [url]() {
                fs::createTree(PORTS_CONFIG_PATH);
                removeTemporaryPortArchive();
                const long status = download::downloadFile(url, PORTS_TEMP_PATH, OFF);
                if (status < 200 || status >= 300 || ProgressEvent::instance().getInterupt())
                    removeTemporaryPortArchive();
            }));
            stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/ports/extracting"_i18n, []() {
                TemporaryPortArchiveCleanup cleanup;
                extract::extract(PORTS_TEMP_PATH, "/", false);
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
    });
    loading->open();
}
