"""Keep MTP install callbacks outside the transport state lock."""

from pathlib import Path


def _replace_range(text: str, start: str, end: str, replacement: str, label: str, after: str = "") -> str:
    offset = text.find(after) if after else 0
    if offset < 0:
        raise RuntimeError(f"MTP patch missing section: {label}")
    begin = text.find(start, offset)
    finish = text.find(end, begin + len(start)) if begin >= 0 else -1
    if begin < 0 or finish < 0:
        raise RuntimeError(f"MTP patch missing markers: {label}")
    return text[:begin] + replacement + text[finish:]


def patch_file(path: Path) -> None:
    raw = path.read_bytes()
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if "struct InstallCallbackGuard" in text:
        if raw != text.encode("utf-8"):
            path.write_bytes(text.encode("utf-8"))
        return

    text = text.replace("#include <algorithm>\n", "#include <algorithm>\n#include <atomic>\n", 1)
    text = _replace_range(text, "struct InstallSharedData {", "constexpr int THREAD_PRIO", '''struct InstallSharedData {
    Mutex mutex;
    CondVar cv;
    std::string current_file;
    void* user;
    OnInstallStart on_start;
    OnInstallWrite on_write;
    OnInstallClose on_close;
    bool in_progress;
    u32 in_flight;
};

''', "shared install state")
    text = _replace_range(text, "InstallSharedData g_shared_data{};", "struct FsProxyBase", '''InstallSharedData g_shared_data{};
std::atomic_bool g_install_enabled{false};

const char* SUPPORTED_EXT[] = {
    ".nsp", ".xci", ".nsz", ".xcz", ".msp",
};

// An active callback keeps the menu's captured callbacks alive while the
// transport lock is free. Shutdown waits for all guards to finish.
struct InstallCallbackGuard {
    InstallCallbackGuard() {
        SCOPED_MUTEX(&g_shared_data.mutex);
        if (g_install_enabled) {
            active = true;
            ++g_shared_data.in_flight;
        }
    }

    ~InstallCallbackGuard() {
        if (!active) return;
        SCOPED_MUTEX(&g_shared_data.mutex);
        --g_shared_data.in_flight;
        condvarWakeAll(&g_shared_data.cv);
    }

    InstallCallbackGuard(const InstallCallbackGuard&) = delete;
    InstallCallbackGuard& operator=(const InstallCallbackGuard&) = delete;
    explicit operator bool() const { return active; }

private:
    bool active{};
};

bool StartInstall(std::string path) {
    InstallCallbackGuard guard;
    if (!guard) return false;

    OnInstallStart on_start;
    {
        SCOPED_MUTEX(&g_shared_data.mutex);
        if (g_shared_data.in_progress || !g_shared_data.current_file.empty() || !g_shared_data.on_start) {
            return false;
        }
        g_shared_data.current_file = path;
        on_start = g_shared_data.on_start;
    }

    if (!on_start(path.c_str())) {
        SCOPED_MUTEX(&g_shared_data.mutex);
        g_shared_data.current_file.clear();
        return false;
    }

    SCOPED_MUTEX(&g_shared_data.mutex);
    g_shared_data.in_progress = true;
    return true;
}

''', "install callback guard")

    install_proxy = "struct FsInstallProxy final : FsProxyVfs {"
    text = _replace_range(text, "    Result FailedIfNotEnabled() {", "    bool IsValidFileType", '''    Result FailedIfNotEnabled() {
        if (!g_install_enabled) {
            App::Notify("Please launch MTP install menu before trying to install"_i18n);
            R_THROW(FsError_NotImplemented);
        }
        R_SUCCEED();
    }

''', "install mode check", install_proxy)
    text = _replace_range(text, "    Result WriteFile(haze::File *file, s64 off", "    void CloseFile(haze::File *file) override {", '''    Result WriteFile(haze::File *file, s64 off, const void *buf, u64 write_size) override {
        auto f = static_cast<File*>(file->impl);
        R_UNLESS(f && f->entry, FsError_PathNotFound);

        if (f->installing) {
            InstallCallbackGuard guard;
            if (!guard) R_THROW(FsError_NotImplemented);

            OnInstallWrite on_write;
            {
                SCOPED_MUTEX(&g_shared_data.mutex);
                on_write = g_shared_data.on_write;
            }
            if (!on_write || !on_write(buf, write_size)) {
                log_write("[MTP] install stream write failed\\n");
                R_THROW(FsError_NotImplemented);
            }
        } else if (!g_install_enabled) {
            R_THROW(FsError_NotImplemented);
        }

        R_TRY(FsProxyVfs::WriteFile(file, off, buf, write_size));
        R_SUCCEED();
    }

''', "install stream write", install_proxy)
    text = _replace_range(text, "    void CloseFile(haze::File *file) override {", "};\n\nstd::shared_ptr<FsInstallProxy>", '''    void CloseFile(haze::File *file) override {
        auto f = static_cast<File*>(file->impl);
        if (!f) return;

        if (f->installing) {
            OnInstallClose on_close;
            {
                InstallCallbackGuard guard;
                if (guard) {
                    SCOPED_MUTEX(&g_shared_data.mutex);
                    on_close = g_shared_data.on_close;
                }
                // Closing can wait for the entire installation; never hold
                // g_shared_data.mutex while invoking this callback.
                if (on_close) on_close();
            }
            SCOPED_MUTEX(&g_shared_data.mutex);
            g_shared_data.in_progress = false;
            g_shared_data.current_file.clear();
        }
        FsProxyVfs::CloseFile(file);
    }
''', "install stream close", install_proxy)

    text = _replace_range(text, "void InitInstallMode(const OnInstallStart&", "} // namespace sphaira::libhaze", '''void InitInstallMode(const OnInstallStart& on_start, const OnInstallWrite& on_write, const OnInstallClose& on_close) {
    SCOPED_MUTEX(&g_shared_data.mutex);
    condvarInit(&g_shared_data.cv);
    g_shared_data.on_start = on_start;
    g_shared_data.on_write = on_write;
    g_shared_data.on_close = on_close;
    g_shared_data.in_progress = false;
    g_shared_data.current_file.clear();
    g_install_enabled = true;
}

void DisableInstallMode() {
    // Publish shutdown before waiting: the menu unblocks its stream first.
    g_install_enabled = false;
    SCOPED_MUTEX(&g_shared_data.mutex);
    while (g_shared_data.in_flight) {
        condvarWaitTimeout(&g_shared_data.cv, &g_shared_data.mutex, 1e+8);
    }
    g_shared_data.on_start = {};
    g_shared_data.on_write = {};
    g_shared_data.on_close = {};
    g_shared_data.in_progress = false;
    g_shared_data.current_file.clear();
}

''', "install mode lifecycle")
    path.write_bytes(text.encode("utf-8"))
