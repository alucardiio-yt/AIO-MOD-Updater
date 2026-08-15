#include <switch.h>

#include <borealis.hpp>
#include <filesystem>
#include <json.hpp>

#include "constants.hpp"
#include "current_cfw.hpp"
#include "fs.hpp"
#include "main_frame.hpp"
#include "utils.hpp"
#include "warning_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

CFW CurrentCfw::running_cfw;

int main(int argc, char* argv[])
{
    
    if (!brls::Application::init(APP_TITLE)) {
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

    
    util::finalizeCfwUpdateAfterSuccessfulBoot();

    brls::Logger::setLogLevel(brls::LogLevel::DEBUG);
    brls::Logger::debug("Start");

    if (std::filesystem::exists(HIDDEN_AIO_FILE)) {
        brls::Application::pushView(new MainFrame());
    }
    else {
        brls::Application::pushView(new WarningPage("menus/main/launch_warning"_i18n));
    }

    while (brls::Application::mainLoop())
        ;

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
