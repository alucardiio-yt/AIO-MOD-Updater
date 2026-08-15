#include "tools_tab.hpp"

#include <filesystem>
#include <fstream>

#include "JC_page.hpp"
#include "PC_page.hpp"
#include "cheats_page.hpp"
#include "confirm_page.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "hide_tabs_page.hpp"
#include "net_page.hpp"
#include "mtp_install_page.hpp"
#include "payload_page.hpp"
#include "utils.hpp"
#include "worker_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
constexpr const char AppVersion[] = APP_VERSION;

constexpr bool ENABLE_MTP_INSTALLER = false;
} 

ToolsTab::ToolsTab(const std::string& tag, const nlohmann::ordered_json& payloads, bool erista, const nlohmann::ordered_json& hideStatus) : brls::List()
{

    const std::string latestTag = tag.empty() ? util::getLatestTag(TAGS_INFO) : tag;
    if (util::isNewerVersion(latestTag, AppVersion)) {
        const std::string latestVersion = util::cleanVersion(latestTag);
        brls::ListItem* updateApp = new brls::ListItem(fmt::format("menus/tools/update_app"_i18n, latestVersion));
        updateApp->setHeight(LISTITEM_HEIGHT);

        updateApp->getClickEvent()->subscribe([latestVersion](brls::View*) {
            const std::string message = fmt::format("menus/tools/update_app"_i18n, latestVersion)
                + "\n\n" + "menus/tools/dl_app"_i18n + std::string(APP_URL);

            auto* dialog = new brls::Dialog(message);
            dialog->addButton("menus/common/yes"_i18n, [dialog](brls::View*) {

                
                dialog->close([]() {
                    auto* stagedFrame = new brls::StagedAppletFrame();
                    stagedFrame->setTitle("menus/common/updating"_i18n);
                    stagedFrame->addStage(new WorkerPage(
                        stagedFrame,
                        "menus/common/downloading"_i18n,
                        []() { util::downloadArchive(APP_URL, contentType::app); }));
                    stagedFrame->addStage(new WorkerPage(
                        stagedFrame,
                        "menus/common/extracting"_i18n,
                        []() { util::extractArchive(contentType::app); }));
                    stagedFrame->addStage(new ConfirmPage_AppUpdate(
                        stagedFrame,
                        "menus/common/all_done"_i18n));

                    brls::Application::pushView(stagedFrame);
                });
            });
            dialog->addButton("menus/common/no"_i18n, [dialog](brls::View*) {
                dialog->close();
            });
            dialog->setCancelable(true);
            dialog->open();
        });

        this->addView(updateApp);
    }

    brls::ListItem* cheats = new brls::ListItem("menus/tools/cheats"_i18n);
    cheats->getClickEvent()->subscribe([](brls::View* view) {
        brls::PopupFrame::open("menus/cheats/menu"_i18n, new CheatsPage(), "", "");
    });
    cheats->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* JCcolor = new brls::ListItem("menus/tools/joy_cons"_i18n);
    JCcolor->getClickEvent()->subscribe([](brls::View* view) {
        brls::Application::pushView(new JCPage());
    });
    JCcolor->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* PCcolor = new brls::ListItem("menus/tools/pro_cons"_i18n);
    PCcolor->getClickEvent()->subscribe([](brls::View* view) {
        brls::Application::pushView(new PCPage());
    });
    PCcolor->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* rebootPayload = new brls::ListItem("menus/tools/inject_payloads"_i18n);
    rebootPayload->getClickEvent()->subscribe([](brls::View* view) {
        brls::PopupFrame::open("menus/tools/inject_payloads"_i18n, new PayloadPage(), "", "");
    });
    rebootPayload->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* netSettings = new brls::ListItem("menus/tools/internet_settings"_i18n);
    netSettings->getClickEvent()->subscribe([](brls::View* view) {
        brls::PopupFrame::open("menus/tools/internet_settings"_i18n, new NetPage(), "", "");
    });
    netSettings->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* browser = new brls::ListItem("menus/tools/browser"_i18n);
    browser->getClickEvent()->subscribe([](brls::View* view) {
        std::string url;
        if (brls::Swkbd::openForText([&url](std::string text) { url = text; }, "cheatslips.com e-mail", "", 256, "https://duckduckgo.com", 0, "Submit", "https://website.tld")) {
            util::openWebBrowser(url);
        }
    });
    browser->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* move = new brls::ListItem("menus/tools/batch_copy"_i18n);
    move->getClickEvent()->subscribe([](brls::View* view) {
        chdir("/");
        std::string error = "";
        if (std::filesystem::exists(COPY_FILES_TXT)) {
            error = fs::copyFiles(COPY_FILES_TXT);
        }
        else {
            error = "menus/tools/batch_copy_config_not_found"_i18n;
        }
        util::showDialogBoxInfo(error);
    });
    move->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* mtpInstaller = new brls::ListItem("menus/tools/mtp_installer"_i18n);
    mtpInstaller->getClickEvent()->subscribe([](brls::View* view) {
        auto* appView = new brls::AppletFrame(true, true);
        appView->setContentView(new MtpInstallPage());
        brls::PopupFrame::open("menus/tools/mtp_installer"_i18n, appView, "", "");
    });
    mtpInstaller->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* cleanUp = new brls::ListItem("menus/tools/clean_up"_i18n);
    cleanUp->getClickEvent()->subscribe([](brls::View* view) {
        std::filesystem::remove(AMS_FILENAME);
        std::filesystem::remove(APP_FILENAME);
        std::filesystem::remove(FIRMWARE_FILENAME);
        std::filesystem::remove(CHEATS_FILENAME);
        std::filesystem::remove(BOOTLOADER_FILENAME);
        std::filesystem::remove(CHEATS_VERSION);
        std::filesystem::remove(CUSTOM_FILENAME);
        fs::removeDir(AMS_DIRECTORY_PATH);
        fs::removeDir(SEPT_DIRECTORY_PATH);
        fs::removeDir(FW_DIRECTORY_PATH);
        util::showDialogBoxInfo("menus/common/all_done"_i18n);
    });
    cleanUp->setHeight(LISTITEM_HEIGHT);

    brls::ListItem* hideTabs = new brls::ListItem("menus/tools/hide_tabs"_i18n);
    hideTabs->getClickEvent()->subscribe([](brls::View* view) {
        brls::PopupFrame::open("menus/tools/hide_tabs"_i18n, new HideTabsPage(), "", "");
    });
    hideTabs->setHeight(LISTITEM_HEIGHT);

    if (!util::getBoolValue(hideStatus, "cheats")) this->addView(cheats);
    if (!util::getBoolValue(hideStatus, "jccolor")) this->addView(JCcolor);
    if (!util::getBoolValue(hideStatus, "pccolor")) this->addView(PCcolor);
    if (erista && !util::getBoolValue(hideStatus, "rebootpayload")) this->addView(rebootPayload);
    if (!util::getBoolValue(hideStatus, "netsettings")) this->addView(netSettings);
    if (!util::getBoolValue(hideStatus, "browser")) this->addView(browser);
    if (!util::getBoolValue(hideStatus, "move")) this->addView(move);
    if (ENABLE_MTP_INSTALLER && !util::getBoolValue(hideStatus, "mtpinstaller")) this->addView(mtpInstaller);
    if (!util::getBoolValue(hideStatus, "cleanup")) this->addView(cleanUp);

    this->addView(hideTabs);
}