#include "ini_editor_page.hpp"

#include "file_explorer_ops.hpp"
#include "utils.hpp"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace brls::i18n::literals;

namespace {

void confirmDiscard(std::function<void()> action) {
    auto* dialog = new brls::Dialog("explorer/ini_discard_prompt"_i18n);
    dialog->addButton("explorer/ini_continue"_i18n, [dialog](brls::View*) { dialog->close(); });
    dialog->addButton("explorer/ini_discard"_i18n, [dialog, action = std::move(action)](brls::View*) { dialog->close(action); });
    dialog->open();
}

} // namespace

IniEditorPage::IniEditorPage(const std::filesystem::path& file) : brls::AppletFrame(true, true) {
    auto extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return std::tolower(c); });
    setTitle(extension == ".ini" ? "explorer/ini_title"_i18n : "explorer/text_title"_i18n);
    setFooterText("explorer/ini_footer"_i18n);
    editorView = new IniEditorView(this, file);
    setContentView(editorView);
}

bool IniEditorPage::onCancel() { return editorView->onBack(); }

IniEditorView::IniEditorView(IniEditorPage* owner, const std::filesystem::path& file) : owner(owner), file(file) {
    try {
        auto extension = file.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (!explorer::isSdPath(file) ||
            (extension != ".ini" && extension != ".json" && extension != ".txt"))
            throw std::runtime_error("Solo se editan archivos INI, JSON o TXT de la microSD");
        jsonFile = extension == ".json";
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size > 128 * 1024) throw std::runtime_error("Archivo demasiado grande para el editor (maximo 128 KiB)");
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("No se pudo abrir el archivo");
        const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (contents.find('\0') != std::string::npos) throw std::runtime_error("El archivo no es texto plano");
        crlf = contents.find("\r\n") != std::string::npos;
        trailingNewline = !contents.empty() && contents.back() == '\n';
        size_t start = 0;
        while (start < contents.size()) {
            const auto end = contents.find('\n', start);
            std::string line = contents.substr(start, end == std::string::npos ? end : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (lines.empty()) lines.emplace_back();
        if (lines.size() > 2000) throw std::runtime_error("Archivo con demasiadas lineas");
        loaded = true;
        status = file.string();
    } catch (const std::exception& e) { status = e.what(); }

    registerAction("explorer/ini_edit"_i18n, brls::Key::A, [this] { edit(); return true; });
    registerAction("", brls::Key::PLUS, [this] { save(); return true; });
    registerAction("", brls::Key::X, [this] { insertLine(); return true; });
    registerAction("", brls::Key::Y, [this] { deleteLine(); return true; });
    registerAction("explorer/back"_i18n, brls::Key::B, [this] { return onBack(); });
    registerAction("", brls::Key::DUP, [this] { move(-1); return true; }, true);
    registerAction("", brls::Key::DDOWN, [this] { move(1); return true; }, true);
}

void IniEditorView::move(int amount) {
    if (!loaded) return;
    cursor = static_cast<size_t>(std::clamp<long long>(static_cast<long long>(cursor) + amount,
        0, static_cast<long long>(lines.size() - 1)));
    if (cursor < first) first = cursor;
    if (cursor >= first + 14) first = cursor - 13;
}

void IniEditorView::edit() {
    if (!loaded) return;
    if (lines[cursor].size() > 512) {
        util::showDialogBoxInfo("explorer/ini_line_too_long"_i18n);
        return;
    }
    std::string updated;
    if (brls::Swkbd::openForText([&](std::string text) { updated = std::move(text); },
            "explorer/ini_edit"_i18n + " " + std::to_string(cursor + 1),
            "explorer/ini_one_line"_i18n, 512,
            lines[cursor], 0, "explorer/accept"_i18n)) {
        if (updated.find('\n') != std::string::npos || updated.find('\r') != std::string::npos) {
            util::showDialogBoxInfo("explorer/ini_no_newlines"_i18n); return;
        }
        if (updated != lines[cursor]) { lines[cursor] = std::move(updated); dirty = true; }
    }
}

void IniEditorView::insertLine() {
    if (!loaded || lines.size() >= 2000) return;
    std::string added;
    if (brls::Swkbd::openForText([&](std::string text) { added = std::move(text); },
            "explorer/ini_insert"_i18n, "explorer/ini_one_line"_i18n, 512)) {
        if (added.find('\n') != std::string::npos || added.find('\r') != std::string::npos) {
            util::showDialogBoxInfo("explorer/ini_no_newlines"_i18n); return;
        }
        lines.insert(lines.begin() + cursor + 1, std::move(added));
        ++cursor;
        if (cursor >= first + 14) first = cursor - 13;
        dirty = true;
    }
}

void IniEditorView::deleteLine() {
    if (!loaded) return;
    auto* dialog = new brls::Dialog("explorer/ini_confirm_delete"_i18n + " " + std::to_string(cursor + 1) + "?");
    dialog->addButton("explorer/cancel"_i18n, [dialog](brls::View*) { dialog->close(); });
    dialog->addButton("explorer/delete"_i18n, [this, dialog](brls::View*) {
        dialog->close([this] {
            if (lines.size() == 1) lines.front().clear();
            else lines.erase(lines.begin() + cursor);
            if (cursor >= lines.size()) cursor = lines.size() - 1;
            dirty = true;
        });
    });
    dialog->open();
}

void IniEditorView::save() {
    if (!loaded || !dirty) return;
    std::string serialized;
    for (size_t i = 0; i < lines.size(); ++i) {
        serialized += lines[i];
        if (i + 1 < lines.size() || trailingNewline) serialized += crlf ? "\r\n" : "\n";
    }
    if (serialized.size() > 128 * 1024) {
        util::showDialogBoxInfo("explorer/text_too_large"_i18n); return;
    }
    if (jsonFile) {
        try { (void)nlohmann::json::parse(serialized); }
        catch (const std::exception& e) {
            util::showDialogBoxInfo("explorer/json_invalid"_i18n + ": " + e.what());
            return;
        }
    }
    const auto temporary = file.string() + ".aio-edit.tmp";
    const auto backup = file.string() + ".aio-edit.bak";
    std::error_code ec;
    if (std::filesystem::exists(temporary, ec) || std::filesystem::exists(backup, ec)) {
        util::showDialogBoxInfo("explorer/ini_temp_exists"_i18n); return;
    }
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("No se pudo crear el archivo temporal");
        output.write(serialized.data(), serialized.size());
        output.flush();
        if (!output.good()) throw std::runtime_error("No se pudo escribir el INI");
        output.close();
        std::filesystem::rename(file, backup, ec);
        if (ec) throw std::runtime_error("No se pudo resguardar el INI original: " + ec.message());
        std::filesystem::rename(temporary, file, ec);
        if (ec) {
            std::error_code restore;
            std::filesystem::rename(backup, file, restore);
            throw std::runtime_error("No se pudo guardar el INI: " + ec.message());
        }
        std::filesystem::remove(backup, ec);
        dirty = false;
        status = "explorer/ini_saved"_i18n + ": " + file.filename().string();
    } catch (const std::exception& e) {
        std::error_code cleanup;
        if (!std::filesystem::exists(file, cleanup) && std::filesystem::exists(backup, cleanup))
            std::filesystem::rename(backup, file, cleanup);
        if (std::filesystem::exists(file, cleanup)) std::filesystem::remove(temporary, cleanup);
        status = e.what();
        util::showDialogBoxInfo(status);
    }
}

bool IniEditorView::onBack() {
    if (dirty) confirmDiscard([] { brls::Application::popView(); });
    else brls::Application::popView();
    return true;
}

void IniEditorView::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
    brls::Style*, brls::FrameContext* ctx) {
    nvgSave(vg);
    nvgIntersectScissor(vg, x + 12.f, y + 8.f, width - 24.f, height - 16.f);
    nvgFontFaceId(vg, ctx->fontStash->regular);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 18.f);
    nvgFillColor(vg, nvgRGB(226, 222, 232));
    nvgText(vg, x + 20.f, y + 24.f, status.c_str(), nullptr);
    if (loaded) {
        for (size_t row = 0; row < 14 && first + row < lines.size(); ++row) {
            const size_t index = first + row;
            const float top = y + 60.f + row * 32.f;
            if (index == cursor) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x + 12.f, top - 15.f, width - 24.f, 30.f, 3.f);
                nvgFillColor(vg, nvgRGB(64, 43, 80));
                nvgFill(vg);
            }
            const auto display = std::to_string(index + 1) + "  " + lines[index];
            nvgFontSize(vg, 16.f);
            nvgFillColor(vg, nvgRGB(226, 222, 232));
            nvgText(vg, x + 20.f, top, display.c_str(), nullptr);
        }
        const auto position = std::to_string(cursor + 1) + " / " + std::to_string(lines.size()) +
            (dirty ? "  ·  " + "explorer/ini_unsaved"_i18n : "");
        nvgFontSize(vg, 15.f);
        nvgFillColor(vg, nvgRGB(166, 160, 174));
        nvgText(vg, x + 20.f, y + height - 20.f, position.c_str(), nullptr);
    }
    nvgRestore(vg);
}
