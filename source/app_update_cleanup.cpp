#include "app_update_cleanup.hpp"
#include "constants.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace app_update_cleanup {
namespace fs = std::filesystem;
namespace {
bool sameFile(const fs::path& left, const fs::path& right)
{
    if (!fs::exists(left) || !fs::exists(right)) return false;
    auto remaining = fs::file_size(left);
    if (!remaining || remaining != fs::file_size(right)) return false;
    std::ifstream first(left, std::ios::binary), second(right, std::ios::binary);
    std::array<char, 4096> a, b;
    while (remaining) {
        const auto n = std::min<std::uintmax_t>(a.size(), remaining);
        if (!first.read(a.data(), n) || !second.read(b.data(), n))
            throw fs::filesystem_error("read", !first ? left : right,
                                       std::make_error_code(std::errc::io_error));
        if (!std::equal(a.begin(), a.begin() + n, b.begin())) return false;
        remaining -= n;
    }
    return true;
}

bool mayHaveReplacedApp(const char* journal)
{
    if (!fs::exists(journal)) return false;
    if (fs::file_size(journal) > 64) return true;
    std::ifstream input(journal);
    std::string state;
    if (!std::getline(input, state)) return true;
    if (!state.empty() && state.back() == '\r') state.pop_back();
    return state != "STAGED" && state != "BACKUP_IN_PROGRESS" && state != "BACKUP_COMPLETE";
}

// Leave the saved boot file in place until the app journal is retired.
Result prepareBoot()
{
    const fs::path live = MARIKO_PAYLOAD_PATH, saved = MARIKO_PAYLOAD_PATH_TEMP;
    if (!fs::exists(saved))
        return sameFile(live, RCM_PAYLOAD_PATH) ? Result::bootRecovery : Result::done;
    if (!fs::is_regular_file(saved) || !fs::file_size(saved) || sameFile(saved, RCM_PAYLOAD_PATH))
        return Result::bootRecovery;
    if (sameFile(saved, live)) return Result::done;
    if (fs::exists(live) && !sameFile(live, RCM_PAYLOAD_PATH)) return Result::bootRecovery;
    if (!sameFile(saved, UPDATE_BIN_PATH) && !sameFile(saved, REBOOT_PAYLOAD_PATH))
        return Result::bootRecovery;

    // Prepare a verified replacement first. If replacing the destination is
    // unsupported by the SD driver, keep both originals for recovery.
    fs::create_directories(APP_PAYLOAD_UPDATE_ROOT);
    const auto replacement = fs::path(APP_PAYLOAD_UPDATE_ROOT) / "boot-restore.tmp";
    fs::copy_file(saved, replacement, fs::copy_options::overwrite_existing);
    if (!sameFile(saved, replacement)) return Result::bootRecovery;
    fs::rename(replacement, live);
    return sameFile(saved, live) ? Result::done : Result::bootRecovery;
}
}

bool pending()
{
    return fs::exists(APP_PAYLOAD_UPDATE_ROOT) || fs::exists(MARIKO_PAYLOAD_PATH_TEMP);
}

Result recoverBoot(bool cfwPending)
{
    if (cfwPending) return Result::cfwPending;
    const auto boot = prepareBoot();
    if (boot == Result::done) fs::remove(MARIKO_PAYLOAD_PATH_TEMP);
    return boot;
}

Result clean(bool cfwPending)
{
    if (cfwPending) return Result::cfwPending;
    const auto backup = fs::path(APP_PAYLOAD_UPDATE_BACKUP_PATH) / "aio-switch-updater-mod.nro";
    if (fs::exists(APP_PAYLOAD_UPDATE_BACKUP_PATH)) {
        for (const auto& item : fs::directory_iterator(APP_PAYLOAD_UPDATE_BACKUP_PATH))
            if (item.path().filename() != backup.filename() || !item.is_regular_file())
                return Result::appRecovery;
    }
    if ((fs::exists(backup) || mayHaveReplacedApp(APP_PAYLOAD_UPDATE_STATE_PATH) ||
         mayHaveReplacedApp(APP_PAYLOAD_UPDATE_STATE_TMP_PATH)) &&
        !sameFile(NRO_PATH, backup) && !sameFile(NRO_PATH, APP_PAYLOAD_UPDATE_STAGED_NRO))
        return Result::appRecovery;
    const auto boot = prepareBoot();
    if (boot != Result::done) return boot;

    fs::remove(APP_PAYLOAD_UPDATE_READY_PATH);
    fs::remove(APP_PAYLOAD_UPDATE_READY_TMP_PATH);
    fs::remove(APP_PAYLOAD_UPDATE_STATE_TMP_PATH);
    fs::remove(APP_PAYLOAD_UPDATE_STATE_PATH);
    fs::remove(backup);
    fs::remove(APP_PAYLOAD_UPDATE_BACKUP_PATH);
    fs::remove_all(APP_PAYLOAD_UPDATE_NEW_PATH);
    fs::remove(fs::path(APP_PAYLOAD_UPDATE_ROOT) / "boot-restore.tmp");
    fs::remove(APP_PAYLOAD_UPDATE_ROOT);
    fs::remove(APP_FILENAME);
    fs::remove(MARIKO_PAYLOAD_PATH_TEMP);
    return Result::done;
}
}
