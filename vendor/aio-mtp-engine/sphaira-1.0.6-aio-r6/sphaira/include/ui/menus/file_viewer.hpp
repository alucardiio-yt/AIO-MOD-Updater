#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include "ui/scrolling_text.hpp"
#include "fs.hpp"

#include <vector>

namespace sphaira::ui::menu::fileview {

struct Menu final : MenuBase {
    Menu(fs::Fs* fs, const fs::FsPath& path);

    auto GetShortTitle() const -> const char* override { return "File"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void EditLine();
    void ShowLineActions();
    void InsertLine();
    void DeleteLine();
    void JoinLine();
    void GoToLine();
    void Undo();
    void Redo();
    void Save();
    void Exit();
    void BeforeChange();
    void UpdateStatus();
    auto BuildText() const -> std::string;

private:
    fs::Fs* const m_fs;
    const fs::FsPath m_path;
    fs::File m_file{};
    s64 m_file_size{};
    std::string m_line_break{"\n"};
    std::string m_saved_text;
    std::vector<std::string> m_lines;
    std::vector<std::vector<std::string>> m_undo;
    std::vector<std::vector<std::string>> m_redo;
    std::unique_ptr<List> m_list;
    ScrollingText m_scrolling_text;
    s64 m_index{};
    bool m_dirty{};
};

} // namespace sphaira::ui::menu::fileview
