#include "apps_tab.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "worker_page.hpp"
#include "progress_event.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
constexpr const char APPS_JSON_URL[] = "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/apps.json";
constexpr const char APPS_TEMP_PATH[] = "/config/aio-switch-updater-mod/apps_temp_download";

bool isEmulator(const std::string& label)
{
    return label.size() >= 3 &&
        std::toupper(static_cast<unsigned char>(label[0])) == 'E' &&
        std::toupper(static_cast<unsigned char>(label[1])) == 'M' &&
        std::toupper(static_cast<unsigned char>(label[2])) == 'U';
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
    while (end < value.size() && characters < 8) {
        const unsigned char first = static_cast<unsigned char>(value[end]);
        const size_t length = first < 0x80 ? 1 : (first & 0xE0) == 0xC0 ? 2
            : (first & 0xF0) == 0xE0 ? 3 : (first & 0xF8) == 0xF0 ? 4 : 1;
        end = std::min(value.size(), end + length);
        ++characters;
    }
    return value.substr(0, end) + (end < value.size() ? "..." : "");
}

class AppsHeader final : public brls::BoxLayout
{
public:
    AppsHeader() : BoxLayout(brls::BoxLayoutOrientation::HORIZONTAL) { setHeight(62); }

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override
    {
        constexpr unsigned margin = 18;
        constexpr unsigned gap = 8;
        constexpr unsigned categoryWidth = 220;
        constexpr unsigned searchWidth = 220;
        const unsigned controlsWidth = categoryWidth * 2 + searchWidth + gap * 2;
        const unsigned labelWidth = getWidth() > controlsWidth + margin * 2 + gap
            ? getWidth() - controlsWidth - margin * 2 - gap : 0;
        const int controlsX = getX() + static_cast<int>(getWidth() > margin + controlsWidth
            ? getWidth() - margin - controlsWidth : 0);

        getChild(0)->setBoundaries(getX() + margin, getY(), labelWidth, getHeight());
        getChild(1)->setBoundaries(controlsX, getY() + 7, categoryWidth, getHeight() - 14);
        getChild(2)->setBoundaries(controlsX + categoryWidth + gap, getY() + 7,
                                   categoryWidth, getHeight() - 14);
        getChild(3)->setBoundaries(controlsX + categoryWidth * 2 + gap * 2, getY() + 7,
                                   searchWidth, getHeight() - 14);
        for (size_t index = 0; index < getViewsCount(); ++index)
            getChild(index)->invalidate(true);
    }
};
}  // namespace

AppsTab::AppsTab() : brls::BoxLayout(brls::BoxLayoutOrientation::VERTICAL, 8)
{
    std::string appsDescription = "menus/apps/description"_i18n;
    appsDescription += "\n";
    appsDescription += "menus/apps/sysmodule_notice"_i18n;
    auto* description = new brls::Label(brls::LabelStyle::DESCRIPTION, appsDescription, true);
    description->setVerticalAlign(NVG_ALIGN_TOP);
    auto* descriptionArea = new brls::BoxLayout(brls::BoxLayoutOrientation::VERTICAL);
    const unsigned topPadding = brls::Application::getStyle()->List.marginTopBottom;
    descriptionArea->setMargins(topPadding, 0, 0, 0);
    descriptionArea->setHeight(topPadding + 100);
    descriptionArea->addView(description);
    addView(descriptionArea);

    auto* header = new AppsHeader();
    countLabel = new brls::Label(brls::LabelStyle::DESCRIPTION, "menus/apps/count"_i18n, false);
    applicationsButton = new brls::Button(brls::ButtonStyle::BORDERED);
    emulatorsButton = new brls::Button(brls::ButtonStyle::BORDERED);
    searchButton = new brls::Button(brls::ButtonStyle::BORDERED);
    header->addView(countLabel);
    header->addView(applicationsButton);
    header->addView(emulatorsButton);
    header->addView(searchButton);
    addView(header);

    catalogView = new brls::ScrollView();
    addView(catalogView, true);

    applicationsButton->getClickEvent()->subscribe([this](brls::View*) {
        searchText.clear();
        emulators = false;
        rebuildCatalog();
        return true;
    });
    emulatorsButton->getClickEvent()->subscribe([this](brls::View*) {
        searchText.clear();
        emulators = true;
        rebuildCatalog();
        return true;
    });
    searchButton->getClickEvent()->subscribe([this](brls::View*) {
        std::string entered;
        if (brls::Swkbd::openForText([&entered](std::string text) { entered = std::move(text); },
                                     "menus/apps/search_title"_i18n, "", 80, searchText, 0,
                                     "menus/apps/search_apply"_i18n,
                                     "menus/apps/search_placeholder"_i18n)) {
            const auto first = entered.find_first_not_of(" \t\r\n");
            const auto last = entered.find_last_not_of(" \t\r\n");
            searchText = first == std::string::npos ? "" : entered.substr(first, last - first + 1);
            rebuildCatalog();
        }
        return true;
    });
    searchButton->registerAction("menus/apps/clear_search"_i18n, brls::Key::X, [this] {
        searchText.clear();
        rebuildCatalog();
        return true;
    });

    loadCatalog();
    rebuildCatalog();
}

void AppsTab::loadCatalog()
{
    const auto links = download::getLinks(APPS_JSON_URL);
    for (const auto& link : links)
        apps.push_back({link.first, link.second});
    loadFailed = apps.empty();
}

void AppsTab::rebuildCatalog()
{
    auto* content = new brls::BoxLayout(brls::BoxLayoutOrientation::VERTICAL);
    content->setMargins(8, 20, 18, 20);
    content->setSpacing(0);
    content->setResize(true);

    size_t visibleCount = 0;
    for (const auto& app : apps) {
        if ((searchText.empty() && isEmulator(app.label) != emulators) ||
            !matchesSearch(app.label, searchText))
            continue;

        const App selected = app;
        auto* item = new brls::ListItem(selected.label);
        item->setHeight(LISTITEM_HEIGHT);
        item->getClickEvent()->subscribe([this, selected](brls::View*) {
            openApp(selected);
            return true;
        });
        content->addView(item);
        ++visibleCount;
    }

    if (visibleCount == 0) {
        auto* message = new brls::Label(brls::LabelStyle::SMALL,
            loadFailed ? "menus/apps/load_failed"_i18n : "menus/apps/no_results"_i18n, true);
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
    updateControls(visibleCount);
}

void AppsTab::updateControls(size_t visibleCount)
{
    const bool categorySelected = searchText.empty();
    applicationsButton->setLabel(std::string(categorySelected && !emulators ? "● " : "") +
                                 "menus/apps/applications"_i18n);
    emulatorsButton->setLabel(std::string(categorySelected && emulators ? "● " : "") +
                              "menus/apps/emulators"_i18n);
    std::string searchLabel = "menus/apps/search"_i18n;
    if (!searchText.empty()) {
        searchLabel += ": ";
        searchLabel += searchPreview(searchText);
    }
    searchButton->setLabel(searchLabel);
    searchButton->setActionAvailable(brls::Key::X, !searchText.empty());
    countLabel->setText(loadFailed ? "menus/apps/tab_title"_i18n
                                   : fmt::format("menus/apps/count"_i18n, visibleCount));
    invalidate();
}

void AppsTab::openApp(const App& app)
{
    const std::string label = app.label;
    const std::string url = app.url;
    const std::string cleanUrl = url.substr(0, url.find_first_of("?#"));
    std::string extensionUrl = cleanUrl;
    std::transform(extensionUrl.begin(), extensionUrl.end(), extensionUrl.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool isZip = extensionUrl.size() >= 4 &&
                       extensionUrl.substr(extensionUrl.size() - 4) == ".zip";

    std::string confirmText = isZip
        ? fmt::format("menus/apps/confirm_zip"_i18n, label)
        : fmt::format("menus/apps/confirm_nro"_i18n, label);
    confirmText += download::spaceSummary(url, isZip);

    brls::StagedAppletFrame* stagedFrame = new brls::StagedAppletFrame();
    stagedFrame->setTitle("menus/apps/popup_title"_i18n);
    stagedFrame->addStage(new ConfirmPage(stagedFrame, confirmText));

    if (isZip) {
        stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/apps/downloading"_i18n, [url]() {
            fs::createTree("/config/aio-switch-updater-mod/");
            download::downloadFile(url, APPS_TEMP_PATH, OFF);
        }));

        stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/apps/extracting"_i18n, []() {
            extract::extract(APPS_TEMP_PATH, "/", false);
            if (!ProgressEvent::instance().getInterupt() && std::filesystem::exists(APPS_TEMP_PATH)) {
                std::filesystem::remove(APPS_TEMP_PATH);
            }
        }));
    }
    else {
        std::string filename = cleanUrl.substr(cleanUrl.find_last_of('/') + 1);
        std::string destPath = std::string("/switch/") + filename;

        stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/apps/downloading"_i18n, [url, destPath]() {
            fs::createTree("/switch/");
            download::downloadFile(url, destPath, OFF);
        }));
    }

    stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "menus/apps/done"_i18n));
    brls::Application::pushView(stagedFrame);
}
