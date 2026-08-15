#include "main_frame.hpp"

#include <fstream>
#include <json.hpp>

#include "about_tab.hpp"
#include "ams_tab.hpp"
#include "apps_tab.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "forwarders_tab.hpp"
#include "language_tab.hpp"
#include "list_download_tab.hpp"
#include "ports_tab.hpp"
#include "tools_tab.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
    constexpr const char AppTitle[] = APP_TITLE;
    constexpr const char AppVersion[] = APP_VERSION;
}  

MainFrame::MainFrame() : TabFrame()
{
    
    auto* style = brls::Application::getStyle();
    style->Sidebar.Item.height = 54;
    this->sidebar->setMargins(25, style->Sidebar.marginRight, 15, style->Sidebar.marginLeft);

    this->setIcon("romfs:/gui_icon.png");
    this->setTitle(AppTitle);

    
    const std::string latestTag = util::getLatestTag(TAGS_INFO);

    s64 freeStorage;
    std::string footer = fmt::format("menus/main/footer_text"_i18n,
                                     AppVersion,
                                     R_SUCCEEDED(fs::getFreeStorageSD(freeStorage)) ? (float)freeStorage / 0x40000000 : -1);

    if (util::isNewerVersion(latestTag, AppVersion)) {
        footer += " ";
        footer += "menus/main/new_update"_i18n;
        footer += fmt::format(" (v{})", util::cleanVersion(latestTag));
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

    if (!util::getBoolValue(hideStatus, "tools"))
        this->addTab("menus/main/tools"_i18n, new ToolsTab(latestTag, util::getValueFromKey(nxlinks, "payloads"), erista, hideStatus));

    if (!util::getBoolValue(hideStatus, "language"))
        this->addTab("menus/main/language"_i18n, new LanguageTab());

    this->registerAction("", brls::Key::B, [this] { return true; });
}