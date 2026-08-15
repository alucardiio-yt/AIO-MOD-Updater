#include "utils.hpp"

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "current_cfw.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "main_frame.hpp"
#include "progress_event.hpp"
#include "reboot_payload.h"
#include "unistd.h"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace util {

    constexpr const char CFW_UPDATE_ROOT_NATIVE[] = "/aio-mod-update";
    constexpr const char CFW_UPDATE_NEW_NATIVE[] = "/aio-mod-update/new";

    enum class CfwDirProbe {
        Missing,
        Empty,
        NonEmpty,
        Error
    };

    static FsFileSystem* cfwSdFileSystem()
    {
        return fsdevGetDeviceFileSystem("sdmc");
    }

    

    static CfwDirProbe cfwProbeDirectory(const char* path)
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return CfwDirProbe::Error;

        FsDir dir{};
        Result rc = fsFsOpenDirectory(sd, path,
            FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
        if (R_SUCCEEDED(rc)) {
            FsDirectoryEntry entry{};
            s64 count = 0;
            rc = fsDirRead(&dir, &count, 1, &entry);
            fsDirClose(&dir);
            if (R_FAILED(rc))
                return CfwDirProbe::Error;
            return count == 0 ? CfwDirProbe::Empty : CfwDirProbe::NonEmpty;
        }

        
        rc = fsFsCreateDirectory(sd, path);
        if (R_SUCCEEDED(rc)) {
            fsFsDeleteDirectory(sd, path);
            return CfwDirProbe::Missing;
        }

        return CfwDirProbe::Error;
    }

    

    static bool cfwResetUpdateRootNative()
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return false;

        Result rc = fsFsCleanDirectoryRecursively(sd, CFW_UPDATE_ROOT_NATIVE);
        if (R_SUCCEEDED(rc))
            return R_SUCCEEDED(fsFsCommit(sd));

        rc = fsFsCreateDirectory(sd, CFW_UPDATE_ROOT_NATIVE);
        if (R_FAILED(rc))
            return false;
        return R_SUCCEEDED(fsFsCommit(sd));
    }

    static bool cfwCreateDirectoryNative(const char* path)
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return false;

        Result rc = fsFsCreateDirectory(sd, path);
        if (R_SUCCEEDED(rc))
            return true;

        const CfwDirProbe probe = cfwProbeDirectory(path);
        return probe == CfwDirProbe::Empty || probe == CfwDirProbe::NonEmpty;
    }

    static bool cfwDeleteTreeNative(FsFileSystem* sd, const char* path)
    {
        FsDirEntryType type{};
        Result rc = fsFsGetEntryType(sd, path, &type);
        if (R_FAILED(rc))
            return true; 
        if (type != FsDirEntryType_Dir)
            return false;

        rc = fsFsCleanDirectoryRecursively(sd, path);
        if (R_FAILED(rc))
            return false;
        rc = fsFsDeleteDirectory(sd, path);
        return R_SUCCEEDED(rc);
    }

    
    
    static bool cfwCommitCleanupNative()
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return false;

        if (!cfwDeleteTreeNative(sd, "/aio-mod-update/backup"))
            return false;
        if (!cfwDeleteTreeNative(sd, "/aio-mod-update/new"))
            return false;

        const char* files_before_state[] = {
            "/aio-mod-update/recovery.bin",
            "/aio-mod-update/recovery.bin.tmp",
            "/aio-mod-update/ready",
            "/aio-mod-update/ready.tmp",
            "/aio-mod-update/payload.next",
            "/aio-mod-update/updater-live.bin"
        };

        for (const char* path : files_before_state) {
            FsDirEntryType type{};
            Result rc = fsFsGetEntryType(sd, path, &type);
            if (R_SUCCEEDED(rc)) {
                if (type != FsDirEntryType_File || R_FAILED(fsFsDeleteFile(sd, path)))
                    return false;
            }
        }

        
        {
            FsDirEntryType type{};
            if (R_SUCCEEDED(fsFsGetEntryType(sd, CFW_UPDATE_STATE_TMP_PATH, &type)) &&
                (type != FsDirEntryType_File || R_FAILED(fsFsDeleteFile(sd, CFW_UPDATE_STATE_TMP_PATH))))
                return false;
        }

        if (R_FAILED(fsFsCommit(sd)))
            return false;

        {
            FsDirEntryType type{};
            if (R_SUCCEEDED(fsFsGetEntryType(sd, CFW_UPDATE_STATE_PATH, &type)) &&
                (type != FsDirEntryType_File || R_FAILED(fsFsDeleteFile(sd, CFW_UPDATE_STATE_PATH))))
                return false;
        }

        if (R_FAILED(fsFsCommit(sd)))
            return false;

        
        fsFsDeleteDirectory(sd, CFW_UPDATE_ROOT_NATIVE);
        return R_SUCCEEDED(fsFsCommit(sd));
    }

    static bool cfwNativeFileSize(const char* path, s64* out_size = nullptr)
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return false;

        FsFile file{};
        Result rc = fsFsOpenFile(sd, path, FsOpenMode_Read, &file);
        if (R_FAILED(rc))
            return false;

        s64 size = 0;
        rc = fsFileGetSize(&file, &size);
        fsFileClose(&file);
        if (R_FAILED(rc) || size <= 0)
            return false;

        if (out_size)
            *out_size = size;
        return true;
    }

    static bool cfwDeleteFileNative(const char* path, bool commit = false)
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return false;

        FsDirEntryType type{};
        Result rc = fsFsGetEntryType(sd, path, &type);
        if (R_FAILED(rc))
            return true; 
        if (type != FsDirEntryType_File)
            return false;

        rc = fsFsDeleteFile(sd, path);
        if (R_FAILED(rc))
            return false;
        return !commit || R_SUCCEEDED(fsFsCommit(sd));
    }

    
    
    static bool cfwWriteTextFileAtomic(const char* final_path, const char* tmp_path,
                                       const std::string& text)
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd || text.empty())
            return false;

        cfwDeleteFileNative(tmp_path);

        Result rc = fsFsCreateFile(sd, tmp_path, static_cast<s64>(text.size()), 0);
        if (R_FAILED(rc))
            return false;

        FsFile file{};
        rc = fsFsOpenFile(sd, tmp_path, FsOpenMode_Write, &file);
        if (R_FAILED(rc)) {
            cfwDeleteFileNative(tmp_path);
            return false;
        }

        rc = fsFileWrite(&file, 0, text.data(), text.size(), FsWriteOption_Flush);
        fsFileClose(&file);
        if (R_FAILED(rc)) {
            cfwDeleteFileNative(tmp_path);
            return false;
        }

        cfwDeleteFileNative(final_path);
        rc = fsFsRenameFile(sd, tmp_path, final_path);
        if (R_FAILED(rc)) {
            cfwDeleteFileNative(tmp_path);
            return false;
        }

        return R_SUCCEEDED(fsFsCommit(sd));
    }

    static std::string cfwReadSmallTextFileNative(const char* path)
    {
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return "";

        FsFile file{};
        Result rc = fsFsOpenFile(sd, path, FsOpenMode_Read, &file);
        if (R_FAILED(rc))
            return "";

        s64 size = 0;
        rc = fsFileGetSize(&file, &size);
        if (R_FAILED(rc) || size <= 0 || size > 256) {
            fsFileClose(&file);
            return "";
        }

        std::string text(static_cast<size_t>(size), '\0');
        u64 bytes_read = 0;
        rc = fsFileRead(&file, 0, &text[0], static_cast<u64>(size),
                        FsReadOption_None, &bytes_read);
        fsFileClose(&file);
        if (R_FAILED(rc) || bytes_read == 0)
            return "";

        text.resize(static_cast<size_t>(bytes_read));
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r' || text.back() == '\0'))
            text.pop_back();
        return text;
    }

    

    static std::string cfwReadSmallTextFileStdio(const char* native_path)
    {
        if (!native_path || native_path[0] != '/')
            return "";

        std::string dev_path = std::string("sdmc:") + native_path;
        FILE* fp = std::fopen(dev_path.c_str(), "rb");
        if (!fp)
            return "";

        char buf[257]{};
        const size_t got = std::fread(buf, 1, 256, fp);
        std::fclose(fp);
        if (got == 0)
            return "";

        std::string text(buf, got);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r' || text.back() == '\0'))
            text.pop_back();
        return text;
    }

    static int g_recovery_copy_stage = 0;

    static bool copyRecoveryPayloadAtomic()
    {
        g_recovery_copy_stage = 0;
        constexpr const char RECOVERY_TMP[] = "/aio-mod-update/recovery.bin.tmp";

        
        g_recovery_copy_stage = 1;
        std::ifstream src(RCM_PAYLOAD_PATH, std::ios::binary);
        if (!src.good())
            return false;

        std::vector<unsigned char> payload;
        payload.reserve(128 * 1024);
        char read_buffer[16 * 1024];
        while (true) {
            src.read(read_buffer, sizeof(read_buffer));
            const std::streamsize got = src.gcount();
            if (got > 0) {
                const auto* begin = reinterpret_cast<const unsigned char*>(read_buffer);
                payload.insert(payload.end(), begin, begin + got);
            }
            if (got < static_cast<std::streamsize>(sizeof(read_buffer)))
                break;
        }

        g_recovery_copy_stage = 2;
        if (src.bad() || payload.empty())
            return false;
        src.close();

        g_recovery_copy_stage = 3;
        FsFileSystem* sd = cfwSdFileSystem();
        if (!sd)
            return false;

        cfwDeleteFileNative(RECOVERY_TMP);
        g_recovery_copy_stage = 4;
        Result rc = fsFsCreateFile(sd, RECOVERY_TMP, static_cast<s64>(payload.size()), 0);
        if (R_FAILED(rc))
            return false;

        g_recovery_copy_stage = 5;
        FsFile dst{};
        rc = fsFsOpenFile(sd, RECOVERY_TMP, FsOpenMode_Write, &dst);
        if (R_FAILED(rc)) {
            cfwDeleteFileNative(RECOVERY_TMP);
            return false;
        }

        g_recovery_copy_stage = 6;
        u64 offset = 0;
        bool ok = true;
        while (offset < payload.size()) {
            const u64 remaining = static_cast<u64>(payload.size()) - offset;
            const u64 chunk = std::min<u64>(remaining, 16 * 1024);
            rc = fsFileWrite(&dst, static_cast<s64>(offset), payload.data() + offset,
                             chunk, FsWriteOption_None);
            if (R_FAILED(rc)) {
                ok = false;
                break;
            }
            offset += chunk;
        }

        if (ok)
            ok = offset == payload.size() && R_SUCCEEDED(fsFileFlush(&dst));
        fsFileClose(&dst);

        if (!ok) {
            cfwDeleteFileNative(RECOVERY_TMP, true);
            return false;
        }

        g_recovery_copy_stage = 7;
        s64 tmp_size = 0;
        if (!cfwNativeFileSize(RECOVERY_TMP, &tmp_size) ||
            tmp_size != static_cast<s64>(payload.size())) {
            cfwDeleteFileNative(RECOVERY_TMP, true);
            return false;
        }

        g_recovery_copy_stage = 8;
        cfwDeleteFileNative(CFW_UPDATE_RECOVERY_PAYLOAD_PATH);
        rc = fsFsRenameFile(sd, RECOVERY_TMP, CFW_UPDATE_RECOVERY_PAYLOAD_PATH);
        if (R_FAILED(rc) || R_FAILED(fsFsCommit(sd))) {
            cfwDeleteFileNative(RECOVERY_TMP, true);
            return false;
        }

        g_recovery_copy_stage = 9;
        s64 final_size = 0;
        const bool final_ok = cfwNativeFileSize(CFW_UPDATE_RECOVERY_PAYLOAD_PATH, &final_size) &&
               final_size == static_cast<s64>(payload.size());
        if (final_ok)
            g_recovery_copy_stage = 0;
        return final_ok;
    }

    bool isArchive(const std::string& path)
    {
        if (std::filesystem::exists(path)) {
            std::ifstream is(path, std::ifstream::binary);
            char zip_signature[4] = {0x50, 0x4B, 0x03, 0x04};  
            char signature[4];
            is.read(signature, 4);
            if (is.good() && std::equal(std::begin(signature), std::end(signature), std::begin(zip_signature), std::end(zip_signature))) {
                return true;
            }
        }
        return false;
    }

    void downloadArchive(const std::string& url, contentType type)
    {
        long status_code;
        downloadArchive(url, type, status_code);
    }

    void downloadArchive(const std::string& url, contentType type, long& status_code)
    {
        fs::createTree(DOWNLOAD_PATH);
        switch (type) {
            case contentType::custom:
                status_code = download::downloadFile(url, CUSTOM_FILENAME, OFF);
                break;
            case contentType::cheats:
                status_code = download::downloadFile(url, CHEATS_FILENAME, OFF);
                break;
            case contentType::fw:
                status_code = download::downloadFile(url, FIRMWARE_FILENAME, OFF);
                break;
            case contentType::app:
                status_code = download::downloadFile(url, APP_FILENAME, OFF);
                break;
            case contentType::bootloaders:
                status_code = download::downloadFile(url, BOOTLOADER_FILENAME, OFF);
                break;
            case contentType::ams_cfw:
                status_code = download::downloadFile(url, AMS_FILENAME, OFF);
                break;
            default:
                break;
        }
        ProgressEvent::instance().setStatusCode(status_code);
    }

    void showDialogBoxInfo(const std::string& text)
    {
        brls::Dialog* dialog;
        dialog = new brls::Dialog(text);
        brls::GenericEvent::Callback callback = [dialog](brls::View* view) {
            dialog->close();
        };
        dialog->addButton("menus/common/ok"_i18n, callback);
        dialog->setCancelable(true);
        dialog->open();
    }

    int showDialogBoxBlocking(const std::string& text, const std::string& opt)
    {
        int result = -1;
        brls::Dialog* dialog = new brls::Dialog(text);
        brls::GenericEvent::Callback callback = [dialog, &result](brls::View* view) {
            result = 0;
            dialog->close();
        };
        dialog->addButton(opt, callback);
        dialog->setCancelable(false);
        dialog->open();
        while (result == -1) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(800000));
        return result;
    }

    int showDialogBoxBlocking(const std::string& text, const std::string& opt1, const std::string& opt2)
    {
        int result = -1;
        brls::Dialog* dialog = new brls::Dialog(text);
        brls::GenericEvent::Callback callback1 = [dialog, &result](brls::View* view) {
            result = 0;
            dialog->close();
        };
        brls::GenericEvent::Callback callback2 = [dialog, &result](brls::View* view) {
            result = 1;
            dialog->close();
        };
        dialog->addButton(opt1, callback1);
        dialog->addButton(opt2, callback2);
        dialog->setCancelable(false);
        dialog->open();
        while (result == -1) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(800000));
        return result;
    }

    void crashIfNotArchive(contentType type)
    {
        std::string filename;
        switch (type) {
            case contentType::custom:
                filename = CUSTOM_FILENAME;
                break;
            case contentType::cheats:
                filename = CHEATS_FILENAME;
                break;
            case contentType::fw:
                filename = FIRMWARE_FILENAME;
                break;
            case contentType::app:
                filename = APP_FILENAME;
                break;
            case contentType::bootloaders:
                filename = BOOTLOADER_FILENAME;
                break;
            case contentType::ams_cfw:
                filename = AMS_FILENAME;
                break;
            default:
                return;
        }
        if (!isArchive(filename)) {
            ProgressEvent::instance().setStatusCode(406);
            ProgressEvent::instance().setStep(ProgressEvent::instance().getMax());
        }
    }

    static std::string readCfwUpdateStateFile(const std::string& path)
    {
        std::string state = cfwReadSmallTextFileNative(path.c_str());
        if (!state.empty())
            return state;
        return cfwReadSmallTextFileStdio(path.c_str());
    }

    static std::string readCfwUpdateState()
    {
        std::string state = readCfwUpdateStateFile(CFW_UPDATE_STATE_PATH);
        if (!state.empty())
            return state;
        return readCfwUpdateStateFile(CFW_UPDATE_STATE_TMP_PATH);
    }

    static bool writeCfwUpdateState(const std::string& state)
    {
        if (!cfwCreateDirectoryNative(CFW_UPDATE_ROOT_NATIVE))
            return false;

        return cfwWriteTextFileAtomic(CFW_UPDATE_STATE_PATH,
                                      CFW_UPDATE_STATE_TMP_PATH,
                                      state + "\n");
    }

    bool hasPendingCfwUpdateTransaction()
    {
        if (!readCfwUpdateState().empty())
            return true;

        if (cfwNativeFileSize(CFW_UPDATE_READY_PATH))
            return true;

        
        
        const CfwDirProbe root = cfwProbeDirectory(CFW_UPDATE_ROOT_NATIVE);
        if (root == CfwDirProbe::Missing || root == CfwDirProbe::Empty)
            return false;
        if (root == CfwDirProbe::Error)
            return true;

        const CfwDirProbe backup = cfwProbeDirectory("/aio-mod-update/backup");

        
        return backup == CfwDirProbe::Empty ||
               backup == CfwDirProbe::NonEmpty ||
               backup == CfwDirProbe::Error;
    }

    bool finalizeCfwUpdateAfterSuccessfulBoot()
    {
        const std::string state = readCfwUpdateState();

        
        
        if (state == "ROLLBACK_IN_PROGRESS" || state == "ROLLBACK_FAILED")
            return false;

        if (state != "PENDING_BOOT" &&
            state != "FINALIZE_BOOT_PAYLOAD" &&
            state != "COMMIT_IN_PROGRESS") {
            return false;
        }

        
        
        if (state != "COMMIT_IN_PROGRESS" && !writeCfwUpdateState("COMMIT_IN_PROGRESS")) {
            brls::Logger::error("CFW updater: unable to write COMMIT_IN_PROGRESS");
            return false;
        }

        
        if (!isErista()) {

            cfwDeleteFileNative(MARIKO_PAYLOAD_PATH_TEMP, true);
        }

        
        if (!cfwCommitCleanupNative()) {
            brls::Logger::error("CFW updater: native commit cleanup failed");
            return false;
        }

        brls::Logger::info("CFW updater: new CFW boot confirmed; transaction committed");
        return true;
    }

    void prepareCfwUpdateStaging()
    {
        auto& progress = ProgressEvent::instance();

        
        
        if (hasPendingCfwUpdateTransaction()) {
            progress.setErrorMessage("menus/ams_update/cfw_pending_transaction"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        if (!cfwResetUpdateRootNative()) {
            progress.setErrorMessage("menus/ams_update/cfw_cleanup_failed"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        if (!cfwCreateDirectoryNative(CFW_UPDATE_NEW_NATIVE)) {
            progress.setErrorMessage("menus/ams_update/cfw_staging_create_failed"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        
        extract::extract(AMS_FILENAME, CFW_UPDATE_NEW_PATH, false);

        
        
        if (!copyRecoveryPayloadAtomic()) {
            std::string message = "menus/ams_update/cfw_recovery_payload_failed"_i18n;

            message += " [R" + std::to_string(g_recovery_copy_stage) + "]";
            progress.setErrorMessage(message);
            progress.setStep(progress.getMax());
            return;
        }
    }

    void validateCfwUpdateStaging(bool erista, bool recoveryAlreadyVerified)
    {
        auto& progress = ProgressEvent::instance();
        progress.setOperation(ProgressOperation::GENERIC);
        progress.setCurrentFile("menus/ams_update/cfw_validating_package"_i18n);
        progress.setTotalSteps(1);
        progress.setStep(0);

        
        const bool hasAtmosphere = cfwNativeFileSize("/aio-mod-update/new/atmosphere/package3");
        const bool hasHekate = cfwNativeFileSize("/aio-mod-update/new/bootloader/update.bin");
        const bool hasRebootPayload = cfwNativeFileSize("/aio-mod-update/new/atmosphere/reboot_payload.bin");
        const bool hasRootPayload = cfwNativeFileSize("/aio-mod-update/new/payload.bin");

        
        const bool hasRecoveryPayload = recoveryAlreadyVerified ||
            cfwNativeFileSize(CFW_UPDATE_RECOVERY_PAYLOAD_PATH);

        auto clearMarkers = []() {
            FsFileSystem* sd = cfwSdFileSystem();
            if (!sd)
                return;
            cfwDeleteFileNative(CFW_UPDATE_READY_PATH);
            cfwDeleteFileNative(CFW_UPDATE_STATE_PATH);
            cfwDeleteFileNative(CFW_UPDATE_STATE_TMP_PATH);
            fsFsCommit(sd);
        };

        if (!hasAtmosphere) {
            clearMarkers();
            progress.setErrorMessage("menus/ams_update/cfw_missing_package3"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        
        if ((!erista && !hasRootPayload) ||
            (erista && !hasHekate && !hasRebootPayload && !hasRootPayload)) {
            clearMarkers();
            progress.setErrorMessage("menus/ams_update/cfw_invalid_boot_method"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        if (!hasRecoveryPayload) {
            clearMarkers();
            progress.setErrorMessage("menus/ams_update/cfw_recovery_missing"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        if (!writeCfwUpdateState("STAGED")) {
            clearMarkers();
            progress.setErrorMessage("menus/ams_update/cfw_journal_failed"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        if (!cfwWriteTextFileAtomic(CFW_UPDATE_READY_PATH,
                                    "/aio-mod-update/ready.tmp",
                                    "AIO_MOD_CFW_UPDATE_V2\n")) {
            clearMarkers();
            progress.setErrorMessage("menus/ams_update/cfw_ready_marker_failed"_i18n);
            progress.setStep(progress.getMax());
            return;
        }

        progress.setCurrentFile("menus/ams_update/cfw_package_ready"_i18n);
        progress.setStep(progress.getMax());
    }

    void prepareAndValidateCfwUpdateStaging(bool erista)
    {

        
        prepareCfwUpdateStaging();

        auto& progress = ProgressEvent::instance();
        if (!progress.getErrorMessage().empty() ||
            progress.getStatusCode() > 399 ||
            progress.getInterupt()) {
            return;
        }

        validateCfwUpdateStaging(erista, true);

        
        if (progress.getErrorMessage().empty() &&
            progress.getStatusCode() <= 399 &&
            !progress.getInterupt() &&
            readCfwUpdateState() == "STAGED" &&
            cfwNativeFileSize(CFW_UPDATE_READY_PATH)) {
            if (!cfwDeleteFileNative(AMS_FILENAME, true)) {
                brls::Logger::warning("CFW updater: unable to remove consumed ams.zip");
            }
        }
    }

    void extractArchive(contentType type, const std::string& version)
    {
        chdir(ROOT_PATH);
        crashIfNotArchive(type);
        switch (type) {
            case contentType::cheats: {
                std::vector<std::string> titles = extract::getInstalledTitlesNs();
                titles = extract::excludeTitles(CHEATS_EXCLUDE, titles);
                extract::extractCheats(CHEATS_FILENAME, titles, CurrentCfw::running_cfw, version);
                break;
            }
            case contentType::fw:
                fs::removeDir(FIRMWARE_PATH);
                fs::createTree(FIRMWARE_PATH);
                extract::extract(FIRMWARE_FILENAME, FIRMWARE_PATH);
                break;
            case contentType::app:
                extract::extract(APP_FILENAME, CONFIG_PATH);
                fs::copyFile(ROMFS_FORWARDER, FORWARDER_PATH);
                break;
            case contentType::custom: {
                int preserveInis = showDialogBoxBlocking("menus/utils/overwrite_inis"_i18n, "menus/common/yes"_i18n, "menus/common/no"_i18n);
                extract::extract(CUSTOM_FILENAME, ROOT_PATH, preserveInis);
                break;
            }
            case contentType::bootloaders: {
                int preserveInis = showDialogBoxBlocking("menus/utils/overwrite_inis"_i18n, "menus/common/yes"_i18n, "menus/common/no"_i18n);
                extract::extract(BOOTLOADER_FILENAME, ROOT_PATH, preserveInis);
                break;
            }
            case contentType::ams_cfw: {
                int preserveInis = showDialogBoxBlocking("menus/utils/overwrite_inis"_i18n, "menus/common/yes"_i18n, "menus/common/no"_i18n);
                int deleteContents = showDialogBoxBlocking("menus/ams_update/delete_sysmodules_flags"_i18n, "menus/common/no"_i18n, "menus/common/yes"_i18n);
                if (deleteContents == 1)
                    removeSysmodulesFlags(AMS_CONTENTS);
                extract::extract(AMS_FILENAME, ROOT_PATH, preserveInis);
                break;
            }
            default:
                break;
        }
        if (type == contentType::ams_cfw || type == contentType::bootloaders || type == contentType::custom)
            fs::copyFiles(COPY_FILES_TXT);
    }

    std::string formatListItemTitle(const std::string& str, size_t maxScore)
    {
        size_t score = 0;
        for (size_t i = 0; i < str.length(); i++) {
            score += std::isupper(str[i]) ? 4 : 3;
            if (score > maxScore) {
                return str.substr(0, i - 1) + "\u2026";
            }
        }
        return str;
    }

    std::string formatApplicationId(u64 ApplicationId)
    {
        return fmt::format("{:016X}", ApplicationId);
    }

    std::vector<std::string> fetchPayloads()
    {
        std::vector<std::string> payloadPaths;
        payloadPaths.push_back(ROOT_PATH);
        if (std::filesystem::exists(PAYLOAD_PATH)) payloadPaths.push_back(PAYLOAD_PATH);
        if (std::filesystem::exists(AMS_PATH)) payloadPaths.push_back(AMS_PATH);
        if (std::filesystem::exists(REINX_PATH)) payloadPaths.push_back(REINX_PATH);
        if (std::filesystem::exists(BOOTLOADER_PATH)) payloadPaths.push_back(BOOTLOADER_PATH);
        if (std::filesystem::exists(BOOTLOADER_PL_PATH)) payloadPaths.push_back(BOOTLOADER_PL_PATH);
        if (std::filesystem::exists(SXOS_PATH)) payloadPaths.push_back(SXOS_PATH);
        if (std::filesystem::exists(ROMFS_PATH)) payloadPaths.push_back(ROMFS_PATH);
        std::vector<std::string> res;
        for (const auto& path : payloadPaths) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (entry.path().extension().string() == ".bin") {
                    if (entry.path().string() != FUSEE_SECONDARY && entry.path().string() != FUSEE_MTC)
                        res.push_back(entry.path().string().c_str());
                }
            }
        }
        return res;
    }

    void shutDown(bool reboot)
    {
        spsmInitialize();
        spsmShutdown(reboot);
    }

    void rebootToPayload(const std::string& path)
    {
        reboot_to_payload(path.c_str(), CurrentCfw::running_cfw != CFW::ams);
    }

    std::string getLatestTag(const std::string& url)
    {
        nlohmann::ordered_json tag;
        download::getRequest(url, tag, {"accept: application/vnd.github.v3+json"});
        if (tag.find("tag_name") != tag.end())
            return tag["tag_name"];
        else
            return "";
    }

    std::string cleanVersion(std::string version)
    {
        while (!version.empty() && std::isspace(static_cast<unsigned char>(version.front())))
            version.erase(version.begin());
        while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back())))
            version.pop_back();

        if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
            version.erase(version.begin());

        return version;
    }

    bool isNewerVersion(const std::string& latestVersion, const std::string& currentVersion)
    {
        auto parse = [](const std::string& raw) {
            const std::string version = cleanVersion(raw);
            std::vector<int> parts;
            int value = 0;
            bool reading = false;

            for (char ch : version) {
                if (std::isdigit(static_cast<unsigned char>(ch))) {
                    value = value * 10 + (ch - '0');
                    reading = true;
                }
                else if (ch == '.' && reading) {
                    parts.push_back(value);
                    value = 0;
                    reading = false;
                }
                else {
                    break;
                }
            }

            if (reading)
                parts.push_back(value);

            return parts;
        };

        auto latest = parse(latestVersion);
        auto current = parse(currentVersion);
        if (latest.empty() || current.empty())
            return false;

        const size_t count = std::max(latest.size(), current.size());
        latest.resize(count, 0);
        current.resize(count, 0);

        for (size_t i = 0; i < count; ++i) {
            if (latest[i] > current[i]) return true;
            if (latest[i] < current[i]) return false;
        }

        return false;
    }

    std::string downloadFileToString(const std::string& url)
    {
        std::vector<uint8_t> bytes;
        download::downloadFile(url, bytes);
        std::string str(bytes.begin(), bytes.end());
        return str;
    }

    std::string getCheatsVersion()
    {
        std::string res = util::downloadFileToString(CHEATS_URL_VERSION);

        return res;
    }

    void saveToFile(const std::string& text, const std::string& path)
    {
        std::ofstream file(path);
        file << text << std::endl;
    }

    std::string readFile(const std::string& path)
    {
        std::string text = "";
        std::ifstream file(path);
        if (file.good()) {
            file >> text;
        }
        return text;
    }

    std::string getAppPath()
    {
        if (envHasArgv()) {
            std::smatch match;
            std::string argv = (char*)envGetArgv();
            if (std::regex_match(argv, match, std::regex(NRO_PATH_REGEX))) {
                if (match.size() >= 2) {
                    return match[1].str();
                }
            }
        }
        return NRO_PATH;
    }

    void restartApp()
    {
        std::string path = "sdmc:" + getAppPath();
        std::string argv = "\"" + path + "\"";
        envSetNextLoad(path.c_str(), argv.c_str());
        romfsExit();
        brls::Application::quit();
    }

    bool isErista()
    {
        SetSysProductModel model;
        setsysGetProductModel(&model);
        return (model == SetSysProductModel_Nx || model == SetSysProductModel_Copper);
    }

    void removeSysmodulesFlags(const std::string& directory)
    {
        for (const auto& e : std::filesystem::recursive_directory_iterator(directory)) {
            if (e.path().string().find("boot2.flag") != std::string::npos) {
                std::filesystem::remove(e.path());
            }
        }
    }

    std::string lowerCase(const std::string& str)
    {
        std::string res = str;
        std::for_each(res.begin(), res.end(), [](char& c) {
            c = std::tolower(c);
        });
        return res;
    }

    std::string upperCase(const std::string& str)
    {
        std::string res = str;
        std::for_each(res.begin(), res.end(), [](char& c) {
            c = std::toupper(c);
        });
        return res;
    }

    std::string getErrorMessage(long status_code)
    {
        std::string res;
        switch (status_code) {
            case 500:
                res = fmt::format("{0:}: Internal Server Error", status_code);
                break;
            case 503:
                res = fmt::format("{0:}: Service Temporarily Unavailable", status_code);
                break;
            default:
                res = fmt::format("error: {0:}", status_code);
                break;
        }
        return res;
    }

    bool isApplet()
    {
        AppletType at = appletGetAppletType();
        return at != AppletType_Application && at != AppletType_SystemApplication;
    }

    std::set<std::string> getExistingCheatsTids()
    {
        std::string path = getContentsPath();
        std::set<std::string> res;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            std::string cheatsPath = entry.path().string() + "/cheats";
            if (std::filesystem::exists(cheatsPath)) {
                res.insert(util::upperCase(cheatsPath.substr(cheatsPath.length() - 7 - 16, 16)));
            }
        }
        return res;
    }

    std::string getContentsPath()
    {
        std::string path;
        switch (CurrentCfw::running_cfw) {
            case CFW::ams:
                path = std::string(AMS_PATH) + std::string(CONTENTS_PATH);
                break;
            case CFW::rnx:
                path = std::string(REINX_PATH) + std::string(CONTENTS_PATH);
                break;
            case CFW::sxos:
                path = std::string(SXOS_PATH) + std::string(TITLES_PATH);
                break;
        }
        return path;
    }

    bool getBoolValue(const nlohmann::ordered_json& jsonFile, const std::string& key)
    {
        return (jsonFile.find(key) != jsonFile.end()) ? jsonFile.at(key).get<bool>() : false;
    }

    const nlohmann::ordered_json getValueFromKey(const nlohmann::ordered_json& jsonFile, const std::string& key)
    {
        return (jsonFile.find(key) != jsonFile.end()) ? jsonFile.at(key) : nlohmann::ordered_json::object();
    }

    int openWebBrowser(const std::string url)
    {
        Result rc = 0;
        int at = appletGetAppletType();
        if (at == AppletType_Application) {  
            WebCommonConfig conf;
            WebCommonReply out;
            rc = webPageCreate(&conf, url.c_str());
            if (R_FAILED(rc))
                return rc;
            webConfigSetJsExtension(&conf, true);
            webConfigSetPageCache(&conf, true);
            webConfigSetBootLoadingIcon(&conf, true);
            webConfigSetWhitelist(&conf, ".*");
            rc = webConfigShow(&conf, &out);
            if (R_FAILED(rc))
                return rc;
        }
        else {  
            showDialogBoxInfo("menus/utils/applet_webbrowser"_i18n);
        }
        return rc;
    }

}  