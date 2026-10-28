#pragma once

#include <switch.h>

#include "ui/menus/install_stream_menu_base.hpp"

namespace sphaira::ui::menu::mtp {

struct Menu final : stream::Menu {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "AIO MOD"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnDisableInstallMode() override;

private:
    bool m_was_mtp_enabled{};
    bool m_usb_state_initialized{};
    bool m_last_pc_connected{};
    UsbDeviceSpeed m_last_logged_speed{UsbDeviceSpeed_None};
    UsbState m_usb_state{UsbState_Detached};
    UsbDeviceSpeed m_usb_speed{UsbDeviceSpeed_None};
};

} // namespace sphaira::ui::menu::mtp
