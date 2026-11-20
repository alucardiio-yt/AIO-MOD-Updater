#include <switch.h>

#include <borealis.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>
#include <json.hpp>

#include "constants.hpp"
#include "app_theme.hpp"
#include "current_cfw.hpp"
#include "fs.hpp"
#include "file_explorer_page.hpp"
#include "main_frame.hpp"
#include "utils.hpp"
#include "warning_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

CFW CurrentCfw::running_cfw;

namespace {
class SplashView final : public brls::View {
public:
    SplashView() : image(std::make_unique<brls::Image>("romfs:/splash.png"))
    {
        image->setScaleType(brls::ImageScaleType::FIT);
        image->setParent(this);
    }

    void show(std::function<void(void)> callback, bool, brls::ViewAnimation animation) override
    {
        brls::View::show(std::move(callback), false, animation);
    }

    void hide(std::function<void(void)> callback, bool animated,
              brls::ViewAnimation animation) override
    {
        brls::View::hide([this, callback = std::move(callback)]() {
            image.reset();
            callback();
        }, animated, animation);
    }

    void layout(NVGcontext*, brls::Style*, brls::FontStash*) override
    {
        if (image)
            image->setBoundaries(x, y, width, height);
    }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style*, brls::FrameContext* ctx) override
    {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        nvgFill(vg);
        if (image)
            image->frame(ctx);

        const float scale = std::min(width / 1280.0f, height / 720.0f);
        const float center = x + width / 2.0f;

        const std::string title = "menus/common/starting_app"_i18n;
        nvgFontFaceId(vg, ctx->fontStash->regular);
        nvgFontSize(vg, 21.0f * scale);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGB(218, 215, 220));
        nvgText(vg, center, y + height * (637.0f / 720.0f), title.c_str(), nullptr);
    }

private:
    std::unique_ptr<brls::Image> image;
};
}

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
    
    // Both variants use the selected palette so the Switch system color setting
    // cannot silently replace it with low-contrast colors.
    auto* appThemes = new brls::LibraryViewsThemeVariantsWrapper(
        new brls::HorizonDarkTheme(), new brls::HorizonDarkTheme());
    if (!brls::Application::init(APP_TITLE, nullptr, appThemes)) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }
    app_theme::load();
    app_theme::apply(appThemes);

    nlohmann::ordered_json languageFile = fs::parseJsonFile(LANGUAGE_JSON);
    if (languageFile.find("language") != languageFile.end()) {
        std::string langCode = languageFile["language"].get<std::string>();
        i18n::loadTranslations(langCode);
    }
    else {
        i18n::loadTranslations();
    }

    romfsInit();
    brls::Application::pushView(new SplashView());
    if (!brls::Application::mainLoop()) {
        romfsExit();
        return EXIT_SUCCESS;
    }

#ifndef __SWITCH__
    brls::Logger::setLogLevel(brls::LogLevel::DEBUG);
#endif

    setsysInitialize();
    plInitialize(PlServiceType_User);
    nsInitialize();
    socketInitializeDefault();
    const bool curlInitialized = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
    if (!curlInitialized)
        brls::Logger::error("Unable to initialize libcurl");
    nxlinkStdio();
    pmdmntInitialize();
    pminfoInitialize();
    splInitialize();

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
    const std::string currentVersion = APP_VERSION;
    const bool showNotes = existingInstallation && shownVersion != currentVersion &&
        (currentVersion == "5.2" || currentVersion == "5.2.1" || currentVersion == "5.3");
    if (showNotes)
        showWhatsNewPage(whatsNewPages(currentVersion == "5.3"
            ? "menus/common/whats_new_53"_i18n
            : currentVersion == "5.2.1"
                ? "menus/common/whats_new_521"_i18n
                : "menus/common/whats_new_52"_i18n), 0, shownVersionPath);
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
    if (curlInitialized)
        curl_global_cleanup();
    splExit();
    pminfoExit();
    pmdmntExit();
    socketExit();
    nsExit();
    setsysExit();
    plExit();
    return EXIT_SUCCESS;
}
