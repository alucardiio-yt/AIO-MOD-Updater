#include "apps_tab.hpp"

#include <filesystem>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "worker_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

constexpr const char APPS_JSON_URL[] = "https://raw.githubusercontent.com/alucardiio-yt/archives-nx/main/apps.json";
constexpr const char APPS_TEMP_PATH[] = "/config/aio-switch-updater-mod/apps_temp_download";

AppsTab::AppsTab() : brls::List()
{
    std::string appsDescription = "menus/apps/description"_i18n;
    appsDescription += "\n";
    appsDescription += "menus/apps/sysmodule_notice"_i18n;

    brls::Label* description = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        appsDescription,
        true);
    this->addView(description);

    this->createList();
}

void AppsTab::createList()
{
    std::vector<std::pair<std::string, std::string>> links = download::getLinks(APPS_JSON_URL);

    if (links.empty()) {
        this->displayNotFound();
        return;
    }

    for (const auto& link : links) {
        const std::string label = link.first;
        const std::string url   = link.second;

        brls::ListItem* item = new brls::ListItem(label);
        item->setHeight(LISTITEM_HEIGHT);

        item->getClickEvent()->subscribe([label, url](brls::View* view) {

            bool isZip = url.size() >= 4 &&
                         url.substr(url.size() - 4) == ".zip";

            std::string confirmText = isZip
                ? fmt::format("menus/apps/confirm_zip"_i18n, label)
                : fmt::format("menus/apps/confirm_nro"_i18n, label);

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
                    if (std::filesystem::exists(APPS_TEMP_PATH)) {
                        std::filesystem::remove(APPS_TEMP_PATH);
                    }
                }));
            }
            else {
                std::string filename = url.substr(url.find_last_of('/') + 1);
                std::string destPath = std::string("/switch/") + filename;

                stagedFrame->addStage(new WorkerPage(stagedFrame, "menus/apps/downloading"_i18n, [url, destPath]() {
                    fs::createTree("/switch/");
                    download::downloadFile(url, destPath, OFF);
                }));
            }

            stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "menus/apps/done"_i18n));
            brls::Application::pushView(stagedFrame);
            return true;
        });

        this->addView(item);
    }
}

void AppsTab::displayNotFound()
{
    brls::Label* notFound = new brls::Label(
        brls::LabelStyle::SMALL,
        "menus/apps/load_failed"_i18n,
        true);
    notFound->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->addView(notFound);
}
