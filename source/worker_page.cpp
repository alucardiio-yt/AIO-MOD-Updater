#include "worker_page.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <string>

#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "main_frame.hpp"
#include "progress_event.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
    std::string formatBytes(double bytes)
    {
        constexpr double KiB = 1024.0;
        constexpr double MiB = KiB * 1024.0;
        constexpr double GiB = MiB * 1024.0;

        if (bytes >= GiB)
            return fmt::format("{:.2f} GB", bytes / GiB);
        if (bytes >= MiB)
            return fmt::format("{:.1f} MB", bytes / MiB);
        if (bytes >= KiB)
            return fmt::format("{:.1f} KB", bytes / KiB);
        return fmt::format("{:.0f} B", bytes);
    }

    int percentage(double current, double total)
    {
        if (total <= 0.0)
            return 0;

        return std::clamp(static_cast<int>(std::round((current / total) * 100.0)), 0, 100);
    }

    std::string shortenPath(const std::string& path, size_t maxLength = 92)
    {
        if (path.length() <= maxLength)
            return path;

        if (maxLength <= 3)
            return path.substr(path.length() - maxLength);

        return "..." + path.substr(path.length() - (maxLength - 3));
    }

    std::string formatEta(double speed, double current, double total)
    {
        if (speed <= 0.0 || total <= current)
            return "";

        double timeRemaining = (total - current) / speed;
        int hours = static_cast<int>(timeRemaining / 3600.0);
        int minutes = static_cast<int>((timeRemaining - hours * 3600.0) / 60.0);
        int seconds = static_cast<int>(timeRemaining - hours * 3600.0 - minutes * 60.0);

        if (hours > 0)
            return fmt::format("{}h {:02d}m {:02d}s", hours, minutes, seconds);
        if (minutes > 0)
            return fmt::format("{}m {:02d}s", minutes, seconds);
        return fmt::format("{}s", seconds);
    }

    std::string buildConsoleText()
    {
        auto& progress = ProgressEvent::instance();
        const ProgressOperation operation = progress.getOperation();
        const std::string currentFile = shortenPath(progress.getCurrentFile());
        const double now = progress.getNow();
        const double total = progress.getTotal();

        if (operation == ProgressOperation::DOWNLOAD) {
            std::string output = currentFile.empty() ? "> ..." : fmt::format("> {}", currentFile);

            if (total > 0.0) {
                output += fmt::format("\n  {} / {}   [{}%]", formatBytes(now), formatBytes(total), percentage(now, total));
            }

            const double speed = progress.getSpeed();
            if (speed > 0.0) {
                const std::string eta = formatEta(speed, now, total);
                output += fmt::format("\n  {}/s", formatBytes(speed));
                if (!eta.empty())
                    output += fmt::format("   ETA {}", eta);
            }

            return output;
        }

        if (operation == ProgressOperation::EXTRACT) {
            const double fileNow = progress.getFileNow();
            const double fileTotal = progress.getFileTotal();
            std::string output = currentFile.empty() ? "> ..." : fmt::format("> {}", currentFile);

            if (fileTotal > 0.0) {
                output += fmt::format("\n  {} / {}   [{}%]", formatBytes(fileNow), formatBytes(fileTotal), percentage(fileNow, fileTotal));
            }

            if (total > 0.0) {
                output += fmt::format("\n  TOTAL {} / {}   [{}%]", formatBytes(now), formatBytes(total), percentage(now, total));
            }

            return output;
        }

        if (operation == ProgressOperation::INSTALL) {
            const int contentIndex = static_cast<int>(progress.getFileNow());
            const int contentCount = static_cast<int>(progress.getFileTotal());
            std::string output = currentFile.empty() ? "> Preparando instalación..." : fmt::format("> {}", currentFile);
            if (total > 0.0)
                output += fmt::format("\n  {} / {}   [{}%]", formatBytes(now), formatBytes(total), percentage(now, total));
            if (contentCount > 0)
                output += fmt::format("\n  CONTENIDO {} / {}", std::clamp(contentIndex, 0, contentCount), contentCount);
            return output;
        }

        if (operation == ProgressOperation::FORWARDER) {
            const int current = static_cast<int>(now);
            const int steps = static_cast<int>(total);
            std::string output = currentFile.empty() ? "> Preparando forwarder..." : fmt::format("> {}", currentFile);
            if (steps > 0) {
                const int shown = std::clamp(current, 0, steps);
                output += fmt::format("\n  PASO {} / {}   [{}%]", shown, steps, percentage(shown, steps));
            }
            return output;
        }

        return "";
    }
}  

WorkerPage::WorkerPage(brls::StagedAppletFrame* frame, const std::string& text, worker_func_t worker_func) : frame(frame), workerFunc(worker_func), text(text)
{
    this->progressDisp = new brls::ProgressDisplay();
    this->progressDisp->setParent(this);

    this->label = new brls::Label(brls::LabelStyle::DIALOG, text, true);
    this->label->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->label->setParent(this);

    this->detailLabel = new brls::Label(brls::LabelStyle::SMALL, "", true);
    this->detailLabel->setHorizontalAlign(NVG_ALIGN_LEFT);
    this->detailLabel->setVerticalAlign(NVG_ALIGN_MIDDLE);
    this->detailLabel->setParent(this);

    this->button = new brls::Button(brls::ButtonStyle::REGULAR);
    this->button->setParent(this);

    this->registerAction("menus/common/cancel"_i18n, brls::Key::B, [this] {
        ProgressEvent::instance().setInterupt(true);
        return true;
    });
    this->registerAction("", brls::Key::A, [this] { return true; });
    this->registerAction("", brls::Key::PLUS, [this] { return true; });
}

void WorkerPage::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    if (this->draw_page) {
        if (!this->workStarted) {
            this->workStarted = true;
            appletSetMediaPlaybackState(true);
            appletBeginBlockingHomeButton(0);
            this->systemLocksActive = true;
            ProgressEvent::instance().reset();
            workerThread = new std::thread(&WorkerPage::doWork, this);
        }
        else if (ProgressEvent::instance().finished()) {

            this->releaseSystemLocks();

            const std::string workerError = ProgressEvent::instance().getErrorMessage();
            if (!workerError.empty()) {
                this->returnToMenuWithError(workerError);
                return;
            }

            const long statusCode = static_cast<long>(ProgressEvent::instance().getStatusCode());
            if (statusCode > 399) {
                this->returnToMenuWithError(
                    fmt::format("menus/errors/error_message"_i18n, util::getErrorMessage(statusCode)));
                return;
            }

            if (ProgressEvent::instance().getInterupt()) {
                this->draw_page = false;
                brls::Application::popView();
            }
            else {
                ProgressEvent::instance().setStatusCode(0);
                frame->nextStage();
            }
        }
        else {
            this->progressDisp->setProgress(ProgressEvent::instance().getStep(), ProgressEvent::instance().getMax());
            this->progressDisp->frame(ctx);

            this->label->setText(this->text);
            this->label->frame(ctx);

            const std::string detailText = buildConsoleText();
            if (!detailText.empty()) {
                this->detailLabel->setText(detailText);

                const float boxX = static_cast<float>(this->detailLabel->getX() - 18);
                const float boxY = static_cast<float>(this->detailLabel->getY() - 10);
                const float boxWidth = static_cast<float>(this->detailLabel->getWidth() + 36);
                const float boxHeight = static_cast<float>(this->detailLabel->getHeight() + 20);

                nvgBeginPath(vg);
                nvgFillColor(vg, a(ctx->theme->buttonRegularBackgroundColor));
                nvgRoundedRect(vg, boxX, boxY, boxWidth, boxHeight, 6.0f);
                nvgFill(vg);

                nvgBeginPath(vg);
                nvgStrokeColor(vg, a(ctx->theme->separatorColor));
                nvgStrokeWidth(vg, 1.0f);
                nvgRoundedRect(vg, boxX, boxY, boxWidth, boxHeight, 6.0f);
                nvgStroke(vg);

                this->detailLabel->frame(ctx);
            }
        }
    }
}

void WorkerPage::layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash)
{
    this->label->setWidth(roundf((float)this->width * style->CrashFrame.labelWidth));

    this->label->setBoundaries(
        this->x + this->width / 2 - this->label->getWidth() / 2,
        this->y + 105,
        this->label->getWidth(),
        70);

    this->progressDisp->setBoundaries(
        this->x + this->width / 2 - style->CrashFrame.buttonWidth,
        this->y + 190,
        style->CrashFrame.buttonWidth * 2,
        style->CrashFrame.buttonHeight);

    const unsigned consoleWidth = static_cast<unsigned>(roundf((float)this->width * 0.76f));
    this->detailLabel->setBoundaries(
        this->x + this->width / 2 - consoleWidth / 2,
        this->y + this->height - 172,
        consoleWidth,
        125);
}

void WorkerPage::doWork()
{
    try {
        if (this->workerFunc)
            this->workerFunc();
    }
    catch (const std::exception& exception) {
        ProgressEvent::instance().setErrorMessage(exception.what());
    }
    catch (...) {
        ProgressEvent::instance().setErrorMessage("Error inesperado durante la operación.");
    }

    
    ProgressEvent::instance().setStep(ProgressEvent::instance().getMax());
}

void WorkerPage::releaseSystemLocks()
{
    if (!this->systemLocksActive)
        return;

    appletEndBlockingHomeButton();
    appletSetMediaPlaybackState(false);
    this->systemLocksActive = false;
}

void WorkerPage::returnToMenuWithError(const std::string& message)
{
    this->draw_page = false;
    ProgressEvent::instance().setStatusCode(0);

    brls::Application::popView(brls::ViewAnimation::FADE, [message]() {
        util::showDialogBoxInfo(message);
    });
}

brls::View* WorkerPage::getDefaultFocus()
{
    return this->button;
}

WorkerPage::~WorkerPage()
{
    this->releaseSystemLocks();

    if (this->workStarted && this->workerThread && this->workerThread->joinable())
        this->workerThread->join();

    if (this->workerThread) {
        delete this->workerThread;
        this->workerThread = nullptr;
    }
    delete this->progressDisp;
    delete this->label;
    delete this->detailLabel;
    delete this->button;
}
