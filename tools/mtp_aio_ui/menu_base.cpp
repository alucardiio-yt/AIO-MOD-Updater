#include "app.hpp"
#include "ui/menus/menu_base.hpp"
#include "ui/nvg_util.hpp"

#include <ctime>

namespace sphaira::ui::menu {

auto MenuBase::GetPolledData(bool force_refresh) -> PolledData {
    static PolledData data{};
    static TimeStamp timestamp{};
    static bool initialized{};

    if (!initialized || force_refresh || timestamp.GetSeconds() >= 1) {
        initialized = true;
        data = {};
        data.sd_free = data.sd_total = 1;
        data.emmc_free = data.emmc_total = 1;

        const auto now = std::time(nullptr);
        localtime_r(&now, &data.tm);
        nifmGetInternetConnectionStatus(&data.type, &data.strength, &data.status);
        nifmGetCurrentIpAddress(&data.ip);
        App::GetSdSize(&data.sd_free, &data.sd_total);
        App::GetEmmcSize(&data.emmc_free, &data.emmc_total);
        timestamp.Update();
    }

    return data;
}

MenuBase::MenuBase(const std::string& title, u32 flags) : m_title{title}, m_flags{flags} {
    SetPos(30, 88, 1220, 558);
    SetAction(Button::START, Action{App::Exit});
}

MenuBase::~MenuBase() = default;

void MenuBase::Update(Controller* controller, TouchInfo* touch) {
    Widget::Update(controller, touch);
}

void MenuBase::Draw(NVGcontext* vg, Theme* theme) {
    const auto text = theme->GetColour(ThemeEntryID_TEXT);
    const auto muted = theme->GetColour(ThemeEntryID_TEXT_INFO);
    const auto line = theme->GetColour(ThemeEntryID_LINE);

    gfx::drawRect(vg, 0.f, 0.f, static_cast<float>(SCREEN_WIDTH), static_cast<float>(SCREEN_HEIGHT), nvgRGBA(4, 3, 7, 255));
    gfx::drawText(vg, 52.f, 49.f, 30.f, text, "AIO MOD", NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawRect(vg, 52.f, 87.f, 1176.f, 1.f, line);
    gfx::drawRect(vg, 52.f, 622.f, 1176.f, 1.f, line);

    if (!m_sub_heading.empty()) {
        m_scroll_sub_heading.Draw(vg, true, 760.f, 49.f, 468.f, 16.f,
            NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, muted, m_sub_heading.c_str());
    }

    Widget::Draw(vg, theme);
}

} // namespace sphaira::ui::menu
