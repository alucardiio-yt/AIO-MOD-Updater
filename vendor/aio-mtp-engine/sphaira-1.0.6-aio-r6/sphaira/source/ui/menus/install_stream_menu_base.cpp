#include "ui/menus/install_stream_menu_base.hpp"
#include "ui/activity_log.hpp"
#include "yati/yati.hpp"
#include "app.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "ui/nvg_util.hpp"
#include "i18n.hpp"
#include <cstdio>
#include <cstring>

namespace sphaira::ui::menu::stream {
namespace {

constexpr u64 MAX_BUFFER_SIZE = 1024ULL*1024ULL*1ULL;
constexpr u64 WAIT_TIMEOUT = 1e+8;
constexpr u64 POLL_INTERVAL_FAST = 1e+6;
constexpr u64 POLL_INTERVAL_SLOW = 1e+7;

} // namespace

Stream::Stream(const fs::FsPath& path, std::stop_token token) {
    m_path = path;
    m_token = token;
    m_active = true;
    m_buffer.reserve(MAX_BUFFER_SIZE);

    mutexInit(&m_mutex);
    condvarInit(&m_can_read);
    condvarInit(&m_can_write);
}

void Stream::Wait(CondVar* cv) {
    // Recheck the stop token even if a wakeup was missed.
    condvarWaitTimeout(cv, std::addressof(m_mutex), WAIT_TIMEOUT);
}

Result Stream::ReadChunk(void* _buf, s64 size, u64* bytes_read) {
    auto buf = static_cast<u8*>(_buf);
    *bytes_read = 0;

    log_write("[Stream::ReadChunk] inside\n");
    ON_SCOPE_EXIT(
        log_write("[Stream::ReadChunk] exiting\n");
    );

    while (!m_token.stop_requested()) {
        SCOPED_MUTEX(&m_mutex);
        if (m_buffer.empty()) {
            if (!m_active) {
                break;
            }
            Wait(std::addressof(m_can_read));
            continue;
        }

        const auto rsize = std::min<s64>(size, m_buffer.size());
        std::memcpy(buf, m_buffer.data(), rsize);
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + rsize);
        condvarWakeAll(&m_can_write);

        size -= rsize;
        buf += rsize;
        *bytes_read += rsize;

        if (!size) {
            R_SUCCEED();
        }
    }

    if (*bytes_read && !m_token.stop_requested()) {
        R_SUCCEED();
    }
    log_write("[Stream::ReadChunk] failed to read\n");
    R_THROW(Result_TransferCancelled);
}

bool Stream::Push(const void* _buf, s64 size) {
    auto buf = static_cast<const u8*>(_buf);
    if (!size) {
        return true;
    }

    log_write("[Stream::Push] inside\n");
    ON_SCOPE_EXIT(
        log_write("[Stream::Push] exiting\n");
    );

    while (!m_token.stop_requested()) {
        SCOPED_MUTEX(&m_mutex);
        if (m_finished) {
            log_write("[Stream::Push] install has finished\n");
            return true;
        }

        if (!m_active) {
            log_write("[Stream::Push] file not active\n");
            break;
        }

        if (m_buffer.size() >= MAX_BUFFER_SIZE) {
            Wait(std::addressof(m_can_write));
            continue;
        }

        const auto wsize = std::min<s64>(size, MAX_BUFFER_SIZE - m_buffer.size());
        const auto offset = m_buffer.size();
        m_buffer.resize(offset + wsize);

        std::memcpy(m_buffer.data() + offset, buf, wsize);
        condvarWakeAll(&m_can_read);

        size -= wsize;
        buf += wsize;
        if (!size) {
            return true;
        }
    }

    log_write("[Stream::Push] failed to push\n");
    return false;
}

void Stream::Finish() {
    SCOPED_MUTEX(&m_mutex);
    m_finished = true;
    condvarWakeAll(&m_can_read);
    condvarWakeAll(&m_can_write);
}

void Stream::Disable() {
    log_write("[Stream::Disable] disabling file\n");

    SCOPED_MUTEX(&m_mutex);
    m_active = false;
    condvarWakeAll(&m_can_read);
    condvarWakeAll(&m_can_write);
}

Menu::Menu(const std::string& title, u32 flags) : MenuBase{title, flags} {
    SetAction(Button::B, Action{"Back"_i18n, [this](){
        SetPop();
    }});

    SetAction(Button::X, Action{"Options"_i18n, [this](){
        App::DisplayInstallOptions(false);
    }});

    App::SetAutoSleepDisabled(true);
    mutexInit(&m_mutex);

}

Menu::~Menu() {
    m_stop_source.request_stop();
    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }
    if (source) {
        source->Disable();
        source->Finish();
    }
    App::SetAutoSleepDisabled(false);
}

void Menu::CancelInstallMode() {
    m_stop_source.request_stop();
    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }
    if (source) {
        source->Disable();
        source->Finish();
    }
    OnDisableInstallMode();
}

bool Menu::WaitForIdle(u64 poll_ns) {
    for (;;) {
        {
            SCOPED_MUTEX(&m_mutex);
            if (m_state != State::Connected && m_state != State::Progress) {
                return true;
            }
        }
        if (GetToken().stop_requested()) {
            return false;
        }
        svcSleepThread(poll_ns);
    }
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        if (m_state != State::Connected) {
            return;
        }
        m_state = State::Progress;
        m_last_result = 0;
        source = m_source;
    }

    App::Push<ui::ProgressBox>(0, "Installing "_i18n, source->GetPath(), [source](auto pbox) -> Result {
        ON_SCOPE_EXIT(source->Finish());
        const auto rc = yati::InstallFromSource(pbox, source.get(), source->GetPath());
        if (R_FAILED(rc)) {
            R_THROW(rc);
        }
        R_SUCCEED();
    }, [this](Result rc){
        {
            SCOPED_MUTEX(&m_mutex);
            m_last_result = rc;
            m_state = R_SUCCEEDED(rc) ? State::Done : State::Failed;
        }
        if (R_SUCCEEDED(rc)) {
            activity_log::Add("[OK]", i18n::get("aio_mtp_done", "Installation completed"), activity_log::Tone::Success);
            activity_log::AddResult(rc, activity_log::Tone::Success);
        } else {
            activity_log::Add("[ERROR]", i18n::get("aio_mtp_failed", "Installation failed"), activity_log::Tone::Error);
            activity_log::AddResult(rc, activity_log::Tone::Error);
        }
    }, ui::ProgressBoxOption::None);
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);
}

bool Menu::OnInstallStart(const char* path) {
    log_write("[Menu::OnInstallStart] inside\n");
    if (!WaitForIdle(POLL_INTERVAL_FAST)) {
        return false;
    }

    {
        SCOPED_MUTEX(&m_mutex);
        if (GetToken().stop_requested()) {
            return false;
        }
        m_source = std::make_shared<Stream>(path, GetToken());
        m_last_result = 0;
        m_state = State::Connected;
    }
    activity_log::Add("[FILE]", path);
    activity_log::Add("[START]", i18n::get("aio_mtp_installing", "Installing package..."), activity_log::Tone::Success);
    log_write("[Menu::OnInstallStart] exiting\n");

    return true;
}

bool Menu::OnInstallWrite(const void* buf, size_t size) {
    log_write("[Menu::OnInstallWrite] inside\n");
    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }
    return source && source->Push(buf, size);
}

void Menu::OnInstallClose() {
    log_write("[Menu::OnInstallClose] inside\n");

    std::shared_ptr<Stream> source;
    {
        SCOPED_MUTEX(&m_mutex);
        source = m_source;
    }
    if (source) {
        source->Disable();
    }
    // Also wait when a small file closes before the next UI frame.
    WaitForIdle(POLL_INTERVAL_SLOW);
}

} // namespace sphaira::ui::menu::stream
