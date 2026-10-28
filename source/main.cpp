#include <switch.h>

#include <borealis.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <json.hpp>

#include "constants.hpp"
#include "current_cfw.hpp"
#include "fs.hpp"
#include "file_explorer_page.hpp"
#include "main_frame.hpp"
#include "utils.hpp"
#include "warning_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

CFW CurrentCfw::running_cfw;

static void showWhatsNewPage(const std::vector<std::string>& pages, std::size_t index,
                            const std::filesystem::path& shownVersionPath)
{
    auto* dialog = new brls::Dialog(pages[index]);
    if (index > 0) {
        dialog->addButton("menus/common/back"_i18n, [dialog, pages, index, shownVersionPath](brls::View*) {
            dialog->close([pages, index, shownVersionPath]() { showWhatsNewPage(pages, index - 1, shownVersionPath); });
        });
    }
    if (index + 1 < pages.size()) {
        dialog->addButton("menus/common/continue"_i18n, [dialog, pages, index, shownVersionPath](brls::View*) {
            dialog->close([pages, index, shownVersionPath]() { showWhatsNewPage(pages, index + 1, shownVersionPath); });
        });
    }
    else {
        dialog->addButton("menus/common/ok"_i18n, [dialog, shownVersionPath](brls::View*) {
            dialog->close([shownVersionPath]() {
                std::ofstream output(shownVersionPath, std::ios::trunc);
                if (output)
                    output << APP_VERSION << '\n';
            });
        });
    }
    dialog->open();
}

static std::vector<std::string> whatsNewPages(const std::string& notes)
{
    std::istringstream stream(notes);
    std::string title, line;
    std::getline(stream, title);
    std::vector<std::string> pages;
    std::string page = title;
    unsigned entries = 0;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        if (entries == 2) {
            pages.push_back(page);
            page = title;
            entries = 0;
        }
        page += "\n\n" + line;
        ++entries;
    }
    pages.push_back(page);
    return pages;
}

static void cleanupLegacyUpdateArtifacts()
{
    const char* legacyDirectories[] = {
        "/config/aio-switch-updater-mod/v3-transition",
        "/config/aio-switch-updater-mod/v4-transition",
        "/config/aio-switch-updater-mod/switch",
        "/switch/aio-v3-updater",
        "/switch/aio-v4-updater",
        "/switch/aio-mod-updater",
    };

    for (const char* path : legacyDirectories) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            continue;

        ec.clear();
        std::filesystem::remove_all(path, ec);
        if (ec)
            brls::Logger::warning("Legacy updater cleanup: unable to remove {}", path);
        else
            brls::Logger::info("Legacy updater cleanup: removed {}", path);
    }

    const char* legacyFiles[] = {
        "/config/aio-switch-updater-mod/aiosu-forwarder.nro",
        "/config/aio-switch-updater-mod/v3-transition.log",
        "/config/aio-switch-updater-mod/v4-transition.log",
    };

    for (const char* path : legacyFiles) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            continue;

        ec.clear();
        std::filesystem::remove(path, ec);
        if (ec)
            brls::Logger::warning("Legacy updater cleanup: unable to remove {}", path);
        else
            brls::Logger::info("Legacy updater cleanup: removed {}", path);
    }

    // Old self-updater builds could leave versioned NRO backups beside the
    // installed application (aio-switch-updater-mod-v*.nro and .star files).
    const std::filesystem::path appDirectory("/switch/aio-switch-updater-mod");
    std::error_code ec;
    if (!std::filesystem::exists(appDirectory, ec) || ec)
        return;

    std::filesystem::directory_iterator it(appDirectory, ec), end;
    if (ec)
        return;

    for (; it != end; it.increment(ec)) {
        if (ec)
            break;

        const std::string name = it->path().filename().string();
        if (name.rfind("aio-switch-updater-mod-v", 0) != 0)
            continue;

        std::error_code removeEc;
        std::filesystem::remove(it->path(), removeEc);
        if (removeEc)
            brls::Logger::warning("Legacy updater cleanup: unable to remove {}", it->path().string());
        else
            brls::Logger::info("Legacy updater cleanup: removed {}", it->path().string());
    }
}

int main(int argc, char* argv[])
{
    
    // AIO MOD uses a dark background in both Switch system themes.
    auto* appThemes = new brls::LibraryViewsThemeVariantsWrapper(
        new brls::HorizonDarkTheme(), new brls::HorizonDarkTheme());
    if (!brls::Application::init(APP_TITLE, nullptr, appThemes)) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    nlohmann::ordered_json languageFile = fs::parseJsonFile(LANGUAGE_JSON);
    if (languageFile.find("language") != languageFile.end()) {
        std::string langCode = languageFile["language"].get<std::string>();
        i18n::loadTranslations(langCode);
    }
    else {
        i18n::loadTranslations();
    }

        
#ifndef __SWITCH__
    brls::Logger::setLogLevel(brls::LogLevel::DEBUG);
#endif

    setsysInitialize();
    plInitialize(PlServiceType_User);
    nsInitialize();
    socketInitializeDefault();
    nxlinkStdio();
    pmdmntInitialize();
    pminfoInitialize();
    splInitialize();
    romfsInit();

    CurrentCfw::running_cfw = CurrentCfw::getCFW();

    fs::createTree(CONFIG_PATH);

    // AIO MOD 4.0 is already running at this point, so legacy transition
    // helpers/backups are no longer needed. Current payload transactions are
    // intentionally NOT touched here.
    cleanupLegacyUpdateArtifacts();

    util::finalizeCfwUpdateAfterSuccessfulBoot();

    brls::Logger::setLogLevel(brls::LogLevel::DEBUG);
    brls::Logger::debug("Start");

    const bool existingInstallation = std::filesystem::exists(HIDDEN_AIO_FILE);
    if (existingInstallation) {
        brls::Application::pushView(new MainFrame());
    }
    else {
        brls::Application::pushView(new WarningPage("menus/main/launch_warning"_i18n));
    }

    // Older versions had no version marker. Show the new-version notes once
    // for existing installations, then remember this build after a successful launch.
    const std::filesystem::path shownVersionPath = std::string(CONFIG_PATH) + "last_shown_version.txt";
    std::string shownVersion;
    { std::ifstream input(shownVersionPath); std::getline(input, shownVersion); }
    const bool showNotes = existingInstallation && shownVersion != APP_VERSION && std::string(APP_VERSION) == "5.2";
    if (showNotes)
        showWhatsNewPage(whatsNewPages("menus/common/whats_new_52"_i18n), 0, shownVersionPath);
    else if (shownVersion != APP_VERSION) {
        std::ofstream output(shownVersionPath, std::ios::trunc);
        if (output)
            output << APP_VERSION << '\n';
    }

    PadState explorerShortcutPad{};
    padInitializeDefault(&explorerShortcutPad);
    padUpdate(&explorerShortcutPad);
    while (brls::Application::mainLoop()) {
        padUpdate(&explorerShortcutPad);
        if (!(padGetButtonsDown(&explorerShortcutPad) & HidNpadButton_ZL))
            continue;

        // Only open the explorer while the main screen owns focus. Dialogs,
        // editors and the explorer itself keep their own ZL behavior.
        for (brls::View* focused = brls::Application::getCurrentFocus(); focused;
             focused = focused->getParent()) {
            if (dynamic_cast<MainFrame*>(focused)) {
                brls::Application::pushView(new FileExplorerPage());
                break;
            }
        }
    }

    romfsExit();
    splExit();
    pminfoExit();
    pmdmntExit();
    socketExit();
    nsExit();
    setsysExit();
    plExit();
    return EXIT_SUCCESS;
}
