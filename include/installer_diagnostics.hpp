#pragma once

#include <switch.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace installer {

enum class ErrorCode {
    None,
    NotEnoughSpace,
    UnsupportedFormat,
    InvalidPfs0,
    PackageTruncated,
    MissingContent,
    InvalidNca,
    FirmwareTooOld,
    AuthorizationRejected,
    UsbDisconnected,
    UsbTimeout,
    UsbUnstable,
    UsbSlow,
    StorageSlow,
    WriteError,
    PlaceholderError,
    RegistrationError,
    AlreadyInstalled,
    MtpObjectTooLarge,
    Cancelled,
    InternalError,
};

struct Diagnostic {
    ErrorCode code = ErrorCode::None;
    std::string aioCode;
    std::string title;
    std::string message;
    std::string advice;
    std::string technical;
    Result result = 0;

    bool ok() const { return code == ErrorCode::None; }
};

struct PackagePreflight {
    Diagnostic diagnostic;
    uint64_t packageSize = 0;
    uint64_t declaredPayloadSize = 0;
    uint32_t fileCount = 0;
    std::string packageType;

    bool ok() const { return diagnostic.ok(); }
};

struct SystemSnapshot {
    std::string firmware;
    int64_t sdFreeBytes = -1;
};

struct TransferSnapshot {
    uint64_t receivedBytes = 0;
    uint64_t writtenBytes = 0;
    uint64_t expectedBytes = 0;
    double usbBytesPerSecond = 0.0;
    double storageBytesPerSecond = 0.0;
    uint32_t usbStalls = 0;
    uint32_t usbErrors = 0;
};

class TransferHealthMonitor {
public:
    void reset(uint64_t expectedBytes = 0);
    void addReceived(uint64_t bytes);
    void addWritten(uint64_t bytes);
    void addUsbStall();
    void addUsbError();
    void markDisconnected();
    TransferSnapshot snapshot() const;
    Diagnostic evaluate(bool transferFinished) const;

private:
    std::atomic<uint64_t> receivedBytes{0};
    std::atomic<uint64_t> writtenBytes{0};
    std::atomic<uint64_t> expectedBytes{0};
    std::atomic<uint32_t> usbStalls{0};
    std::atomic<uint32_t> usbErrors{0};
    std::atomic<uint64_t> startedNs{0};
    std::atomic<uint64_t> lastReceivedNs{0};
    std::atomic<bool> disconnected{false};
};

SystemSnapshot getSystemSnapshot();
PackagePreflight preflightPackage(const std::string& path, bool checkSdSpace = true);
Diagnostic makeDiagnostic(ErrorCode code, const std::string& technical = {}, Result result = 0);
std::string formatDiagnostic(const Diagnostic& diagnostic);
std::string getLogDirectory();
std::string writeDiagnosticLog(const Diagnostic& diagnostic,
                               const SystemSnapshot& system,
                               const std::string& packagePath = {},
                               const PackagePreflight* preflight = nullptr,
                               const TransferSnapshot* transfer = nullptr);
std::string getLastLogPath();

} 
