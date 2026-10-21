#include "ui/activity_log.hpp"

#include "defines.hpp"
#include "i18n.hpp"
#include "ui/nvg_util.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace sphaira::ui::activity_log {
namespace {

struct Entry {
    std::string tag;
    std::string text;
    Tone tone{};
};

constexpr std::size_t MAX_ENTRIES = 500;
constexpr float LINE_HEIGHT = 27.f;

Mutex g_mutex{};
std::vector<Entry> g_entries{};
std::size_t g_offset_from_tail{};
std::size_t g_visible_capacity{1};

Mutex g_copy_mutex{};
CopyStatus g_copy{};
u64 g_next_copy_id{};
u64 g_copy_started_ns{};

auto colourForTone(Theme* theme, Tone tone) -> NVGcolor {
    switch (tone) {
        case Tone::Muted:
            return theme->GetColour(ThemeEntryID_TEXT_INFO);
        case Tone::Success:
            return nvgRGBA(83, 214, 139, 255);
        case Tone::Warning:
            return nvgRGBA(238, 183, 82, 255);
        case Tone::Error:
            return theme->GetColour(ThemeEntryID_ERROR);
        case Tone::Normal:
        default:
            return theme->GetColour(ThemeEntryID_TEXT);
    }
}

bool isOneOf(Result result, std::initializer_list<Result> values) {
    return std::find(values.begin(), values.end(), result) != values.end();
}

auto resultExplanation(Result result) -> std::string {
    if (R_SUCCEEDED(result)) {
        return i18n::get("aio_result_success", "Completed");
    }

    if (isOneOf(result, {
            Result_TransferCancelled, Result_UsbCancelled, Result_UsbUploadExit,
            SvcError_Cancelled, SvcError_TerminationRequested})) {
        return i18n::get("aio_result_cancelled", "Operation cancelled or USB connection interrupted");
    }

    if (isOneOf(result, {
            MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer),
            static_cast<Result>(FsError_UnsupportedSdkVersion)})) {
        return i18n::get("aio_result_old_firmware", "System firmware is too old or incompatible");
    }

    if (isOneOf(result, {
            static_cast<Result>(FsError_UsableSpaceNotEnoughMmcCalibration),
            static_cast<Result>(FsError_UsableSpaceNotEnoughMmcSafe),
            static_cast<Result>(FsError_UsableSpaceNotEnoughMmcUser),
            static_cast<Result>(FsError_UsableSpaceNotEnoughMmcSystem),
            static_cast<Result>(FsError_UsableSpaceNotEnoughSdCard)})) {
        return i18n::get("aio_result_no_space", "Not enough free storage space");
    }

    if (result == static_cast<Result>(FsError_PathNotFound)) {
        return i18n::get("aio_result_path_missing", "File or path was not found");
    }
    if (result == static_cast<Result>(FsError_PortSdCardNoDevice)) {
        return i18n::get("aio_result_sd_unavailable", "microSD is unavailable or was removed");
    }
    if (result == static_cast<Result>(NcmError_ContentNotFound) || result == Result_YatiNcaNotFound) {
        return i18n::get("aio_result_content_missing", "A required content file was not found");
    }
    if (result == static_cast<Result>(NcmError_ContentMetaNotFound)) {
        return i18n::get("aio_result_content_meta_missing", "Required content metadata (CNMT) was not found");
    }
    if (result == static_cast<Result>(NcmError_InvalidContentMetaDatabase) ||
        result == Result_YatiNcmDbCorruptHeader || result == Result_YatiNcmDbCorruptInfos) {
        return i18n::get("aio_result_database", "Installed-content database is invalid or damaged");
    }

    if (isOneOf(result, {
            Result_YatiContainerNotFound, Result_NspBadMagic, Result_NspInvalidHeader,
            Result_XciBadMagic, Result_XciSecurePartitionNotFound,
            Result_MspMissingManifest, Result_MspInvalidManifest, Result_MspInvalidTitleId,
            Result_MspInvalidVersion, Result_MspInvalidPatchset, Result_MspInvalidEntry,
            Result_MspDuplicateEntry, Result_MspNoPayload})) {
        return i18n::get("aio_result_package_invalid", "Package is invalid, incomplete or unreadable");
    }

    if (isOneOf(result, {
            Result_NcaBadMagic, Result_YatiInvalidNcaReadSize,
            Result_YatiInvalidNcaSigKeyGen, Result_YatiInvalidNcaMagic})) {
        return i18n::get("aio_result_nca_invalid", "NCA content is invalid or unreadable");
    }

    if (isOneOf(result, {
            Result_NcaFailedNcaHeaderHashVerify, Result_YatiInvalidNcaSignature0,
            Result_YatiInvalidNcaSignature1, Result_YatiInvalidNcaSha256})) {
        return i18n::get("aio_result_verification_failed", "Content verification failed; the file may be damaged");
    }

    if (isOneOf(result, {
            Result_KeyMissingNcaKeyArea, Result_KeyMissingTitleKek,
            Result_KeyMissingMasterKey, Result_KeyFailedDecyptETicketDeviceKey,
            Result_NcaBadSigKeyGen})) {
        return i18n::get("aio_result_keys_missing", "Required keys are missing or do not support this content");
    }

    if (isOneOf(result, {
            Result_EsBadTitleKeyType, Result_EsPersonalisedTicketDeviceIdMissmatch,
            Result_EsFailedDecryptPersonalisedTicket, Result_EsBadDecryptedPersonalisedTicketSize,
            Result_EsBadTicketSize, Result_EsInvalidTicketBadRightsId,
            Result_EsInvalidTicketFromatVersion, Result_EsInvalidTicketKeyType,
            Result_EsInvalidTicketKeyRevision, Result_YatiTicketNotFound,
            Result_YatiInvalidTicketBadRightsId, Result_YatiCertNotFound})) {
        return i18n::get("aio_result_ticket_invalid", "Ticket, certificate or title key is missing or invalid");
    }

    if (isOneOf(result, {
            Result_YatiNczSectionNotFound, Result_YatiInvalidNczSectionCount,
            Result_YatiNczBlockNotFound, Result_YatiInvalidNczBlockVersion,
            Result_YatiInvalidNczBlockType, Result_YatiInvalidNczBlockTotal,
            Result_YatiInvalidNczBlockSizeExponent, Result_YatiInvalidNczZstdError})) {
        return i18n::get("aio_result_ncz_invalid", "Compressed NCZ data is invalid or damaged");
    }

    if (isOneOf(result, {
            Result_UsbDsBadDeviceSpeed, Result_UsbBadMagic, Result_UsbBadVersion,
            Result_UsbBadCount, Result_UsbBadBufferAlign, Result_UsbBadTransferSize,
            Result_UsbEmptyTransferSize, Result_UsbOverflowTransferSize,
            Result_UsbUploadBadMagic, Result_UsbUploadBadCount,
            Result_UsbUploadBadTransferSize, Result_UsbUploadBadTotalSize,
            Result_UsbUploadBadCommand,
            MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsRead),
            MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite)})) {
        return i18n::get("aio_result_usb_error", "USB communication failed; reconnect or try another cable or port");
    }

    if (isOneOf(result, {
            Result_StreamBadSeek, Result_FsUnknownStdioError, Result_FsStdioFailedToSeek,
            Result_FsStdioFailedToRead, Result_FsStdioFailedToWrite,
            Result_FsStdioFailedToOpenFile, Result_FsStdioFailedToCreate,
            Result_FsStdioFailedToTruncate, Result_FsStdioFailedToFlush,
            MAKERESULT(Module_Libnx, LibnxError_IoError)})) {
        return i18n::get("aio_result_io_error", "File could not be read or written");
    }

    if (isOneOf(result, {
            SvcError_OutOfResource, SvcError_OutOfMemory, SvcError_OutOfHandles,
            OsError_ResultOutOfMemory, OsError_ResultOutOfResource,
            MAKERESULT(Module_Libnx, LibnxError_OutOfMemory),
            MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed)})) {
        return i18n::get("aio_result_memory", "Not enough memory or system resources");
    }

    switch (R_MODULE(result)) {
        case Module_Fs:
            return i18n::get("aio_result_fs_error", "Filesystem or microSD error");
        case Module_Ncm:
            return i18n::get("aio_result_database", "Installed-content database is invalid or damaged");
        case Module_Usb:
            return i18n::get("aio_result_usb_error", "USB communication failed; reconnect or try another cable or port");
        case Module_Sphaira:
            return i18n::get("aio_result_installer_error", "Installer could not process this package");
        default:
            return i18n::get("aio_result_unknown", "Unknown error; use the code below for diagnosis");
    }
}

} // namespace

void Add(const std::string& tag, const std::string& text, Tone tone) {
    SCOPED_MUTEX(&g_mutex);

    // When the user is inspecting older output, keep the same lines on screen
    // even if the installer appends new messages in the background.
    if (g_offset_from_tail && !g_entries.empty()) {
        ++g_offset_from_tail;
    }

    g_entries.push_back({tag, text, tone});
    if (g_entries.size() > MAX_ENTRIES) {
        g_entries.erase(g_entries.begin());
        if (g_offset_from_tail) {
            --g_offset_from_tail;
        }
    }
}

void AddResult(Result result, Tone tone) {
    char code[64]{};
    std::snprintf(code, sizeof(code), "0x%08X  (M%u D%u)",
        static_cast<u32>(result), static_cast<unsigned>(R_MODULE(result)),
        static_cast<unsigned>(R_DESCRIPTION(result)));
    Add("[CODE]", code, tone);
    Add(R_SUCCEEDED(result) ? "[RESULT]" : "[CAUSE]", resultExplanation(result), tone);
}

void ScrollOlder(std::size_t lines) {
    SCOPED_MUTEX(&g_mutex);
    const auto max_offset = g_entries.size() > g_visible_capacity
        ? g_entries.size() - g_visible_capacity
        : 0;
    g_offset_from_tail = std::min(g_offset_from_tail + lines, max_offset);
}

void ScrollNewer(std::size_t lines) {
    SCOPED_MUTEX(&g_mutex);
    g_offset_from_tail = lines >= g_offset_from_tail ? 0 : g_offset_from_tail - lines;
}

auto BeginCopy(const std::string& path, u64 total) -> u64 {
    u64 id{};
    {
        SCOPED_MUTEX(&g_copy_mutex);
        id = ++g_next_copy_id;
        g_copy = {
            .active = true,
            .path = path,
            .id = id,
            .copied = 0,
            .total = total,
            .bytes_per_second = 0,
        };
        g_copy_started_ns = armTicksToNs(armGetSystemTick());
    }

    Add("[COPY]", i18n::get("aio_mtp_copying", "Copying to microSD") + ": " + path, Tone::Normal);
    return id;
}

void UpdateCopy(u64 id, u64 copied, u64 total) {
    SCOPED_MUTEX(&g_copy_mutex);
    if (!g_copy.active || g_copy.id != id) {
        return;
    }

    g_copy.copied = std::max(g_copy.copied, copied);
    g_copy.total = std::max(g_copy.total, total);
    const auto now_ns = armTicksToNs(armGetSystemTick());
    const auto elapsed_ns = now_ns > g_copy_started_ns ? now_ns - g_copy_started_ns : 0;
    if (elapsed_ns) {
        g_copy.bytes_per_second = static_cast<u64>(
            static_cast<double>(g_copy.copied) * 1000000000.0 / static_cast<double>(elapsed_ns));
    }
}

void FinishCopy(u64 id, bool complete, Result result) {
    std::string path;
    u64 copied{};
    bool publish{};
    {
        SCOPED_MUTEX(&g_copy_mutex);
        if (g_copy.id != id) {
            return;
        }
        path = g_copy.path;
        copied = g_copy.copied;
        g_copy.active = false;
        publish = true;
    }

    if (!publish) {
        return;
    }

    if (complete && R_SUCCEEDED(result)) {
        Add("[COPY OK]", i18n::get("aio_mtp_copy_done", "Copy completed") + ": " + path, Tone::Success);
        Add("[DATA]", utils::formatSizeNetwork(copied), Tone::Muted);
    } else {
        Add("[COPY ERROR]", i18n::get("aio_mtp_copy_incomplete", "Copy incomplete") + ": " + path, Tone::Error);
        if (R_FAILED(result)) {
            AddResult(result, Tone::Error);
        }
    }
}

auto GetCopyStatus() -> CopyStatus {
    SCOPED_MUTEX(&g_copy_mutex);
    return g_copy;
}

auto Draw(NVGcontext* vg, Theme* theme, float x, float y, float w, float h) -> DrawInfo {
    std::vector<Entry> visible;
    DrawInfo info{};

    const auto capacity = std::max<std::size_t>(1, static_cast<std::size_t>((h - 24.f) / LINE_HEIGHT));
    {
        SCOPED_MUTEX(&g_mutex);
        const auto total = g_entries.size();
        g_visible_capacity = capacity;
        const auto max_offset = total > capacity ? total - capacity : 0;
        g_offset_from_tail = std::min(g_offset_from_tail, max_offset);
        const auto end = total > g_offset_from_tail ? total - g_offset_from_tail : 0;
        const auto start = end > capacity ? end - capacity : 0;

        visible.assign(g_entries.begin() + start, g_entries.begin() + end);
        info.first = visible.empty() ? 0 : start + 1;
        info.last = end;
        info.total = total;
        info.following_tail = g_offset_from_tail == 0;
    }

    gfx::drawRect(vg, x, y, w, h, nvgRGBA(7, 6, 11, 245), 3.f);
    gfx::drawRect(vg, x, y, 3.f, h, theme->GetColour(ThemeEntryID_FOCUS));

    nvgSave(vg);
    nvgIntersectScissor(vg, x + 10.f, y + 8.f, w - 20.f, h - 16.f);
    float line_y = y + 25.f;
    for (const auto& entry : visible) {
        const auto line = entry.tag + "  " + entry.text;
        gfx::drawText(vg, x + 22.f, line_y, 17.f, colourForTone(theme, entry.tone),
            line.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        line_y += LINE_HEIGHT;
    }
    nvgRestore(vg);

    return info;
}

} // namespace sphaira::ui::activity_log
