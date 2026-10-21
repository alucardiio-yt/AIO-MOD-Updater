#pragma once

#include <borealis.hpp>

#include "file_explorer_ops.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#ifdef __SWITCH__
#include <switch.h>
#endif

class FileExplorerPage;

class FileExplorerView final : public brls::View {
public:
    explicit FileExplorerView(FileExplorerPage* owner);
    ~FileExplorerView() override;
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
        brls::Style* style, brls::FrameContext* ctx) override;
    brls::View* getDefaultFocus() override { return this; }
    bool onBack();

private:
    struct Pane {
        explorer::path::path folder{"/"};
        std::vector<explorer::Entry> entries;
        std::set<std::string> selected;
        size_t cursor{};
        size_t first{};
    };

    FileExplorerPage* owner;
    std::array<Pane, 2> panes;
    unsigned active{};
    bool dualPane{};
#ifdef __SWITCH__
    PadState shortcutPad{};
#endif
    std::mutex dataMutex;
    std::unordered_map<std::string, std::uintmax_t> sizeCache;
    std::array<std::thread, 2> scanThreads;
    std::array<std::atomic_bool, 2> stopScans{false, false};
    std::thread jobThread;
    std::atomic_bool jobCancel{false};
    std::atomic_bool jobFinished{false};
    std::atomic<std::uintmax_t> jobCompleted{0};
    std::atomic<std::uintmax_t> jobTotal{0};
    std::string jobResult;
    explorer::Job runningJob;
    std::string status;
    bool busy{};
    std::vector<explorer::path::path> clipboard;
    bool clipboardCut{};

    void refresh(unsigned which);
    void invalidateSizeCache();
    void startSizeScan(unsigned which);
    void step(int delta);
    void switchPane();
    void togglePanels();
    void open();
    void fileMenu(const explorer::Entry& entry);
    void folderMenu(const explorer::Entry& entry);
    void entryMenu(const explorer::Entry& entry);
    void stageClipboard(const std::vector<explorer::path::path>& paths, bool cut);
    void paste(const explorer::path::path& destination);
    void renameEntry(const explorer::Entry& entry);
    void refreshAll(bool recalculateSizes);
    void toggleSelection();
    void actions();
    void createFolder();
    void startJob(explorer::Job job);
    void finishJob();
    std::vector<explorer::path::path> sources() const;
    void error(const std::string& message);
};

class FileExplorerPage final : public brls::AppletFrame {
public:
    FileExplorerPage();
    bool onCancel() override;
    FileExplorerView* explorerView{};
};
