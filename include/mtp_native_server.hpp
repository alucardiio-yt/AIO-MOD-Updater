#pragma once

#include <switch.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "installer_diagnostics.hpp"

namespace mtp {

enum class ServerState {
    Stopped,
    Starting,
    WaitingForHost,
    Connected,
    Receiving,
    Preflighting,
    Ready,
    Error,
};

struct CleanupResult {
    uint32_t removedFiles = 0;
    uint32_t failedFiles = 0;
    uint64_t freedBytes = 0;
};

struct ServerStatus {
    ServerState state = ServerState::Stopped;
    bool running = false;
    bool hostConnected = false;
    UsbDeviceSpeed speed = UsbDeviceSpeed_None;
    std::string stateText;
    std::string currentFile;
    std::string lastStagedPath;
    installer::TransferSnapshot transfer;
    installer::Diagnostic lastDiagnostic;
};

class NativeMtpServer {
public:
    static NativeMtpServer& instance();

    NativeMtpServer(const NativeMtpServer&) = delete;
    NativeMtpServer& operator=(const NativeMtpServer&) = delete;

    bool start();
    void stop();
    bool isRunning() const;
    ServerStatus snapshot() const;

    bool deleteLastStaged();
    CleanupResult cleanupTemporaryFiles();
    std::string getInboxDirectory() const;

private:
    NativeMtpServer();
    ~NativeMtpServer();

    void threadMain();
    bool initializeUsb();
    void cleanupUsb();
    void commandLoop();
    bool waitForConfigured();

    bool processOneCommand();
    bool serviceClassRequest();

    bool endpointTransfer(UsbDsEndpoint* endpoint, void* buffer, size_t size,
                          uint64_t timeoutNs, uint32_t& transferred, bool receive);
    bool receiveIntoBuffer(uint64_t timeoutNs, bool transferActive);
    bool readExact(void* out, size_t size, uint64_t timeoutNs, bool transferActive);
    bool drainExact(uint64_t size, bool transferActive);
    bool sendExact(const void* data, size_t size);

    bool sendResponse(uint16_t code, uint32_t transaction,
                      const std::vector<uint32_t>& params = {});
    bool sendData(uint16_t operation, uint32_t transaction,
                  const std::vector<uint8_t>& payload);

    void dispatchCommand(uint16_t operation, uint32_t transaction,
                         const std::vector<uint32_t>& params);

    void handleSendObjectInfo(uint32_t transaction, const std::vector<uint32_t>& params);
    void handleSendObject(uint32_t transaction);
    void handleDeleteObject(uint32_t transaction, const std::vector<uint32_t>& params);

    std::vector<uint8_t> makeDeviceInfo() const;
    std::vector<uint8_t> makeStorageIds() const;
    std::vector<uint8_t> makeStorageInfo() const;
    std::vector<uint8_t> makeObjectHandles() const;
    std::vector<uint8_t> makeObjectInfo() const;
    std::vector<uint8_t> makeObjectPropsSupported() const;
    std::vector<uint8_t> makeObjectPropDesc(uint16_t property, bool& valid) const;
    std::vector<uint8_t> makeObjectPropValue(uint16_t property, bool& valid) const;

    void setState(ServerState state, const std::string& text = {});
    void setDiagnostic(const installer::Diagnostic& diagnostic);
    void resetPendingObject();
    void failTransfer(const installer::Diagnostic& diagnostic, uint16_t mtpResponse,
                      uint32_t transaction, bool deletePartial = true);
    void unlockHome();
    bool currentObjectExists() const;
    std::string currentObjectName() const;
    uint64_t currentObjectSize() const;

    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::thread worker;

    mutable std::mutex statusMutex;
    ServerStatus status;

    UsbDsInterface* interface = nullptr;
    UsbDsEndpoint* epIn = nullptr;
    UsbDsEndpoint* epOut = nullptr;
    UsbDsEndpoint* epEvent = nullptr;
    bool usbInitialized = false;
    bool homeBlocked = false;

    void* rxBuffer = nullptr;
    void* txBuffer = nullptr;
    void* ctrlBuffer = nullptr;
    size_t rxAvailable = 0;
    size_t rxOffset = 0;

    uint32_t sessionId = 0;
    uint32_t objectHandle = 1;

    std::string pendingName;
    std::string pendingPartPath;
    std::string pendingFinalPath;
    uint64_t pendingSize = 0;
    uint16_t pendingFormat = 0;
    bool pendingValid = false;

    std::atomic<bool> transferCancelRequested{false};
    installer::TransferHealthMonitor health;
};

const char* speedName(UsbDeviceSpeed speed);
const char* stateName(ServerState state);

} 
