#include "aio_app_updater.h"

#include <libs/fatfs/ff.h>
#include <power/max77620.h>
#include <utils/btn.h>
#include <utils/sprintf.h>
#include <utils/util.h>
#include <string.h>

#include "err.h"
#include "fs/fscopy.h"
#include "gfx/gfx.h"
#include "gfx/gfxutils.h"

extern int launch_payload(char *path);

#define APP_UPDATE_ROOT       "sd:/aio-mod-app-update"
#define APP_UPDATE_READY      APP_UPDATE_ROOT "/ready"
#define APP_UPDATE_STATE      APP_UPDATE_ROOT "/state"
#define APP_UPDATE_STATE_TMP  APP_UPDATE_ROOT "/state.tmp"
#define APP_UPDATE_NEW_DIR    APP_UPDATE_ROOT "/new"
#define APP_UPDATE_BACKUP_DIR APP_UPDATE_ROOT "/backup"

#define APP_NEW_NRO           APP_UPDATE_NEW_DIR "/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro"
#define APP_BACKUP_NRO        APP_UPDATE_BACKUP_DIR "/aio-switch-updater-mod.nro"
#define APP_TARGET_NRO        "sd:/switch/aio-switch-updater-mod/aio-switch-updater-mod.nro"

#define MARIKO_SAVED_PAYLOAD  "sd:/payload.bin.aio"

#define STATE_STAGED               "STAGED"
#define STATE_BACKUP_IN_PROGRESS   "BACKUP_IN_PROGRESS"
#define STATE_BACKUP_COMPLETE      "BACKUP_COMPLETE"
#define STATE_COPY_IN_PROGRESS     "COPY_IN_PROGRESS"
#define STATE_COPY_COMPLETE        "COPY_COMPLETE"
#define STATE_ROLLBACK_IN_PROGRESS "ROLLBACK_IN_PROGRESS"
#define STATE_ROLLBACK_COMPLETE    "ROLLBACK_COMPLETE"
#define STATE_COMMIT_IN_PROGRESS   "COMMIT_IN_PROGRESS"

// -------------------------------------------------------------------------
// AIO MOD payload UI. This intentionally mirrors the CFW updater UI.
// Transactional filesystem behavior remains separate below.
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

static const char *g_copy_title = "Copying file";
static const char *g_copy_detail = "Please wait...";
static int g_copy_base_percent = 0;
static int g_copy_span_percent = 0;

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
    gfx_printf("APP UPDATER");
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
    if (!out || out_size < 8)
        return;

    out[0] = 0;

    if (!source) {
        strcpy(out, "aio-switch-updater-mod.nro");
        return;
    }

    const char *name = strrchr(source, '/');
    name = name ? name + 1 : source;

    if (!name[0])
        name = "aio-switch-updater-mod.nro";

    strncpy(out, name, out_size - 1);
    out[out_size - 1] = 0;
}

static void ui_copy_progress_callback(const char *source, u32 file_percent)
{
    int overall = g_copy_base_percent + (int)((file_percent * g_copy_span_percent) / 100);
    ui_draw_progress(g_copy_title, overall, g_copy_detail);

    char display_path[96];
    ui_compact_copy_path(source, display_path, sizeof(display_path));

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

static void ui_begin_copy(const char *title, const char *detail, int base_percent, int span_percent)
{
    g_copy_title = title ? title : "Copying file";
    g_copy_detail = detail ? detail : "Please wait...";
    g_copy_base_percent = base_percent;
    g_copy_span_percent = span_percent;

    FileCopySetQuiet(true);
    FileCopySetProgressCallback(ui_copy_progress_callback);
    ui_draw_progress(g_copy_title, g_copy_base_percent, g_copy_detail);
}

static void ui_end_copy(void)
{
    FileCopySetProgressCallback(NULL);
    FileCopySetQuiet(false);
}

static void ui_wait_for_power(void)
{
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
                   "The new AIO MOD was installed and verified.",
                   "Temporary update files were removed.",
                   "Press POWER to continue.",
                   "",
                   "OK", 0);
    ui_wait_for_power();
}

static void ui_warning_wait(const char *stage, int code)
{
    ui_result_page(UI_WARNING,
                   "Update installed with a warning",
                   "The new AIO MOD is installed, but final cleanup was incomplete.",
                   "Temporary update files may remain on the SD card.",
                   "Press POWER to continue.",
                   "Please report the code shown above.",
                   stage, code);
    ui_wait_for_power();
}

static void ui_rollback_success_wait(void)
{
    ui_result_page(UI_WARNING,
                   "Update cancelled safely",
                   "An error occurred during the update.",
                   "The previous AIO MOD was restored successfully.",
                   "Press POWER to continue.",
                   "",
                   g_last_error_stage, g_last_error_code);
    ui_wait_for_power();
}

static void ui_unchanged_wait(void)
{
    ui_result_page(UI_ERROR,
                   "Update could not be started",
                   "The installed AIO MOD was not modified.",
                   "Your previous installation is still available.",
                   "Press POWER to continue.",
                   "",
                   g_last_error_stage, g_last_error_code);
    ui_wait_for_power();
}

static void ui_fatal_wait_poweroff(const char *stage, int code)
{
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

static bool path_exists(const char *path)
{
    FILINFO info;
    return f_stat(path, &info) == FR_OK;
}

static bool valid_nonempty_file(const char *path)
{
    FILINFO info;

    if (f_stat(path, &info) != FR_OK)
        return false;

    if (info.fattrib & AM_DIR)
        return false;

    return info.fsize > 0;
}

static bool file_sizes_match(const char *a, const char *b)
{
    FILINFO ia;
    FILINFO ib;

    if (f_stat(a, &ia) != FR_OK || f_stat(b, &ib) != FR_OK)
        return false;

    if ((ia.fattrib & AM_DIR) || (ib.fattrib & AM_DIR))
        return false;

    return ia.fsize > 0 && ia.fsize == ib.fsize;
}

static FRESULT mkdir_if_needed(const char *path)
{
    FRESULT res = f_mkdir(path);
    return (res == FR_OK || res == FR_EXIST) ? FR_OK : res;
}

static FRESULT remove_path_if_exists(const char *path)
{
    FILINFO info;
    FRESULT res = f_stat(path, &info);

    if (res == FR_NO_FILE || res == FR_NO_PATH)
        return FR_OK;

    if (res != FR_OK)
        return res;

    if (info.fattrib & AM_DIR) {
        ErrCode_t err = FolderDelete(path);
        return (FRESULT)err.err;
    }

    res = f_unlink(path);
    return (res == FR_NO_FILE) ? FR_OK : res;
}

static bool read_state_from(const char *path, char *out, u32 out_size)
{
    if (!out || out_size < 2)
        return false;

    FIL fp;
    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    UINT got = 0;
    FRESULT res = f_read(&fp, out, out_size - 1, &got);
    f_close(&fp);

    if (res != FR_OK || got == 0)
        return false;

    out[got] = 0;
    for (u32 i = 0; i < got; i++) {
        if (out[i] == '\r' || out[i] == '\n') {
            out[i] = 0;
            break;
        }
    }

    return out[0] != 0;
}

static bool read_state(char *out, u32 out_size)
{
    if (read_state_from(APP_UPDATE_STATE, out, out_size))
        return true;

    return read_state_from(APP_UPDATE_STATE_TMP, out, out_size);
}

static FRESULT write_state(const char *state)
{
    if (!state)
        return FR_INVALID_PARAMETER;

    FRESULT res = mkdir_if_needed(APP_UPDATE_ROOT);
    if (res != FR_OK)
        return res;

    remove_path_if_exists(APP_UPDATE_STATE_TMP);

    FIL fp;
    res = f_open(&fp, APP_UPDATE_STATE_TMP, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
        return res;

    UINT len = (UINT)strlen(state);
    UINT written = 0;
    res = f_write(&fp, state, len, &written);

    if (res == FR_OK && written != len)
        res = FR_DISK_ERR;

    if (res == FR_OK) {
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

    res = remove_path_if_exists(APP_UPDATE_STATE);
    if (res != FR_OK)
        return res;

    return f_rename(APP_UPDATE_STATE_TMP, APP_UPDATE_STATE);
}

static bool state_is(const char *state, const char *expected)
{
    return state && expected && strcmp(state, expected) == 0;
}

static FRESULT copy_verified(const char *src, const char *dst)
{
    if (!valid_nonempty_file(src))
        return FR_NO_FILE;

    ErrCode_t err = FileCopy(src, dst, COPY_MODE_PRINT);
    FRESULT res = (FRESULT)err.err;

    if (res != FR_OK)
        return res;

    return file_sizes_match(src, dst) ? FR_OK : FR_DISK_ERR;
}

static FRESULT restore_mariko_payload(void)
{
    if (!valid_nonempty_file(MARIKO_SAVED_PAYLOAD))
        return FR_NO_FILE;

    FRESULT res = remove_path_if_exists("sd:/payload.bin");
    if (res != FR_OK)
        return res;

    res = f_rename(MARIKO_SAVED_PAYLOAD, "sd:/payload.bin");
    if (res != FR_OK)
        return res;

    return valid_nonempty_file("sd:/payload.bin") ? FR_OK : FR_DISK_ERR;
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

    ui_fatal_wait_poweroff("BOOT", 1);
}

static void boot_installed_cfw(bool patched_or_mariko)
{
    if (g_commit_warning_code)
        ui_warning_wait("COMMIT", g_commit_warning_code);
    else
        ui_success_wait();

    boot_installed_cfw_now(patched_or_mariko);
}

static bool restore_boot_payload_if_needed(bool patched_or_mariko)
{
    if (!patched_or_mariko)
        return true;

    FRESULT res = restore_mariko_payload();
    if (res != FR_OK) {
        ui_set_error("BOOT-PAYLOAD", (int)res);
        return false;
    }

    return true;
}

static FRESULT cleanup_transaction(void)
{
    FRESULT result = FR_OK;
    FRESULT res;

    res = remove_path_if_exists(APP_UPDATE_NEW_DIR);
    if (res != FR_OK && result == FR_OK)
        result = res;

    res = remove_path_if_exists(APP_UPDATE_BACKUP_DIR);
    if (res != FR_OK && result == FR_OK)
        result = res;

    res = remove_path_if_exists(APP_UPDATE_READY);
    if (res != FR_OK && result == FR_OK)
        result = res;

    res = remove_path_if_exists(APP_UPDATE_STATE_TMP);
    if (res != FR_OK && result == FR_OK)
        result = res;

    if (result == FR_OK) {
        res = remove_path_if_exists(APP_UPDATE_STATE);
        if (res != FR_OK)
            result = res;
    }

    if (result == FR_OK) {
        res = remove_path_if_exists(APP_UPDATE_ROOT);
        if (res != FR_OK)
            result = res;
    }

    return result;
}

static FRESULT prepare_backup(void)
{
    FRESULT res = mkdir_if_needed(APP_UPDATE_BACKUP_DIR);
    if (res != FR_OK)
        return res;

    remove_path_if_exists(APP_BACKUP_NRO);

    ui_begin_copy("Backing up current AIO MOD", "Creating a recovery copy...", 15, 20);
    res = copy_verified(APP_TARGET_NRO, APP_BACKUP_NRO);
    ui_end_copy();
    return res;
}

static bool rollback_transaction(bool patched_or_mariko)
{
    ui_draw_progress("Restoring previous AIO MOD", 35, "Reverting the application safely...");

    if (write_state(STATE_ROLLBACK_IN_PROGRESS) != FR_OK) {
        ui_set_error("ROLLBACK-JOURNAL", FR_DISK_ERR);
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return false;
    }

    if (!valid_nonempty_file(APP_BACKUP_NRO)) {
        ui_set_error("ROLLBACK-BACKUP", FR_NO_FILE);
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return false;
    }

    ui_begin_copy("Restoring previous AIO MOD", "Restoring the verified backup...", 40, 35);
    FRESULT res = copy_verified(APP_BACKUP_NRO, APP_TARGET_NRO);
    ui_end_copy();

    if (res != FR_OK) {
        ui_set_error("ROLLBACK-COPY", (int)res);
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return false;
    }

    if (write_state(STATE_ROLLBACK_COMPLETE) != FR_OK) {
        ui_set_error("ROLLBACK-STATE", FR_DISK_ERR);
        if (!restore_boot_payload_if_needed(patched_or_mariko)) {
            ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
            return false;
        }
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return false;
    }

    if (!restore_boot_payload_if_needed(patched_or_mariko)) {
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return false;
    }

    ui_draw_progress("Finalizing recovery", 92, "Cleaning temporary recovery files...");
    res = cleanup_transaction();
    if (res != FR_OK)
        g_commit_warning_code = (int)res;

    ui_rollback_success_wait();
    boot_installed_cfw_now(patched_or_mariko);
    return true;
}

static void abort_before_changes(bool patched_or_mariko)
{
    if (!restore_boot_payload_if_needed(patched_or_mariko)) {
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return;
    }

    ui_draw_progress("Cancelling update", 90, "Removing prepared update files...");
    FRESULT res = cleanup_transaction();
    if (res != FR_OK && g_last_error_code == 0)
        ui_set_error("CLEANUP", (int)res);

    ui_unchanged_wait();
    boot_installed_cfw_now(patched_or_mariko);
}

static void commit_transaction(bool patched_or_mariko)
{
    ui_draw_progress("Finalizing update", 94, "Saving final update state...");

    if (write_state(STATE_COMMIT_IN_PROGRESS) != FR_OK) {
        ui_set_error("COMMIT-JOURNAL", FR_DISK_ERR);
        rollback_transaction(patched_or_mariko);
        return;
    }

    if (!restore_boot_payload_if_needed(patched_or_mariko)) {
        ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
        return;
    }

    ui_draw_progress("Finalizing update", 97, "Cleaning temporary update files...");
    FRESULT res = cleanup_transaction();
    if (res != FR_OK)
        g_commit_warning_code = (int)res;

    ui_draw_progress("Finalizing update", 100, "AIO MOD update verified.");
    boot_installed_cfw(patched_or_mariko);
}

bool aio_app_update_pending(void)
{
    return path_exists(APP_UPDATE_READY) ||
           path_exists(APP_UPDATE_STATE) ||
           path_exists(APP_UPDATE_STATE_TMP) ||
           path_exists(APP_UPDATE_BACKUP_DIR);
}

void aio_app_update_run(bool patched_or_mariko)
{
    ui_draw_progress("Preparing AIO MOD update", 5, "Reading update journal...");

    char state[64] = {0};
    bool have_state = read_state(state, sizeof(state));

    if (!have_state && path_exists(APP_UPDATE_READY)) {
        strcpy(state, STATE_STAGED);
        if (write_state(state) != FR_OK) {
            ui_set_error("JOURNAL", FR_DISK_ERR);
            abort_before_changes(patched_or_mariko);
            return;
        }
        have_state = true;
    }

    if (!have_state && valid_nonempty_file(APP_BACKUP_NRO)) {
        ui_set_error("JOURNAL", FR_INVALID_OBJECT);
        rollback_transaction(patched_or_mariko);
        return;
    }

    if (!have_state) {
        ui_set_error("STATE", 1);
        abort_before_changes(patched_or_mariko);
        return;
    }

    if (state_is(state, STATE_COMMIT_IN_PROGRESS)) {
        ui_draw_progress("Finalizing update", 96, "Resuming final cleanup...");

        if (!restore_boot_payload_if_needed(patched_or_mariko)) {
            ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
            return;
        }

        FRESULT res = cleanup_transaction();
        if (res != FR_OK)
            g_commit_warning_code = (int)res;

        boot_installed_cfw(patched_or_mariko);
        return;
    }

    if (state_is(state, STATE_ROLLBACK_COMPLETE)) {
        ui_draw_progress("Finalizing recovery", 94, "Resuming recovery cleanup...");

        if (!restore_boot_payload_if_needed(patched_or_mariko)) {
            ui_fatal_wait_poweroff(g_last_error_stage, g_last_error_code);
            return;
        }

        FRESULT res = cleanup_transaction();
        if (res != FR_OK)
            ui_set_error("ROLLBACK-CLEANUP", (int)res);

        ui_rollback_success_wait();
        boot_installed_cfw_now(patched_or_mariko);
        return;
    }

    if (state_is(state, STATE_ROLLBACK_IN_PROGRESS)) {
        ui_set_error("ROLLBACK-RESUME", 0);
        rollback_transaction(patched_or_mariko);
        return;
    }

    if (state_is(state, STATE_COPY_IN_PROGRESS)) {
        ui_set_error("INSTALL-INTERRUPTED", 0);
        ui_draw_progress("Interrupted update detected", 30, "Restoring the previous AIO MOD...");
        rollback_transaction(patched_or_mariko);
        return;
    }

    if (state_is(state, STATE_COPY_COMPLETE)) {
        commit_transaction(patched_or_mariko);
        return;
    }

    ui_draw_progress("Validating update", 8, "Checking the prepared AIO MOD package...");

    if (!valid_nonempty_file(APP_NEW_NRO)) {
        ui_set_error("PACKAGE", FR_NO_FILE);

        if (state_is(state, STATE_STAGED))
            abort_before_changes(patched_or_mariko);
        else
            rollback_transaction(patched_or_mariko);
        return;
    }

    if (!valid_nonempty_file(APP_TARGET_NRO)) {
        ui_set_error("INSTALLED-NRO", FR_NO_FILE);

        if (valid_nonempty_file(APP_BACKUP_NRO))
            rollback_transaction(patched_or_mariko);
        else
            abort_before_changes(patched_or_mariko);
        return;
    }

    ui_draw_progress("Validating update", 10, "AIO MOD package verified.");

    if (state_is(state, STATE_STAGED)) {
        if (write_state(STATE_BACKUP_IN_PROGRESS) != FR_OK) {
            ui_set_error("JOURNAL-BACKUP", FR_DISK_ERR);
            abort_before_changes(patched_or_mariko);
            return;
        }
        strcpy(state, STATE_BACKUP_IN_PROGRESS);
    }

    if (state_is(state, STATE_BACKUP_IN_PROGRESS)) {
        FRESULT res = prepare_backup();
        if (res != FR_OK) {
            ui_set_error("BACKUP", (int)res);
            abort_before_changes(patched_or_mariko);
            return;
        }

        if (write_state(STATE_BACKUP_COMPLETE) != FR_OK) {
            ui_set_error("JOURNAL-BACKUP", FR_DISK_ERR);
            abort_before_changes(patched_or_mariko);
            return;
        }
        strcpy(state, STATE_BACKUP_COMPLETE);
    }

    if (state_is(state, STATE_BACKUP_COMPLETE)) {
        ui_draw_progress("Verifying backup", 38, "Checking the recovery copy...");

        if (!valid_nonempty_file(APP_BACKUP_NRO) ||
            !file_sizes_match(APP_BACKUP_NRO, APP_TARGET_NRO)) {
            ui_set_error("BACKUP-VERIFY", FR_DISK_ERR);
            rollback_transaction(patched_or_mariko);
            return;
        }

        if (write_state(STATE_COPY_IN_PROGRESS) != FR_OK) {
            ui_set_error("JOURNAL-INSTALL", FR_DISK_ERR);
            rollback_transaction(patched_or_mariko);
            return;
        }

        ui_begin_copy("Installing new AIO MOD", "Replacing the application outside Horizon...", 42, 43);
        FRESULT res = copy_verified(APP_NEW_NRO, APP_TARGET_NRO);
        ui_end_copy();

        if (res != FR_OK) {
            ui_set_error("INSTALL", (int)res);
            rollback_transaction(patched_or_mariko);
            return;
        }

        ui_draw_progress("Verifying installation", 90, "Checking the installed AIO MOD NRO...");

        if (!file_sizes_match(APP_NEW_NRO, APP_TARGET_NRO)) {
            ui_set_error("INSTALL-VERIFY", FR_DISK_ERR);
            rollback_transaction(patched_or_mariko);
            return;
        }

        if (write_state(STATE_COPY_COMPLETE) != FR_OK) {
            ui_set_error("JOURNAL-INSTALL", FR_DISK_ERR);
            rollback_transaction(patched_or_mariko);
            return;
        }

        commit_transaction(patched_or_mariko);
        return;
    }

    ui_set_error("UNKNOWN-STATE", 1);

    if (valid_nonempty_file(APP_BACKUP_NRO))
        rollback_transaction(patched_or_mariko);
    else
        abort_before_changes(patched_or_mariko);
}
