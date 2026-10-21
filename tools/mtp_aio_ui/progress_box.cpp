#include "ui/progress_box.hpp"
#include "ui/option_box.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "threaded_file_transfer.hpp"
#include "i18n.hpp"
#include "ui/activity_log.hpp"

#include "utils/utils.hpp"
#include "utils/thread.hpp"

#include <cstring>
#include <cmath>

namespace sphaira::ui {
namespace {

void threadFunc(void* arg) {
    auto d = static_cast<ProgressBox::ThreadData*>(arg);
    d->result = d->callback(d->pbox);
    d->pbox->RequestExit();
}

} // namespace

ProgressBox::ProgressBox(int image, const std::string& action, const std::string& title, const ProgressBoxCallback& callback, const ProgressBoxDoneCallback& done, ProgressBoxOption option)
: m_done{done}
, m_action{action}
, m_title{title}
, m_source_title{title}
, m_image{image} {
    App::SetAutoSleepDisabled(true);
    if (App::GetApp()->m_progress_boost_mode.Get()) {
        App::SetBoostMode(true);
    }

    SetAction(Button::B, Action{"Cancel"_i18n, [this](){
        RequestExit();
    }});

    if (option == ProgressBoxOption::ScreenToggle && App::AcquireScreenToggle()) {
        m_screen_toggle_enabled = true;
        SetAction(Button::Y, Action{[](){
            App::SetScreenDisabled(true);
        }});
    }

    m_pos.w = 770.f;
    m_pos.h = m_screen_toggle_enabled ? 355.f : 295.f;
    m_pos.x = (SCREEN_WIDTH / 2.f) - (m_pos.w / 2.f);
    m_pos.y = (SCREEN_HEIGHT / 2.f) - (m_pos.h / 2.f);

    // create cancel event.
    ueventCreate(&m_uevent, false);

    m_thread_data.pbox = this;
    m_thread_data.callback = callback;
    if (R_FAILED(utils::CreateThread(&m_thread, threadFunc, &m_thread_data))) {
        log_write("failed to create thead\n");
    }
    if (R_FAILED(threadStart(&m_thread))) {
        log_write("failed to start thread\n");
    }
}

ProgressBox::~ProgressBox() {
    ueventSignal(GetCancelEvent());
    m_stop_source.request_stop();

    if (R_FAILED(threadWaitForExit(&m_thread))) {
        log_write("failed to join thread\n");
    }
    if (R_FAILED(threadClose(&m_thread))) {
        log_write("failed to close thread\n");
    }

    FreeImage();
    if (m_screen_toggle_enabled) {
        App::ReleaseScreenToggle();
    }

    if (m_done) {
        m_done(m_thread_data.result);
    }

    App::SetBoostMode(false);
    App::SetAutoSleepDisabled(false);
}

auto ProgressBox::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);

    if (controller->GotDown(Button::UP)) activity_log::ScrollOlder();
    if (controller->GotDown(Button::DOWN)) activity_log::ScrollNewer();
    if (controller->GotDown(Button::L)) activity_log::ScrollOlder(10);
    if (controller->GotDown(Button::R)) activity_log::ScrollNewer(10);

    if (m_screen_toggle_enabled && touch->is_clicked) {
        for (const auto& button : GetFooterButtons()) {
            if (touch->in_range(button.GetPos())) {
                FireAction(button.m_button);
                break;
            }
        }
    }

    if (ShouldExit()) {
        SetPop();
    }
}

auto ProgressBox::Draw(NVGcontext* vg, Theme* theme) -> void {
    mutexLock(&m_mutex);
    m_image_data.clear();
    if (m_timestamp.GetSeconds()) {
        m_timestamp.Update();
        m_speed = m_offset - m_last_offset;
        m_last_offset = m_offset;
    }

    const auto title = m_title;
    const auto source_title = m_source_title;
    const auto transfer = m_transfer;
    const auto size = m_size;
    const auto offset = m_offset;
    const auto speed = m_speed;
    m_image_pending = 0;
    m_is_image_pending = false;
    mutexUnlock(&m_mutex);

    FreeImage();

    const auto text = theme->GetColour(ThemeEntryID_TEXT);
    const auto muted = theme->GetColour(ThemeEntryID_TEXT_INFO);
    const auto accent = theme->GetColour(ThemeEntryID_FOCUS);
    const auto line = theme->GetColour(ThemeEntryID_LINE);

    gfx::drawRect(vg, 0.f, 0.f, static_cast<float>(SCREEN_WIDTH), static_cast<float>(SCREEN_HEIGHT), nvgRGBA(4, 3, 7, 255));
    gfx::drawText(vg, 52.f, 49.f, 30.f, text, "AIO MOD", NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, 1228.f, 49.f, 17.f, accent,
        i18n::get("aio_mtp_installing", "Installing package...").c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    gfx::drawRect(vg, 52.f, 87.f, 1176.f, 1.f, line);

    const auto info = activity_log::Draw(vg, theme, 52.f, 108.f, 1176.f, 360.f);

    u32 percentage{};
    if (size > 0 && offset > 0) {
        percentage = static_cast<u32>((static_cast<double>(offset) / static_cast<double>(size)) * 100.0);
        if (percentage > 100) percentage = 100;
    }

    const auto live_text = !transfer.empty() ? transfer : (!title.empty() ? title : source_title);
    gfx::drawText(vg, 52.f, 493.f, 17.f, accent, "[LIVE]", NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, 132.f, 493.f, 17.f, text,
        (live_text.empty() ? i18n::get("aio_mtp_preparing", "Preparing installation") : live_text).c_str(),
        NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    char progress_text[128]{};
    const auto data = size > 0
        ? utils::formatSizeNetwork(offset) + " / " + utils::formatSizeNetwork(size)
        : std::string{"-"};
    const auto rate = speed > 0 ? utils::formatSizeNetwork(speed) + "/s" : std::string{"-"};
    std::snprintf(progress_text, sizeof(progress_text), "%u%%   %s   %s", percentage, data.c_str(), rate.c_str());
    gfx::drawText(vg, 1228.f, 493.f, 17.f, muted, progress_text, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);

    constexpr float progress_x = 52.f;
    constexpr float progress_y = 525.f;
    constexpr float progress_w = 1176.f;
    gfx::drawRect(vg, progress_x, progress_y, progress_w, 8.f, theme->GetColour(ThemeEntryID_PROGRESSBAR_BACKGROUND), 2.f);
    if (percentage) {
        gfx::drawRect(vg, progress_x, progress_y, progress_w * (static_cast<float>(percentage) / 100.f), 8.f,
            theme->GetColour(ThemeEntryID_PROGRESSBAR), 2.f);
    }

    char position[64]{};
    if (info.total) {
        std::snprintf(position, sizeof(position), "%zu-%zu / %zu%s", info.first, info.last, info.total,
            info.following_tail ? "  AUTO" : "");
    }
    gfx::drawText(vg, 52.f, 571.f, 16.f, muted, position, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, 1228.f, 571.f, 16.f, muted,
        i18n::get("aio_mtp_scroll_hint", "UP/DOWN: scroll  |  L/R: page").c_str(),
        NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);

    gfx::drawRect(vg, 52.f, 622.f, 1176.f, 1.f, line);
    const auto cancel_text = std::string{"B  "} + "Cancel"_i18n;
    gfx::drawText(vg, 1228.f, 672.f, 17.f, muted, cancel_text.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
}

auto ProgressBox::GetFooterButtons() const -> Widget::uiButtons {
    Widget::uiButtons buttons;
    const auto y = m_pos.y + m_pos.h - 40.f;

    const auto add_button = [&](Button button, const std::string& hint, float center_x) {
        Widget::uiButtons entry{uiButton{button, hint}};
        Widget::SetupUiButtons(entry, {center_x, y});

        auto& item = entry.front();
        const auto visual_left = item.m_button_pos.x - item.m_button_pos.w;
        const auto visual_right = item.m_hint_pos.x;
        const auto offset = center_x - (visual_left + visual_right) / 2.f;

        item.m_button_pos.x += offset;
        item.m_hint_pos.x += offset;
        item.SetX(item.GetX() + offset);
        buttons.emplace_back(std::move(item));
    };

    add_button(Button::Y, "Screen Off"_i18n, m_pos.x + m_pos.w * 0.25f);
    add_button(Button::B, "Back"_i18n, m_pos.x + m_pos.w * 0.75f);
    return buttons;
}

auto ProgressBox::SetActionName(const std::string& action)  -> ProgressBox& {
    bool changed{};
    {
        SCOPED_MUTEX(&m_mutex);
        changed = !action.empty() && action != m_action;
        m_action = action;
    }
    if (changed) activity_log::Add("[STATE]", action, activity_log::Tone::Success);
    return *this;
}

auto ProgressBox::SetTitle(const std::string& title)  -> ProgressBox& {
    bool changed{};
    {
        SCOPED_MUTEX(&m_mutex);
        changed = !title.empty() && title != m_title;
        m_title = title;
    }
    if (changed) activity_log::Add("[TITLE]", title);
    return *this;
}

auto ProgressBox::NewTransfer(const std::string& transfer)  -> ProgressBox& {
    {
        SCOPED_MUTEX(&m_mutex);
        m_transfer = transfer;
        m_size = 0;
        m_offset = 0;
        m_last_offset = 0;
        m_timestamp.Update();
    }
    if (!transfer.empty()) activity_log::Add("[TASK]", transfer, activity_log::Tone::Muted);
    return *this;
}

auto ProgressBox::ResetTranfser() -> ProgressBox& {
    SCOPED_MUTEX(&m_mutex);
    m_size = 0;
    m_offset = 0;
    m_last_offset = 0;
    m_timestamp.Update();
    return *this;
}

auto ProgressBox::UpdateTransfer(s64 offset, s64 size)  -> ProgressBox& {
    SCOPED_MUTEX(&m_mutex);
    m_size = size;
    m_offset = offset;
    return *this;
}

auto ProgressBox::SetImage(int image) -> ProgressBox& {
    SCOPED_MUTEX(&m_mutex);
    m_image_pending = image;
    m_is_image_pending = true;
    return *this;
}

auto ProgressBox::SetImageData(std::vector<u8>& data) -> ProgressBox& {
    SCOPED_MUTEX(&m_mutex);
    std::swap(m_image_data, data);
    return *this;
}

auto ProgressBox::SetImageDataConst(std::span<const u8> data) -> ProgressBox& {
    SCOPED_MUTEX(&m_mutex);
    m_image_data.resize(data.size());
    std::memcpy(m_image_data.data(), data.data(), m_image_data.size());
    return *this;
}

void ProgressBox::RequestExit() {
    SCOPED_MUTEX(&m_mutex);
    m_stop_source.request_stop();
    ueventSignal(GetCancelEvent());

    // cancel any registered events.
    for (auto& e : m_cancel_events) {
        ueventSignal(e);
    }
}

auto ProgressBox::ShouldExit() -> bool {
    return m_stop_source.stop_requested();
}

auto ProgressBox::ShouldExitResult() -> Result {
    if (ShouldExit()) {
        R_THROW(Result_TransferCancelled);
    }
    R_SUCCEED();
}

void ProgressBox::AddCancelEvent(UEvent* event) {
    if (!event) {
        return;
    }

    SCOPED_MUTEX(&m_mutex);
    if (std::ranges::find(m_cancel_events, event) == m_cancel_events.end()) {
        m_cancel_events.emplace_back(event);
    }
}

void ProgressBox::RemoveCancelEvent(const UEvent* event) {
    if (!event) {
        return;
    }

    SCOPED_MUTEX(&m_mutex);
    m_cancel_events.erase(std::remove(m_cancel_events.begin(), m_cancel_events.end(), event), m_cancel_events.end());
}

auto ProgressBox::CopyFile(fs::Fs* fs_src, fs::Fs* fs_dst, const fs::FsPath& src_path, const fs::FsPath& dst_path, bool single_threaded) -> Result {
    const auto is_file_based_emummc = App::IsFileBaseEmummc();
    const auto is_both_native = fs_src->IsNative() && fs_dst->IsNative();

    fs::File src_file;
    R_TRY(fs_src->OpenFile(src_path, FsOpenMode_Read, &src_file));

    s64 src_size;
    R_TRY(src_file.GetSize(&src_size));

    // this can fail if it already exists so we ignore the result.
    // if the file actually failed to be created, the result is implicitly
    // handled when we try and open it for writing.
    fs_dst->CreateFile(dst_path, src_size, 0);

    fs::File dst_file;
    R_TRY(fs_dst->OpenFile(dst_path, FsOpenMode_Write, &dst_file));
    R_TRY(dst_file.SetSize(src_size));

    R_TRY(thread::Transfer(this, src_size,
        [&](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
            const auto rc = src_file.Read(off, data, size, 0, bytes_read);

            if (is_both_native && is_file_based_emummc) {
                svcSleepThread(2e+6); // 2ms
            }

            return rc;
        },
        [&](const void* data, s64 off, s64 size) -> Result {
            const auto rc = dst_file.Write(off, data, size, 0);

            if (is_both_native && is_file_based_emummc) {
                svcSleepThread(2e+6); // 2ms
            }

            return rc;
        }, single_threaded ? thread::Mode::SingleThreaded : thread::Mode::MultiThreaded
    ));

    R_SUCCEED();
}

auto ProgressBox::CopyFile(fs::Fs* fs, const fs::FsPath& src_path, const fs::FsPath& dst_path, bool single_threaded) -> Result {
    return CopyFile(fs, fs, src_path, dst_path, single_threaded);
}

auto ProgressBox::CopyFile(const fs::FsPath& src_path, const fs::FsPath& dst_path, bool single_threaded) -> Result {
    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    return CopyFile(&fs, src_path, dst_path, single_threaded);
}

void ProgressBox::Yield() {
    svcSleepThread(YieldType_WithoutCoreMigration);
}

void ProgressBox::FreeImage() {
    if (m_image && m_own_image) {
        nvgDeleteImage(App::GetVg(), m_image);
    }

    m_image = 0;
    m_own_image = false;
}

} // namespace sphaira::ui
