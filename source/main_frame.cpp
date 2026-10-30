#include "main_frame.hpp"

#include <fstream>
#include <json.hpp>

#include "about_tab.hpp"
#include "ams_tab.hpp"
#include "apps_tab.hpp"
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

    if (!util::getBoolValue(hideStatus, "about"))
        this->addTab("menus/main/about"_i18n, new AboutTab());

    if (!util::getBoolValue(hideStatus, "atmosphere"))
        this->addTab("menus/main/update_ams"_i18n, new AmsTab_Regular(nxlinks, erista));

    if (!util::getBoolValue(hideStatus, "firmwares"))
        this->addTab("menus/main/download_firmware"_i18n, new ListDownloadTab(contentType::fw, nxlinks));

    if (!util::getBoolValue(hideStatus, "cheats"))
        this->addTab("menus/main/download_cheats"_i18n, new ListDownloadTab(contentType::cheats));

    if (!util::getBoolValue(hideStatus, "apps"))
        this->addTab("menus/main/apps"_i18n, new AppsTab());

    if (!util::getBoolValue(hideStatus, "ports"))
        this->addTab("menus/main/ports"_i18n, new PortsTab());

    if (!util::getBoolValue(hideStatus, "forwarders"))
        this->addTab("menus/main/forwarders"_i18n, new ForwardersTab());

    if (!util::getBoolValue(hideStatus, "mtpinstaller"))
        this->addTab("menus/main/mtp_installer"_i18n, new MtpInstallPage());

    if (!util::getBoolValue(hideStatus, "tools"))
        this->addTab("menus/main/tools"_i18n, new ToolsTab(latestTag, util::getValueFromKey(nxlinks, "payloads"), erista, hideStatus));

    if (!util::getBoolValue(hideStatus, "language"))
        this->addTab("menus/main/language"_i18n, new LanguageTab());

    this->registerAction("", brls::Key::B, [this] { return true; });
    // The shortcut itself is handled by libnx in main.cpp. This adds its hint
    // beside the standard Exit/OK hints without changing input behavior.
    this->registerAction("explorer/main_hint"_i18n, brls::Key::ZL, [] { return true; });
}
