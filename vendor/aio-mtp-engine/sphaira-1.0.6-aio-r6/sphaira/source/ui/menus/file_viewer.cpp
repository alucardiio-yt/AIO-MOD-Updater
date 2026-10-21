#include "ui/menus/file_viewer.hpp"

#include "app.hpp"
#include "i18n.hpp"
#include "swkbd.hpp"
#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/popup_list.hpp"

#include <algorithm>
#include <span>

namespace sphaira::ui::menu::fileview {
namespace {

constexpr std::size_t HISTORY_LIMIT = 32;

} // namespace

Menu::Menu(fs::Fs* fs, const fs::FsPath& path)
: MenuBase{path, MenuFlag_None}
, m_fs{fs}
, m_path{path} {
    SetActions(
        std::make_pair(Button::A, Action{"Edit"_i18n, [this](){ EditLine(); }}),
        std::make_pair(Button::X, Action{"Line actions"_i18n, [this](){ ShowLineActions(); }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){ Exit(); }})
    );

    std::string buf;
    if (R_SUCCEEDED(m_fs->OpenFile(m_path, FsOpenMode_Read, &m_file))) {
        m_file.GetSize(&m_file_size);
        buf.resize(m_file_size);

        u64 read_bytes{};
        if (R_FAILED(m_file.Read(0, buf.data(), buf.size(), FsReadOption_None, &read_bytes))) {
            buf.clear();
        } else {
            buf.resize(read_bytes);
        }
        m_file.Close();
    }

    m_saved_text = buf;
    if (buf.find("\r\n") != std::string::npos) {
        m_line_break = "\r\n";
    }

    std::size_t start{};
    while (start <= buf.size()) {
        const auto end = buf.find('\n', start);
        auto line = buf.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        m_lines.emplace_back(std::move(line));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (m_lines.empty()) {
        m_lines.emplace_back();
    }

    const Vec4 list_pos{45.f, 100.f, 1180.f, 530.f};
    const Vec4 item_pos{55.f, 105.f, 1150.f, 58.f};
    m_list = std::make_unique<List>(1, 9, list_pos, item_pos, Vec2{0.f, 1.f});
    m_list->SetScrollBarPos(1222.f, 110.f, 510.f);
    UpdateStatus();
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    m_list->OnUpdate(controller, touch, m_index, m_lines.size(), [this](bool touched, s64 index){
        if (m_index != index) {
            m_scrolling_text.Reset();
            m_index = index;
        }
        if (touched) {
            FireAction(Button::A);
        }
        UpdateStatus();
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    m_list->Draw(vg, theme, m_lines.size(), [this](auto* vg, auto* theme, const Vec4& pos, s64 index){
        const bool selected = m_index == index;
        if (selected) {
            gfx::drawRect(vg, pos, theme->GetColour(ThemeEntryID_GRID));
            gfx::drawRectOutline(vg, theme, 4.f, pos);
        } else {
            gfx::drawRect(vg, pos.x, pos.y + pos.h, pos.w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        const auto colour = theme->GetColour(selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT);
        gfx::drawTextArgs(vg, pos.x + 68.f, pos.y + pos.h / 2.f, 17.f,
            NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%ld", index + 1);
        gfx::drawRect(vg, pos.x + 82.f, pos.y + 10.f, 1.f, pos.h - 20.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));

        const auto& line = m_lines[index];
        m_scrolling_text.Draw(vg, selected, pos.x + 100.f, pos.y + pos.h / 2.f,
            pos.w - 120.f, 19.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, colour,
            line.empty() ? std::string{" "} : line);
    });
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    UpdateStatus();
}

void Menu::EditLine() {
    if (m_lines.empty()) {
        return;
    }

    std::string output;
    const auto title = i18n::Reorder("Edit Line ", std::to_string(m_index + 1));
    if (R_SUCCEEDED(swkbd::ShowText(output, title.c_str(), "Edit"_i18n.c_str(),
            m_lines[m_index].c_str(), 0, PATH_MAX - 1)) && output != m_lines[m_index]) {
        BeforeChange();
        m_lines[m_index] = std::move(output);
        m_scrolling_text.Reset();
        UpdateStatus();
    }
}

void Menu::ShowLineActions() {
    const PopupList::Items actions{
        "New line"_i18n,
        "Delete line"_i18n,
        "Join with next line"_i18n,
        "Go to line..."_i18n,
        "Undo"_i18n,
        "Redo"_i18n,
        "Save All"_i18n,
    };
    App::Push<PopupList>("Line actions"_i18n, actions, [this](auto index){
        if (!index) {
            return;
        }
        switch (*index) {
            case 0: InsertLine(); break;
            case 1: DeleteLine(); break;
            case 2: JoinLine(); break;
            case 3: GoToLine(); break;
            case 4: Undo(); break;
            case 5: Redo(); break;
            case 6: Save(); break;
        }
    });
}

void Menu::InsertLine() {
    const auto pos = std::min<std::size_t>(m_index + 1, m_lines.size());
    std::string output;
    const auto title = i18n::Reorder("Edit Line ", std::to_string(pos + 1));
    if (R_FAILED(swkbd::ShowText(output, title.c_str(), "New line"_i18n.c_str(),
            nullptr, 0, PATH_MAX - 1))) {
        return;
    }

    BeforeChange();
    m_lines.insert(m_lines.begin() + pos, std::move(output));
    m_index = pos;
    m_scrolling_text.Reset();
    UpdateStatus();
}

void Menu::DeleteLine() {
    BeforeChange();
    if (m_lines.size() == 1) {
        m_lines.front().clear();
        m_index = 0;
    } else {
        m_lines.erase(m_lines.begin() + m_index);
        m_index = std::min<s64>(m_index, m_lines.size() - 1);
    }
    m_scrolling_text.Reset();
    UpdateStatus();
}

void Menu::JoinLine() {
    if (m_index + 1 >= static_cast<s64>(m_lines.size())) {
        App::Notify("No next line to join"_i18n);
        return;
    }
    BeforeChange();
    m_lines[m_index] += m_lines[m_index + 1];
    m_lines.erase(m_lines.begin() + m_index + 1);
    m_scrolling_text.Reset();
    UpdateStatus();
}

void Menu::GoToLine() {
    s64 line = m_index + 1;
    const auto initial = std::to_string(line);
    if (R_SUCCEEDED(swkbd::ShowNumPad(line, "Go to line..."_i18n.c_str(),
            "Line:"_i18n.c_str(), initial.c_str(), 1, 8))) {
        m_index = std::clamp<s64>(line - 1, 0, m_lines.size() - 1);
        const auto page_start = std::max<s64>(0, m_index - 4);
        m_list->SetYoff(page_start * m_list->GetMaxY());
        m_scrolling_text.Reset();
        UpdateStatus();
    }
}

void Menu::Undo() {
    if (m_undo.empty()) {
        App::Notify("Nothing to undo"_i18n);
        return;
    }
    m_redo.emplace_back(std::move(m_lines));
    m_lines = std::move(m_undo.back());
    m_undo.pop_back();
    m_index = std::min<s64>(m_index, m_lines.size() - 1);
    m_dirty = BuildText() != m_saved_text;
    m_scrolling_text.Reset();
    UpdateStatus();
}

void Menu::Redo() {
    if (m_redo.empty()) {
        App::Notify("Nothing to redo"_i18n);
        return;
    }
    m_undo.emplace_back(std::move(m_lines));
    m_lines = std::move(m_redo.back());
    m_redo.pop_back();
    m_index = std::min<s64>(m_index, m_lines.size() - 1);
    m_dirty = BuildText() != m_saved_text;
    m_scrolling_text.Reset();
    UpdateStatus();
}

void Menu::Save() {
    if (!m_dirty) {
        App::Notify("No changes to save"_i18n);
        return;
    }

    const auto text = BuildText();
    const auto bytes = std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()};
    if (R_FAILED(m_fs->write_entire_file(m_path, bytes))) {
        App::Notify("Save failed!"_i18n);
        return;
    }

    m_saved_text = text;
    m_file_size = text.size();
    m_dirty = false;
    m_undo.clear();
    m_redo.clear();
    UpdateStatus();
    App::Notify("Saved successfully."_i18n);
}

void Menu::Exit() {
    if (!m_dirty) {
        SetPop();
        return;
    }

    App::Push<OptionBox>(
        "Unsaved changes. Exit without saving?"_i18n,
        "Keep Editing"_i18n,
        "Discard"_i18n,
        0,
        [this](auto index){
            if (index && *index == 1) {
                SetPop();
            }
        }
    );
}

void Menu::BeforeChange() {
    m_undo.emplace_back(m_lines);
    if (m_undo.size() > HISTORY_LIMIT) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_dirty = true;
}

void Menu::UpdateStatus() {
    SetTitleSubHeading("Line: "_i18n + std::to_string(m_index + 1) + " / " + std::to_string(m_lines.size()));
    SetSubHeading(m_dirty ? "Modified - use Line actions to save"_i18n : "UTF-8 text editor"_i18n);
}

auto Menu::BuildText() const -> std::string {
    std::string text;
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        if (i) {
            text += m_line_break;
        }
        text += m_lines[i];
    }
    return text;
}

} // namespace sphaira::ui::menu::fileview
