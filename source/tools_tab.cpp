#include "tools_tab.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <borealis/header.hpp>

#include "JC_page.hpp"
#include "PC_page.hpp"
#include "cheats_page.hpp"
#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "diagnostic_log.hpp"
#include "diagnostics_page.hpp"
#include "extract.hpp"
#include "file_explorer_page.hpp"
#include "fs.hpp"
#include "net_page.hpp"
#include "payload_page.hpp"
#include "progress_event.hpp"
#include "port_uninstall_page.hpp"
#include "utils.hpp"
#include "worker_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

namespace {
constexpr const char AppVersion[] = APP_VERSION;
constexpr const char APPS_TEMP_PATH[] = "/config/aio-switch-updater-mod/apps_temp_download";
constexpr const char MTP_ENGINE_TEMP_PATH[] = "/switch/aio-switch-updater-mod/config/mtp-engine/aio-mtp-engine.nro.tmp";

constexpr std::array<const char*, 9> cleanupTargets{{
    AMS_FILENAME,
    APP_FILENAME,
    FIRMWARE_FILENAME,
    CHEATS_FILENAME,
    BOOTLOADER_FILENAME,
    CUSTOM_FILENAME,
    PORTS_TEMP_PATH,
    APPS_TEMP_PATH,
    MTP_ENGINE_TEMP_PATH,
}};

void cleanDownloadedFiles()
{
    auto& progress = ProgressEvent::instance();
    progress.setTotalSteps(static_cast<int>(cleanupTargets.size()));
    unsigned failures = 0;

    for (const char* target : cleanupTargets) {
        if (progress.getInterupt())
            return;

        progress.setCurrentFile(target);
        std::error_code error;
        const bool exists = std::filesystem::exists(target, error);
        if (error) {
            ++failures;
        }
        else if (exists) {
            if (std::filesystem::is_regular_file(target, error) && !error)
                std::filesystem::remove(target, error);
            else
                error = std::make_error_code(std::errc::invalid_argument);
            if (error)
                ++failures;
        }
        progress.incrementStep(1);
    }

    if (failures)
        throw std::runtime_error(fmt::format("menus/tools/clean_up_failed"_i18n, failures));
}

brls::Header* compactHeader(const std::string& label)
{
    auto* header = new brls::Header(label);
    header->setHeight(36);
    return header;
}
} // namespace

ToolsTab::ToolsTab(const std::string& tag, const nlohmann::ordered_json& payloads, bool erista, const nlohmann::ordered_json& hideStatus) : brls::List()
{
    // Reuse the original AIO-Switch-Updater self-update mechanism, but point it
    // at the AIO MOD GitHub releases repository and compare versions safely.
    const std::string latestTag = tag.empty() ? util::getLatestTag(TAGS_INFO) : tag;
    brls::ListItem* updateApp = nullptr;
    if (util::isNewerVersion(latestTag, AppVersion)) {
        const std::string latestVersion = util::cleanVersion(latestTag);
        updateApp = new brls::ListItem(fmt::format("menus/tools/update_app"_i18n, latestVersion));
        updateApp->setHeight(LISTITEM_HEIGHT);

        updateApp->getClickEvent()->subscribe([latestVersion](brls::View*) {
            const std::string preflightError = util::appUpdatePreflightError();
            if (!preflightError.empty()) {
                diagnostic_log::record("menus/diagnostics/area_update"_i18n, preflightError);
                util::showDialogBoxInfo(preflightError);
                return;
            }
            const std::string message = fmt::format("menus/tools/update_app"_i18n, latestVersion)
                + download::spaceSummary(APP_URL, true);

            auto* dialog = new brls::Dialog(message);
            dialog->addButton("menus/common/yes"_i18n, [dialog](brls::View*) {
                // Dialog::close() removes the current view asynchronously.
                // Do not push the updater frame until that pop has finished,
                // otherwise Borealis can pop/delete the newly pushed frame.
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
                        "menus/tools/app_update_ready"_i18n));

                    brls::Application::pushView(stagedFrame);
                });
            });
            dialog->addButton("menus/common/no"_i18n, [dialog](brls::View*) {
                dialog->close();
            });
            dialog->setCancelable(true);
            dialog->open();
        });

    }

    brls::ListItem* explorer = new brls::ListItem("explorer/files"_i18n);
    explorer->setHeight(LISTITEM_HEIGHT);
    explorer->getClickEvent()->subscribe([](brls::View*) {
        brls::Application::pushView(new FileExplorerPage());
    });

    auto* removePorts = new brls::ListItem("menus/tools/remove_ports"_i18n);
    removePorts->setHeight(LISTITEM_HEIGHT);
    removePorts->getClickEvent()->subscribe([](brls::View*) { openPortUninstaller(); });

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

    brls::ListItem* cleanUp = new brls::ListItem("menus/tools/clean_up"_i18n);
    cleanUp->getClickEvent()->subscribe([](brls::View* view) {
        auto* dialog = new brls::Dialog("menus/tools/clean_up_confirm"_i18n);
        dialog->addButton("menus/common/yes"_i18n, [dialog](brls::View*) {
            dialog->close([]() {
                auto* stagedFrame = new brls::StagedAppletFrame();
                stagedFrame->setTitle("menus/tools/clean_up"_i18n);
                stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/tools/clean_up_running"_i18n, cleanDownloadedFiles));
                stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "menus/tools/clean_up_done"_i18n));
                brls::Application::pushView(stagedFrame);
            });
        });
        dialog->addButton("menus/common/no"_i18n, [dialog](brls::View*) { dialog->close(); });
        dialog->setCancelable(true);
        dialog->open();
    });
    cleanUp->setHeight(LISTITEM_HEIGHT);

    if (updateApp) {
        this->addView(compactHeader("menus/tools/section_app"_i18n));
        this->addView(updateApp);
    }

    this->addView(compactHeader("menus/tools/section_files"_i18n));
    this->addView(explorer);
    this->addView(removePorts);
    if (!util::getBoolValue(hideStatus, "cheats")) this->addView(cheats);
    if (!util::getBoolValue(hideStatus, "move")) this->addView(move);
    if (!util::getBoolValue(hideStatus, "cleanup")) this->addView(cleanUp);

    this->addView(compactHeader("menus/diagnostics/section"_i18n));
    auto* errorLog = new brls::ListItem("menus/diagnostics/errors_title"_i18n);
    errorLog->setHeight(LISTITEM_HEIGHT);
    errorLog->getClickEvent()->subscribe([](brls::View*) { openErrorLogPage(); });
    this->addView(errorLog);

    auto* connectionTest = new brls::ListItem("menus/diagnostics/connection_title"_i18n);
    connectionTest->setHeight(LISTITEM_HEIGHT);
    connectionTest->getClickEvent()->subscribe([](brls::View*) { openConnectionPage(); });
    this->addView(connectionTest);

    auto* sdStatus = new brls::ListItem("menus/diagnostics/sd_title"_i18n);
    sdStatus->setHeight(LISTITEM_HEIGHT);
    sdStatus->getClickEvent()->subscribe([](brls::View*) { openSdCardPage(); });
    this->addView(sdStatus);

    const bool showJoyCon = !util::getBoolValue(hideStatus, "jccolor");
    const bool showProCon = !util::getBoolValue(hideStatus, "pccolor");
    if (showJoyCon || showProCon)
        this->addView(compactHeader("menus/tools/section_controllers"_i18n));
    if (showJoyCon) this->addView(JCcolor);
    if (showProCon) this->addView(PCcolor);

    const bool showNetwork = !util::getBoolValue(hideStatus, "netsettings");
    const bool showBrowser = !util::getBoolValue(hideStatus, "browser");
    const bool showPayload = erista && !util::getBoolValue(hideStatus, "rebootpayload");
    if (showNetwork || showBrowser || showPayload)
        this->addView(compactHeader(showPayload
            ? "menus/tools/section_connection"_i18n
            : "menus/tools/section_network"_i18n));
    if (showNetwork) this->addView(netSettings);
    if (showBrowser) this->addView(browser);
    if (showPayload) this->addView(rebootPayload);
}

void ToolsTab::customSpacing(brls::View* current, brls::View* next, int* spacing)
{
    if (dynamic_cast<brls::Header*>(next))
        *spacing = 15;
    else if (dynamic_cast<brls::Header*>(current))
        *spacing = 8;
}
