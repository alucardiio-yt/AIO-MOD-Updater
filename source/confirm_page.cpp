#include "confirm_page.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "fs.hpp"
#include "main_frame.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
bool usableFile(const std::string& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > 0;
}

bool copyFileVerified(const std::string& from, const std::string& to)
{
    std::error_code error;
    const auto sourceSize = std::filesystem::file_size(from, error);
    if (error || sourceSize == 0)
        return false;
    std::ifstream source(from, std::ios::binary);
    if (!source)
        return false;
    std::ofstream destination(to, std::ios::binary | std::ios::trunc);
    if (!destination)
        return false;
    destination << source.rdbuf();
    destination.flush();
    if (!destination || source.bad())
        return false;
    destination.close();
    if (!destination)
        return false;
    const auto copiedSize = std::filesystem::file_size(to, error);
    return !error && copiedSize == sourceSize;
}
}

ConfirmPage::ConfirmPage(brls::StagedAppletFrame* frame, const std::string& text, bool defaultAction)
{
    this->button = (new brls::Button(brls::ButtonStyle::REGULAR))->setLabel("menus/common/back"_i18n);
    this->button->setParent(this);
    if (defaultAction) {
        this->button->getClickEvent()->subscribe([frame, this](View* view) {
            if (!frame->isLastStage())
                frame->nextStage();
            else
                brls::Application::pushView(new MainFrame());
        });
    }

    this->label = new brls::Label(brls::LabelStyle::DIALOG, text, true);
    this->label->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->label->setParent(this);
}

ConfirmPage_Done::ConfirmPage_Done(brls::StagedAppletFrame* frame, const std::string& text, bool defaultAction) : ConfirmPage(frame, text, defaultAction)
{
    this->button->setLabel("menus/common/back"_i18n);
    this->done = true;
}

ConfirmPage_AppUpdate::ConfirmPage_AppUpdate(brls::StagedAppletFrame* frame, const std::string& text) : ConfirmPage_Done(frame, text)
{
    this->button->getClickEvent()->subscribe([](View* view) {
        // The update has already been staged under /aio-mod-app-update.
        // Reboot into the embedded TegraExplorer payload so the running NRO
        // can be replaced while Horizon is not active.
        const std::string preflightError = util::appUpdatePreflightError();
        if (!preflightError.empty()) {
            util::showDialogBoxInfo(preflightError);
            return;
        }
        if (!usableFile(APP_PAYLOAD_UPDATE_READY_PATH) ||
            !usableFile(APP_PAYLOAD_UPDATE_STAGED_NRO)) {
            util::showDialogBoxInfo("menus/errors/app_update_stage_missing"_i18n);
            return;
        }
        if (!usableFile(RCM_PAYLOAD_PATH)) {
            util::showDialogBoxInfo("menus/errors/app_update_payload_missing"_i18n);
            return;
        }
        if (util::isErista()) {
            util::rebootToPayload(RCM_PAYLOAD_PATH);
            return;
        }

        // Patched Erista / Mariko / OLED cannot use reboot-to-payload in the
        // same way. Preserve the normal root payload, temporarily install the
        // embedded updater as /payload.bin, then reboot.
        bool bootPayloadSaved = false;
        if (std::filesystem::exists(MARIKO_PAYLOAD_PATH)) {
            bootPayloadSaved = copyFileVerified(MARIKO_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH_TEMP);
        }
        else if (std::filesystem::exists(UPDATE_BIN_PATH)) {
            bootPayloadSaved = copyFileVerified(UPDATE_BIN_PATH, MARIKO_PAYLOAD_PATH_TEMP);
        }
        else if (std::filesystem::exists(REBOOT_PAYLOAD_PATH)) {
            bootPayloadSaved = copyFileVerified(REBOOT_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH_TEMP);
        }

        if (!bootPayloadSaved) {
            util::showDialogBoxInfo("menus/errors/app_update_payload_backup"_i18n);
            return;
        }

        if (!copyFileVerified(RCM_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH)) {
            if (!copyFileVerified(MARIKO_PAYLOAD_PATH_TEMP, MARIKO_PAYLOAD_PATH)) {
                util::showDialogBoxInfo("menus/errors/app_update_payload_restore"_i18n);
                return;
            }
            util::showDialogBoxInfo("menus/errors/app_update_payload_install"_i18n);
            return;
        }

        util::shutDown(true);
    });
    this->registerAction("", brls::Key::B, [this] { return true; });
}

ConfirmPage_AmsUpdate::ConfirmPage_AmsUpdate(brls::StagedAppletFrame* frame, const std::string& text, bool erista) : ConfirmPage_Done(frame, text)
{
    this->button->getClickEvent()->subscribe([this, erista](View* view) {
        if (erista) {
            util::rebootToPayload(RCM_PAYLOAD_PATH);
        }
        else {
            bool bootPayloadSaved = false;
            if (std::filesystem::exists(MARIKO_PAYLOAD_PATH)) {
                bootPayloadSaved = copyFileVerified(MARIKO_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH_TEMP);
            }
            else if (std::filesystem::exists(UPDATE_BIN_PATH)) {
                bootPayloadSaved = copyFileVerified(UPDATE_BIN_PATH, MARIKO_PAYLOAD_PATH_TEMP);
            }
            else if (std::filesystem::exists(REBOOT_PAYLOAD_PATH)) {
                bootPayloadSaved = copyFileVerified(REBOOT_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH_TEMP);
            }

            if (!bootPayloadSaved) {
                util::showDialogBoxInfo("menus/errors/app_update_payload_backup"_i18n);
                return;
            }
            if (!copyFileVerified(RCM_PAYLOAD_PATH, MARIKO_PAYLOAD_PATH)) {
                if (!copyFileVerified(MARIKO_PAYLOAD_PATH_TEMP, MARIKO_PAYLOAD_PATH)) {
                    util::showDialogBoxInfo("menus/errors/app_update_payload_restore"_i18n);
                    return;
                }
                util::showDialogBoxInfo("menus/errors/app_update_payload_install"_i18n);
                return;
            }
            util::shutDown(true);
        }
    });
    this->registerAction("", brls::Key::B, [this] { return true; });
};

void ConfirmPage::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    if (!this->done) {
        auto end = std::chrono::high_resolution_clock::now();
        auto missing = std::max(1l - std::chrono::duration_cast<std::chrono::seconds>(end - start).count(), 0l);
        auto text = std::string("menus/common/continue"_i18n);
        if (missing > 0) {
            this->button->setLabel(fmt::format("{} ({})", text, missing));
            this->button->setState(brls::ButtonState::DISABLED);
        }
        else {
            this->button->setLabel(text);
            this->button->setState(brls::ButtonState::ENABLED);
        }
        this->button->invalidate();
    }
    else {
        this->button->setState(brls::ButtonState::ENABLED);
    }
    this->label->frame(ctx);
    this->button->frame(ctx);
}

brls::View* ConfirmPage::getDefaultFocus()
{
    return this->button;
}

void ConfirmPage::layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash)
{
    this->label->setWidth(this->width);
    this->label->invalidate(true);
    
    this->label->setBoundaries(
        this->x + this->width / 2 - this->label->getWidth() / 2,
        this->y + (this->height - this->label->getHeight() - this->y - style->CrashFrame.buttonHeight) / 2,
        this->label->getWidth(),
        this->label->getHeight());

    this->button->setBoundaries(
        this->x + this->width / 2 - style->CrashFrame.buttonWidth / 2,
        this->y + (this->height - style->CrashFrame.buttonHeight * 3),
        style->CrashFrame.buttonWidth,
        style->CrashFrame.buttonHeight);
    this->button->invalidate();

    start = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(150);
}

ConfirmPage::~ConfirmPage()
{
    delete this->label;
    delete this->button;
}
