#include "mtp_install_page.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <switch.h>

#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
constexpr const char ENGINE_ROMFS[] = "romfs:/aio_mtp_engine.nro";
constexpr const char ENGINE_DIR[] = "/switch/aio-switch-updater-mod/config/mtp-engine";
constexpr const char ENGINE_PATH[] = "/switch/aio-switch-updater-mod/config/mtp-engine/aio-mtp-engine.nro";
constexpr const char ENGINE_TEMP[] = "/switch/aio-switch-updater-mod/config/mtp-engine/aio-mtp-engine.nro.tmp";

bool validNro(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.good())
        return false;

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0x1000)
        return false;

    file.seekg(0x10, std::ios::beg);
    std::array<char, 4> magic{};
    file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    return file.good() && magic[0] == 'N' && magic[1] == 'R' && magic[2] == 'O' && magic[3] == '0';
}

bool copyEngineAtomically()
{
    std::error_code ec;
    std::filesystem::create_directories(ENGINE_DIR, ec);
    if (ec)
        return false;

    std::filesystem::remove(ENGINE_TEMP, ec);
    ec.clear();

    std::ifstream input(ENGINE_ROMFS, std::ios::binary);
    if (!input.good())
        return false;
    input.seekg(0, std::ios::end);
    const auto expectedSize = input.tellg();
    input.seekg(0, std::ios::beg);
    if (!input.good() || expectedSize < 0x1000)
        return false;

    std::ofstream output(ENGINE_TEMP, std::ios::binary | std::ios::trunc);
    if (!output.good())
        return false;

    std::array<char, 1024 * 256> buffer{};
    std::streamoff copied = 0;
    while (input.good()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
            copied += count;
            if (!output.good())
                break;
        }
    }

    output.flush();
    const bool copyComplete = !input.bad() && output.good() && copied == expectedSize;
    output.close();
    input.close();

    if (!copyComplete || !output.good() || !validNro(ENGINE_TEMP)) {
        std::filesystem::remove(ENGINE_TEMP, ec);
        return false;
    }

    std::filesystem::remove(ENGINE_PATH, ec);
    ec.clear();
    std::filesystem::rename(ENGINE_TEMP, ENGINE_PATH, ec);
    if (ec) {
        std::filesystem::remove(ENGINE_TEMP, ec);
        return false;
    }

    return validNro(ENGINE_PATH);
}
} // namespace

MtpInstallPage::MtpInstallPage() : brls::List()
{
    auto* description = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        "menus/mtp_installer/description"_i18n,
        true);
    this->addView(description);

    auto* formats = new brls::ListItem("menus/mtp_installer/formats"_i18n);
    formats->setValue("NSP / XCI / NSZ / XCZ / MSP");
    formats->setHeight(LISTITEM_HEIGHT);
    this->addView(formats);

    auto* launch = new brls::ListItem("menus/mtp_installer/launch"_i18n);
    launch->setHeight(LISTITEM_HEIGHT);
    launch->getClickEvent()->subscribe([this](brls::View*) {
        this->launchEngine();
        return true;
    });
    this->addView(launch);

}

void MtpInstallPage::cleanLegacyMtpState()
{
    // These directories belong exclusively to AIO MOD's retired experimental
    // MTP/NCM implementation. The Sphaira/YATI engine does not use them.
    std::error_code ec;
    std::filesystem::remove_all("/switch/aio-switch-updater-mod/config/install_work", ec);
    ec.clear();
    std::filesystem::remove_all("/switch/aio-switch-updater-mod/config/mtp_inbox", ec);
}

bool MtpInstallPage::prepareEngine()
{
    cleanLegacyMtpState();

    // Always refresh the hidden SD copy from the NRO embedded in this AIO MOD
    // build. This guarantees the helper and AIO release cannot drift apart.
    return copyEngineAtomically();
}

void MtpInstallPage::launchEngine()
{
    if (!prepareEngine()) {
        util::showDialogBoxInfo("menus/mtp_installer/launch_failed"_i18n);
        return;
    }

    const std::string path = std::string("sdmc:") + ENGINE_PATH;
    const std::string argv = "\"" + path + "\"";

    envSetNextLoad(path.c_str(), argv.c_str());
    romfsExit();
    brls::Application::quit();
}
