#include "mtp_install_page.hpp"
#include "nsp_ncm_probe.hpp"
#include "nsp_transaction_installer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "constants.hpp"
#include "installer_diagnostics.hpp"
#include "mtp_native_server.hpp"
#include "utils.hpp"
#include "worker_page.hpp"
#include "confirm_page.hpp"
#include "progress_event.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace {
std::string formatGiB(int64_t bytes)
{
    if (bytes < 0)
        return "?";
    return fmt::format("{:.1f} GB", static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0);
}

std::string formatBytes(uint64_t bytes)
{
    constexpr double KiB = 1024.0;
    constexpr double MiB = KiB * 1024.0;
    constexpr double GiB = MiB * 1024.0;
    if (bytes >= static_cast<uint64_t>(GiB))
        return fmt::format("{:.2f} GB", static_cast<double>(bytes) / GiB);
    if (bytes >= static_cast<uint64_t>(MiB))
        return fmt::format("{:.1f} MB", static_cast<double>(bytes) / MiB);
    if (bytes >= static_cast<uint64_t>(KiB))
        return fmt::format("{:.1f} KB", static_cast<double>(bytes) / KiB);
    return fmt::format("{} B", bytes);
}

std::string formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0)
        return "0 MB/s";
    return fmt::format("{:.1f} MB/s", bytesPerSecond / 1024.0 / 1024.0);
}

std::string metaTypeName(uint8_t type)
{
    switch (type) {
        case NcmContentMetaType_Application: return "Application (Base Game)";
        case NcmContentMetaType_Patch: return "Patch (Update)";
        case NcmContentMetaType_AddOnContent: return "AddOnContent (DLC)";
        default: return fmt::format("0x{:02X}", static_cast<unsigned>(type));
    }
}

std::vector<std::string> wrapReportLine(const std::string& input, size_t maxChars = 72)
{
    std::vector<std::string> out;
    if (input.empty()) {
        out.emplace_back("");
        return out;
    }

    std::string remaining = input;
    while (remaining.size() > maxChars) {
        size_t cut = remaining.rfind(' ', maxChars);
        if (cut == std::string::npos || cut < maxChars / 2)
            cut = maxChars;
        out.push_back(remaining.substr(0, cut));
        size_t next = cut;
        while (next < remaining.size() && remaining[next] == ' ') ++next;
        remaining.erase(0, next);
    }
    if (!remaining.empty()) out.push_back(remaining);
    return out;
}

void openScrollableReport(const std::string& title, const std::string& text)
{
    auto* frame = new brls::AppletFrame(true, true);
    auto* list = new brls::List();
    list->setMargins(18, 24, 24, 24);
    list->setSpacing(2);

    std::stringstream ss(text);
    std::string rawLine;
    while (std::getline(ss, rawLine)) {
        if (rawLine.empty()) {
            list->addView(new brls::ListItemGroupSpacing(false));
            continue;
        }

        for (const auto& line : wrapReportLine(rawLine)) {
            auto* item = new brls::ListItem(line);
            item->setHeight(42);
            item->setTextSize(20);
            item->setDrawTopSeparator(false);
            list->addView(item);
        }
    }

    if (list->getViewsCount() == 0) {
        auto* item = new brls::ListItem("-");
        item->setHeight(42);
        item->setDrawTopSeparator(false);
        list->addView(item);
    }

    frame->setContentView(list);
    brls::PopupFrame::open(title, frame, "", "");
}

const char* stateKey(mtp::ServerState state)
{
    switch (state) {
        case mtp::ServerState::Stopped: return "menus/mtp_installer/state_stopped";
        case mtp::ServerState::Starting: return "menus/mtp_installer/state_starting";
        case mtp::ServerState::WaitingForHost: return "menus/mtp_installer/state_waiting";
        case mtp::ServerState::Connected: return "menus/mtp_installer/state_connected";
        case mtp::ServerState::Receiving: return "menus/mtp_installer/state_receiving";
        case mtp::ServerState::Preflighting: return "menus/mtp_installer/state_validating";
        case mtp::ServerState::Ready: return "menus/mtp_installer/state_ready";
        case mtp::ServerState::Error: return "menus/mtp_installer/state_error";
    }
    return "menus/mtp_installer/state_error";
}
}

MtpInstallPage::MtpInstallPage() : brls::List()
{
    auto* description = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        "menus/mtp_installer/description"_i18n,
        true);
    this->addView(description);

    systemInfo = new brls::ListItem("menus/mtp_installer/system_status"_i18n);
    systemInfo->setHeight(LISTITEM_HEIGHT);
    systemInfo->getClickEvent()->subscribe([this](brls::View*) {
        refreshSystemInfo();
        return true;
    });
    this->addView(systemInfo);

    serverAction = new brls::ListItem("menus/mtp_installer/mtp_server"_i18n);
    serverAction->setHeight(LISTITEM_HEIGHT);
    serverAction->getClickEvent()->subscribe([this](brls::View*) {
        toggleMtpServer();
        return true;
    });
    this->addView(serverAction);

    mtpStatus = new brls::ListItem("menus/mtp_installer/mtp_status"_i18n);
    mtpStatus->setHeight(LISTITEM_HEIGHT);
    mtpStatus->getClickEvent()->subscribe([this](brls::View*) {
        showMtpDiagnostic();
        return true;
    });
    this->addView(mtpStatus);

    transferInfo = new brls::ListItem("menus/mtp_installer/transfer_status"_i18n);
    transferInfo->setHeight(LISTITEM_HEIGHT);
    this->addView(transferInfo);

    stagedPackage = new brls::ListItem("menus/mtp_installer/staged_package"_i18n);
    stagedPackage->setHeight(LISTITEM_HEIGHT);
    stagedPackage->getClickEvent()->subscribe([this](brls::View*) {
        deleteStagedPackage();
        return true;
    });
    this->addView(stagedPackage);

    auto* cleanup = new brls::ListItem("menus/mtp_installer/cleanup_temp"_i18n);
    cleanup->setHeight(LISTITEM_HEIGHT);
    cleanup->getClickEvent()->subscribe([this](brls::View*) {
        cleanupTemporaryFiles();
        return true;
    });
    this->addView(cleanup);

    auto* preflight = new brls::ListItem("menus/mtp_installer/analyze_nsp"_i18n);
    preflight->setHeight(LISTITEM_HEIGHT);
    preflight->getClickEvent()->subscribe([this](brls::View*) {
        analyzeLocalPackage();
        return true;
    });
    this->addView(preflight);

    auto* ncmProbe = new brls::ListItem("menus/mtp_installer/ncm_probe"_i18n);
    ncmProbe->setHeight(LISTITEM_HEIGHT);
    ncmProbe->getClickEvent()->subscribe([this](brls::View*) {
        runNcmProbe();
        return true;
    });
    this->addView(ncmProbe);

    auto* cnmtAnalyze = new brls::ListItem("menus/mtp_installer/cnmt_analyze"_i18n);
    cnmtAnalyze->setHeight(LISTITEM_HEIGHT);
    cnmtAnalyze->getClickEvent()->subscribe([this](brls::View*) {
        analyzeCnmt();
        return true;
    });
    this->addView(cnmtAnalyze);

    auto* install = new brls::ListItem("menus/mtp_installer/install_staged"_i18n);
    install->setHeight(LISTITEM_HEIGHT);
    install->getClickEvent()->subscribe([this](brls::View*) {
        installStagedPackage();
        return true;
    });
    this->addView(install);

    auto* logs = new brls::ListItem("menus/mtp_installer/last_log"_i18n);
    logs->setHeight(LISTITEM_HEIGHT);
    logs->getClickEvent()->subscribe([this](brls::View*) {
        showLastLog();
        return true;
    });
    this->addView(logs);

    auto* transport = new brls::Label(
        brls::LabelStyle::SMALL,
        "menus/mtp_installer/transport_phase"_i18n,
        true);
    this->addView(transport);

    refreshSystemInfo();
    refreshMtpStatus(true);
}

MtpInstallPage::~MtpInstallPage()
{

    mtp::NativeMtpServer::instance().stop();
}

void MtpInstallPage::frame(brls::FrameContext* ctx)
{
    refreshMtpStatus(false);
    brls::List::frame(ctx);
}

void MtpInstallPage::refreshSystemInfo()
{
    const auto s = installer::getSystemSnapshot();
    const std::string info = i18n::getStr(
        "menus/mtp_installer/system_value",
        s.firmware,
        formatGiB(s.sdFreeBytes));
    if (systemInfo)
        systemInfo->setValue(info, false, false);
}

void MtpInstallPage::refreshMtpStatus(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && lastStatusRefresh.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatusRefresh).count() < 400)
        return;
    lastStatusRefresh = now;

    const auto s = mtp::NativeMtpServer::instance().snapshot();

    if (serverAction) {
        serverAction->setValue(
            s.running ? "menus/mtp_installer/stop_mtp"_i18n : "menus/mtp_installer/start_mtp"_i18n,
            false,
            false);
    }

    if (mtpStatus) {
        std::string value = i18n::getStr(stateKey(s.state));
        if (s.hostConnected)
            value += fmt::format(" | {}", mtp::speedName(s.speed));
        if (s.state == mtp::ServerState::Error && !s.lastDiagnostic.aioCode.empty())
            value += fmt::format(" | {}", s.lastDiagnostic.aioCode);
        mtpStatus->setValue(value, s.state == mtp::ServerState::Stopped, false);
    }

    if (transferInfo) {
        if (s.transfer.expectedBytes > 0) {
            const double pct = std::min(100.0,
                100.0 * static_cast<double>(s.transfer.receivedBytes) /
                static_cast<double>(s.transfer.expectedBytes));
            transferInfo->setValue(i18n::getStr(
                "menus/mtp_installer/transfer_value",
                formatBytes(s.transfer.receivedBytes),
                formatBytes(s.transfer.expectedBytes),
                pct,
                formatSpeed(s.transfer.usbBytesPerSecond),
                formatSpeed(s.transfer.storageBytesPerSecond)), false, false);
        } else {
            transferInfo->setValue("menus/mtp_installer/no_transfer"_i18n, true, false);
        }
    }

    if (stagedPackage) {
        if (!s.lastStagedPath.empty())
            stagedPackage->setValue(std::filesystem::path(s.lastStagedPath).filename().string(), false, false);
        else
            stagedPackage->setValue("menus/mtp_installer/no_staged_package"_i18n, true, false);
    }
}

void MtpInstallPage::toggleMtpServer()
{
    auto& server = mtp::NativeMtpServer::instance();
    if (server.isRunning()) {
        server.stop();
    } else if (!server.start()) {
        util::showDialogBoxInfo("menus/mtp_installer/mtp_start_failed"_i18n);
    }
    refreshSystemInfo();
    refreshMtpStatus(true);
}

void MtpInstallPage::showMtpDiagnostic()
{
    const auto s = mtp::NativeMtpServer::instance().snapshot();
    if (s.lastDiagnostic.ok()) {
        util::showDialogBoxInfo(i18n::getStr(
            "menus/mtp_installer/mtp_status_help",
            "AIO MOD",
            "SD Card Install"));
        return;
    }
    util::showDialogBoxInfo(installer::formatDiagnostic(s.lastDiagnostic));
}

void MtpInstallPage::deleteStagedPackage()
{
    const auto before = mtp::NativeMtpServer::instance().snapshot();
    if (before.lastStagedPath.empty()) {
        util::showDialogBoxInfo("menus/mtp_installer/no_staged_package"_i18n);
        return;
    }

    if (mtp::NativeMtpServer::instance().deleteLastStaged())
        util::showDialogBoxInfo("menus/mtp_installer/staged_deleted"_i18n);
    else
        util::showDialogBoxInfo("menus/mtp_installer/staged_delete_failed"_i18n);
    refreshSystemInfo();
    refreshMtpStatus(true);
}

void MtpInstallPage::cleanupTemporaryFiles()
{
    const auto state = mtp::NativeMtpServer::instance().snapshot().state;
    if (state == mtp::ServerState::Receiving) {
        util::showDialogBoxInfo("menus/mtp_installer/cleanup_busy"_i18n);
        return;
    }

    const auto result = mtp::NativeMtpServer::instance().cleanupTemporaryFiles();
    const double mb = static_cast<double>(result.freedBytes) / 1024.0 / 1024.0;
    util::showDialogBoxInfo(i18n::getStr(
        "menus/mtp_installer/cleanup_result",
        result.removedFiles,
        mb,
        result.failedFiles));
    refreshSystemInfo();
    refreshMtpStatus(true);
}

void MtpInstallPage::analyzeLocalPackage()
{
    std::string path;
    const bool accepted = brls::Swkbd::openForText(
        [&path](std::string text) { path = std::move(text); },
        "menus/mtp_installer/nsp_path_title"_i18n,
        "",
        512,
        "/games/game.nsp",
        0,
        "common/ok"_i18n,
        "/games/game.nsp");

    if (!accepted || path.empty())
        return;

    if (path.rfind("sdmc:/", 0) == 0)
        path.erase(0, 5);
    if (path.empty() || path.front() != '/')
        path.insert(path.begin(), '/');

    const auto preflight = installer::preflightPackage(path, true);
    const auto system = installer::getSystemSnapshot();
    installer::writeDiagnosticLog(preflight.diagnostic, system, path, &preflight, nullptr);

    std::string message;
    if (preflight.ok()) {
        message = i18n::getStr(
            "menus/mtp_installer/preflight_ok",
            preflight.fileCount,
            static_cast<double>(preflight.packageSize) / 1024.0 / 1024.0 / 1024.0,
            preflight.diagnostic.aioCode);
    } else {
        std::string key = "menus/mtp_installer/preflight_generic_error";
        switch (preflight.diagnostic.code) {
            case installer::ErrorCode::NotEnoughSpace:
                key = "menus/mtp_installer/preflight_space_error";
                break;
            case installer::ErrorCode::UnsupportedFormat:
                key = "menus/mtp_installer/preflight_format_error";
                break;
            case installer::ErrorCode::InvalidPfs0:
                key = "menus/mtp_installer/preflight_pfs0_error";
                break;
            case installer::ErrorCode::PackageTruncated:
                key = "menus/mtp_installer/preflight_truncated_error";
                break;
            default:
                break;
        }
        message = i18n::getStr(
            key,
            preflight.diagnostic.aioCode,
            preflight.diagnostic.technical);
    }
    util::showDialogBoxInfo(message);
}

void MtpInstallPage::runNcmProbe()
{
    auto& server = mtp::NativeMtpServer::instance();
    const auto snapshot = server.snapshot();
    if (snapshot.state == mtp::ServerState::Receiving) {
        util::showDialogBoxInfo("menus/mtp_installer/ncm_probe_busy"_i18n);
        return;
    }
    if (snapshot.lastStagedPath.empty() || !std::filesystem::exists(snapshot.lastStagedPath)) {
        util::showDialogBoxInfo("menus/mtp_installer/ncm_probe_no_package"_i18n);
        return;
    }

    const auto manifest = installer::inspectNspManifest(snapshot.lastStagedPath);
    if (!manifest.ok()) {
        util::showDialogBoxInfo(installer::formatDiagnostic(manifest.diagnostic));
        return;
    }

    openScrollableReport(
        "menus/mtp_installer/ncm_probe"_i18n,
        i18n::getStr(
            "menus/mtp_installer/ncm_probe_manifest",
            manifest.ncaCount,
            manifest.cnmtNcaCount,
            manifest.ticketCount,
            manifest.certCount,
            static_cast<double>(manifest.totalNcaBytes) / 1024.0 / 1024.0 / 1024.0));

    const auto result = installer::runSafeNcmProbe(snapshot.lastStagedPath);
    const auto system = installer::getSystemSnapshot();
    installer::writeDiagnosticLog(result.diagnostic, system, snapshot.lastStagedPath, nullptr, nullptr);

    if (result.ok()) {
        openScrollableReport(
            "menus/mtp_installer/ncm_probe"_i18n,
            i18n::getStr(
                "menus/mtp_installer/ncm_probe_ok",
                result.testedContentName,
                static_cast<double>(result.testedBytes) / 1024.0 / 1024.0,
                result.diagnostic.aioCode));
    } else {
        openScrollableReport(
            "menus/mtp_installer/ncm_probe"_i18n,
            i18n::getStr(
                "menus/mtp_installer/ncm_probe_failed",
                result.diagnostic.aioCode,
                result.diagnostic.technical));
    }
    refreshSystemInfo();
    refreshMtpStatus(true);
}

void MtpInstallPage::analyzeCnmt()
{
    auto& server = mtp::NativeMtpServer::instance();
    const auto snapshot = server.snapshot();
    if (snapshot.state == mtp::ServerState::Receiving) {
        util::showDialogBoxInfo("menus/mtp_installer/cnmt_busy"_i18n);
        return;
    }
    if (snapshot.lastStagedPath.empty() || !std::filesystem::exists(snapshot.lastStagedPath)) {
        util::showDialogBoxInfo("menus/mtp_installer/cnmt_no_package"_i18n);
        return;
    }

    const auto result = installer::analyzeCnmtRequirements(snapshot.lastStagedPath);
    const auto system = installer::getSystemSnapshot();
    installer::writeDiagnosticLog(result.diagnostic, system, snapshot.lastStagedPath, nullptr, nullptr);

    if (result.ok()) {
        openScrollableReport(
            "menus/mtp_installer/cnmt_analyze"_i18n,
            i18n::getStr(
                "menus/mtp_installer/cnmt_ok",
                installer::formatTitleId(result.titleId),
                result.titleVersion,
                static_cast<unsigned>(result.metaType),
                result.matchedContentCount,
                static_cast<double>(result.expectedInstallBytes) / 1024.0 / 1024.0 / 1024.0,
                installer::formatPackedSystemVersion(result.requiredSystemVersion),
                result.diagnostic.aioCode));
    } else {
        std::string details = result.diagnostic.technical;
        if (!result.missingContentIds.empty()) {
            details += "\nMissing: " + result.missingContentIds.front();
            if (result.missingContentIds.size() > 1)
                details += " +" + std::to_string(result.missingContentIds.size() - 1);
        }
        if (!result.sizeMismatchContentIds.empty()) {
            details += "\nSize mismatch: " + result.sizeMismatchContentIds.front();
            if (result.sizeMismatchContentIds.size() > 1)
                details += " +" + std::to_string(result.sizeMismatchContentIds.size() - 1);
        }
        openScrollableReport(
            "menus/mtp_installer/cnmt_analyze"_i18n,
            i18n::getStr(
                "menus/mtp_installer/cnmt_failed",
                result.diagnostic.title,
                result.diagnostic.message,
                result.diagnostic.advice,
                result.diagnostic.aioCode,
                details));
    }
    refreshSystemInfo();
    refreshMtpStatus(true);
}

void MtpInstallPage::installStagedPackage()
{
    auto& server = mtp::NativeMtpServer::instance();
    const auto snapshot = server.snapshot();
    if (snapshot.state == mtp::ServerState::Receiving) {
        util::showDialogBoxInfo("menus/mtp_installer/install_busy"_i18n);
        return;
    }
    if (snapshot.lastStagedPath.empty() || !std::filesystem::exists(snapshot.lastStagedPath)) {
        util::showDialogBoxInfo("menus/mtp_installer/cnmt_no_package"_i18n);
        return;
    }

    const auto analysis = installer::analyzeCnmtRequirements(snapshot.lastStagedPath);
    if (!analysis.ok()) {
        openScrollableReport("menus/mtp_installer/install_precheck_failed"_i18n,
                             installer::formatDiagnostic(analysis.diagnostic));
        return;
    }

    const auto system = installer::getSystemSnapshot();
    const std::string packageName = std::filesystem::path(snapshot.lastStagedPath).filename().string();
    std::ostringstream report;
    report << "Package: " << packageName << "\n\n";
    report << "Title ID: " << installer::formatTitleId(analysis.titleId) << "\n";
    report << "Version: " << analysis.titleVersion << "\n";
    report << "Type: " << metaTypeName(analysis.metaType) << "\n";
    report << "Destination: SD Card\n\n";
    report << "Install size: " << formatBytes(analysis.expectedInstallBytes) << "\n";
    report << "Current firmware: " << system.firmware << "\n";
    report << "Required firmware: " << installer::formatPackedSystemVersion(analysis.requiredSystemVersion) << "\n";
    report << "Contents: " << analysis.matchedContentCount << " / " << analysis.expectedContentCount << "\n\n";
    report << "Package status: OK\nFirmware status: Compatible\nSpace status: Sufficient\n\n";
    report << "Transactional installer: Base Game + Update + DLC.\n";
    report << "Existing shared ContentIds are reused and never deleted by rollback.\n";
    report << "Same/newer versions and base-game replacement are refused.";

    auto* frame = new brls::AppletFrame(true, true);
    auto* list = new brls::List();
    list->setMargins(18, 24, 24, 24);
    list->setSpacing(2);
    std::stringstream lines(report.str());
    std::string raw;
    while (std::getline(lines, raw)) {
        if (raw.empty()) { list->addView(new brls::ListItemGroupSpacing(false)); continue; }
        for (const auto& line : wrapReportLine(raw)) {
            auto* row = new brls::ListItem(line);
            row->setHeight(42); row->setTextSize(20); row->setDrawTopSeparator(false); list->addView(row);
        }
    }
    list->addView(new brls::ListItemGroupSpacing(true));
    auto* installButton = new brls::ListItem("menus/mtp_installer/install_confirm_button"_i18n);
    installButton->setHeight(LISTITEM_HEIGHT);
    const std::string nspPath = snapshot.lastStagedPath;
    installButton->getClickEvent()->subscribe([nspPath](brls::View*) {
        brls::Application::popView(brls::ViewAnimation::FADE, [nspPath]() {
            auto* staged = new brls::StagedAppletFrame();
            staged->setTitle("menus/mtp_installer/install_title"_i18n);
            staged->addStage(new WorkerPage(staged, "menus/mtp_installer/installing_real"_i18n, [nspPath]() {
                auto& p = ProgressEvent::instance();
                p.setOperation(ProgressOperation::INSTALL);
                p.setTotalSteps(100);
                const auto result = installer::installNspToSdTransactional(nspPath,
                    [&p](uint64_t current, uint64_t total, const std::string& stage, uint32_t idx, uint32_t count) {
                        p.setInstallProgress(static_cast<double>(current), static_cast<double>(total), stage,
                                             static_cast<int>(idx), static_cast<int>(count));
                    });
                const auto system = installer::getSystemSnapshot();
                installer::writeDiagnosticLog(result.diagnostic, system, nspPath, nullptr, nullptr);
                if (!result.ok()) {
                    p.setErrorMessage(installer::formatDiagnostic(result.diagnostic));
                } else {
                    p.setCurrentFile("Installation complete");
                    p.setNow(p.getTotal());
                    
                    mtp::NativeMtpServer::instance().deleteLastStaged();
                }
            }));
            staged->addStage(new ConfirmPage_Done(staged, "menus/mtp_installer/install_done"_i18n));
            brls::Application::pushView(staged);
        });
        return true;
    });
    list->addView(installButton);
    frame->setContentView(list);
    brls::PopupFrame::open("menus/mtp_installer/install_title"_i18n, frame, "", "");
}

void MtpInstallPage::showLastLog()
{
    const std::string path = installer::getLastLogPath();
    if (!std::filesystem::exists(path)) {
        util::showDialogBoxInfo("menus/mtp_installer/no_log"_i18n);
        return;
    }

    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    openScrollableReport("menus/mtp_installer/last_log"_i18n, text);
}
