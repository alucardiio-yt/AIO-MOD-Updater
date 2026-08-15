#include "mtp_native_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <malloc.h>
#include <system_error>

namespace mtp {
namespace {

constexpr const char INBOX_DIR[] = "/switch/aio-switch-updater-mod/config/mtp_inbox";
constexpr uint32_t STORAGE_ID = 0x00010001;
constexpr uint32_t ROOT_PARENT = 0x00000000;
constexpr uint32_t OBJECT_HANDLE = 1;
constexpr uint64_t SPACE_RESERVE = 64ULL * 1024ULL * 1024ULL;
constexpr size_t IO_BUFFER_SIZE = 256 * 1024;
constexpr size_t CTRL_BUFFER_SIZE = 0x1000;
constexpr uint64_t POLL_NS = 250ULL * 1000ULL * 1000ULL;
constexpr uint64_t SEND_TIMEOUT_NS = 10ULL * 1000ULL * 1000ULL * 1000ULL;

constexpr uint16_t CONTAINER_COMMAND = 1;
constexpr uint16_t CONTAINER_DATA = 2;
constexpr uint16_t CONTAINER_RESPONSE = 3;
constexpr uint16_t CONTAINER_EVENT = 4;

constexpr uint16_t OP_GET_DEVICE_INFO = 0x1001;
constexpr uint16_t OP_OPEN_SESSION = 0x1002;
constexpr uint16_t OP_CLOSE_SESSION = 0x1003;
constexpr uint16_t OP_GET_STORAGE_IDS = 0x1004;
constexpr uint16_t OP_GET_STORAGE_INFO = 0x1005;
constexpr uint16_t OP_GET_NUM_OBJECTS = 0x1006;
constexpr uint16_t OP_GET_OBJECT_HANDLES = 0x1007;
constexpr uint16_t OP_GET_OBJECT_INFO = 0x1008;
constexpr uint16_t OP_DELETE_OBJECT = 0x100B;
constexpr uint16_t OP_SEND_OBJECT_INFO = 0x100C;
constexpr uint16_t OP_SEND_OBJECT = 0x100D;
constexpr uint16_t OP_GET_OBJECT_PROPS_SUPPORTED = 0x9801;
constexpr uint16_t OP_GET_OBJECT_PROP_DESC = 0x9802;
constexpr uint16_t OP_GET_OBJECT_PROP_VALUE = 0x9803;

constexpr uint16_t RES_OK = 0x2001;
constexpr uint16_t RES_GENERAL_ERROR = 0x2002;
constexpr uint16_t RES_SESSION_NOT_OPEN = 0x2003;
constexpr uint16_t RES_OPERATION_NOT_SUPPORTED = 0x2005;
constexpr uint16_t RES_INCOMPLETE_TRANSFER = 0x2007;
constexpr uint16_t RES_INVALID_STORAGE_ID = 0x2008;
constexpr uint16_t RES_INVALID_OBJECT_HANDLE = 0x2009;
constexpr uint16_t RES_INVALID_OBJECT_FORMAT = 0x200B;
constexpr uint16_t RES_STORE_FULL = 0x200C;
constexpr uint16_t RES_INVALID_OBJECT_INFO = 0x2015;
constexpr uint16_t RES_INVALID_PARENT = 0x201A;
constexpr uint16_t RES_INVALID_PARAMETER = 0x201D;
constexpr uint16_t RES_SESSION_ALREADY_OPEN = 0x201E;
constexpr uint16_t RES_INVALID_OBJECT_PROP = 0xA801;

constexpr uint16_t FORMAT_UNDEFINED = 0x3000;
constexpr uint16_t FORMAT_ASSOCIATION = 0x3001;

constexpr uint16_t PROP_STORAGE_ID = 0xDC01;
constexpr uint16_t PROP_OBJECT_FORMAT = 0xDC02;
constexpr uint16_t PROP_OBJECT_SIZE = 0xDC04;
constexpr uint16_t PROP_PARENT = 0xDC0B;
constexpr uint16_t PROP_PERSISTENT_ID = 0xDC41;
constexpr uint16_t PROP_NAME = 0xDC44;

constexpr uint16_t TYPE_UINT16 = 0x0004;
constexpr uint16_t TYPE_UINT32 = 0x0006;
constexpr uint16_t TYPE_UINT64 = 0x0008;
constexpr uint16_t TYPE_UINT128 = 0x000A;
constexpr uint16_t TYPE_STRING = 0xFFFF;

constexpr uint8_t MTP_REQ_CANCEL = 0x64;
constexpr uint8_t MTP_REQ_RESET = 0x66;
constexpr uint8_t MTP_REQ_GET_DEVICE_STATUS = 0x67;

#pragma pack(push, 1)
struct ContainerHeader {
    uint32_t length;
    uint16_t type;
    uint16_t code;
    uint32_t transaction;
};
#pragma pack(pop)
static_assert(sizeof(ContainerHeader) == 12);

uint16_t read16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void add8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void add16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
void add32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void add64(std::vector<uint8_t>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void add128(std::vector<uint8_t>& out, uint64_t lo, uint64_t hi)
{
    add64(out, lo);
    add64(out, hi);
}

void addString(std::vector<uint8_t>& out, const std::string& value)
{
    if (value.empty()) {
        add8(out, 0);
        return;
    }

    
    std::vector<uint16_t> chars;
    for (size_t i = 0; i < value.size() && chars.size() < 253;) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        uint32_t cp = '?';
        size_t used = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < value.size()) {
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(value[i + 1]) & 0x3F);
            used = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < value.size()) {
            cp = ((c & 0x0F) << 12) |
                 ((static_cast<uint8_t>(value[i + 1]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(value[i + 2]) & 0x3F);
            used = 3;
        }
        i += used;
        if (cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF))
            chars.push_back(static_cast<uint16_t>(cp));
        else
            chars.push_back('?');
    }

    add8(out, static_cast<uint8_t>(chars.size() + 1));
    for (uint16_t ch : chars)
        add16(out, ch);
    add16(out, 0);
}

template <typename T>
void addArray(std::vector<uint8_t>& out, const std::vector<T>& values);

template <>
void addArray<uint16_t>(std::vector<uint8_t>& out, const std::vector<uint16_t>& values)
{
    add32(out, static_cast<uint32_t>(values.size()));
    for (uint16_t v : values)
        add16(out, v);
}

template <>
void addArray<uint32_t>(std::vector<uint8_t>& out, const std::vector<uint32_t>& values)
{
    add32(out, static_cast<uint32_t>(values.size()));
    for (uint32_t v : values)
        add32(out, v);
}

std::string utf16Name(const uint8_t* p, size_t remaining, bool& ok)
{
    ok = false;
    if (remaining < 1)
        return {};
    const uint8_t count = p[0];
    if (count == 0) {
        ok = true;
        return {};
    }
    const size_t need = 1 + static_cast<size_t>(count) * 2;
    if (need > remaining)
        return {};

    std::string out;
    for (size_t i = 0; i + 1 < count; ++i) {
        const uint16_t ch = read16(p + 1 + i * 2);
        if (ch == 0)
            break;
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    ok = true;
    return out;
}

bool isSafeNspName(const std::string& name)
{
    if (name.empty() || name.size() > 220)
        return false;
    if (name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
        return false;
    for (unsigned char c : name) {
        if (c < 0x20)
            return false;
    }
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.size() >= 4 && lower.substr(lower.size() - 4) == ".nsp";
}

uint64_t fileSize(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

std::string baseName(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

} 

const char* speedName(UsbDeviceSpeed speed)
{
    switch (speed) {
        case UsbDeviceSpeed_Low: return "USB 1.x Low";
        case UsbDeviceSpeed_Full: return "USB 1.1 Full";
        case UsbDeviceSpeed_High: return "USB 2.0 High";
        case UsbDeviceSpeed_Super: return "USB 3.x Super";
        default: return "N/A";
    }
}

const char* stateName(ServerState state)
{
    switch (state) {
        case ServerState::Stopped: return "Stopped";
        case ServerState::Starting: return "Starting";
        case ServerState::WaitingForHost: return "Waiting for PC";
        case ServerState::Connected: return "PC connected";
        case ServerState::Receiving: return "Receiving NSP";
        case ServerState::Preflighting: return "Validating NSP";
        case ServerState::Ready: return "NSP ready";
        case ServerState::Error: return "Error";
    }
    return "Unknown";
}

NativeMtpServer& NativeMtpServer::instance()
{
    static NativeMtpServer server;
    return server;
}

NativeMtpServer::NativeMtpServer()
{
    status.state = ServerState::Stopped;
    status.stateText = stateName(ServerState::Stopped);
    status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
}

NativeMtpServer::~NativeMtpServer() { stop(); }

std::string NativeMtpServer::getInboxDirectory() const { return INBOX_DIR; }

bool NativeMtpServer::isRunning() const { return running.load(std::memory_order_acquire); }

void NativeMtpServer::setState(ServerState newState, const std::string& text)
{
    std::scoped_lock lock(statusMutex);
    status.state = newState;
    status.stateText = text.empty() ? stateName(newState) : text;
    status.running = running.load(std::memory_order_relaxed);
}

void NativeMtpServer::setDiagnostic(const installer::Diagnostic& diagnostic)
{
    std::scoped_lock lock(statusMutex);
    status.lastDiagnostic = diagnostic;
}

ServerStatus NativeMtpServer::snapshot() const
{
    std::scoped_lock lock(statusMutex);
    ServerStatus copy = status;
    copy.running = running.load(std::memory_order_relaxed);
    if (copy.state == ServerState::Receiving)
        copy.transfer = health.snapshot();
    return copy;
}

bool NativeMtpServer::start()
{
    if (running.exchange(true, std::memory_order_acq_rel))
        return true;

    stopRequested.store(false, std::memory_order_release);
    transferCancelRequested.store(false, std::memory_order_release);
    setState(ServerState::Starting);
    {
        std::scoped_lock lock(statusMutex);
        status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
        status.speed = UsbDeviceSpeed_None;
        status.hostConnected = false;
        status.transfer = {};
    }

    
    cleanupTemporaryFiles();

    try {
        worker = std::thread(&NativeMtpServer::threadMain, this);
    } catch (...) {
        running.store(false, std::memory_order_release);
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::InternalError,
            "No se pudo crear el hilo del servidor MTP"));
        setState(ServerState::Error);
        return false;
    }
    return true;
}

void NativeMtpServer::stop()
{
    if (!running.load(std::memory_order_acquire) && !worker.joinable())
        return;

    stopRequested.store(true, std::memory_order_release);
    transferCancelRequested.store(true, std::memory_order_release);
    if (epOut)
        usbDsEndpoint_Cancel(epOut);
    if (epIn)
        usbDsEndpoint_Cancel(epIn);

    if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
        worker.join();

    running.store(false, std::memory_order_release);
    setState(ServerState::Stopped);
}

void NativeMtpServer::unlockHome()
{
    if (homeBlocked) {
        appletEndBlockingHomeButton();
        appletSetMediaPlaybackState(false);
        homeBlocked = false;
    }
}

bool NativeMtpServer::initializeUsb()
{
    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) {
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::InternalError,
            "usbDsInitialize falló", rc));
        return false;
    }
    usbInitialized = true;

    auto fail = [this](const char* where, Result result) {
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::InternalError, where, result));
        return false;
    };

    if (R_FAILED(rc = usbDsClearDeviceData()))
        return fail("usbDsClearDeviceData falló", rc);

    const u16 lang = 0x0409;
    u8 langIndex = 0, manufacturerIndex = 0, productIndex = 0, serialIndex = 0, interfaceIndex = 0;
    if (R_FAILED(rc = usbDsAddUsbLanguageStringDescriptor(&langIndex, &lang, 1)))
        return fail("No se pudo registrar el idioma USB", rc);
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&manufacturerIndex, "Alucardio")))
        return fail("No se pudo registrar fabricante USB", rc);
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&productIndex, "AIO MOD")))
        return fail("No se pudo registrar producto USB", rc);
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&serialIndex, "AIO-MOD-" APP_VERSION)))
        return fail("No se pudo registrar serial USB", rc);
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&interfaceIndex, "MTP")))
        return fail("No se pudo registrar interfaz MTP", rc);

    usb_device_descriptor dev{};
    dev.bLength = USB_DT_DEVICE_SIZE;
    dev.bDescriptorType = USB_DT_DEVICE;
    dev.bcdUSB = 0x0200;
    dev.bDeviceClass = USB_CLASS_PER_INTERFACE;
    dev.bDeviceSubClass = 0;
    dev.bDeviceProtocol = 0;
    dev.bMaxPacketSize0 = 0x40;

    dev.idVendor = 0x057E;
    dev.idProduct = 0x4010;
    dev.bcdDevice = 0x0300;
    dev.iManufacturer = manufacturerIndex;
    dev.iProduct = productIndex;
    dev.iSerialNumber = serialIndex;
    dev.bNumConfigurations = 1;

    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev)))
        return fail("Descriptor USB Full Speed rechazado", rc);
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev)))
        return fail("Descriptor USB High Speed rechazado", rc);

    if (R_FAILED(rc = usbDsRegisterInterface(&interface)))
        return fail("No se pudo registrar la interfaz MTP", rc);

    const u8 bulkNumber = static_cast<u8>((interface->interface_index + 1) & 0x0F);
    const u8 eventNumber = static_cast<u8>((interface->interface_index + 2) & 0x0F);
    const u8 bulkInAddress = static_cast<u8>(USB_ENDPOINT_IN | bulkNumber);
    const u8 bulkOutAddress = static_cast<u8>(USB_ENDPOINT_OUT | bulkNumber);
    const u8 eventInAddress = static_cast<u8>(USB_ENDPOINT_IN | eventNumber);

    usb_interface_descriptor intf{};
    intf.bLength = USB_DT_INTERFACE_SIZE;
    intf.bDescriptorType = USB_DT_INTERFACE;
    intf.bInterfaceNumber = interface->interface_index;
    intf.bAlternateSetting = 0;
    intf.bNumEndpoints = 3;
    intf.bInterfaceClass = USB_CLASS_IMAGE;
    intf.bInterfaceSubClass = 0x01;
    intf.bInterfaceProtocol = 0x01;
    intf.iInterface = interfaceIndex;

    usb_endpoint_descriptor bulkIn{};
    bulkIn.bLength = USB_DT_ENDPOINT_SIZE;
    bulkIn.bDescriptorType = USB_DT_ENDPOINT;
    bulkIn.bEndpointAddress = bulkInAddress;
    bulkIn.bmAttributes = USB_TRANSFER_TYPE_BULK;
    bulkIn.bInterval = 0;

    usb_endpoint_descriptor bulkOut = bulkIn;
    bulkOut.bEndpointAddress = bulkOutAddress;

    usb_endpoint_descriptor eventIn{};
    eventIn.bLength = USB_DT_ENDPOINT_SIZE;
    eventIn.bDescriptorType = USB_DT_ENDPOINT;
    eventIn.bEndpointAddress = eventInAddress;
    eventIn.bmAttributes = USB_TRANSFER_TYPE_INTERRUPT;
    eventIn.bInterval = 0x0A;

    for (UsbDeviceSpeed speed : {UsbDeviceSpeed_Full, UsbDeviceSpeed_High}) {
        bulkIn.wMaxPacketSize = speed == UsbDeviceSpeed_High ? 512 : 64;
        bulkOut.wMaxPacketSize = bulkIn.wMaxPacketSize;
        eventIn.wMaxPacketSize = 64;

        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(interface, speed, &intf, sizeof(intf))))
            return fail("No se pudo añadir descriptor de interfaz MTP", rc);
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(interface, speed, &bulkIn, sizeof(bulkIn))))
            return fail("No se pudo añadir endpoint MTP IN", rc);
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(interface, speed, &bulkOut, sizeof(bulkOut))))
            return fail("No se pudo añadir endpoint MTP OUT", rc);
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(interface, speed, &eventIn, sizeof(eventIn))))
            return fail("No se pudo añadir endpoint MTP Event", rc);
    }

    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(interface, &epIn, bulkInAddress)))
        return fail("No se pudo abrir endpoint MTP IN", rc);
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(interface, &epOut, bulkOutAddress)))
        return fail("No se pudo abrir endpoint MTP OUT", rc);
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(interface, &epEvent, eventInAddress)))
        return fail("No se pudo abrir endpoint MTP Event", rc);

    
    usbDsEndpoint_SetZlt(epIn, true);

    rxBuffer = memalign(0x1000, IO_BUFFER_SIZE);
    txBuffer = memalign(0x1000, IO_BUFFER_SIZE);
    ctrlBuffer = memalign(0x1000, CTRL_BUFFER_SIZE);
    if (!rxBuffer || !txBuffer || !ctrlBuffer) {
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::InternalError,
            "No se pudieron reservar buffers USB alineados"));
        return false;
    }
    std::memset(rxBuffer, 0, IO_BUFFER_SIZE);
    std::memset(txBuffer, 0, IO_BUFFER_SIZE);
    std::memset(ctrlBuffer, 0, CTRL_BUFFER_SIZE);

    if (R_FAILED(rc = usbDsInterface_EnableInterface(interface)))
        return fail("No se pudo habilitar interfaz MTP", rc);
    if (R_FAILED(rc = usbDsEnable()))
        return fail("No se pudo habilitar usb:ds", rc);

    return true;
}

void NativeMtpServer::cleanupUsb()
{
    unlockHome();

    if (epOut)
        usbDsEndpoint_Cancel(epOut);
    if (epIn)
        usbDsEndpoint_Cancel(epIn);
    if (epEvent)
        usbDsEndpoint_Cancel(epEvent);

    if (interface)
        usbDsInterface_DisableInterface(interface);
    if (usbInitialized)
        usbDsDisable();

    if (epEvent) usbDsEndpoint_Close(epEvent);
    if (epOut) usbDsEndpoint_Close(epOut);
    if (epIn) usbDsEndpoint_Close(epIn);
    if (interface) usbDsInterface_Close(interface);

    epEvent = nullptr;
    epOut = nullptr;
    epIn = nullptr;
    interface = nullptr;

    if (usbInitialized)
        usbDsExit();
    usbInitialized = false;

    std::free(rxBuffer);
    std::free(txBuffer);
    std::free(ctrlBuffer);
    rxBuffer = txBuffer = ctrlBuffer = nullptr;
    rxAvailable = rxOffset = 0;
}

void NativeMtpServer::threadMain()
{
    std::error_code ec;
    std::filesystem::create_directories(INBOX_DIR, ec);

    if (!initializeUsb()) {
        cleanupUsb();
        running.store(false, std::memory_order_release);
        setState(ServerState::Error);
        return;
    }

    setState(ServerState::WaitingForHost);
    commandLoop();
    cleanupUsb();

    running.store(false, std::memory_order_release);
    if (!stopRequested.load(std::memory_order_acquire))
        setState(ServerState::Error);
    else
        setState(ServerState::Stopped);
}

bool NativeMtpServer::waitForConfigured()
{
    while (!stopRequested.load(std::memory_order_acquire)) {
        UsbState usbState = UsbState_Detached;
        if (R_SUCCEEDED(usbDsGetState(&usbState)) && usbState == UsbState_Configured) {
            UsbDeviceSpeed speed = UsbDeviceSpeed_None;
            usbDsGetSpeed(&speed);
            {
                std::scoped_lock lock(statusMutex);
                status.hostConnected = true;
                status.speed = speed;
            }

            
            if (snapshot().state != ServerState::Error)
                setState(ServerState::Connected);
            return true;
        }
        {
            std::scoped_lock lock(statusMutex);
            status.hostConnected = false;
            status.speed = UsbDeviceSpeed_None;
        }
        setState(ServerState::WaitingForHost);
        svcSleepThread(POLL_NS);
    }
    return false;
}

void NativeMtpServer::commandLoop()
{
    while (!stopRequested.load(std::memory_order_acquire)) {
        if (!waitForConfigured())
            break;

        sessionId = 0;
        rxAvailable = rxOffset = 0;
        resetPendingObject();

        while (!stopRequested.load(std::memory_order_acquire)) {
            UsbState state = UsbState_Detached;
            if (R_FAILED(usbDsGetState(&state)) || state != UsbState_Configured)
                break;
            serviceClassRequest();
            if (!processOneCommand())
                break;
        }

        unlockHome();
        if (!pendingPartPath.empty()) {
            std::error_code removeEc;
            std::filesystem::remove(pendingPartPath, removeEc);
        }
        resetPendingObject();
        {
            std::scoped_lock lock(statusMutex);
            status.hostConnected = false;
        }
    }
}

bool NativeMtpServer::endpointTransfer(UsbDsEndpoint* endpoint, void* buffer, size_t size,
                                       uint64_t timeoutNs, uint32_t& transferred, bool receive)
{
    transferred = 0;
    if (!endpoint || !buffer || size > std::numeric_limits<uint32_t>::max())
        return false;

    u32 urbId = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(endpoint, buffer, size, &urbId);
    if (R_FAILED(rc)) {
        if (receive)
            health.addUsbError();
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    while (!stopRequested.load(std::memory_order_acquire)) {
        rc = eventWait(&endpoint->CompletionEvent, POLL_NS);
        if (R_SUCCEEDED(rc)) {
            UsbDsReportData report{};
            if (R_FAILED(usbDsEndpoint_GetReportData(endpoint, &report))) {
                if (receive)
                    health.addUsbError();
                return false;
            }
            u32 requested = 0;
            if (R_FAILED(usbDsParseReportData(&report, urbId, &requested, &transferred))) {
                if (receive)
                    health.addUsbError();
                return false;
            }
            return true;
        }

        serviceClassRequest();

        UsbState state = UsbState_Detached;
        if (R_FAILED(usbDsGetState(&state)) || state != UsbState_Configured) {
            usbDsEndpoint_Cancel(endpoint);
            if (receive)
                health.markDisconnected();
            return false;
        }

        if (receive && transferCancelRequested.load(std::memory_order_acquire)) {
            usbDsEndpoint_Cancel(endpoint);
            return false;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (!receive && static_cast<uint64_t>(elapsed) >= timeoutNs) {
            usbDsEndpoint_Cancel(endpoint);
            return false;
        }

        if (receive && pendingValid) {
            const auto diag = health.evaluate(false);
            if (diag.code == installer::ErrorCode::UsbTimeout) {
                usbDsEndpoint_Cancel(endpoint);
                return false;
            }
        }
    }

    usbDsEndpoint_Cancel(endpoint);
    return false;
}

bool NativeMtpServer::receiveIntoBuffer(uint64_t timeoutNs, bool transferActive)
{
    (void)timeoutNs;
    rxOffset = 0;
    rxAvailable = 0;
    uint32_t transferred = 0;
    const bool savedPending = pendingValid;
    if (!transferActive)
        pendingValid = false; 
    const bool ok = endpointTransfer(epOut, rxBuffer, IO_BUFFER_SIZE, UINT64_MAX, transferred, true);
    if (!transferActive)
        pendingValid = savedPending;
    if (!ok || transferred == 0)
        return false;
    rxAvailable = transferred;
    return true;
}

bool NativeMtpServer::readExact(void* out, size_t size, uint64_t timeoutNs, bool transferActive)
{
    auto* dst = static_cast<uint8_t*>(out);
    size_t done = 0;
    while (done < size && !stopRequested.load(std::memory_order_acquire)) {
        if (rxOffset >= rxAvailable) {
            if (!receiveIntoBuffer(timeoutNs, transferActive))
                return false;
        }
        const size_t take = std::min(size - done, rxAvailable - rxOffset);
        std::memcpy(dst + done, static_cast<uint8_t*>(rxBuffer) + rxOffset, take);
        rxOffset += take;
        done += take;
    }
    return done == size;
}

bool NativeMtpServer::drainExact(uint64_t size, bool transferActive)
{
    std::array<uint8_t, 4096> scratch{};
    while (size > 0) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size, scratch.size()));
        if (!readExact(scratch.data(), chunk, UINT64_MAX, transferActive))
            return false;
        size -= chunk;
    }
    return true;
}

bool NativeMtpServer::sendExact(const void* data, size_t size)
{
    const auto* src = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size && !stopRequested.load(std::memory_order_acquire)) {
        const size_t chunk = std::min(size - done, IO_BUFFER_SIZE);
        std::memcpy(txBuffer, src + done, chunk);
        uint32_t transferred = 0;
        if (!endpointTransfer(epIn, txBuffer, chunk, SEND_TIMEOUT_NS, transferred, false) || transferred != chunk)
            return false;
        done += transferred;
    }
    return done == size;
}

bool NativeMtpServer::sendResponse(uint16_t code, uint32_t transaction,
                                   const std::vector<uint32_t>& params)
{
    if (params.size() > 5)
        return false;
    std::vector<uint8_t> data;
    data.reserve(sizeof(ContainerHeader) + params.size() * 4);
    add32(data, static_cast<uint32_t>(sizeof(ContainerHeader) + params.size() * 4));
    add16(data, CONTAINER_RESPONSE);
    add16(data, code);
    add32(data, transaction);
    for (uint32_t p : params)
        add32(data, p);
    return sendExact(data.data(), data.size());
}

bool NativeMtpServer::sendData(uint16_t operation, uint32_t transaction,
                               const std::vector<uint8_t>& payload)
{
    if (payload.size() > std::numeric_limits<uint32_t>::max() - sizeof(ContainerHeader))
        return false;
    std::vector<uint8_t> packet;
    packet.reserve(sizeof(ContainerHeader) + payload.size());
    add32(packet, static_cast<uint32_t>(sizeof(ContainerHeader) + payload.size()));
    add16(packet, CONTAINER_DATA);
    add16(packet, operation);
    add32(packet, transaction);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return sendExact(packet.data(), packet.size());
}

bool NativeMtpServer::processOneCommand()
{
    ContainerHeader header{};
    if (!readExact(&header, sizeof(header), UINT64_MAX, false))
        return false;

    if (header.length < sizeof(ContainerHeader) || header.length > sizeof(ContainerHeader) + 5 * 4 ||
        header.type != CONTAINER_COMMAND) {
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::InternalError,
            "Contenedor MTP de comando no válido"));
        return false;
    }

    const size_t payloadSize = header.length - sizeof(ContainerHeader);
    if ((payloadSize % 4) != 0)
        return sendResponse(RES_INVALID_PARAMETER, header.transaction);

    std::vector<uint8_t> payload(payloadSize);
    if (payloadSize && !readExact(payload.data(), payload.size(), UINT64_MAX, false))
        return false;

    std::vector<uint32_t> params;
    for (size_t i = 0; i < payload.size(); i += 4)
        params.push_back(read32(payload.data() + i));

    dispatchCommand(header.code, header.transaction, params);
    return true;
}

void NativeMtpServer::dispatchCommand(uint16_t op, uint32_t trans,
                                      const std::vector<uint32_t>& params)
{
    
    if (op != OP_GET_DEVICE_INFO && op != OP_OPEN_SESSION && sessionId == 0) {
        sendResponse(RES_SESSION_NOT_OPEN, trans);
        return;
    }

    switch (op) {
        case OP_GET_DEVICE_INFO:
            sendData(op, trans, makeDeviceInfo());
            sendResponse(RES_OK, trans);
            break;
        case OP_OPEN_SESSION:
            if (params.empty() || params[0] == 0) {
                sendResponse(RES_INVALID_PARAMETER, trans);
            } else if (sessionId != 0) {
                sendResponse(RES_SESSION_ALREADY_OPEN, trans, {sessionId});
            } else {
                sessionId = params[0];
                sendResponse(RES_OK, trans);
            }
            break;
        case OP_CLOSE_SESSION:
            sessionId = 0;
            sendResponse(RES_OK, trans);
            break;
        case OP_GET_STORAGE_IDS:
            sendData(op, trans, makeStorageIds());
            sendResponse(RES_OK, trans);
            break;
        case OP_GET_STORAGE_INFO:
            if (!params.empty() && params[0] != STORAGE_ID && params[0] != 0xFFFFFFFF) {
                sendResponse(RES_INVALID_STORAGE_ID, trans);
                break;
            }
            sendData(op, trans, makeStorageInfo());
            sendResponse(RES_OK, trans);
            break;
        case OP_GET_NUM_OBJECTS: {
            const uint32_t count = currentObjectExists() ? 1 : 0;
            sendResponse(RES_OK, trans, {count});
            break;
        }
        case OP_GET_OBJECT_HANDLES:
            sendData(op, trans, makeObjectHandles());
            sendResponse(RES_OK, trans);
            break;
        case OP_GET_OBJECT_INFO:
            if (params.empty() || params[0] != OBJECT_HANDLE || !currentObjectExists()) {
                sendResponse(RES_INVALID_OBJECT_HANDLE, trans);
                break;
            }
            sendData(op, trans, makeObjectInfo());
            sendResponse(RES_OK, trans);
            break;
        case OP_DELETE_OBJECT:
            handleDeleteObject(trans, params);
            break;
        case OP_SEND_OBJECT_INFO:
            handleSendObjectInfo(trans, params);
            break;
        case OP_SEND_OBJECT:
            handleSendObject(trans);
            break;
        case OP_GET_OBJECT_PROPS_SUPPORTED:
            if (!params.empty() && params[0] != FORMAT_UNDEFINED && params[0] != FORMAT_ASSOCIATION) {
                sendResponse(RES_INVALID_OBJECT_FORMAT, trans);
                break;
            }
            sendData(op, trans, makeObjectPropsSupported());
            sendResponse(RES_OK, trans);
            break;
        case OP_GET_OBJECT_PROP_DESC: {
            if (params.empty()) {
                sendResponse(RES_INVALID_PARAMETER, trans);
                break;
            }
            bool valid = false;
            auto data = makeObjectPropDesc(static_cast<uint16_t>(params[0]), valid);
            if (!valid)
                sendResponse(RES_INVALID_OBJECT_PROP, trans);
            else {
                sendData(op, trans, data);
                sendResponse(RES_OK, trans);
            }
            break;
        }
        case OP_GET_OBJECT_PROP_VALUE: {
            if (params.size() < 2 || params[0] != OBJECT_HANDLE || !currentObjectExists()) {
                sendResponse(RES_INVALID_OBJECT_HANDLE, trans);
                break;
            }
            bool valid = false;
            auto data = makeObjectPropValue(static_cast<uint16_t>(params[1]), valid);
            if (!valid)
                sendResponse(RES_INVALID_OBJECT_PROP, trans);
            else {
                sendData(op, trans, data);
                sendResponse(RES_OK, trans);
            }
            break;
        }
        default:
            sendResponse(RES_OPERATION_NOT_SUPPORTED, trans);
            break;
    }
}

std::vector<uint8_t> NativeMtpServer::makeDeviceInfo() const
{
    const std::vector<uint16_t> ops = {
        OP_GET_DEVICE_INFO, OP_OPEN_SESSION, OP_CLOSE_SESSION,
        OP_GET_STORAGE_IDS, OP_GET_STORAGE_INFO, OP_GET_NUM_OBJECTS,
        OP_GET_OBJECT_HANDLES, OP_GET_OBJECT_INFO, OP_DELETE_OBJECT,
        OP_SEND_OBJECT_INFO, OP_SEND_OBJECT,
        OP_GET_OBJECT_PROPS_SUPPORTED, OP_GET_OBJECT_PROP_DESC, OP_GET_OBJECT_PROP_VALUE,
    };
    const std::vector<uint16_t> formats = {FORMAT_UNDEFINED, FORMAT_ASSOCIATION};
    std::vector<uint8_t> d;
    add16(d, 100);                 
    add32(d, 0x00000006);          
    add16(d, 0x0064);              
    addString(d, "microsoft.com: 1.0;");
    add16(d, 0);                   
    addArray<uint16_t>(d, ops);
    addArray<uint16_t>(d, {});     
    addArray<uint16_t>(d, {});     
    addArray<uint16_t>(d, {});     
    addArray<uint16_t>(d, formats);
    addString(d, "Alucardio");
    addString(d, "AIO MOD");
    addString(d, "AIO MOD " APP_VERSION);
    addString(d, "AIO-MOD-" APP_VERSION);
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeStorageIds() const
{
    std::vector<uint8_t> d;
    addArray<uint32_t>(d, {STORAGE_ID});
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeStorageInfo() const
{
    s64 totalRaw = 0;
    s64 freeRaw = 0;
    const bool totalOk = R_SUCCEEDED(nsGetTotalSpaceSize(NcmStorageId_SdCard, &totalRaw));
    const bool freeOk = R_SUCCEEDED(nsGetFreeSpaceSize(NcmStorageId_SdCard, &freeRaw));
    const uint64_t freeBytes = freeOk && freeRaw > 0 ? static_cast<uint64_t>(freeRaw) : 0;
    const uint64_t capacity = totalOk && totalRaw > 0
        ? static_cast<uint64_t>(totalRaw)
        : std::max<uint64_t>(freeBytes, 1);

    std::vector<uint8_t> d;
    add16(d, 0x0003); 
    add16(d, 0x0002); 
    add16(d, 0x0000); 
    add64(d, capacity);
    add64(d, freeBytes);
    add32(d, 0xFFFFFFFF);
    addString(d, "SD Card Install");
    addString(d, "AIO-MOD");
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeObjectHandles() const
{
    std::vector<uint8_t> d;
    if (currentObjectExists())
        addArray<uint32_t>(d, {OBJECT_HANDLE});
    else
        addArray<uint32_t>(d, {});
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeObjectInfo() const
{
    const uint64_t size = currentObjectSize();
    std::vector<uint8_t> d;
    add32(d, STORAGE_ID);
    add16(d, FORMAT_UNDEFINED);
    add16(d, 0); 
    add32(d, size > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(size));
    add16(d, 0); 
    add32(d, 0); 
    add32(d, 0); add32(d, 0); 
    add32(d, 0); add32(d, 0); add32(d, 0); 
    add32(d, ROOT_PARENT);
    add16(d, 0); 
    add32(d, 0); 
    add32(d, 0); 
    addString(d, currentObjectName());
    addString(d, ""); 
    addString(d, ""); 
    addString(d, ""); 
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeObjectPropsSupported() const
{
    std::vector<uint8_t> d;
    addArray<uint16_t>(d, {PROP_STORAGE_ID, PROP_OBJECT_FORMAT, PROP_OBJECT_SIZE,
                           PROP_PARENT, PROP_PERSISTENT_ID, PROP_NAME});
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeObjectPropDesc(uint16_t prop, bool& valid) const
{
    valid = true;
    std::vector<uint8_t> d;
    add16(d, prop);
    switch (prop) {
        case PROP_STORAGE_ID:
            add16(d, TYPE_UINT32); add8(d, 0); add32(d, 0); add32(d, 0); add8(d, 0); break;
        case PROP_OBJECT_FORMAT:
            add16(d, TYPE_UINT16); add8(d, 0); add16(d, 0); add32(d, 0); add8(d, 0); break;
        case PROP_OBJECT_SIZE:
            add16(d, TYPE_UINT64); add8(d, 0); add64(d, 0); add32(d, 0); add8(d, 0); break;
        case PROP_PARENT:
            add16(d, TYPE_UINT32); add8(d, 0); add32(d, 0); add32(d, 0); add8(d, 0); break;
        case PROP_PERSISTENT_ID:
            add16(d, TYPE_UINT128); add8(d, 0); add128(d, 0, 0); add32(d, 0); add8(d, 0); break;
        case PROP_NAME:
            add16(d, TYPE_STRING); add8(d, 0); addString(d, ""); add32(d, 0); add8(d, 0); break;
        default:
            valid = false;
            d.clear();
            break;
    }
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeObjectPropValue(uint16_t prop, bool& valid) const
{
    valid = true;
    std::vector<uint8_t> d;
    switch (prop) {
        case PROP_STORAGE_ID: add32(d, STORAGE_ID); break;
        case PROP_OBJECT_FORMAT: add16(d, FORMAT_UNDEFINED); break;
        case PROP_OBJECT_SIZE: add64(d, currentObjectSize()); break;
        case PROP_PARENT: add32(d, ROOT_PARENT); break;
        case PROP_PERSISTENT_ID: add128(d, OBJECT_HANDLE, 0); break;
        case PROP_NAME: addString(d, currentObjectName()); break;
        default: valid = false; break;
    }
    return d;
}

void NativeMtpServer::handleSendObjectInfo(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (!params.empty() && params[0] != 0 && params[0] != STORAGE_ID) {
        sendResponse(RES_INVALID_STORAGE_ID, trans);
        return;
    }
    if (params.size() >= 2 && params[1] != 0 && params[1] != 0xFFFFFFFF) {
        sendResponse(RES_INVALID_PARENT, trans);
        return;
    }

    ContainerHeader dataHeader{};
    if (!readExact(&dataHeader, sizeof(dataHeader), UINT64_MAX, false))
        return;
    if (dataHeader.type != CONTAINER_DATA || dataHeader.code != OP_SEND_OBJECT_INFO ||
        dataHeader.transaction != trans || dataHeader.length < sizeof(ContainerHeader) + 53 ||
        dataHeader.length > sizeof(ContainerHeader) + 64 * 1024) {
        if (dataHeader.length > sizeof(ContainerHeader) && dataHeader.length < 1024 * 1024)
            drainExact(dataHeader.length - sizeof(ContainerHeader), false);
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    std::vector<uint8_t> data(dataHeader.length - sizeof(ContainerHeader));
    if (!readExact(data.data(), data.size(), UINT64_MAX, false))
        return;

    const uint32_t storageId = read32(data.data());
    const uint16_t format = read16(data.data() + 4);
    const uint32_t size32 = read32(data.data() + 8);
    const uint32_t parent = read32(data.data() + 38);
    bool nameOk = false;
    const std::string name = utf16Name(data.data() + 52, data.size() - 52, nameOk);

    if ((storageId != 0 && storageId != STORAGE_ID) ||
        (parent != 0 && parent != 0xFFFFFFFF) || format == FORMAT_ASSOCIATION ||
        !nameOk || !isSafeNspName(name)) {
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::UnsupportedFormat,
            "MTP object rechazado: " + name));
        sendResponse(format == FORMAT_ASSOCIATION ? RES_INVALID_OBJECT_FORMAT : RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    if (size32 == 0xFFFFFFFFU) {
        const auto d = installer::makeDiagnostic(installer::ErrorCode::MtpObjectTooLarge,
            "MTP ObjectInfo size=0xFFFFFFFF (>4 GiB o tamaño desconocido)");
        setDiagnostic(d);
        installer::writeDiagnosticLog(d, installer::getSystemSnapshot(), name, nullptr, nullptr);
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    const auto system = installer::getSystemSnapshot();
    const uint64_t required = static_cast<uint64_t>(size32) + SPACE_RESERVE;
    if (system.sdFreeBytes >= 0 && required > static_cast<uint64_t>(system.sdFreeBytes)) {
        const auto d = installer::makeDiagnostic(installer::ErrorCode::NotEnoughSpace,
            "mtp_size=" + std::to_string(size32) + ", reserve=" + std::to_string(SPACE_RESERVE) +
            ", free=" + std::to_string(system.sdFreeBytes));
        setDiagnostic(d);
        installer::writeDiagnosticLog(d, system, name, nullptr, nullptr);
        sendResponse(RES_STORE_FULL, trans);
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(INBOX_DIR, ec);

    
    deleteLastStaged();

    pendingName = name;
    pendingSize = size32;
    pendingFormat = format;
    pendingPartPath = std::string(INBOX_DIR) + "/" + name + ".part";
    pendingFinalPath = std::string(INBOX_DIR) + "/" + name;
    pendingValid = true;
    transferCancelRequested.store(false, std::memory_order_release);

    {
        std::scoped_lock lock(statusMutex);
        status.currentFile = name;
        status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
        status.transfer = {};
    }
    setState(ServerState::Connected);

    sendResponse(RES_OK, trans, {STORAGE_ID, ROOT_PARENT, OBJECT_HANDLE});
}

void NativeMtpServer::handleSendObject(uint32_t trans)
{
    if (!pendingValid) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    ContainerHeader dataHeader{};
    if (!readExact(&dataHeader, sizeof(dataHeader), UINT64_MAX, false))
        return;
    if (dataHeader.type != CONTAINER_DATA || dataHeader.code != OP_SEND_OBJECT || dataHeader.transaction != trans) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    if (dataHeader.length == 0xFFFFFFFFU || dataHeader.length < sizeof(ContainerHeader) ||
        static_cast<uint64_t>(dataHeader.length - sizeof(ContainerHeader)) != pendingSize) {
        const uint64_t declared = dataHeader.length >= sizeof(ContainerHeader) && dataHeader.length != 0xFFFFFFFFU
            ? dataHeader.length - sizeof(ContainerHeader) : 0;
        if (declared && declared < (8ULL * 1024ULL * 1024ULL * 1024ULL))
            drainExact(declared, true);
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::PackageTruncated,
            "MTP SendObject length=" + std::to_string(declared) + ", expected=" + std::to_string(pendingSize)),
            RES_INCOMPLETE_TRANSFER, trans);
        return;
    }

    std::error_code ec;
    std::filesystem::remove(pendingPartPath, ec);
    std::ofstream out(pendingPartPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::WriteError,
            "No se pudo crear " + pendingPartPath), RES_GENERAL_ERROR, trans);
        return;
    }

    health.reset(pendingSize);
    transferCancelRequested.store(false, std::memory_order_release);
    setState(ServerState::Receiving);

    appletSetMediaPlaybackState(true);
    if (R_SUCCEEDED(appletBeginBlockingHomeButton(0)))
        homeBlocked = true;

    uint64_t remaining = pendingSize;
    bool readFailed = false;
    bool writeFailed = false;

    while (remaining > 0 && !stopRequested.load(std::memory_order_acquire) &&
           !transferCancelRequested.load(std::memory_order_acquire)) {
        if (rxOffset >= rxAvailable) {
            if (!receiveIntoBuffer(UINT64_MAX, true)) {
                readFailed = true;
                break;
            }
        }
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, rxAvailable - rxOffset));
        health.addReceived(chunk);
        out.write(reinterpret_cast<const char*>(static_cast<uint8_t*>(rxBuffer) + rxOffset),
                  static_cast<std::streamsize>(chunk));
        if (!out) {
            writeFailed = true;
            break;
        }
        health.addWritten(chunk);
        rxOffset += chunk;
        remaining -= chunk;
    }

    out.flush();
    const bool flushOk = static_cast<bool>(out);
    out.close();
    unlockHome();

    if (stopRequested.load(std::memory_order_acquire) || transferCancelRequested.load(std::memory_order_acquire)) {
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::Cancelled,
            "Transferencia MTP cancelada"), RES_INCOMPLETE_TRANSFER, trans);
        return;
    }

    if (writeFailed || !flushOk) {
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::WriteError,
            "Fallo escribiendo " + pendingPartPath), RES_GENERAL_ERROR, trans);
        return;
    }

    if (readFailed || remaining != 0) {
        auto diag = health.evaluate(false);
        if (diag.ok()) {
            UsbState usbState = UsbState_Detached;
            if (R_FAILED(usbDsGetState(&usbState)) || usbState != UsbState_Configured)
                diag = installer::makeDiagnostic(installer::ErrorCode::UsbDisconnected,
                    "remaining=" + std::to_string(remaining));
            else
                diag = installer::makeDiagnostic(installer::ErrorCode::UsbTimeout,
                    "remaining=" + std::to_string(remaining));
        }
        failTransfer(diag, RES_INCOMPLETE_TRANSFER, trans);
        return;
    }

    const auto transfer = health.snapshot();
    const auto transferResult = health.evaluate(true);
    if (!transferResult.ok() && transferResult.code != installer::ErrorCode::UsbSlow) {
        failTransfer(transferResult, RES_INCOMPLETE_TRANSFER, trans);
        return;
    }

    std::filesystem::remove(pendingFinalPath, ec);
    std::filesystem::rename(pendingPartPath, pendingFinalPath, ec);
    if (ec) {
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::WriteError,
            "rename: " + ec.message()), RES_GENERAL_ERROR, trans, false);
        return;
    }

    setState(ServerState::Preflighting);
    const auto preflight = installer::preflightPackage(pendingFinalPath, false);
    const auto system = installer::getSystemSnapshot();
    installer::writeDiagnosticLog(preflight.diagnostic, system, pendingFinalPath, &preflight, &transfer);

    if (!preflight.ok()) {
        setDiagnostic(preflight.diagnostic);
        std::filesystem::remove(pendingFinalPath, ec);
        {
            std::scoped_lock lock(statusMutex);
            status.lastStagedPath.clear();
            status.transfer = transfer;
        }
        setState(ServerState::Error);
        sendResponse(RES_INCOMPLETE_TRANSFER, trans);
        resetPendingObject();
        return;
    }

    {
        std::scoped_lock lock(statusMutex);
        status.lastStagedPath = pendingFinalPath;
        status.currentFile = pendingName;
        status.transfer = transfer;
        status.lastDiagnostic = transferResult.code == installer::ErrorCode::UsbSlow
            ? transferResult : preflight.diagnostic;
    }
    setState(ServerState::Ready);
    sendResponse(RES_OK, trans);
    resetPendingObject();
}

void NativeMtpServer::handleDeleteObject(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (params.empty() || params[0] != OBJECT_HANDLE || !currentObjectExists()) {
        sendResponse(RES_INVALID_OBJECT_HANDLE, trans);
        return;
    }
    if (deleteLastStaged())
        sendResponse(RES_OK, trans);
    else
        sendResponse(RES_GENERAL_ERROR, trans);
}

void NativeMtpServer::failTransfer(const installer::Diagnostic& diagnostic, uint16_t mtpResponse,
                                   uint32_t transaction, bool deletePartial)
{
    unlockHome();
    const auto transfer = health.snapshot();
    setDiagnostic(diagnostic);
    if (deletePartial && !pendingPartPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(pendingPartPath, ec);
    }
    installer::writeDiagnosticLog(diagnostic, installer::getSystemSnapshot(), pendingName,
                                  nullptr, &transfer);
    {
        std::scoped_lock lock(statusMutex);
        status.transfer = transfer;
    }
    setState(ServerState::Error);
    UsbState usbState = UsbState_Detached;
    if (usbInitialized && R_SUCCEEDED(usbDsGetState(&usbState)) && usbState == UsbState_Configured)
        sendResponse(mtpResponse, transaction);
    resetPendingObject();
}

void NativeMtpServer::resetPendingObject()
{
    pendingName.clear();
    pendingPartPath.clear();
    pendingFinalPath.clear();
    pendingSize = 0;
    pendingFormat = 0;
    pendingValid = false;
    transferCancelRequested.store(false, std::memory_order_release);
}

bool NativeMtpServer::serviceClassRequest()
{
    if (!interface)
        return false;
    if (R_FAILED(eventWait(&interface->SetupEvent, 0)))
        return false;

    usb_control_setup setup{};
    if (R_FAILED(usbDsInterface_GetSetupPacket(interface, &setup, sizeof(setup))))
        return false;

    const bool classRequest = (setup.bmRequestType & 0x60) == USB_REQUEST_TYPE_CLASS;
    if (!classRequest)
        return false;

    auto finishCtrlIn = [this](size_t size) {
        u32 urb = 0;
        if (R_FAILED(usbDsInterface_CtrlInPostBufferAsync(interface, ctrlBuffer, size, &urb)))
            return false;
        if (R_FAILED(eventWait(&interface->CtrlInCompletionEvent, 1000ULL * 1000ULL * 1000ULL)))
            return false;
        UsbDsReportData report{};
        u32 requested = 0, transferred = 0;
        return R_SUCCEEDED(usbDsInterface_GetCtrlInReportData(interface, &report)) &&
               R_SUCCEEDED(usbDsParseReportData(&report, urb, &requested, &transferred));
    };
    auto finishCtrlOut = [this](size_t size) {
        u32 urb = 0;
        if (R_FAILED(usbDsInterface_CtrlOutPostBufferAsync(interface, ctrlBuffer, size, &urb)))
            return false;
        if (R_FAILED(eventWait(&interface->CtrlOutCompletionEvent, 1000ULL * 1000ULL * 1000ULL)))
            return false;
        UsbDsReportData report{};
        u32 requested = 0, transferred = 0;
        return R_SUCCEEDED(usbDsInterface_GetCtrlOutReportData(interface, &report)) &&
               R_SUCCEEDED(usbDsParseReportData(&report, urb, &requested, &transferred));
    };

    switch (setup.bRequest) {
        case MTP_REQ_GET_DEVICE_STATUS: {
            auto* p = static_cast<uint8_t*>(ctrlBuffer);
            p[0] = 4; p[1] = 0; p[2] = static_cast<uint8_t>(RES_OK); p[3] = static_cast<uint8_t>(RES_OK >> 8);
            if (!finishCtrlIn(std::min<size_t>(4, setup.wLength)))
                usbDsInterface_StallCtrl(interface);
            return true;
        }
        case MTP_REQ_RESET:
            transferCancelRequested.store(true, std::memory_order_release);
            sessionId = 0;
            if (!finishCtrlOut(0))
                usbDsInterface_StallCtrl(interface);
            return true;
        case MTP_REQ_CANCEL:
            if (!finishCtrlOut(std::min<size_t>(setup.wLength, 6)))
                usbDsInterface_StallCtrl(interface);
            transferCancelRequested.store(true, std::memory_order_release);
            return true;
        default:
            usbDsInterface_StallCtrl(interface);
            return true;
    }
}

bool NativeMtpServer::currentObjectExists() const
{
    std::string path;
    {
        std::scoped_lock lock(statusMutex);
        path = status.lastStagedPath;
    }
    return !path.empty() && std::filesystem::exists(path);
}

std::string NativeMtpServer::currentObjectName() const
{
    std::string path;
    {
        std::scoped_lock lock(statusMutex);
        path = status.lastStagedPath;
    }
    return path.empty() ? std::string{} : baseName(path);
}

uint64_t NativeMtpServer::currentObjectSize() const
{
    std::string path;
    {
        std::scoped_lock lock(statusMutex);
        path = status.lastStagedPath;
    }
    return path.empty() ? 0 : fileSize(path);
}

CleanupResult NativeMtpServer::cleanupTemporaryFiles()
{
    CleanupResult result{};
    const std::array<const char*, 2> dirs = {
        INBOX_DIR,
        "/switch/aio-switch-updater-mod/config/install_work",
    };

    
    if (snapshot().state == ServerState::Receiving)
        return result;

    for (const char* dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || ec)
            continue;

        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
            if (ec)
                break;
            const auto& entry = *it;
            if (!entry.is_regular_file(ec) || ec)
                continue;

            const auto path = entry.path();
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext != ".part" && ext != ".tmp" && ext != ".partial")
                continue;

            const uint64_t size = entry.file_size(ec);
            if (ec) {
                ec.clear();
                ++result.failedFiles;
                continue;
            }
            if (std::filesystem::remove(path, ec) && !ec) {
                ++result.removedFiles;
                result.freedBytes += size;
            } else {
                ++result.failedFiles;
                ec.clear();
            }
        }
    }
    return result;
}

bool NativeMtpServer::deleteLastStaged()
{
    std::string path;
    {
        std::scoped_lock lock(statusMutex);
        path = status.lastStagedPath;
    }
    if (path.empty())
        return true;

    std::error_code ec;
    const bool removed = !std::filesystem::exists(path) || std::filesystem::remove(path, ec);
    if (removed && !ec) {
        std::scoped_lock lock(statusMutex);
        if (status.lastStagedPath == path) {
            status.lastStagedPath.clear();
            status.currentFile.clear();
            status.transfer = {};
            if (status.state == ServerState::Ready)
                status.state = ServerState::Connected;
        }
        return true;
    }
    return false;
}

} 
