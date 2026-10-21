#pragma once

#include <borealis.hpp>

#include <filesystem>
#include <string>
#include <vector>

class IniEditorPage;

class IniEditorView final : public brls::View {
public:
    IniEditorView(IniEditorPage* owner, const std::filesystem::path& file);
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
        brls::Style* style, brls::FrameContext* ctx) override;
    brls::View* getDefaultFocus() override { return this; }
    bool onBack();

private:
    IniEditorPage* owner;
    std::filesystem::path file;
    std::vector<std::string> lines;
    size_t cursor{};
    size_t first{};
    bool dirty{};
    bool crlf{};
    bool trailingNewline{};
    bool loaded{};
    bool jsonFile{};
    std::string status;

    void move(int amount);
    void edit();
    void insertLine();
    void deleteLine();
    void save();
};

class IniEditorPage final : public brls::AppletFrame {
public:
    explicit IniEditorPage(const std::filesystem::path& file);
    bool onCancel() override;
    IniEditorView* editorView{};
};
