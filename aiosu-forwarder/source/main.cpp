#include <filesystem>
#include <string>

#include <switch.h>

#define PATH           "/switch/aio-switch-updater-mod/"
#define FULL_PATH      "/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro"
#define CONFIG_PATH    "/config/aio-switch-updater-mod/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro"
#define BACKUP_PATH    "/switch/aio-switch-updater-mod/aio-switch-updater-mod-update-backup.nro"
#define FORWARDER_PATH "/config/aio-switch-updater-mod/aiosu-forwarder.nro"
#define CONFIG_SWITCH  "/config/aio-switch-updater-mod/switch/"

static int removeDir(const char* path)
{
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        return -1;
    return fsFsDeleteDirectoryRecursively(fs, path);
}

int main(int argc, char* argv[])
{
    std::error_code ec;
    std::filesystem::create_directories(PATH, ec);

    if (std::filesystem::exists(CONFIG_PATH)) {
        
        std::filesystem::remove(BACKUP_PATH, ec);
        if (std::filesystem::exists(FULL_PATH)) {
            std::filesystem::rename(FULL_PATH, BACKUP_PATH, ec);
            if (ec) {
                envSetNextLoad(FULL_PATH, FULL_PATH);
                return 0;
            }
        }

        ec.clear();
        std::filesystem::rename(CONFIG_PATH, FULL_PATH, ec);
        if (ec) {
            
            if (std::filesystem::exists(BACKUP_PATH)) {
                std::error_code rollbackEc;
                std::filesystem::rename(BACKUP_PATH, FULL_PATH, rollbackEc);
            }
            envSetNextLoad(FULL_PATH, FULL_PATH);
            return 0;
        }

        std::filesystem::remove(BACKUP_PATH, ec);
        removeDir(CONFIG_SWITCH);
    }

    std::filesystem::remove(FORWARDER_PATH, ec);

    envSetNextLoad(FULL_PATH, FULL_PATH);
    return 0;
}
