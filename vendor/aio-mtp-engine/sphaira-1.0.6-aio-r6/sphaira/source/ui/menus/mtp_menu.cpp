#include "ui/menus/mtp_menu.hpp"

#include "app.hpp"
#include "haze_helper.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "ui/activity_log.hpp"
#include "ui/nvg_util.hpp"
#include "usb/usbds.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <cstdio>

namespace sphaira::ui::menu::mtp {
namespace {

std::string tr(const char* key, const char* fallback) {
    return i18n::get(key, fallback);
}

} // namespace

Menu::Menu(u32 flags) : stream::Menu{tr("aio_mtp_title", "USB Installer"), flags} {
    RemoveAction(Button::X);
    SetAction(Button::B, Action{tr("aio_mtp_return", "Return to AIO MOD"), [this]() {
        SetPop();
    }});

    m_was_mtp_enabled = libhaze::IsInit();
    if (!m_was_mtp_enabled) {
        log_write("[AIO MTP] enabling isolated MTP service\n");
        libhaze::Init();
    }

    libhaze::InitInstallMode(
        [this](const char* path) { return OnInstallStart(path); },
        [this](const void* buffer, size_t size) { return OnInstallWrite(buffer, size); },
        [this]() { return OnInstallClose(); });

    activity_log::Add("[AIO MOD]", tr("aio_mtp_service_ready", "MTP installer ready"), activity_log::Tone::Success);
    activity_log::Add("[MOUNT]", tr("aio_mtp_sd_mount", "microSD card: file access"));
    activity_log::Add("[MOUNT]", tr("aio_mtp_install_mount", "SD Card Install: package installer"));
    activity_log::Add("[READY]", tr("aio_mtp_drop", "Drag a package to SD Card Install"), activity_log::Tone::Muted);
}

Menu::~Menu() {
    CancelInstallMode();
    if (!m_was_mtp_enabled) {
        libhaze::Exit();
    }
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    stream::Menu::Update(controller, touch);

    static TimeStamp poll_timestamp;
    if (poll_timestamp.GetSeconds() >= 1) {
        poll_timestamp.Update();
        usbDsGetState(&m_usb_state);
        usbDsGetSpeed(&m_usb_speed);

        const auto state = i18n::get(GetUsbDsStateStr(m_usb_state));
        const auto speed = i18n::get(GetUsbDsSpeedStr(m_usb_speed));
        SetSubHeading(tr("aio_mtp_usb", "USB") + ": " + state + "  |  " + speed);

        const bool connected = m_usb_state == UsbState_Configured;
        if (!m_usb_state_initialized || connected != m_last_pc_connected) {
            activity_log::Add("[USB]", connected
                ? tr("aio_mtp_connected", "PC connected")
                : tr("aio_mtp_waiting", "Waiting for the PC"),
                connected ? activity_log::Tone::Success : activity_log::Tone::Warning);
            m_usb_state_initialized = true;
            m_last_pc_connected = connected;
            if (!connected) {
                m_last_logged_speed = UsbDeviceSpeed_None;
            }
        }

        if (connected && m_usb_speed != m_last_logged_speed) {
            activity_log::Add("[USB]", tr("aio_mtp_usb_speed", "USB speed") + ": " + speed,
                activity_log::Tone::Muted);
            if (m_usb_speed == UsbDeviceSpeed_Low || m_usb_speed == UsbDeviceSpeed_Full) {
                activity_log::Add("[WARNING]",
                    tr("aio_mtp_usb_slow", "Slow USB connection; try another cable or port"),
                    activity_log::Tone::Warning);
            }
            m_last_logged_speed = m_usb_speed;
        }
    }

    if (controller->GotDown(Button::UP)) activity_log::ScrollOlder();
    if (controller->GotDown(Button::DOWN)) activity_log::ScrollNewer();
    if (controller->GotDown(Button::L)) activity_log::ScrollOlder(12);
    if (controller->GotDown(Button::R)) activity_log::ScrollNewer(12);
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    stream::Menu::Draw(vg, theme);

    const auto muted = theme->GetColour(ThemeEntryID_TEXT_INFO);
    const auto line = theme->GetColour(ThemeEntryID_LINE);
    const bool pc_connected = m_usb_state == UsbState_Configured;

    const std::string status = pc_connected
        ? tr("aio_mtp_connected", "PC connected")
        : tr("aio_mtp_waiting", "Waiting for the PC");
    const auto ok = nvgRGBA(83, 214, 139, 255);
    const auto warn = nvgRGBA(238, 183, 82, 255);
    const auto status_colour = pc_connected ? ok : warn;

    gfx::drawText(vg, 52.f, 124.f, 20.f, muted, "USB / MTP", NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, 1228.f, 124.f, 18.f, status_colour, status.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    gfx::drawRect(vg, 52.f, 150.f, 1176.f, 1.f, line);

    const auto copy = activity_log::GetCopyStatus();
    const auto log_height = copy.active ? 300.f : 408.f;
    const auto info = activity_log::Draw(vg, theme, 52.f, 176.f, 1176.f, log_height);

    if (copy.active) {
        constexpr float panel_y = 490.f;
        constexpr float panel_h = 105.f;
        constexpr float bar_x = 70.f;
        constexpr float bar_y = 566.f;
        constexpr float bar_w = 1140.f;
        constexpr float bar_h = 10.f;
        const auto accent = theme->GetColour(ThemeEntryID_FOCUS);

        gfx::drawRect(vg, 52.f, panel_y, 1176.f, panel_h, nvgRGBA(7, 6, 11, 245), 3.f);
        gfx::drawRect(vg, 52.f, panel_y, 3.f, panel_h, accent);

        const auto title = tr("aio_mtp_copying", "Copying to microSD") + ": " + copy.path;
        nvgSave(vg);
        nvgIntersectScissor(vg, 70.f, panel_y + 5.f, 1140.f, 30.f);
        gfx::drawText(vg, 70.f, panel_y + 23.f, 18.f, theme->GetColour(ThemeEntryID_TEXT),
            title.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgRestore(vg);

        std::string progress = utils::formatSizeNetwork(copy.copied);
        std::string remaining = tr("aio_mtp_remaining", "Remaining") + ": -";
        float ratio{};
        if (copy.total) {
            ratio = std::clamp(static_cast<float>(copy.copied) / static_cast<float>(copy.total), 0.f, 1.f);
            const auto left = copy.total > copy.copied ? copy.total - copy.copied : 0;
            char percent[16]{};
            std::snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(ratio * 100.f));
            progress = std::string{percent} + "  " + progress + " / " + utils::formatSizeNetwork(copy.total);
            remaining = tr("aio_mtp_remaining", "Remaining") + ": " + utils::formatSizeNetwork(left);
            if (copy.bytes_per_second) {
                const auto seconds = left / copy.bytes_per_second;
                char eta[32]{};
                std::snprintf(eta, sizeof(eta), "  |  ~%llu:%02llu",
                    static_cast<unsigned long long>(seconds / 60),
                    static_cast<unsigned long long>(seconds % 60));
                remaining += eta;
            }
        }
        const auto speed = copy.bytes_per_second
            ? utils::formatSizeNetwork(copy.bytes_per_second) + "/s"
            : std::string{"-"};

        gfx::drawText(vg, 70.f, panel_y + 53.f, 16.f, muted, progress.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        gfx::drawText(vg, 1210.f, panel_y + 53.f, 16.f, muted,
            (remaining + "  |  " + speed).c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        gfx::drawRect(vg, bar_x, bar_y, bar_w, bar_h, nvgRGBA(44, 41, 50, 255), 3.f);
        if (ratio > 0.f) {
            gfx::drawRect(vg, bar_x, bar_y, bar_w * ratio, bar_h, accent, 3.f);
        }
    }

    char position[64]{};
    if (info.total) {
        std::snprintf(position, sizeof(position), "%zu-%zu / %zu%s", info.first, info.last, info.total,
            info.following_tail ? "  AUTO" : "");
    }
    gfx::drawText(vg, 52.f, 614.f, 16.f, muted, position, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, 1228.f, 614.f, 16.f, muted,
        tr("aio_mtp_scroll_hint", "UP/DOWN: scroll  |  L/R: page").c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
}

void Menu::OnDisableInstallMode() {
    libhaze::DisableInstallMode();
}

} // namespace sphaira::ui::menu::mtp
