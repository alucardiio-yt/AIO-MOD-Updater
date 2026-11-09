#include "dialogue_page.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "fs.hpp"
#include "main_frame.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
bool copyPayloadVerified(const char* from, const char* to)
{
    std::error_code error;
    const auto sourceSize = std::filesystem::file_size(from, error);
    if (error || sourceSize == 0)
        return false;
    std::ifstream source(from, std::ios::binary);
    std::ofstream destination(to, std::ios::binary | std::ios::trunc);
    if (!source || !destination)
        return false;
    destination << source.rdbuf();
    destination.flush();
    if (!destination || source.bad())
        return false;
    destination.close();
    if (!destination || std::filesystem::file_size(to, error) != sourceSize || error)
        return false;
    std::ifstream original(from, std::ios::binary);
    std::ifstream copied(to, std::ios::binary);
    if (!original || !copied)
        return false;
    std::array<char, 8192> left{}, right{};
    while (original && copied) {
        original.read(left.data(), left.size());
        copied.read(right.data(), right.size());
        if (original.gcount() != copied.gcount() ||
            std::memcmp(left.data(), right.data(), static_cast<size_t>(original.gcount())) != 0)
            return false;
    }
    return original.eof() && copied.eof() && !original.bad() && !copied.bad();
}
}

void DialoguePage::CreateView()
{
    this->button1 = (new brls::Button(brls::ButtonStyle::REGULAR))->setLabel("menus/common/yes"_i18n);
    this->button1->setParent(this);
    this->button2 = (new brls::Button(brls::ButtonStyle::REGULAR))->setLabel("menus/common/no"_i18n);
    this->button2->setParent(this);

    this->instantiateButtons();

    this->label->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->label->setParent(this);

    this->navigationMap.add(
        this->button1,
        brls::FocusDirection::RIGHT,
        this->button2);

    this->navigationMap.add(
        this->button2,
        brls::FocusDirection::LEFT,
        this->button1);

    this->registerAction("", brls::Key::B, [this] { return true; });
}

void DialoguePage::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    this->label->frame(ctx);
    this->button1->frame(ctx);
    this->button2->frame(ctx);
}

void DialoguePage::layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash)
{
    this->label->setWidth(this->width);
    this->label->invalidate(true);
    this->label->setBoundaries(
        this->x + this->width / 2 - this->label->getWidth() / 2,
        this->y + (this->height - this->label->getHeight() - this->y - style->CrashFrame.buttonHeight) / 2,
        this->label->getWidth(),
        this->label->getHeight());
    this->button1->setBoundaries(
        this->x + this->width / 2 - style->CrashFrame.buttonWidth / 2 - 200,
        this->y + (this->height - style->CrashFrame.buttonHeight * 3),
        style->CrashFrame.buttonWidth,
        style->CrashFrame.buttonHeight);
    this->button1->invalidate();

    this->button2->setBoundaries(
        this->x + this->width / 2 - style->CrashFrame.buttonWidth / 2 + 200,
        this->y + (this->height - style->CrashFrame.buttonHeight * 3),
        style->CrashFrame.buttonWidth,
        style->CrashFrame.buttonHeight);
    this->button2->invalidate();
}

brls::View* DialoguePage::getDefaultFocus()
{
    return this->button1;
}

brls::View* DialoguePage::getNextFocus(brls::FocusDirection direction, brls::View* currentView)
{
    return this->navigationMap.getNextFocus(direction, currentView);
}

void DialoguePage_ams::instantiateButtons()
{
    this->button1->getClickEvent()->subscribe([this](View* view) {
        if (!frame->isLastStage())
            frame->nextStage();
        else {
            brls::Application::pushView(new MainFrame());
        }
    });

    this->button2->getClickEvent()->subscribe([this](View* view) {
        auto hasPayload = [](const char* path) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            return !error && size > 0;
        };
        if (!hasPayload(RCM_PAYLOAD_PATH)) {
            util::showDialogBoxInfo("menus/errors/cfw_payload_copy"_i18n);
            return;
        }
        bool directReboot = false;
        const Result hardwareResult = util::canRebootToPayload(directReboot);
        if (R_FAILED(hardwareResult)) {
            util::showDialogBoxInfo("menus/errors/cfw_reboot_check"_i18n);
            return;
        }
        if (directReboot) {
            if (util::rebootToPayload(RCM_PAYLOAD_PATH) != 0) {
                util::showDialogBoxInfo("menus/errors/cfw_reboot_failed"_i18n);
                return;
            }
        }
        else {
            // Preserve the current root payload exactly when present. This is
            // the payload that actually booted this SD, so it is the best
            // rollback source. If no root payload exists, keep the previous
            // known-good fallback behavior.
            if (std::filesystem::exists(MARIKO_PAYLOAD_PATH_TEMP)) {
                util::showDialogBoxInfo("menus/errors/cfw_payload_pending"_i18n);
                return;
            }
            const bool hadRootPayload = std::filesystem::exists(MARIKO_PAYLOAD_PATH);
            bool saved = false;
            if (hadRootPayload && hasPayload(MARIKO_PAYLOAD_PATH)) {
                saved = copyPayloadVerified(MARIKO_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH_TEMP);
            }
            else if (!hadRootPayload && hasPayload(UPDATE_BIN_PATH)) {
                saved = copyPayloadVerified(UPDATE_BIN_PATH, MARIKO_PAYLOAD_PATH_TEMP);
            }
            else if (!hadRootPayload && hasPayload(REBOOT_PAYLOAD_PATH)) {
                saved = copyPayloadVerified(REBOOT_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH_TEMP);
            }
            if (!saved) {
                std::error_code cleanupError;
                std::filesystem::remove(MARIKO_PAYLOAD_PATH_TEMP, cleanupError);
                util::showDialogBoxInfo("menus/errors/cfw_payload_copy"_i18n);
                return;
            }
            if (!copyPayloadVerified(RCM_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH)) {
                if (hadRootPayload && !copyPayloadVerified(MARIKO_PAYLOAD_PATH_TEMP, MARIKO_PAYLOAD_PATH)) {
                    util::showDialogBoxInfo("menus/errors/cfw_payload_restore"_i18n);
                    return;
                }
                if (!hadRootPayload) {
                    std::error_code cleanupError;
                    std::filesystem::remove(MARIKO_PAYLOAD_PATH, cleanupError);
                }
                std::error_code cleanupError;
                std::filesystem::remove(MARIKO_PAYLOAD_PATH_TEMP, cleanupError);
                util::showDialogBoxInfo("menus/errors/cfw_payload_copy"_i18n);
                return;
            }
            Result rebootResult = spsmInitialize();
            if (R_SUCCEEDED(rebootResult)) {
                rebootResult = spsmShutdown(true);
                spsmExit();
            }
            if (R_FAILED(rebootResult)) {
                if (hadRootPayload && !copyPayloadVerified(MARIKO_PAYLOAD_PATH_TEMP, MARIKO_PAYLOAD_PATH)) {
                    util::showDialogBoxInfo("menus/errors/cfw_payload_restore"_i18n);
                    return;
                }
                if (!hadRootPayload) {
                    std::error_code cleanupError;
                    std::filesystem::remove(MARIKO_PAYLOAD_PATH, cleanupError);
                }
                std::error_code cleanupError;
                std::filesystem::remove(MARIKO_PAYLOAD_PATH_TEMP, cleanupError);
                util::showDialogBoxInfo("menus/errors/cfw_reboot_failed"_i18n);
                return;
            }
        }
        brls::Application::quit();
    });

    this->label = new brls::Label(brls::LabelStyle::DIALOG, "menus/ams_update/install_hekate"_i18n + "\n\n" + this->text, true);
    start = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(150);
}

void DialoguePage_ams::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    this->label->frame(ctx);
    this->button1->frame(ctx);

    auto end = std::chrono::high_resolution_clock::now();
    auto missing = std::max(1l - std::chrono::duration_cast<std::chrono::seconds>(end - start).count(), 0l);
    auto text = std::string("menus/common/no"_i18n);
    if (missing > 0) {
        this->button2->setLabel(text + " (" + std::to_string(missing) + ")");
        this->button2->setState(brls::ButtonState::DISABLED);
    }
    else {
        this->button2->setLabel(text);
        this->button2->setState(brls::ButtonState::ENABLED);
    }
    this->button2->invalidate();
    this->button2->frame(ctx);
}

void DialoguePage_fw::instantiateButtons()
{
    this->button2->getClickEvent()->subscribe([this](View* view) {
        if (!frame->isLastStage())
            frame->nextStage();
        else {
            brls::Application::pushView(new MainFrame());
        }
    });

    this->button1->getClickEvent()->subscribe([this](View* view) {
        envSetNextLoad(DAYBREAK_PATH, fmt::format("\"{}\" \"/firmware\"", DAYBREAK_PATH).c_str());
        romfsExit();
        brls::Application::quit();
    });

    this->label = new brls::Label(brls::LabelStyle::DIALOG, fmt::format("{}\n\n{}", this->text, "menus/firmware/launch_daybreak"_i18n), true);
}

void DialoguePage_confirm::instantiateButtons()
{
    this->button1->getClickEvent()->subscribe([this](View* view) {
        if (!frame->isLastStage())
            frame->nextStage();
        else {
            brls::Application::pushView(new MainFrame());
        }
    });

    this->button2->getClickEvent()->subscribe([this](View* view) {
        brls::Application::pushView(new MainFrame());
    });

    this->label = new brls::Label(brls::LabelStyle::DIALOG, this->text, true);
}
