#include "aio_cfw_updater.h"

#include <libs/fatfs/ff.h>
#include <mem/heap.h>
#include <soc/bpmp.h>
#include <utils/btn.h>
#include <utils/sprintf.h>
#include <utils/util.h>
#include <string.h>

#include "../fs/fscopy.h"
#include "../fs/fsutils.h"
#include "../fs/fstypes.h"
#include "../fs/readers/folderReader.h"
#include "../gfx/gfx.h"
#include "../gfx/gfxutils.h"
#include "../hid/hid.h"
#include "../utils/vector.h"

extern int launch_payload(char *path);

#define UPDATE_ROOT             "sd:/aio-mod-update"
#define UPDATE_NEW              "sd:/aio-mod-update/new"
#define UPDATE_BACKUP           "sd:/aio-mod-update/backup"
#define UPDATE_READY            "sd:/aio-mod-update/ready"
#define UPDATE_STATE            "sd:/aio-mod-update/state"
#define UPDATE_STATE_TMP        "sd:/aio-mod-update/state.tmp"
#define UPDATE_PAYLOAD_NEXT     "sd:/aio-mod-update/payload.next"
#define UPDATE_UPDATER_SAVED    "sd:/aio-mod-update/updater-live.bin"
#define MARIKO_PAYLOAD_FALLBACK "sd:/payload.bin.aio"

// Phase 2.5 developer-only fault injection. Keep this at 0 for any public
// release. When enabled, tests are armed only by a one-shot text file on SD.
#define AIO_CFW_ENABLE_FAULT_TESTS 0
#define FAULT_TEST_FILE            "sd:/switch/aio-switch-updater-mod/config/cfw_fault_test.txt"
#define FAULT_TEST_USED            "sd:/switch/aio-switch-updater-mod/config/cfw_fault_test.last"

#define STATE_STAGED               "STAGED"
#define STATE_BACKUP_IN_PROGRESS   "BACKUP_IN_PROGRESS"
#define STATE_BACKUP_COMPLETE      "BACKUP_COMPLETE"
#define STATE_COPY_IN_PROGRESS     "COPY_IN_PROGRESS"
#define STATE_COPY_COMPLETE        "COPY_COMPLETE"
#define STATE_FINALIZE_PAYLOAD     "FINALIZE_BOOT_PAYLOAD"
#define STATE_PENDING_BOOT         "PENDING_BOOT"
#define STATE_ROLLBACK_IN_PROGRESS "ROLLBACK_IN_PROGRESS"
#define STATE_ROLLBACK_FAILED      "ROLLBACK_FAILED"
#define STATE_COMMIT_IN_PROGRESS   "COMMIT_IN_PROGRESS"

#if AIO_CFW_ENABLE_FAULT_TESTS
typedef enum {
    FAULT_NONE = 0,
    FAULT_BACKUP_INTERRUPT,
    FAULT_BACKUP_COMPLETE_INTERRUPT,
    FAULT_COPY_INTERRUPT,
    FAULT_COPY_COMPLETE_INTERRUPT,
    FAULT_COPY_ERROR,
    FAULT_ROLLBACK_INTERRUPT,
    FAULT_COMMIT_INTERRUPT
} FaultTestMode;

static FaultTestMode g_fault_test = FAULT_NONE;
static bool g_fault_fired = false;
#endif


// -------------------------------------------------------------------------
// AIO MOD payload UI. Presentation only: transactional filesystem behavior
// stays in the already-validated updater logic below.
// -------------------------------------------------------------------------
#define UI_BG          0xFF17131D
#define UI_PANEL       0xFF211A2C
#define UI_PANEL_2     0xFF2B2337
#define UI_ACCENT      0xFFA855F7
#define UI_TEXT        0xFFF5F3F7
#define UI_MUTED       0xFFB9B1C2
#define UI_SUCCESS     0xFF4ADE80
#define UI_ERROR       0xFFFF5C7A
#define UI_WARNING     0xFFFBBF24
#define UI_BAR_BG      0xFF3A3047

#define UI_PAGE_NONE      0
#define UI_PAGE_PROGRESS  1
#define UI_PAGE_RESULT    2

static const char *g_last_error_stage = "UNKNOWN";
static int g_last_error_code = 0;
static int g_commit_warning_code = 0;
static int g_ui_page = UI_PAGE_NONE;

static void ui_set_error(const char *stage, int code)
{
    g_last_error_stage = stage ? stage : "UNKNOWN";
    g_last_error_code = code;
}

static void ui_draw_shell(void)
{
    gfx_con.mute = false;
    gfx_con.fntsz = 16;
    gfx_box(0, 0, 1279, 719, UI_BG);
    gfx_box(0, 0, 1279, 64, UI_PANEL);
    gfx_box(0, 62, 1279, 65, UI_ACCENT);

    gfx_con_setpos(54, 22);
    gfx_con_setcol(UI_TEXT, 0, UI_BG);
    gfx_printf("AIO MOD");

    gfx_con_setpos(1000, 22);
    gfx_con_setcol(UI_MUTED, 0, UI_BG);
    gfx_printf("CFW UPDATER");
}

static void ui_prepare_progress_page(void)
{
    if (g_ui_page == UI_PAGE_PROGRESS)
        return;

    ui_draw_shell();
    g_ui_page = UI_PAGE_PROGRESS;

    gfx_con_setpos(100, 625);
    gfx_con_setcol(UI_MUTED, 0, UI_BG);
    gfx_printf("Do not power off your console while the update is in progress.");
}

static void ui_clear_text_row(int x0, int y0, int x1, int y1)
{
    gfx_box(x0, y0, x1, y1, UI_BG);
}

static void ui_draw_progress(const char *title, int percent, const char *detail)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    ui_prepare_progress_page();

    // Redraw only the dynamic areas. Avoid clearing the full framebuffer on
    // every component; this removes the visible flashes between operations.
    ui_clear_text_row(100, 118, 1180, 160);
    ui_clear_text_row(100, 174, 1180, 215);
    ui_clear_text_row(100, 228, 1180, 292);
    ui_clear_text_row(100, 372, 1180, 410);

    gfx_con_setpos(100, 130);
    gfx_con_setcol(UI_TEXT, 0, UI_BG);
    gfx_printf("%s", title ? title : "Working...");

    gfx_con_setpos(100, 186);
    gfx_con_setcol(UI_MUTED, 0, UI_BG);
    gfx_printf("%s", detail ? detail : "Please wait...");

    const int bar_x0 = 100;
    const int bar_y0 = 325;
    const int bar_x1 = 1179;
    const int bar_y1 = 357;
    gfx_box(bar_x0, bar_y0, bar_x1, bar_y1, UI_BAR_BG);
    if (percent > 0) {
        int fill = bar_x0 + ((bar_x1 - bar_x0) * percent) / 100;
        gfx_box(bar_x0, bar_y0, fill, bar_y1, UI_ACCENT);
    }

    gfx_con_setpos(100, 382);
    gfx_con_setcol(UI_TEXT, 0, UI_BG);
    gfx_printf("%d%%", percent);
}

static void ui_compact_copy_path(const char *source, char *out, u32 out_size)
{
    if (!out || out_size < 8) return;
    out[0] = 0;

    if (!source) {
        strcpy(out, "Unknown file");
        return;
    }

    const char *path = source;
    const char *prefix = UPDATE_NEW "/";
    const u32 prefix_len = strlen(prefix);
    if (!strncmp(path, prefix, prefix_len))
        path += prefix_len;

    // Hidden compatibility content must never leak to the public UI.
    if (!strncmp(path, "Next/", 5) || !strcmp(path, "Next")) {
        strcpy(out, "Updating compatibility files...");
        return;
    }

    const u32 max_chars = 60;
    u32 len = strlen(path);
    if (len <= max_chars) {
        strncpy(out, path, out_size - 1);
        out[out_size - 1] = 0;
        return;
    }

    const char *tail = path + (len - (max_chars - 3));
    s_printf(out, "...%s", tail);
}

static void ui_copy_progress_callback(const char *source, u32 file_percent)
{
    if (g_ui_page != UI_PAGE_PROGRESS)
        return;

    char display_path[96];
    ui_compact_copy_path(source, display_path, sizeof(display_path));

    // Only this small region changes while a file is copied, so the user gets
    // live filenames without the old scrolling debug console or framebuffer flash.
    ui_clear_text_row(100, 228, 1180, 292);

    gfx_con_setpos(100, 232);
    gfx_con_setcol(UI_MUTED, 0, UI_BG);
    gfx_printf("Current file:");

    gfx_con_setpos(100, 260);
    gfx_con_setcol(UI_TEXT, 0, UI_BG);
    gfx_puts_limit(display_path, 60);

    gfx_con_setpos(1080, 260);
    gfx_con_setcol(UI_MUTED, 0, UI_BG);
    gfx_printf("%3d%%", file_percent);
}

static void ui_wait_for_power(void)
{
    // Require a fresh physical POWER press, not a button that was still held
    // when entering the result screen.
    while (btn_read() & BTN_POWER)
        usleep(10000);
    while (!(btn_read() & BTN_POWER))
        usleep(10000);
    while (btn_read() & BTN_POWER)
        usleep(10000);
}

static void ui_result_page(u32 color, const char *title,
                           const char *message_line_1, const char *message_line_2,
                           const char *action_line_1, const char *action_line_2,
                           const char *stage, int code)
{
    ui_draw_shell();
    g_ui_page = UI_PAGE_RESULT;

    // Fixed rows are intentional. gfx_printf() resets horizontal position on
    // '\n', which caused RC3's second lines to jump to the far left.
    gfx_box(100, 120, 1179, 525, UI_PANEL_2);
    gfx_box(100, 120, 108, 525, color);

    gfx_con_setpos(150, 158);
    gfx_con_setcol(color, 0, UI_BG);
    gfx_printf("%s", title ? title : "Update status");

    gfx_con_setpos(150, 225);
    gfx_con_setcol(UI_TEXT, 0, UI_BG);
    gfx_printf("%s", message_line_1 ? message_line_1 : "");

    if (message_line_2 && message_line_2[0]) {
        gfx_con_setpos(150, 257);
        gfx_printf("%s", message_line_2);
    }

    if (stage && stage[0]) {
        gfx_con_setpos(150, 326);
        gfx_con_setcol(UI_MUTED, 0, UI_BG);
        gfx_printf("Report code: %s / %d", stage, code);
    }

    gfx_con_setpos(150, 426);
    gfx_con_setcol(UI_TEXT, 0, UI_BG);
    gfx_printf("%s", action_line_1 ? action_line_1 : "Press POWER to continue.");

    if (action_line_2 && action_line_2[0]) {
        gfx_con_setpos(150, 458);
        gfx_con_setcol(UI_MUTED, 0, UI_BG);
        gfx_printf("%s", action_line_2);
    }

    gfx_con_setpos(100, 625);
    gfx_con_setcol(UI_MUTED, 0, UI_BG);
    gfx_printf("Need support? Take a photo of this screen before pressing POWER.");
}

static void ui_success_wait(void)
{
    ui_result_page(UI_SUCCESS,
                   "Update completed successfully",
                   "The new CFW was installed and verified.",
                   "Temporary update files were removed.",
                   "Press POWER to start the new CFW.",
                   "",
                   "OK", 0);
    ui_wait_for_power();
}

static void ui_warning_wait(const char *stage, int code)
{
    ui_result_page(UI_WARNING,
                   "Update installed with a warning",
                   "The new CFW is installed, but final cleanup was incomplete.",
                   "Temporary update files may remain on the SD card.",
                   "Press POWER to start the new CFW.",
                   "Please report the code shown above.",
                   stage, code);
    ui_wait_for_power();
}

static void ui_rollback_success_wait(void)
{
    ui_result_page(UI_WARNING,
                   "Update cancelled safely",
                   "An error occurred during the update.",
                   "The previous CFW was restored successfully.",
                   "Press POWER to start the previous CFW.",
                   "",
                   g_last_error_stage, g_last_error_code);
    ui_wait_for_power();
}

static void ui_unchanged_wait(void)
{
    ui_result_page(UI_ERROR,
                   "Update could not be started",
                   "The installed CFW was not modified.",
                   "Your previous installation is still available.",
                   "Press POWER to return to the previous CFW.",
                   "",
                   g_last_error_stage, g_last_error_code);
    ui_wait_for_power();
}

static void ui_fatal_wait_poweroff(const char *stage, int code, const char *message)
{
    // The stage/code carry the diagnostic detail. Keep the visible copy short
    // enough to stay inside the result card on the fixed 16 px console font.
    (void)message;
    ui_result_page(UI_ERROR,
                   "Recovery required",
                   "The update could not be recovered automatically.",
                   "The recovery backup has been preserved.",
                   "Press POWER to turn off the console.",
                   "Do not delete the recovery files from the SD card.",
                   stage, code);
    ui_wait_for_power();
    power_set_state(POWER_OFF);
}

static const char *managed_paths[] = {
    "atmosphere",
    "bootloader",
    "Next",
    "scripts",
    "boot.dat",
    "boot.ini",
    "exosphere.ini",
    "hbmenu.nro",
    "payload.bin"
};

// Compatibility cleanup for a legacy root directory. It remains managed so
// rollback can restore it on a failed update, but its name and contents are
// intentionally never printed by this updater.
static bool is_hidden_legacy_path(const char *name)
{
    return name && strcmp(name, "Next") == 0;
}

static bool path_exists(const char *path)
{
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}


// Recursive deletion without FolderDelete(), because FolderDelete prints each
// filename. This keeps hidden compatibility cleanup completely invisible.
static FRESULT remove_tree_silent(const char *path)
{
    int read_res = 0;
    Vector_t entries = ReadFolder(path, &read_res);
    if (read_res) {
        clearFileVector(&entries);
        return (FRESULT)read_res;
    }

    vecDefArray(FSEntry_t *, fs, entries);
    FRESULT result = FR_OK;

    for (u32 i = 0; i < entries.count && result == FR_OK; i++) {
        char *child = CombinePaths(path, fs[i].name);
        if (!child) {
            result = FR_NOT_ENOUGH_CORE;
            break;
        }

        if (fs[i].isDir)
            result = remove_tree_silent(child);
        else
            result = f_unlink(child);

        free(child);
    }

    clearFileVector(&entries);
    if (result != FR_OK)
        return result;

    return f_unlink(path);
}

static FRESULT remove_path_silent(const char *path)
{
    FILINFO fno;
    FRESULT res = f_stat(path, &fno);

    if (res == FR_NO_FILE || res == FR_NO_PATH)
        return FR_OK;
    if (res != FR_OK)
        return res;

    if (fno.fattrib & AM_DIR)
        return remove_tree_silent(path);

    return f_unlink(path);
}

static FRESULT mkdir_if_needed(const char *path)
{
    FRESULT res = f_mkdir(path);
    return (res == FR_EXIST) ? FR_OK : res;
}

// Sentinel files remember that a managed root path did not exist before the
// update. This lets rollback remove a newly-created staged path instead of
// leaving it behind. Numeric marker names avoid exposing hidden path names.
static void absent_marker_path(u32 index, char *out)
{
    s_printf(out, "%s/.absent%u", UPDATE_BACKUP, index);
}

static FRESULT create_absent_marker(u32 index)
{
    char marker[256];
    absent_marker_path(index, marker);
    if (path_exists(marker))
        return FR_OK;

    FIL fp;
    FRESULT res = f_open(&fp, marker, FA_CREATE_ALWAYS | FA_WRITE);
    if (res == FR_OK)
        f_close(&fp);
    return res;
}

static bool read_state_from(const char *path, char *out, u32 out_size)
{
    if (!out || out_size < 2)
        return false;

    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    UINT read = 0;
    FRESULT res = f_read(&fp, out, out_size - 1, &read);
    f_close(&fp);
    if (res != FR_OK || read == 0)
        return false;

    out[read] = 0;
    for (u32 i = 0; i < read; i++) {
        if (out[i] == '\r' || out[i] == '\n') {
            out[i] = 0;
            break;
        }
    }
    return out[0] != 0;
}

static bool read_state(char *out, u32 out_size)
{
    if (read_state_from(UPDATE_STATE, out, out_size))
        return true;

    // If power was lost in the tiny replace window, state.tmp is a valid
    // fallback and is never discarded merely because state is missing.
    return read_state_from(UPDATE_STATE_TMP, out, out_size);
}

static FRESULT write_state(const char *state)
{
    if (!state)
        return FR_INVALID_PARAMETER;

    FRESULT res = mkdir_if_needed(UPDATE_ROOT);
    if (res != FR_OK)
        return res;

    remove_path_silent(UPDATE_STATE_TMP);

    FIL fp;
    res = f_open(&fp, UPDATE_STATE_TMP, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
        return res;

    UINT written = 0;
    UINT len = (UINT)strlen(state);
    res = f_write(&fp, state, len, &written);
    if (res == FR_OK && written == len) {
        const char nl = '\n';
        UINT nl_written = 0;
        res = f_write(&fp, &nl, 1, &nl_written);
        if (res == FR_OK && nl_written != 1)
            res = FR_DISK_ERR;
    }

    if (res == FR_OK)
        res = f_sync(&fp);
    f_close(&fp);

    if (res != FR_OK)
        return res;

    res = remove_path_silent(UPDATE_STATE);
    if (res != FR_OK)
        return res;

    return f_rename(UPDATE_STATE_TMP, UPDATE_STATE);
}

static bool state_is(const char *state, const char *expected)
{
    return state && expected && strcmp(state, expected) == 0;
}

#if AIO_CFW_ENABLE_FAULT_TESTS
static FaultTestMode parse_fault_test_mode(const char *token)
{
    if (!token || !token[0])
        return FAULT_NONE;
    if (strcmp(token, "backup_interrupt") == 0)
        return FAULT_BACKUP_INTERRUPT;
    if (strcmp(token, "backup_complete_interrupt") == 0)
        return FAULT_BACKUP_COMPLETE_INTERRUPT;
    if (strcmp(token, "copy_interrupt") == 0)
        return FAULT_COPY_INTERRUPT;
    if (strcmp(token, "copy_complete_interrupt") == 0)
        return FAULT_COPY_COMPLETE_INTERRUPT;
    if (strcmp(token, "copy_error") == 0)
        return FAULT_COPY_ERROR;
    if (strcmp(token, "rollback_interrupt") == 0)
        return FAULT_ROLLBACK_INTERRUPT;
    if (strcmp(token, "commit_interrupt") == 0)
        return FAULT_COMMIT_INTERRUPT;
    return FAULT_NONE;
}

static void load_fault_test_mode(void)
{
    char token[64] = {0};
    if (!read_state_from(FAULT_TEST_FILE, token, sizeof(token)))
        return;

    FaultTestMode mode = parse_fault_test_mode(token);
    if (mode == FAULT_NONE) {
        EPRINTFARGS("TEST MODE: Unknown fault token '%s'. Ignoring it.\n", token);
        return;
    }

    // A test must be one-shot. If the trigger cannot be consumed safely, do
    // not arm it, otherwise a recovery reboot could loop forever.
    remove_path_silent(FAULT_TEST_USED);
    FRESULT res = f_rename(FAULT_TEST_FILE, FAULT_TEST_USED);
    if (res != FR_OK) {
        EPRINTFARGS("TEST MODE: Could not consume trigger (%d). Test disabled.\n", res);
        return;
    }

    g_fault_test = mode;
    WPRINTFARGS("TEST MODE armed: %s\n", token);
}

static void fault_relaunch_recovery(FaultTestMode mode, const char *label)
{
    if (g_fault_test != mode || g_fault_fired)
        return;

    g_fault_fired = true;
    WPRINTFARGS("\n[TEST] Simulated interruption: %s\n", label);
    WPRINTF("[TEST] Re-launching recovery payload...\n");
    usleep(600000);

    if (path_exists(UPDATE_ROOT "/recovery.bin"))
        launch_payload(UPDATE_ROOT "/recovery.bin");

    // Never continue destructive work if the test payload failed to relaunch.
    EPRINTF("[TEST] Could not re-launch recovery.bin. Halting safely.\n");
    EPRINTF("Power off, then launch /aio-mod-update/recovery.bin manually.\n");
    while (true)
        bpmp_halt();
}
#endif

static bool file_sizes_match(const char *a, const char *b)
{
    FILINFO fa;
    FILINFO fb;
    if (f_stat(a, &fa) != FR_OK || f_stat(b, &fb) != FR_OK)
        return false;
    if ((fa.fattrib & AM_DIR) || (fb.fattrib & AM_DIR))
        return false;
    return fa.fsize == fb.fsize;
}

static FRESULT move_managed_to_backup(bool patched_or_mariko)
{
    // Phase 2 rule: NEVER delete an existing backup on entry. If a path is
    // already in backup and gone from root, it was completed before a reset.
    FRESULT res = mkdir_if_needed(UPDATE_BACKUP);
    if (res != FR_OK)
        return res;

    const u32 count = sizeof(managed_paths) / sizeof(managed_paths[0]);
    for (u32 i = 0; i < count; i++) {
        const char *name = managed_paths[i];

        char src[256];
        char dst[256];

        // On patched/Mariko /payload.bin is the currently-running updater. Do
        // not move it during the long transaction. AIO MOD preserved the
        // previous boot payload as /payload.bin.aio before launching us; move
        // THAT snapshot into backup/payload.bin instead. This keeps automatic
        // recovery alive while making the backup hardware-independent.
        if (patched_or_mariko && strcmp(name, "payload.bin") == 0)
            s_printf(src, "%s", MARIKO_PAYLOAD_FALLBACK);
        else
            s_printf(src, "sd:/%s", name);
        s_printf(dst, "%s/%s", UPDATE_BACKUP, name);

        char absent_marker[256];
        absent_marker_path(i, absent_marker);

        const bool src_exists = path_exists(src);
        const bool dst_exists = path_exists(dst);
        const bool was_absent = path_exists(absent_marker);

        if (dst_exists) {
            if (!src_exists) {
                // Already backed up before an interruption.
                continue;
            }

            // Never overwrite a recovery copy. Both existing during the backup
            // phase is treated as an unsafe/ambiguous condition.
            return FR_EXIST;
        }

        if (was_absent) {
            // Original path was confirmed absent before an interruption.
            continue;
        }

        if (!src_exists) {
            res = create_absent_marker(i);
            if (res != FR_OK)
                return res;
            // Missing original paths are tracked internally; keep the public UI clean.
#if AIO_CFW_ENABLE_FAULT_TESTS
            fault_relaunch_recovery(FAULT_BACKUP_INTERRUPT, "during backup");
#endif
            continue;
        }

        if (is_hidden_legacy_path(name))
            ui_draw_progress("Backing up current CFW", 12 + (int)((i + 1) * 18 / count), "Saving current configuration...");
        else {
            char ui_detail[96];
            s_printf(ui_detail, "Saving %s", name);
            ui_draw_progress("Backing up current CFW", 12 + (int)((i + 1) * 18 / count), ui_detail);
        }

        res = f_rename(src, dst);
        if (res != FR_OK)
            return res;
#if AIO_CFW_ENABLE_FAULT_TESTS
        fault_relaunch_recovery(FAULT_BACKUP_INTERRUPT, "during backup");
#endif
    }

    return FR_OK;
}

static FRESULT restore_backup(bool patched_or_mariko)
{
    const u32 count = sizeof(managed_paths) / sizeof(managed_paths[0]);
    FRESULT result = FR_OK;

    ui_draw_progress("Restoring previous CFW", 35, "Reverting changes safely...");

    for (u32 i = 0; i < count; i++) {
        const char *name = managed_paths[i];
        char src[256];
        char dst[256];
        s_printf(src, "%s/%s", UPDATE_BACKUP, name);
        s_printf(dst, "sd:/%s", name);

        if (!path_exists(src)) {
            // A previous rollback attempt may already have moved it back.
            continue;
        }

        FRESULT res = remove_path_silent(dst);
        if (res == FR_OK)
            res = f_rename(src, dst);

        if (res != FR_OK) {
            result = res;
            if (!is_hidden_legacy_path(name))
                EPRINTFARGS("Could not restore %s\n", name);
        }
    }

    if (patched_or_mariko && path_exists(MARIKO_PAYLOAD_FALLBACK)) {
        FRESULT res = remove_path_silent("sd:/payload.bin");
        if (res == FR_OK)
            res = f_rename(MARIKO_PAYLOAD_FALLBACK, "sd:/payload.bin");
        if (res != FR_OK) {
            result = res;
            EPRINTF("Could not restore the previous boot payload.\n");
        }
    }

    return result;
}

static FRESULT rollback_transaction(bool patched_or_mariko)
{
    FRESULT res = write_state(STATE_ROLLBACK_IN_PROGRESS);
    if (res != FR_OK)
        return res;

    ui_draw_progress("Restoring previous CFW", 30, "An update error occurred. Restoring the backup...");
    bool previous_mute = gfx_con.mute;
    gfx_con.mute = true;

    // Remove partially-installed boot-critical paths only when their original
    // copies still exist in backup. Paths already restored on a prior attempt
    // are deliberately left untouched, making rollback resumable.
    const u32 count = sizeof(managed_paths) / sizeof(managed_paths[0]);
    for (u32 i = 0; i < count; i++) {
        const char *name = managed_paths[i];
        char backup_path[256];
        char root_path[256];
        char absent_marker[256];
        s_printf(backup_path, "%s/%s", UPDATE_BACKUP, name);
        s_printf(root_path, "sd:/%s", name);
        absent_marker_path(i, absent_marker);

        // If the original existed, remove the partial new path before restore.
        // If it was originally absent, remove any path created by the new pack.
        const bool has_backup = path_exists(backup_path);
        const bool was_absent = path_exists(absent_marker);
        if (has_backup || was_absent) {
            res = remove_path_silent(root_path);
            if (res != FR_OK)
                break;
#if AIO_CFW_ENABLE_FAULT_TESTS
            fault_relaunch_recovery(FAULT_ROLLBACK_INTERRUPT, "during rollback");
#endif

            // Once an originally-absent path has been removed during rollback,
            // its sentinel has served its purpose. Removing it makes a
            // completed-but-not-yet-cleaned rollback detectable as an empty
            // backup directory on the next Horizon boot.
            if (!has_backup && was_absent) {
                res = f_unlink(absent_marker);
                if (res != FR_OK && res != FR_NO_FILE)
                    break;
            }
        }
    }

    if (res == FR_OK)
        res = restore_backup(patched_or_mariko);

    if (res == FR_OK && patched_or_mariko) {
        remove_path_silent(UPDATE_PAYLOAD_NEXT);
        remove_path_silent(UPDATE_UPDATER_SAVED);
        remove_path_silent(MARIKO_PAYLOAD_FALLBACK);
    }

    if (res != FR_OK) {
        write_state(STATE_ROLLBACK_FAILED);
        gfx_con.mute = previous_mute;
        return res;
    }

    // Only destroy the transaction after every available backup item has been
    // restored. This cleanup is silent so hidden compatibility paths never leak.
    res = remove_path_silent(UPDATE_ROOT);
    gfx_con.mute = previous_mute;
    return res;
}

static FRESULT copy_staged_entry(FSEntry_t *entry)
{
    char src[512];
    char dst[512];
    s_printf(src, "%s/%s", UPDATE_NEW, entry->name);
    s_printf(dst, "sd:/%s", entry->name);

    // RC4: filesystem operations are unchanged; only the old scrolling copy
    // console is suppressed. FileCopy/FolderCopy report the active file through
    // a presentation callback while they perform the exact same copy routine.
    if (entry->isDir) {
        ErrCode_t err = FolderCopy(src, "sd:/");
        return (FRESULT)err.err;
    }

    ErrCode_t err = FileCopy(src, dst, COPY_MODE_PRINT);
    return (FRESULT)err.err;
}

static FRESULT copy_staged_root(bool patched_or_mariko)
{
    int read_res = 0;
    Vector_t entries = ReadFolder(UPDATE_NEW, &read_res);
    if (read_res) {
        clearFileVector(&entries);
        return (FRESULT)read_res;
    }

    vecDefArray(FSEntry_t *, fs, entries);
    FRESULT result = FR_OK;

    for (u32 i = 0; i < entries.count && result == FR_OK; i++) {
        // Never install the incompatible legacy directory from a staged pack.
        if (is_hidden_legacy_path(fs[i].name))
            continue;

        // On patched/Mariko, keep the updater as root payload during the long
        // copy. The staged payload is installed in a short finalization step.
        if (patched_or_mariko && strcmp(fs[i].name, "payload.bin") == 0)
            continue;

        char ui_detail[128];
        s_printf(ui_detail, "Copying %s%s", fs[i].name, fs[i].isDir ? "/" : "");
        int ui_pct = 34 + (int)(((i + 1) * 38) / (entries.count ? entries.count : 1));
        ui_draw_progress("Installing new CFW", ui_pct, ui_detail);

        // Keep TegraExplorer's proven copy engine, but route its presentation to
        // our single Current file row instead of printing/scanning the console.
        FileCopySetQuiet(true);
        FileCopySetProgressCallback(ui_copy_progress_callback);
        result = copy_staged_entry(&fs[i]);
        FileCopySetProgressCallback(NULL);
        FileCopySetQuiet(false);
        if (result == FR_OK) {
#if AIO_CFW_ENABLE_FAULT_TESTS
            fault_relaunch_recovery(FAULT_COPY_INTERRUPT, "during CFW copy");
            if (g_fault_test == FAULT_COPY_ERROR || g_fault_test == FAULT_ROLLBACK_INTERRUPT) {
                WPRINTF("\n[TEST] Injecting a controlled copy error.\n");
                result = FR_DISK_ERR;
            }
#endif
        }
    }

    clearFileVector(&entries);
    return result;
}

static bool staged_package_valid(bool patched_or_mariko)
{
    if (!path_exists(UPDATE_NEW "/atmosphere/package3"))
        return false;

    if (patched_or_mariko)
        return path_exists(UPDATE_NEW "/payload.bin");

    return path_exists(UPDATE_NEW "/bootloader/update.bin") ||
           path_exists(UPDATE_NEW "/atmosphere/reboot_payload.bin") ||
           path_exists(UPDATE_NEW "/payload.bin");
}

static bool installed_core_valid(bool patched_or_mariko)
{
    if (!file_sizes_match(UPDATE_NEW "/atmosphere/package3", "sd:/atmosphere/package3"))
        return false;

    if (patched_or_mariko)
        return true; // root payload is finalized separately below.

    if (path_exists(UPDATE_NEW "/bootloader/update.bin"))
        return file_sizes_match(UPDATE_NEW "/bootloader/update.bin", "sd:/bootloader/update.bin");

    if (path_exists(UPDATE_NEW "/atmosphere/reboot_payload.bin"))
        return file_sizes_match(UPDATE_NEW "/atmosphere/reboot_payload.bin", "sd:/atmosphere/reboot_payload.bin");

    if (path_exists(UPDATE_NEW "/payload.bin"))
        return file_sizes_match(UPDATE_NEW "/payload.bin", "sd:/payload.bin");

    return false;
}

static FRESULT finalize_patched_payload(void)
{
    const char *staged = UPDATE_NEW "/payload.bin";

    if (!path_exists(staged))
        return FR_NO_FILE;

    // If the root already matches the staged payload, a reset happened after
    // the swap and before the journal advanced. Treat it as completed.
    if (file_sizes_match(staged, "sd:/payload.bin")) {
        remove_path_silent(UPDATE_PAYLOAD_NEXT);
        return FR_OK;
    }

    // First make a complete new copy away from the live root payload.
    FRESULT res = remove_path_silent(UPDATE_PAYLOAD_NEXT);
    if (res != FR_OK)
        return res;

    WPRINTF("Preparing boot payload... ");
    ErrCode_t copy_res = FileCopy(staged, UPDATE_PAYLOAD_NEXT, COPY_MODE_PRINT);
    WPRINTF("\n");
    if (copy_res.err)
        return (FRESULT)copy_res.err;

    if (!file_sizes_match(staged, UPDATE_PAYLOAD_NEXT))
        return FR_DISK_ERR;

    // Preserve the updater binary by rename. This operation is intentionally
    // delayed until every other CFW file has copied and verified.
    if (path_exists("sd:/payload.bin") && !path_exists(UPDATE_UPDATER_SAVED)) {
        res = f_rename("sd:/payload.bin", UPDATE_UPDATER_SAVED);
        if (res != FR_OK)
            return res;
    }

    // Resume cleanly if power was lost after the previous rename.
    if (!path_exists("sd:/payload.bin")) {
        res = f_rename(UPDATE_PAYLOAD_NEXT, "sd:/payload.bin");
        if (res != FR_OK)
            return res;
    }

    if (!file_sizes_match(staged, "sd:/payload.bin"))
        return FR_DISK_ERR;

    remove_path_silent(UPDATE_PAYLOAD_NEXT);
    return FR_OK;
}

// Finalize the transaction entirely inside the payload.  Once the journal is
// advanced to COMMIT_IN_PROGRESS the freshly-installed CFW has already passed
// all on-SD verification, so rollback is intentionally no longer offered.
// Cleanup is idempotent and state is deleted last, allowing a manually
// re-launched recovery payload to resume an interrupted commit.
static FRESULT commit_transaction(void)
{
    FRESULT res = write_state(STATE_COMMIT_IN_PROGRESS);
    if (res != FR_OK)
        return res;

    WPRINTF("\nFinalizing update...\n");

    // Recovery data first. If power is lost here, COMMIT_IN_PROGRESS remains
    // and a re-launched updater can safely continue deleting what is left.
    res = remove_path_silent(UPDATE_BACKUP);
    if (res != FR_OK)
        return res;

#if AIO_CFW_ENABLE_FAULT_TESTS
    fault_relaunch_recovery(FAULT_COMMIT_INTERRUPT, "during final cleanup");
#endif

    res = remove_path_silent(UPDATE_NEW);
    if (res != FR_OK)
        return res;

    res = remove_path_silent(UPDATE_READY);
    if (res != FR_OK)
        return res;

    res = remove_path_silent(UPDATE_PAYLOAD_NEXT);
    if (res != FR_OK)
        return res;

    res = remove_path_silent(UPDATE_UPDATER_SAVED);
    if (res != FR_OK)
        return res;

    // AIO MOD may have used this temporary root snapshot to enter the updater
    // on patched/Mariko hardware. By COMMIT its previous contents have already
    // been represented in backup/payload.bin and are intentionally discarded.
    res = remove_path_silent(MARIKO_PAYLOAD_FALLBACK);
    if (res != FR_OK)
        return res;

    // Keep recovery.bin until every large directory has been removed. The
    // running payload is already resident in memory, so deleting its SD copy is
    // safe at this stage.
    res = remove_path_silent(UPDATE_ROOT "/recovery.bin");
    if (res != FR_OK)
        return res;

    res = remove_path_silent(UPDATE_STATE_TMP);
    if (res != FR_OK)
        return res;

    // Journal last. After this succeeds the transaction is committed even if
    // power is lost before the now-empty root directory is removed.
    res = remove_path_silent(UPDATE_STATE);
    if (res != FR_OK)
        return res;

    // Best effort: an empty /aio-mod-update directory is harmless and Horizon
    // will treat it as no pending transaction.
    FRESULT root_res = f_unlink(UPDATE_ROOT);
    if (root_res != FR_OK && root_res != FR_NO_FILE && root_res != FR_NO_PATH)
        return root_res;

    return FR_OK;
}

static void boot_installed_cfw_now(bool patched_or_mariko)
{
    if (patched_or_mariko) {
        power_set_state(POWER_OFF_REBOOT);
        return;
    }

    if (path_exists("sd:/bootloader/update.bin"))
        launch_payload("sd:/bootloader/update.bin");

    if (path_exists("sd:/atmosphere/reboot_payload.bin"))
        launch_payload("sd:/atmosphere/reboot_payload.bin");

    if (path_exists("sd:/payload.bin"))
        launch_payload("sd:/payload.bin");

    ui_fatal_wait_poweroff("BOOT", 1,
        "No valid payload was found to start the CFW.\nThe installed files were preserved for inspection.");
}

static void boot_installed_cfw(bool patched_or_mariko)
{
    if (g_commit_warning_code)
        ui_warning_wait("COMMIT", g_commit_warning_code);
    else
        ui_success_wait();

    boot_installed_cfw_now(patched_or_mariko);
}

static void boot_previous_after_rollback(bool patched_or_mariko)
{
    ui_rollback_success_wait();

    if (patched_or_mariko) {
        power_set_state(POWER_OFF_REBOOT);
        return;
    }

    boot_installed_cfw_now(false);
}

static void fail_before_changes(bool patched_or_mariko)
{
    // On patched/Mariko, AIO MOD temporarily replaced /payload.bin with this
    // updater before booting it. Restore the known-good boot payload if the
    // transaction never reached the backup phase.
    if (patched_or_mariko && path_exists(MARIKO_PAYLOAD_FALLBACK)) {
        remove_path_silent("sd:/payload.bin");
        f_rename(MARIKO_PAYLOAD_FALLBACK, "sd:/payload.bin");
    }

    remove_path_silent(UPDATE_ROOT);
    ui_unchanged_wait();

    if (patched_or_mariko) {
        power_set_state(POWER_OFF_REBOOT);
        return;
    }

    boot_installed_cfw_now(false);
}

bool aio_cfw_update_pending(void)
{
    char state[64];
    return path_exists(UPDATE_READY) ||
           read_state(state, sizeof(state)) ||
           path_exists(UPDATE_BACKUP);
}

void aio_cfw_update_run(bool patched_or_mariko)
{
    ui_draw_progress("Preparing CFW update", 5, "Reading update journal...");

#if AIO_CFW_ENABLE_FAULT_TESTS
    load_fault_test_mode();
#endif

    char state[64] = {0};
    bool have_state = read_state(state, sizeof(state));

    // Backward compatibility with Phase 1/1.1 staging markers.
    if (!have_state && path_exists(UPDATE_READY)) {
        strcpy(state, STATE_STAGED);
        if (write_state(state) != FR_OK) {
            ui_set_error("JOURNAL", FR_DISK_ERR);
            fail_before_changes(patched_or_mariko);
            return;
        }
        have_state = true;
    }

    // A backup without a journal is never destroyed. Prefer recovery over
    // guessing which destructive step was interrupted.
    if (!have_state && path_exists(UPDATE_BACKUP)) {
        ui_set_error("JOURNAL", FR_INVALID_OBJECT);
        FRESULT rb = rollback_transaction(patched_or_mariko);
        if (rb == FR_OK)
            boot_previous_after_rollback(patched_or_mariko);
        else
            ui_fatal_wait_poweroff("ROLLBACK", (int)rb, "Recovery could not be completed. The backup was preserved.");
        return;
    }

    if (!have_state) {
        ui_fatal_wait_poweroff("STATE", 1, "No valid pending CFW update was found.");
        return;
    }

    // COMMIT_IN_PROGRESS means installation verification already succeeded and
    // only final cleanup was interrupted. Rollback is no longer appropriate;
    // resume the idempotent cleanup and then boot the installed CFW.
    if (state_is(state, STATE_COMMIT_IN_PROGRESS)) {
        WPRINTF("Resuming final update cleanup...\n");
        ui_draw_progress("Finalizing update", 96, "Resuming final cleanup...");
        bool previous_mute = gfx_con.mute;
        gfx_con.mute = true;
        FRESULT commit_res = commit_transaction();
        gfx_con.mute = previous_mute;
        if (commit_res != FR_OK)
            g_commit_warning_code = (int)commit_res;
        boot_installed_cfw(patched_or_mariko);
        return;
    }

    // Compatibility with Phase 2.0-2.5.x. Those builds waited for Horizon to
    // confirm boot. In Phase 2.6 a manually launched new updater can finalize
    // such a verified transaction directly instead of asking for confirmation.
    if (state_is(state, STATE_PENDING_BOOT)) {
        WPRINTF("Legacy verified transaction found. Finalizing...\n");
        ui_draw_progress("Finalizing update", 96, "Resuming final cleanup...");
        bool previous_mute = gfx_con.mute;
        gfx_con.mute = true;
        FRESULT commit_res = commit_transaction();
        gfx_con.mute = previous_mute;
        if (commit_res != FR_OK)
            g_commit_warning_code = (int)commit_res;
        boot_installed_cfw(patched_or_mariko);
        return;
    }

    if (state_is(state, STATE_ROLLBACK_IN_PROGRESS) || state_is(state, STATE_ROLLBACK_FAILED)) {
        ui_set_error("ROLLBACK-RESUME", 0);
        FRESULT res = rollback_transaction(patched_or_mariko);
        if (res == FR_OK) {
            WPRINTF("Previous CFW restored successfully.\n");
            boot_previous_after_rollback(patched_or_mariko);
        }
        else {
            ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored. The backup was NOT deleted.");
        }
        return;
    }

    ui_draw_progress("Validating update", 8, "Checking the prepared CFW package...");
    if (!staged_package_valid(patched_or_mariko)) {
        EPRINTF("ERROR: The CFW package is incomplete.\n");
        if (state_is(state, STATE_STAGED)) {
            ui_set_error("PACKAGE", FR_NO_FILE);
            fail_before_changes(patched_or_mariko);
        }
        else {
            ui_set_error("PACKAGE", FR_NO_FILE);
            FRESULT res = rollback_transaction(patched_or_mariko);
            if (res == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored automatically. The backup was preserved.");
        }
        return;
    }
    ui_draw_progress("Validating update", 10, "CFW package verified.");

    if (state_is(state, STATE_STAGED)) {
        if (write_state(STATE_BACKUP_IN_PROGRESS) != FR_OK) {
            ui_set_error("JOURNAL-BACKUP", FR_DISK_ERR);
            fail_before_changes(patched_or_mariko);
            return;
        }
        strcpy(state, STATE_BACKUP_IN_PROGRESS);
    }

    if (state_is(state, STATE_BACKUP_IN_PROGRESS)) {
        ui_draw_progress("Backing up current CFW", 12, "Creating a recovery snapshot...");
        FRESULT res = move_managed_to_backup(patched_or_mariko);
#if AIO_CFW_ENABLE_FAULT_TESTS
        if (res == FR_OK)
            fault_relaunch_recovery(FAULT_BACKUP_COMPLETE_INTERRUPT, "after backup, before journal advance");
#endif
        if (res != FR_OK) {
            ui_set_error("BACKUP", (int)res);
            res = rollback_transaction(patched_or_mariko);
            if (res == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }

        if (write_state(STATE_BACKUP_COMPLETE) != FR_OK) {
            ui_set_error("JOURNAL-BACKUP", FR_DISK_ERR);
            FRESULT rb = rollback_transaction(patched_or_mariko);
            if (rb == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)rb, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }
        strcpy(state, STATE_BACKUP_COMPLETE);
    }

    if (state_is(state, STATE_BACKUP_COMPLETE)) {
        if (write_state(STATE_COPY_IN_PROGRESS) != FR_OK) {
            ui_set_error("JOURNAL-COPY", FR_DISK_ERR);
            FRESULT rb = rollback_transaction(patched_or_mariko);
            if (rb == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)rb, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }
        strcpy(state, STATE_COPY_IN_PROGRESS);
    }

    if (state_is(state, STATE_COPY_IN_PROGRESS)) {
        ui_draw_progress("Installing new CFW", 32, "Copying the prepared package...");
        FRESULT res = copy_staged_root(patched_or_mariko);
        if (res != FR_OK) {
            ui_set_error("COPY", (int)res);
            res = rollback_transaction(patched_or_mariko);
            if (res == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }

        // Enforce silent removal even if an unexpected copy recreated it.
        remove_path_silent("sd:/Next");

        if (!installed_core_valid(patched_or_mariko)) {
            ui_set_error("VERIFY", FR_DISK_ERR);
            res = rollback_transaction(patched_or_mariko);
            if (res == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }

        if (write_state(STATE_COPY_COMPLETE) != FR_OK) {
            ui_set_error("JOURNAL-COPY", FR_DISK_ERR);
            FRESULT rb = rollback_transaction(patched_or_mariko);
            if (rb == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)rb, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }
        strcpy(state, STATE_COPY_COMPLETE);
#if AIO_CFW_ENABLE_FAULT_TESTS
        fault_relaunch_recovery(FAULT_COPY_COMPLETE_INTERRUPT, "after copy verification");
#endif
    }

    if (patched_or_mariko && state_is(state, STATE_COPY_COMPLETE)) {
        if (write_state(STATE_FINALIZE_PAYLOAD) != FR_OK) {
            ui_set_error("JOURNAL-PAYLOAD", FR_DISK_ERR);
            FRESULT rb = rollback_transaction(patched_or_mariko);
            if (rb == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)rb, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }
        strcpy(state, STATE_FINALIZE_PAYLOAD);
    }

    if (patched_or_mariko && state_is(state, STATE_FINALIZE_PAYLOAD)) {
        ui_draw_progress("Preparing boot payload", 78, "Finalizing the console boot payload...");
        bool previous_mute = gfx_con.mute;
        gfx_con.mute = true;
        FRESULT res = finalize_patched_payload();
        gfx_con.mute = previous_mute;
        if (res != FR_OK) {
            ui_set_error("PAYLOAD", (int)res);
            res = rollback_transaction(patched_or_mariko);
            if (res == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }

        // Verify the final root payload before COMMIT. Once COMMIT starts the
        // old backup will be intentionally destroyed.
        if (!file_sizes_match(UPDATE_NEW "/payload.bin", "sd:/payload.bin")) {
            ui_set_error("PAYLOAD-VERIFY", FR_DISK_ERR);
            res = rollback_transaction(patched_or_mariko);
            if (res == FR_OK)
                boot_previous_after_rollback(patched_or_mariko);
            else
                ui_fatal_wait_poweroff("ROLLBACK", (int)res, "The previous CFW could not be restored automatically. The backup was preserved.");
            return;
        }
    }

    ui_draw_progress("Verifying installation", 88, "Checking the installed CFW...");
    ui_draw_progress("Finalizing update", 96, "Removing backup and temporary update files...");

    bool previous_mute = gfx_con.mute;
    gfx_con.mute = true;
    FRESULT commit_res = commit_transaction();
    gfx_con.mute = previous_mute;
    if (commit_res != FR_OK) {
        // COMMIT_IN_PROGRESS may already be durable and the backup may already
        // be partially removed, so never attempt rollback from this point.
        g_commit_warning_code = (int)commit_res;
    }

    boot_installed_cfw(patched_or_mariko);
}
