#include "main_frame.hpp"

#include <fstream>
#include <json.hpp>

#include "about_tab.hpp"
#include "ams_tab.hpp"
#include "app_theme.hpp"
#include "apps_tab.hpp"
#include "catalog_news.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "forwarders_tab.hpp"
#include "file_explorer_page.hpp"
#include "language_tab.hpp"
#include "list_download_tab.hpp"
#include "mtp_install_page.hpp"
#include "ports_tab.hpp"
#include "tools_tab.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
    constexpr const char AppTitle[] = APP_TITLE;
    constexpr const char AppVersion[] = APP_VERSION;
}  // namespace

MainFrame::MainFrame() : TabFrame()
{
    appletMode = util::isApplet();
    // Fit all main tabs above the footer after adding the Forwarders tab.
    auto* style = brls::Application::getStyle();
    style->Sidebar.Item.height = 50;
    this->sidebar->setMargins(25, style->Sidebar.marginRight, 15, style->Sidebar.marginLeft);

    this->setIcon("romfs:/gui_icon.png");
    this->setTitle(AppTitle);

    // Check the AIO MOD GitHub release once at startup. The same tag is used
    // for both the footer notification and the Tools update entry.
    const std::string latestTag = util::getLatestTag(TAGS_INFO);

    s64 freeStorage;
    std::string footer = fmt::format("menus/main/footer_text"_i18n,
                                     AppVersion,
                                     R_SUCCEEDED(fs::getFreeStorageSD(freeStorage)) ? (float)freeStorage / 0x40000000 : -1);

    if (util::isNewerVersion(latestTag, AppVersion)) {
        // Keep the update notice short enough to leave room for the action hints.
        footer = fmt::format("{} (v{})", "menus/main/new_update"_i18n,
                             util::cleanVersion(latestTag));
    }

    this->setFooterText(footer);

    json hideStatus = fs::parseJsonFile(HIDE_TABS_JSON);
    nlohmann::ordered_json nxlinks;
    download::getRequest(NXLINKS_URL, nxlinks);

    bool erista = util::isErista();

    auto addIconTab = [this](const std::string& label, brls::View* view, const char* icon) {
        this->addTab(label, view);
        auto* item = static_cast<brls::SidebarItem*>(
            this->sidebar->getChild(this->sidebar->getViewsCount() - 1));
        item->setIcon(icon);
        return item;
    };

    if (!util::getBoolValue(hideStatus, "about"))
        addIconTab("menus/main/about"_i18n, new AboutTab(), "\uE88E");

    if (!util::getBoolValue(hideStatus, "atmosphere"))
        addIconTab("menus/main/update_ams"_i18n, new AmsTab_Regular(nxlinks, erista), "\uE62A");

    if (!util::getBoolValue(hideStatus, "firmwares"))
        addIconTab("menus/main/download_firmware"_i18n, new ListDownloadTab(contentType::fw, nxlinks), "\uE322");

    if (!util::getBoolValue(hideStatus, "themes"))
        addIconTab("menus/themes/title"_i18n, app_theme::createTab(), "\uE40A");

    if (!util::getBoolValue(hideStatus, "apps")) {
        auto* item = addIconTab("menus/main/apps"_i18n, new AppsTab(), "\uE5C3");
        if (catalog_news::hasNew(catalog_news::Catalog::Apps))
            item->setBadge("menus/main/new_badge"_i18n);
        item->getFocusEvent()->subscribe([item](brls::View*) {
            if (catalog_news::hasNew(catalog_news::Catalog::Apps) &&
                catalog_news::markSeen(catalog_news::Catalog::Apps))
                item->setBadge("");
        });
    }

    if (!util::getBoolValue(hideStatus, "ports")) {
        auto* item = addIconTab("menus/main/ports"_i18n, new PortsTab(), "\uE338");
        if (catalog_news::hasNew(catalog_news::Catalog::Ports))
            item->setBadge("menus/main/new_badge"_i18n);
        item->getFocusEvent()->subscribe([item](brls::View*) {
            if (catalog_news::hasNew(catalog_news::Catalog::Ports) &&
                catalog_news::markSeen(catalog_news::Catalog::Ports))
                item->setBadge("");
        });
    }

    if (!util::getBoolValue(hideStatus, "forwarders"))
        addIconTab("menus/main/forwarders"_i18n, new ForwardersTab(), "\uE157");

    if (!util::getBoolValue(hideStatus, "mtpinstaller"))
        addIconTab("menus/main/mtp_installer"_i18n, new MtpInstallPage(), "\uE1E0");

    if (!util::getBoolValue(hideStatus, "tools"))
        addIconTab("menus/main/tools"_i18n, new ToolsTab(latestTag, util::getValueFromKey(nxlinks, "payloads"), erista, hideStatus), "\uE8B8");

    if (!util::getBoolValue(hideStatus, "language"))
        addIconTab("menus/main/language"_i18n, new LanguageTab(), "\uE894");

    this->registerAction("", brls::Key::B, [this] { return true; });
    // The shortcut itself is handled by libnx in main.cpp. This adds its hint
    // beside the standard Exit/OK hints without changing input behavior.
    this->registerAction("explorer/main_hint"_i18n, brls::Key::ZL, [] { return true; });
}

void MainFrame::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
                     brls::Style* style, brls::FrameContext* ctx)
{
    brls::TabFrame::draw(vg, x, y, width, height, style, ctx);
    if (!appletMode)
        return;

    const std::string warning = "menus/main/applet_mode_warning"_i18n;
    nvgFontFaceId(vg, ctx->fontStash->regular);
    nvgFontSize(vg, 18);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, a(ctx->theme->textColor));
    nvgText(vg, x + width - style->AppletFrame.separatorSpacing,
            y + style->AppletFrame.headerHeightRegular / 2 + style->AppletFrame.titleOffset,
            warning.c_str(), nullptr);
}
