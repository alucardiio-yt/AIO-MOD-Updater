#include <switch.h>
#include <memory>
#include <string>
#include "app.hpp"
#include "log.hpp"
#include "owo.hpp"
#include "ui/menus/mtp_menu.hpp"
#include "ui/menus/menu_base.hpp"
#include "utils/core.hpp"
#include "i18n.hpp"
#include <cstdio>
#include <cstring>

namespace {
long aioLanguageIndex() {
    FILE* file = std::fopen("sdmc:/config/aio-switch-updater-mod/language.json", "rb");
    if (!file) return -1;
    char data[256]{};
    const auto read = std::fread(data, 1, sizeof(data) - 1, file);
    std::fclose(file);
    data[read] = '\0';
    if (std::strstr(data, "\"es\"")) return 6;
    if (std::strstr(data, "\"fr\"")) return 3;
    if (std::strstr(data, "\"en-US\"") || std::strstr(data, "\"en\"")) return 1;
    return -1;
}
}

int main(int argc, char** argv) {
    if (!argc || !argv) {
        return 1;
    }

    if (R_FAILED(sphaira::utils::SetCurrentThreadSafeAffinity())) {
        return 1;
    }

    auto app = std::make_unique<sphaira::App>(argv[0]);

    if (const auto language = aioLanguageIndex(); language >= 0) {
        sphaira::i18n::exit();
        sphaira::i18n::init(language);
    }
    for (size_t i = 0; i < sphaira::App::GetThemeMetaList().size(); ++i) {
        if (sphaira::App::GetThemeMetaList()[i].name == "AIO MOD") {
            sphaira::App::SetTheme(static_cast<s64>(i));
            break;
        }
    }
    sphaira::App::SetThemeMusicEnable(false);

    // AIO MOD launches this helper intentionally as an installer. Keep the
    // engine isolated from the user's normal Sphaira configuration, allow
    // installation on either sysMMC or emuMMC, and target SD by default.
    sphaira::App::SetMtpEnable(false);
    sphaira::App::SetInstallSysmmcEnable(true);
    sphaira::App::SetInstallEmummcEnable(true);
    sphaira::App::SetInstallSdEnable(true);

    app->Push<sphaira::ui::menu::mtp::Menu>(sphaira::ui::menu::MenuFlag_None);
    app->Loop();

    // Returning with B closes the helper and chainloads AIO MOD again.
    const char* aio_path = "sdmc:/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro";
    const std::string aio_argv = std::string("\"") + aio_path + "\"";
    envSetNextLoad(aio_path, aio_argv.c_str());
    return 0;
}

extern "C" {

void userAppInit(void) {
    sphaira::App::SetBoostMode(true);

    // https://github.com/mtheall/ftpd/blob/e27898f0c3101522311f330e82a324861e0e3f7e/source/switch/init.c#L31
    const SocketInitConfig socket_config_application = {
        .tcp_tx_buf_size = 1024 * 64,
        .tcp_rx_buf_size = 1024 * 64,
        .tcp_tx_buf_max_size = 1024 * 1024 * 4,
        .tcp_rx_buf_max_size = 1024 * 1024 * 4,
        .udp_tx_buf_size = 0x2400, // same as default
        .udp_rx_buf_size = 0xA500, // same as default
        .sb_efficiency = 8,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_Auto,
    };

    const SocketInitConfig socket_config_applet = {
        .tcp_tx_buf_size = 1024 * 32,
        .tcp_rx_buf_size = 1024 * 64,
        .tcp_tx_buf_max_size = 1024 * 256,
        .tcp_rx_buf_max_size = 1024 * 256,
        .udp_tx_buf_size = 0x2400, // same as default
        .udp_rx_buf_size = 0xA500, // same as default
        .sb_efficiency = 4,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_Auto,
    };

    const auto is_application = sphaira::App::IsApplication();

    const auto socket_config = is_application ? socket_config_application : socket_config_applet;

    Result rc;
    if (R_FAILED(rc = appletLockExit()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = socketInitialize(&socket_config)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = plInitialize(PlServiceType_User)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = nifmInitialize(NifmServiceType_User)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = accountInitialize(is_application ? AccountServiceType_Application : AccountServiceType_System)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = setInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = hidsysInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = ncmInitialize()))
        diagAbortWithResult(rc);

    // it doesn't matter if this fails.
    appletSetScreenShotPermission(AppletScreenShotPermission_Enable);

    log_nxlink_init();
}

void userAppExit(void) {
    log_nxlink_exit();

    ncmExit();
    hidsysExit();
    setExit();
    accountExit();
    nifmExit();
    plExit();
    socketExit();
    // NOTE (DMC): prevents exfat corruption.
    if (auto fs = fsdevGetDeviceFileSystem("sdmc:")) {
        fsFsCommit(fs);
    }

    sphaira::App::SetBoostMode(false);
    appletUnlockExit();

    if (sphaira::core_launch_pending()) {
        svcExitProcess();
    }
}

} // extern "C"
