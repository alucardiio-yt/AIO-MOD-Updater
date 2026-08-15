#pragma once

#include <borealis.hpp>

#include <chrono>

class MtpInstallPage : public brls::List
{
public:
    MtpInstallPage();
    ~MtpInstallPage() override;

    void frame(brls::FrameContext* ctx) override;

private:
    void refreshSystemInfo();
    void refreshMtpStatus(bool force = false);
    void toggleMtpServer();
    void showMtpDiagnostic();
    void deleteStagedPackage();
    void cleanupTemporaryFiles();
    void analyzeLocalPackage();
    void runNcmProbe();
    void analyzeCnmt();
    void installStagedPackage();
    void showLastLog();

    brls::ListItem* systemInfo = nullptr;
    brls::ListItem* serverAction = nullptr;
    brls::ListItem* mtpStatus = nullptr;
    brls::ListItem* transferInfo = nullptr;
    brls::ListItem* stagedPackage = nullptr;

    std::chrono::steady_clock::time_point lastStatusRefresh{};
};
