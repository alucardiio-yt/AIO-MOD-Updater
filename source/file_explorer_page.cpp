#include "file_explorer_page.hpp"

#include "ini_editor_page.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <stdexcept>

using namespace brls::i18n::literals;

namespace {

namespace fs = std::filesystem;

void text(NVGcontext* vg, brls::FrameContext* ctx, float x, float y, float size,
    const std::string& value, NVGcolor color, int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE) {
    nvgFontFaceId(vg, ctx->fontStash->regular);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, color);
    nvgText(vg, x, y, value.c_str(), nullptr);
}

void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor color) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 4.f);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

bool endsWith(std::string name, const char* suffix) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto len = std::char_traits<char>::length(suffix);
    return name.size() >= len && name.compare(name.size() - len, len, suffix) == 0;
}

std::string operationStatus(explorer::Operation operation) {
    switch (operation) {
        case explorer::Operation::Copy: return "explorer/copying"_i18n;
        case explorer::Operation::Move: return "explorer/moving"_i18n;
        case explorer::Operation::Delete: return "explorer/deleting"_i18n;
        case explorer::Operation::ExtractZip: return "explorer/extracting"_i18n;
    }
    return "explorer/working"_i18n;
}

void confirm(const std::string& message, const std::string& accept, std::function<void()> action) {
    auto* dialog = new brls::Dialog(message);
    dialog->addButton("explorer/cancel"_i18n, [dialog](brls::View*) { dialog->close(); });
    dialog->addButton(accept, [dialog, action = std::move(action)](brls::View*) {
        dialog->close(action);
    });
    dialog->open();
}

} // namespace

FileExplorerPage::FileExplorerPage() : brls::AppletFrame(true, true) {
    setTitle("explorer/title"_i18n);
    setFooterText("explorer/footer_single"_i18n);
    explorerView = new FileExplorerView(this);
    setContentView(explorerView);
}

bool FileExplorerPage::onCancel() { return explorerView->onBack(); }

FileExplorerView::FileExplorerView(FileExplorerPage* owner) : owner(owner) {
#ifdef __SWITCH__
    padInitializeDefault(&shortcutPad);
    padUpdate(&shortcutPad);
#endif
    refresh(0);
    registerAction("explorer/open"_i18n, brls::Key::A, [this] { open(); return true; });
    registerAction("explorer/back"_i18n, brls::Key::B, [this] { return onBack(); });
    registerAction("", brls::Key::Y, [this] { toggleSelection(); return true; });
    registerAction("", brls::Key::X, [this] { actions(); return true; });
    registerAction("", brls::Key::L, [this] { switchPane(); return true; });
    registerAction("", brls::Key::R, [this] { switchPane(); return true; });
    registerAction("", brls::Key::MINUS, [this] { createFolder(); return true; });
    registerAction("", brls::Key::PLUS, [this] {
        if (busy) return true;
        const auto& pane = panes[active];
        if (pane.entries.empty()) actions();
        else if (pane.entries[pane.cursor].directory) folderMenu(pane.entries[pane.cursor]);
        else fileMenu(pane.entries[pane.cursor]);
        return true;
    });
#ifdef __SWITCH__
    // Keep Borealis from moving focus; the held-button navigation runs in draw().
    registerAction("", brls::Key::DUP, [] { return true; }, true);
    registerAction("", brls::Key::DDOWN, [] { return true; }, true);
#else
    registerAction("", brls::Key::DUP, [this] { step(-1); return true; }, true);
    registerAction("", brls::Key::DDOWN, [this] { step(1); return true; }, true);
#endif
}

FileExplorerView::~FileExplorerView() {
    for (auto& stop : stopScans) stop = true;
    jobCancel = true;
    for (auto& thread : scanThreads) if (thread.joinable()) thread.join();
    if (jobThread.joinable()) jobThread.join();
}

void FileExplorerView::error(const std::string& message) {
    status = message;
    util::showDialogBoxInfo(message);
}

bool pathInside(const fs::path& child, const fs::path& parent) {
    const auto c = explorer::clean(child).generic_string();
    const auto p = explorer::clean(parent).generic_string();
    return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/');
}

using MenuOption = std::pair<std::string, std::function<void()>>;

void openMenu(const std::string& title, std::vector<MenuOption> options) {
    auto* frame = new brls::AppletFrame(true, true);
    auto* list = new brls::List();
    for (auto& [label, callback] : options) {
        auto* item = new brls::ListItem(label);
        item->getClickEvent()->subscribe([callback = std::move(callback)](brls::View*) {
            brls::Application::popView(brls::ViewAnimation::FADE, callback);
        });
        list->addView(item);
    }
    frame->setContentView(list);
    brls::PopupFrame::open(title, frame);
}

void FileExplorerView::invalidateSizeCache() {
    for (auto& stop : stopScans) stop = true;
    for (auto& thread : scanThreads) if (thread.joinable()) thread.join();
    std::lock_guard lock(dataMutex);
    sizeCache.clear();
}

void FileExplorerView::refreshAll(bool recalculateSizes) {
    if (recalculateSizes) invalidateSizeCache();
    refresh(active);
    if (dualPane) refresh(1 - active);
}

void FileExplorerView::refresh(unsigned which) {
    stopScans[which] = true;
    if (scanThreads[which].joinable()) scanThreads[which].join();
    stopScans[which] = false;
    auto& pane = panes[which];
    try {
        auto entries = explorer::list(pane.folder);
        std::lock_guard lock(dataMutex);
        pane.entries = std::move(entries);
        for (auto& entry : pane.entries) {
            if (!entry.directory) continue;
            if (auto found = sizeCache.find(entry.path.string()); found != sizeCache.end()) {
                entry.size = found->second;
                entry.sizeKnown = true;
            }
        }
        if (pane.cursor >= pane.entries.size()) pane.cursor = pane.entries.empty() ? 0 : pane.entries.size() - 1;
        if (pane.first > pane.cursor) pane.first = pane.cursor;
        for (auto it = pane.selected.begin(); it != pane.selected.end();) {
            const bool exists = std::any_of(pane.entries.begin(), pane.entries.end(),
                [&](const auto& e) { return e.path.string() == *it; });
            if (!exists) it = pane.selected.erase(it); else ++it;
        }
    } catch (const std::exception& e) { status = e.what(); }
    startSizeScan(which);
}

void FileExplorerView::startSizeScan(unsigned which) {
    scanThreads[which] = std::thread([this, which] {
        size_t count;
        {
            std::lock_guard lock(dataMutex);
            count = panes[which].entries.size();
        }
        for (size_t index = 0; index < count && !stopScans[which]; ++index) {
            explorer::path::path path;
            {
                std::lock_guard lock(dataMutex);
                if (index >= panes[which].entries.size()) return;
                if (!panes[which].entries[index].directory || panes[which].entries[index].sizeKnown) continue;
                path = panes[which].entries[index].path;
            }
            try {
                const auto bytes = explorer::directorySize(path, stopScans[which]);
                if (stopScans[which]) return;
                std::lock_guard lock(dataMutex);
                if (index < panes[which].entries.size() && panes[which].entries[index].path == path) {
                    panes[which].entries[index].size = bytes;
                    panes[which].entries[index].sizeKnown = true;
                    sizeCache[path.string()] = bytes;
                }
            } catch (...) { /* A directory may disappear while scanning. */ }
        }
    });
}

void FileExplorerView::step(int delta) {
    if (busy) return;
    auto& p = panes[active];
    if (p.entries.empty()) return;
    p.cursor = static_cast<size_t>(std::clamp<long long>(static_cast<long long>(p.cursor) + delta,
        0, static_cast<long long>(p.entries.size() - 1)));
    constexpr size_t rows = 10;
    if (p.cursor < p.first) p.first = p.cursor;
    if (p.cursor >= p.first + rows) p.first = p.cursor - rows + 1;
}

void FileExplorerView::switchPane() { if (!busy && dualPane) active = 1 - active; }

void FileExplorerView::togglePanels() {
    if (busy) return;
    dualPane = !dualPane;
    owner->setFooterText(dualPane ? "explorer/footer_double"_i18n : "explorer/footer_single"_i18n);
    if (dualPane) refresh(1 - active);
}

bool FileExplorerView::onBack() {
    if (busy) { jobCancel = true; return true; }
    auto& p = panes[active];
    if (p.folder != explorer::path::path("/")) {
        p.folder = p.folder.parent_path();
        p.cursor = p.first = 0;
        p.selected.clear();
        refresh(active);
        return true;
    }
    brls::Application::popView();
    return true;
}

void FileExplorerView::open() {
    if (busy) return;
    auto& p = panes[active];
    if (p.entries.empty()) return;
    const auto entry = p.entries[p.cursor];
    if (entry.directory) {
        p.folder = entry.path;
        p.cursor = p.first = 0;
        p.selected.clear();
        refresh(active);
    } else if (endsWith(entry.name, ".ini") || endsWith(entry.name, ".json") ||
               endsWith(entry.name, ".txt")) {
        brls::Application::pushView(new IniEditorPage(entry.path));
    } else fileMenu(entry);
}

void FileExplorerView::fileMenu(const explorer::Entry& entry) { entryMenu(entry); }

void FileExplorerView::folderMenu(const explorer::Entry& entry) { entryMenu(entry); }

void FileExplorerView::entryMenu(const explorer::Entry& entry) {
    if (busy) return;
    const std::vector<explorer::path::path> one{entry.path};
    const auto destination = entry.directory ? entry.path : panes[active].folder;
    std::vector<MenuOption> options;
    if (endsWith(entry.name, ".ini") || endsWith(entry.name, ".json") || endsWith(entry.name, ".txt"))
        options.emplace_back("explorer/edit_text"_i18n, [entry] {
            brls::Application::pushView(new IniEditorPage(entry.path));
        });
    if (endsWith(entry.name, ".zip"))
        options.emplace_back("explorer/extract"_i18n, [this, entry] {
            const auto target = dualPane ? panes[1 - active].folder : panes[active].folder;
            confirm("explorer/confirm_extract"_i18n + "\n" + entry.name + " -> " + target.string(),
                "explorer/extract"_i18n,
                [this, entry, target] { startJob({explorer::Operation::ExtractZip, {entry.path}, target}); });
        });
    options.emplace_back("explorer/clipboard_copy"_i18n, [this, one] { stageClipboard(one, false); });
    options.emplace_back("explorer/clipboard_cut"_i18n, [this, one] { stageClipboard(one, true); });
    if (!clipboard.empty())
        options.emplace_back("explorer/paste"_i18n, [this, destination] { paste(destination); });
    options.emplace_back("explorer/rename"_i18n, [this, entry] { renameEntry(entry); });
    options.emplace_back("explorer/delete"_i18n, [this, one, entry] {
        confirm("explorer/confirm_delete"_i18n + " " + entry.name + "?", "explorer/delete"_i18n,
            [this, one] { startJob({explorer::Operation::Delete, one, {}}); });
    });
    openMenu(entry.name, std::move(options));
}

void FileExplorerView::stageClipboard(const std::vector<explorer::path::path>& paths, bool cut) {
    if (paths.empty()) return;
    clipboard = paths;
    clipboardCut = cut;
    status = (cut ? "explorer/staged_cut"_i18n : "explorer/staged_copy"_i18n) +
        " " + std::to_string(paths.size()) + " " + "explorer/items"_i18n;
}

void FileExplorerView::paste(const explorer::path::path& destination) {
    if (busy) return;
    if (clipboard.empty()) { error("explorer/clipboard_empty"_i18n); return; }
    if (clipboardCut) {
        confirm("explorer/confirm_paste_move"_i18n + " " + destination.string() + "?",
            "explorer/move"_i18n, [this, destination] {
                startJob({explorer::Operation::Move, clipboard, destination});
            });
    } else startJob({explorer::Operation::Copy, clipboard, destination});
}

void FileExplorerView::renameEntry(const explorer::Entry& entry) {
    if (busy) return;
    if (entry.directory && pathInside(panes[1 - active].folder, entry.path)) {
        error("explorer/rename_open_folder"_i18n);
        return;
    }
    std::string name;
    const bool accepted = brls::Swkbd::openForText([&](std::string value) { name = std::move(value); },
        "explorer/rename"_i18n, entry.path.parent_path().string(), 255, entry.name);
    if (!accepted || name.empty() || name == entry.name) return;
    if (name == "." || name == ".." || name.find_first_of("/:\\") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; })) {
        error("explorer/invalid_name"_i18n); return;
    }
    const auto target = entry.path.parent_path() / name;
    std::error_code ec;
    if (!explorer::isSdPath(target) || fs::exists(target, ec) || ec) {
        error("explorer/target_exists"_i18n); return;
    }
    invalidateSizeCache();
    fs::rename(entry.path, target, ec);
    if (ec) error("explorer/rename_failed"_i18n + ": " + ec.message());
    else {
        status = "explorer/renamed"_i18n + ": " + name;
        if (std::any_of(clipboard.begin(), clipboard.end(), [&](const auto& p) {
            return pathInside(p, entry.path);
        })) clipboard.clear();
    }
    panes[active].selected.clear();
    refreshAll(false);
}

void FileExplorerView::toggleSelection() {
    if (busy || panes[active].entries.empty()) return;
    auto& p = panes[active];
    const auto name = p.entries[p.cursor].path.string();
    if (!p.selected.erase(name)) p.selected.insert(name);
}

std::vector<explorer::path::path> FileExplorerView::sources() const {
    const auto& p = panes[active];
    std::vector<explorer::path::path> paths;
    if (!p.selected.empty()) {
        for (const auto& e : p.entries) if (p.selected.count(e.path.string())) paths.push_back(e.path);
    } else if (!p.entries.empty()) paths.push_back(p.entries[p.cursor].path);
    return paths;
}

void FileExplorerView::actions() {
    if (busy) return;
    const auto paths = sources();
    const auto destination = panes[1 - active].folder;
    std::vector<MenuOption> options;
    options.emplace_back("explorer/new_folder"_i18n, [this] { createFolder(); });
    if (!clipboard.empty())
        options.emplace_back("explorer/paste"_i18n, [this] { paste(panes[active].folder); });
    options.emplace_back("explorer/refresh"_i18n, [this] { refreshAll(true); });
    if (!paths.empty()) {
        options.emplace_back("explorer/clipboard_copy"_i18n, [this, paths] { stageClipboard(paths, false); });
        options.emplace_back("explorer/clipboard_cut"_i18n, [this, paths] { stageClipboard(paths, true); });
        if (paths.size() == 1) {
            const auto& p = panes[active];
            const auto it = std::find_if(p.entries.begin(), p.entries.end(), [&](const auto& e) {
                return e.path == paths.front();
            });
            if (it != p.entries.end())
                options.emplace_back("explorer/rename"_i18n, [this, entry = *it] { renameEntry(entry); });
        }
    }
    if (!paths.empty() && dualPane) {
        options.emplace_back("explorer/copy_other"_i18n, [this, paths, destination] {
            startJob({explorer::Operation::Copy, paths, destination});
        });
        options.emplace_back("explorer/move_other"_i18n, [this, paths, destination] {
            confirm("explorer/confirm_move"_i18n + " " + std::to_string(paths.size()) +
                " " + "explorer/items"_i18n + "?", "explorer/move"_i18n,
                [this, paths, destination] { startJob({explorer::Operation::Move, paths, destination}); });
        });
    }
    if (!paths.empty()) options.emplace_back("explorer/delete"_i18n, [this, paths] {
        confirm("explorer/confirm_delete"_i18n + " " + std::to_string(paths.size()) +
            " " + "explorer/items"_i18n + "?", "explorer/delete"_i18n,
            [this, paths] { startJob({explorer::Operation::Delete, paths, {}}); });
    });
    openMenu("explorer/actions"_i18n, std::move(options));
}

void FileExplorerView::createFolder() {
    if (busy) return;
    std::string name;
    const bool accepted = brls::Swkbd::openForText([&](std::string value) { name = std::move(value); },
        "explorer/new_folder"_i18n, "explorer/destination"_i18n + ": " + panes[active].folder.string(), 100);
    if (!accepted || name.empty()) return;
    if (name == "." || name == ".." || name.find_first_of("/:\\") != std::string::npos) {
        error("explorer/invalid_name"_i18n); return;
    }
    const auto target = panes[active].folder / name;
    std::error_code ec;
    const bool created = explorer::path::create_directory(target, ec);
    if (ec) { error("explorer/create_failed"_i18n + ": " + ec.message()); return; }
    if (!created) { error("explorer/target_exists"_i18n); return; }
    refresh(active);
    if (dualPane) refresh(1 - active);
}

void FileExplorerView::startJob(explorer::Job job) {
    if (busy) return;
    for (auto& stop : stopScans) stop = true;
    for (auto& thread : scanThreads) if (thread.joinable()) thread.join();
    if (jobThread.joinable()) jobThread.join();
    busy = true;
    jobCancel = false;
    jobFinished = false;
    jobCompleted = jobTotal = 0;
    runningJob = job;
    {
        std::lock_guard lock(dataMutex);
        jobCurrentFile.clear();
    }
    status = operationStatus(job.operation);
    jobThread = std::thread([this, job = std::move(job)] {
        const auto result = explorer::run(job, jobCancel, jobCompleted, jobTotal,
            [this](const explorer::path::path& path) {
                std::lock_guard lock(dataMutex);
                jobCurrentFile = path.filename().string();
            });
        {
            std::lock_guard lock(dataMutex);
            jobResult = result;
        }
        jobFinished.store(true, std::memory_order_release);
    });
}

void FileExplorerView::finishJob() {
    if (!busy || !jobFinished.load(std::memory_order_acquire)) return;
    if (jobThread.joinable()) jobThread.join();
    busy = false;
    if (jobResult.rfind("ERROR:", 0) != 0 &&
        (runningJob.operation == explorer::Operation::Move || runningJob.operation == explorer::Operation::Delete) &&
        std::any_of(clipboard.begin(), clipboard.end(), [&](const auto& p) {
            return std::any_of(runningJob.sources.begin(), runningJob.sources.end(), [&](const auto& source) {
                return pathInside(p, source);
            });
        })) clipboard.clear();
    if (runningJob.operation == explorer::Operation::Move || runningJob.operation == explorer::Operation::Delete) {
        clipboard.erase(std::remove_if(clipboard.begin(), clipboard.end(), [](const auto& path) {
            std::error_code ec;
            return !fs::exists(path, ec) && !ec;
        }), clipboard.end());
    }
    if (clipboard.empty()) clipboardCut = false;
    {
        std::lock_guard lock(dataMutex);
        if (jobResult == "Elementos eliminados") status = "explorer/deleted"_i18n;
        else if (jobResult == "Elementos movidos") status = "explorer/moved"_i18n;
        else if (jobResult == "Copia completada") status = "explorer/copied"_i18n;
        else if (jobResult == "ZIP descomprimido") status = "explorer/extracted"_i18n;
        else status = jobResult;
    }
    invalidateSizeCache();
    for (auto& pane : panes) {
        std::error_code ec;
        while (pane.folder != fs::path("/") && !fs::exists(pane.folder, ec) && !ec) {
            pane.folder = pane.folder.parent_path();
            pane.cursor = pane.first = 0;
            pane.selected.clear();
        }
    }
    refresh(active);
    if (dualPane) refresh(1 - active);
    if (status.rfind("ERROR:", 0) == 0) util::showDialogBoxInfo(status);
}

void FileExplorerView::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
    brls::Style*, brls::FrameContext* ctx) {
#ifdef __SWITCH__
    padUpdate(&shortcutPad);
    if (brls::Application::getCurrentFocus() == this) {
        if (padGetButtonsDown(&shortcutPad) & HidNpadButton_ZL) togglePanels();

        const auto held = padGetButtons(&shortcutPad);
        const bool up = (held & HidNpadButton_AnyUp) != 0;
        const bool down = (held & HidNpadButton_AnyDown) != 0;
        const int direction = busy || up == down ? 0 : (up ? -1 : 1);
        const auto now = std::chrono::steady_clock::now();
        if (direction != heldNavigationDirection) {
            heldNavigationDirection = direction;
            if (direction) {
                step(direction);
                nextNavigationRepeat = now + std::chrono::milliseconds(330);
            }
        } else if (direction && now >= nextNavigationRepeat) {
            step(direction);
            nextNavigationRepeat = now + std::chrono::milliseconds(85);
        }
    } else {
        heldNavigationDirection = 0;
    }
#endif
    finishJob();
    const auto fg = nvgRGB(232, 229, 238);
    const auto muted = nvgRGB(163, 158, 174);
    const auto accent = nvgRGB(177, 113, 229);
    const float gap = 14.f;
    const float paneWidth = dualPane ? (static_cast<float>(width) - gap) / 2.f : static_cast<float>(width);
    const float rowHeight = 43.f;
    std::lock_guard lock(dataMutex);
    for (unsigned n = 0; n < (dualPane ? 2u : 1u); ++n) {
        const unsigned paneIndex = dualPane ? n : active;
        const auto& pane = panes[paneIndex];
        const float left = static_cast<float>(x) + n * (paneWidth + gap);
        rect(vg, left, y + 8.f, paneWidth, height - 16.f, nvgRGB(20, 17, 27));
        if (paneIndex == active) rect(vg, left, y + 8.f, 4.f, height - 16.f, accent);
        nvgSave(vg);
        nvgIntersectScissor(vg, left + 14.f, y + 10.f, paneWidth - 28.f, height - 20.f);
        text(vg, ctx, left + 20.f, y + 30.f, 18.f, pane.folder.string(), fg);
        text(vg, ctx, left + paneWidth - 20.f, y + 57.f, 14.f,
            std::to_string(pane.entries.size()) + " " + "explorer/items"_i18n + " · " +
                std::to_string(pane.selected.size()) + " " + "explorer/selected"_i18n,
            muted, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        for (size_t row = 0; row < 10 && pane.first + row < pane.entries.size(); ++row) {
            const size_t index = pane.first + row;
            const auto& entry = pane.entries[index];
            const float rowY = y + 83.f + row * rowHeight;
            if (index == pane.cursor && paneIndex == active)
                rect(vg, left + 12.f, rowY - 18.f, paneWidth - 24.f, rowHeight - 3.f, nvgRGB(65, 43, 81));
            const std::string prefix = pane.selected.count(entry.path.string()) ? "[x] " : "    ";
            text(vg, ctx, left + 20.f, rowY, 16.f,
                prefix + (entry.directory ? "> " : "  ") + entry.name, fg);
            std::string meta = entry.sizeKnown ? explorer::formatSize(entry.size) : "explorer/calculating"_i18n;
            if (!entry.modified.empty()) meta += "  ·  " + entry.modified;
            text(vg, ctx, left + paneWidth - 20.f, rowY + 17.f, 12.f, meta,
                muted, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        }
        nvgRestore(vg);
    }
    if (busy) {
        const float panelY = y + height - 78.f;
        rect(vg, x + 8.f, panelY, width - 16.f, 70.f, nvgRGB(29, 23, 39));
        const std::string current = jobCurrentFile.empty() ? "explorer/calculating"_i18n : jobCurrentFile;
        nvgSave(vg);
        nvgIntersectScissor(vg, x + 20.f, panelY + 3.f, width - 175.f, 28.f);
        text(vg, ctx, x + 20.f, panelY + 19.f, 16.f, status + ": " + current, fg);
        nvgRestore(vg);

        const auto total = jobTotal.load();
        const auto completed = jobCompleted.load();
        const bool bytes = runningJob.operation == explorer::Operation::Copy ||
            runningJob.operation == explorer::Operation::ExtractZip;
        std::string amount = "explorer/calculating"_i18n;
        float fraction = 0.f;
        if (total > 0) {
            fraction = std::clamp(static_cast<float>(static_cast<double>(completed) / static_cast<double>(total)), 0.f, 1.f);
            amount = std::to_string(static_cast<int>(std::round(fraction * 100.f))) + "%  ";
            amount += bytes ? explorer::formatSize(completed) + " / " + explorer::formatSize(total)
                            : std::to_string(completed) + " / " + std::to_string(total) + " " + "explorer/items"_i18n;
        }
        text(vg, ctx, x + width - 20.f, panelY + 19.f, 14.f, "B " + "explorer/cancel"_i18n,
            muted, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        text(vg, ctx, x + 20.f, panelY + 41.f, 13.f, amount, muted);
        const float barX = x + 20.f;
        const float barY = panelY + 52.f;
        const float barWidth = width - 40.f;
        rect(vg, barX, barY, barWidth, 9.f, nvgRGB(62, 49, 74));
        if (total > 0) {
            if (fraction > 0.f) rect(vg, barX, barY, barWidth * fraction, 9.f, accent);
        } else {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const float offset = static_cast<float>(ms % 1400) / 1400.f;
            rect(vg, barX + (barWidth - barWidth * .22f) * offset, barY, barWidth * .22f, 9.f, accent);
        }
    } else {
        text(vg, ctx, x + 16.f, y + height - 19.f, 14.f, status, muted);
    }
}
