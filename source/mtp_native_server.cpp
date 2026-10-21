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

#include "constants.hpp"
#include "mtp_install_stream.hpp"
#include "nsp_transaction_installer.hpp"
#include "package_reader.hpp"

namespace mtp {
namespace {

constexpr const char INBOX_DIR[] = "/switch/aio-switch-updater-mod/config/mtp_inbox";
constexpr uint32_t STORAGE_ID = 0x00010001;
constexpr uint32_t ROOT_PARENT = 0x00000000;
constexpr uint64_t SPACE_RESERVE = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t SPLIT_CHUNK_SIZE = 1ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr size_t IO_BUFFER_SIZE = 4 * 1024 * 1024;
constexpr size_t CTRL_BUFFER_SIZE = 0x1000;
constexpr uint64_t POLL_NS = 250ULL * 1000ULL * 1000ULL;
constexpr uint64_t SEND_TIMEOUT_NS = 10ULL * 1000ULL * 1000ULL * 1000ULL;

constexpr uint16_t CONTAINER_COMMAND = 1;
constexpr uint16_t CONTAINER_DATA = 2;
constexpr uint16_t CONTAINER_RESPONSE = 3;
constexpr uint16_t CONTAINER_EVENT = 4;

constexpr uint16_t EVENT_OBJECT_REMOVED = 0x4003;

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
constexpr uint16_t OP_SET_OBJECT_PROP_VALUE = 0x9804;
constexpr uint16_t OP_GET_OBJECT_PROP_LIST = 0x9805;
constexpr uint16_t OP_SEND_OBJECT_PROP_LIST = 0x9808;

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
constexpr uint16_t RES_DEVICE_BUSY = 0x2019;
constexpr uint16_t RES_INVALID_OBJECT_PROP = 0xA801;

constexpr uint16_t FORMAT_UNDEFINED = 0x3000;
constexpr uint16_t FORMAT_ASSOCIATION = 0x3001;

constexpr uint16_t PROP_STORAGE_ID = 0xDC01;
constexpr uint16_t PROP_OBJECT_FORMAT = 0xDC02;
constexpr uint16_t PROP_OBJECT_SIZE = 0xDC04;
constexpr uint16_t PROP_OBJECT_FILE_NAME = 0xDC07;
constexpr uint16_t PROP_PARENT = 0xDC0B;
constexpr uint16_t PROP_PERSISTENT_ID = 0xDC41;

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

uint64_t read64(const uint8_t* p)
{
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i)
        out |= static_cast<uint64_t>(p[i]) << (i * 8);
    return out;
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

    // MTP strings are UTF-16LE and the character count includes the NUL.
    // File names used by this phase are normally ASCII/UTF-8. Decode enough
    // UTF-8 here to preserve Latin names instead of exposing raw bytes.
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

std::string splitFinalPath(const std::string& base, uint32_t index)
{
    if (index == 0)
        return base;
    char suffix[16]{};
    std::snprintf(suffix, sizeof(suffix), ".%02u", index);
    return base + suffix;
}

std::string splitTempPath(const std::string& base, uint32_t index)
{
    return splitFinalPath(base, index) + ".partial";
}

size_t mtpValueSize(uint16_t type, const uint8_t* data, size_t remaining, bool& ok)
{
    ok = false;
    size_t scalar = 0;
    switch (type) {
        case 0x0001: case 0x0002: scalar = 1; break;
        case 0x0003: case 0x0004: scalar = 2; break;
        case 0x0005: case 0x0006: scalar = 4; break;
        case 0x0007: case 0x0008: scalar = 8; break;
        case 0x0009: case 0x000A: scalar = 16; break;
        case TYPE_STRING: {
            if (remaining < 1) return 0;
            const size_t bytes = 1 + static_cast<size_t>(data[0]) * 2;
            if (bytes > remaining) return 0;
            ok = true;
            return bytes;
        }
        default:
            break;
    }
    if (scalar) {
        if (remaining < scalar) return 0;
        ok = true;
        return scalar;
    }

    // Array data types are the scalar type with bit 14 set and a UINT32 count prefix.
    if ((type & 0x4000) != 0 && remaining >= 4) {
        const uint16_t baseType = static_cast<uint16_t>(type & ~0x4000);
        size_t elem = 0;
        switch (baseType) {
            case 0x0001: case 0x0002: elem = 1; break;
            case 0x0003: case 0x0004: elem = 2; break;
            case 0x0005: case 0x0006: elem = 4; break;
            case 0x0007: case 0x0008: elem = 8; break;
            case 0x0009: case 0x000A: elem = 16; break;
            default: return 0;
        }
        const uint32_t count = read32(data);
        if (count > (remaining - 4) / elem) return 0;
        ok = true;
        return 4 + static_cast<size_t>(count) * elem;
    }
    return 0;
}

} // namespace

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
        case ServerState::Receiving: return "Streaming NSP from PC";
        case ServerState::Preflighting: return "Validating NSP";
        case ServerState::Ready: return "NSP ready";
        case ServerState::Installing: return "Installing NSP";
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
bool NativeMtpServer::isInstalling() const { return installerBusy.load(std::memory_order_acquire); }

void NativeMtpServer::setInstalling(bool installing)
{
    installerBusy.store(installing, std::memory_order_release);
    if (installing) {
        setState(ServerState::Installing);
        return;
    }

    if (currentObjectExists()) {
        setState(ServerState::Ready);
        return;
    }

    bool connected = false;
    {
        std::scoped_lock lock(statusMutex);
        connected = status.hostConnected;
    }
    setState(connected ? ServerState::Connected : ServerState::WaitingForHost);
}

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
    if (copy.state == ServerState::Receiving || copy.state == ServerState::Installing)
        copy.transfer = health.snapshot();
    return copy;
}

bool NativeMtpServer::start()
{
    if (running.exchange(true, std::memory_order_acq_rel))
        return true;

    stopRequested.store(false, std::memory_order_release);
    transferCancelRequested.store(false, std::memory_order_release);
    installerBusy.store(false, std::memory_order_release);
    setState(ServerState::Starting);
    {
        std::scoped_lock lock(statusMutex);
        status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
        status.speed = UsbDeviceSpeed_None;
        status.hostConnected = false;
        status.transfer = {};
    }

    // A power loss or forced exit can leave .part/.tmp files behind. Remove only
    // AIO-owned transient files; completed staged NSPs are deliberately preserved.
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
    const std::string usbSerial = std::string("AIO-MOD-") + APP_VERSION;
    if (R_FAILED(rc = usbDsAddUsbStringDescriptor(&serialIndex, usbSerial.c_str())))
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
    // A distinct PID prevents Windows from confusing this responder with RCM,
    // DBI or other Switch USB modes that may have a WinUSB/libusbK driver.
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

    // ZLT must be enabled on both directions. Windows MTP can end transfers on
    // exact max-packet boundaries in either direction; leaving OUT without ZLT
    // is a known source of hangs on back-to-back uploads.
    usbDsEndpoint_SetZlt(epIn, true);
    usbDsEndpoint_SetZlt(epOut, true);

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
            // Keep a transfer error visible until the user starts another transfer or
            // restarts the responder. The old loop immediately overwrote Error with
            // Connected because Windows usually leaves the USB device configured.
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
        removePendingParts();
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
        pendingValid = false; // idle command reads must never trigger the active-transfer timeout
    const bool ok = endpointTransfer(epOut, rxBuffer, IO_BUFFER_SIZE, UINT64_MAX, transferred, true);
    if (!transferActive)
        pendingValid = savedPending;
    if (!ok)
        return false;
    rxShortPacket = transferred < IO_BUFFER_SIZE;
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
            if (rxAvailable == 0)
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


bool NativeMtpServer::sendEvent(uint16_t eventCode, const std::vector<uint32_t>& params)
{
    if (!epEvent || params.size() > 3)
        return false;

    std::array<uint8_t, 12 + 3 * 4> packet{};
    const uint32_t length = static_cast<uint32_t>(sizeof(ContainerHeader) + params.size() * sizeof(uint32_t));
    auto* header = reinterpret_cast<ContainerHeader*>(packet.data());
    header->length = length;
    header->type = CONTAINER_EVENT;
    header->code = eventCode;
    header->transaction = 0;

    size_t offset = sizeof(ContainerHeader);
    for (uint32_t value : params) {
        packet[offset + 0] = static_cast<uint8_t>(value);
        packet[offset + 1] = static_cast<uint8_t>(value >> 8);
        packet[offset + 2] = static_cast<uint8_t>(value >> 16);
        packet[offset + 3] = static_cast<uint8_t>(value >> 24);
        offset += 4;
    }

    uint32_t transferred = 0;
    // Windows normally keeps the interrupt endpoint polled. Events are advisory
    // for the virtual Install storage, so a short timeout must never stall the
    // command loop if the host does not currently have an interrupt URB queued.
    return endpointTransfer(epEvent, packet.data(), length,
                            250ULL * 1000ULL * 1000ULL, transferred, false) &&
           transferred == length;
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
    // DeviceInfo and OpenSession are the only operations used before a session.
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
                {
                    std::scoped_lock lock(statusMutex);
                    status.currentFile.clear();
                    status.lastCompletedFile.clear();
                    status.lastFailedFile.clear();
                    status.sessionInstalledCount = 0;
                    status.sessionFailedCount = 0;
                    status.installStage = "Waiting for NSP data";
                    status.installCurrent = 0;
                    status.installTotal = 0;
                    status.installContentIndex = 0;
                    status.installContentCount = 0;
                    status.lastInstallSucceeded = false;
                    status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
                    status.transfer = {};
                }
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
            if (params.empty() || params[0] != currentObjectHandle() || !currentObjectExists()) {
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
        case OP_SEND_OBJECT_PROP_LIST:
            handleSendObjectPropList(trans, params);
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
            if (params.size() < 2 || params[0] != currentObjectHandle() || !currentObjectExists()) {
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
        case OP_SET_OBJECT_PROP_VALUE:
            handleSetObjectPropValue(trans, params);
            break;
        case OP_GET_OBJECT_PROP_LIST:
            handleGetObjectPropList(trans, params);
            break;
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
        OP_SEND_OBJECT_INFO, OP_SEND_OBJECT, OP_SEND_OBJECT_PROP_LIST,
        OP_GET_OBJECT_PROPS_SUPPORTED, OP_GET_OBJECT_PROP_DESC, OP_GET_OBJECT_PROP_VALUE,
        OP_SET_OBJECT_PROP_VALUE, OP_GET_OBJECT_PROP_LIST,
    };
    const std::vector<uint16_t> formats = {FORMAT_UNDEFINED, FORMAT_ASSOCIATION};
    std::vector<uint8_t> d;
    add16(d, 100);                 // StandardVersion 1.00
    add32(d, 0x00000006);          // Microsoft MTP vendor extension ID
    add16(d, 0x0064);              // extension version 1.00
    addString(d, "microsoft.com: 1.0;");
    add16(d, 0);                   // functional mode
    addArray<uint16_t>(d, ops);
    addArray<uint16_t>(d, {EVENT_OBJECT_REMOVED}); // events
    addArray<uint16_t>(d, {});     // device props
    addArray<uint16_t>(d, {});     // capture formats
    addArray<uint16_t>(d, formats);
    addString(d, "Alucardio");
    addString(d, "AIO MOD");
    addString(d, std::string(APP_VERSION) + " MTP Installer");
    addString(d, std::string("AIO-MOD-") + APP_VERSION);
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
    add16(d, 0x0003); // removable RAM
    add16(d, 0x0002); // generic hierarchical filesystem
    add16(d, 0x0000); // read/write
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
        addArray<uint32_t>(d, {currentObjectHandle()});
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
    add16(d, 0); // protection
    add32(d, size > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<uint32_t>(size));
    add16(d, 0); // thumb format
    add32(d, 0); // thumb size
    add32(d, 0); add32(d, 0); // thumb w/h
    add32(d, 0); add32(d, 0); add32(d, 0); // image w/h/depth
    add32(d, ROOT_PARENT);
    add16(d, 0); // association type
    add32(d, 0); // association desc
    add32(d, 0); // sequence
    addString(d, currentObjectName());
    addString(d, ""); // created
    addString(d, ""); // modified
    addString(d, ""); // keywords
    return d;
}

std::vector<uint8_t> NativeMtpServer::makeObjectPropsSupported() const
{
    std::vector<uint8_t> d;
    addArray<uint16_t>(d, {PROP_STORAGE_ID, PROP_OBJECT_FORMAT, PROP_OBJECT_SIZE,
                           PROP_OBJECT_FILE_NAME, PROP_PARENT, PROP_PERSISTENT_ID});
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
        case PROP_OBJECT_FILE_NAME:
            add16(d, TYPE_STRING); add8(d, 1); addString(d, ""); add32(d, 0); add8(d, 0); break;
        case PROP_PARENT:
            add16(d, TYPE_UINT32); add8(d, 0); add32(d, 0); add32(d, 0); add8(d, 0); break;
        case PROP_PERSISTENT_ID:
            add16(d, TYPE_UINT128); add8(d, 0); add128(d, 0, 0); add32(d, 0); add8(d, 0); break;
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
        case PROP_OBJECT_FILE_NAME: addString(d, currentObjectName()); break;
        case PROP_PARENT: add32(d, ROOT_PARENT); break;
        case PROP_PERSISTENT_ID: add128(d, currentObjectHandle(), 0); break;
        default: valid = false; break;
    }
    return d;
}


std::vector<uint8_t> NativeMtpServer::makeObjectPropList(uint32_t handle, uint16_t requested, bool& valid) const
{
    valid = true;
    std::vector<uint8_t> d;
    if (!currentObjectExists()) {
        add32(d, 0);
        return d;
    }
    if (handle != currentObjectHandle() && handle != 0xFFFFFFFFU) {
        valid = false;
        return {};
    }

    const std::array<uint16_t, 6> props = {
        PROP_STORAGE_ID, PROP_OBJECT_FORMAT, PROP_OBJECT_SIZE,
        PROP_OBJECT_FILE_NAME, PROP_PARENT, PROP_PERSISTENT_ID
    };
    std::vector<uint16_t> selected;
    if (requested == 0xFFFFU || requested == 0) {
        selected.assign(props.begin(), props.end());
    } else {
        if (std::find(props.begin(), props.end(), requested) == props.end()) {
            valid = false;
            return {};
        }
        selected.push_back(requested);
    }

    add32(d, static_cast<uint32_t>(selected.size()));
    for (uint16_t prop : selected) {
        add32(d, currentObjectHandle());
        add16(d, prop);
        switch (prop) {
            case PROP_STORAGE_ID: add16(d, TYPE_UINT32); add32(d, STORAGE_ID); break;
            case PROP_OBJECT_FORMAT: add16(d, TYPE_UINT16); add16(d, FORMAT_UNDEFINED); break;
            case PROP_OBJECT_SIZE: add16(d, TYPE_UINT64); add64(d, currentObjectSize()); break;
            case PROP_OBJECT_FILE_NAME: add16(d, TYPE_STRING); addString(d, currentObjectName()); break;
            case PROP_PARENT: add16(d, TYPE_UINT32); add32(d, ROOT_PARENT); break;
            case PROP_PERSISTENT_ID: add16(d, TYPE_UINT128); add128(d, currentObjectHandle(), 0); break;
            default: valid = false; return {};
        }
    }
    return d;
}

void NativeMtpServer::recoverStagedPackage()
{
    std::error_code ec;
    if (!std::filesystem::exists(INBOX_DIR, ec) || ec)
        return;

    std::filesystem::path candidate;
    std::filesystem::file_time_type newest{};
    bool haveCandidate = false;
    for (std::filesystem::directory_iterator it(INBOX_DIR, ec), end; it != end && !ec; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext != ".nsp")
            continue; // split .01/.02 parts are discovered through their base .nsp

        const auto when = it->last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!haveCandidate || when > newest) {
            candidate = it->path();
            newest = when;
            haveCandidate = true;
        }
    }
    if (!haveCandidate)
        return;

    const std::string path = candidate.string();
    const auto preflight = installer::preflightPackage(path, false);
    if (!preflight.ok()) {
        installer::writeDiagnosticLog(preflight.diagnostic, installer::getSystemSnapshot(), path, &preflight, nullptr);
        installer::PackageReader::removePackage(path);
        return;
    }

    {
        std::scoped_lock lock(statusMutex);
        status.lastStagedPath = path;
        status.currentFile = candidate.filename().string();
        status.lastDiagnostic = preflight.diagnostic;
        status.transfer = {};
    }
}

bool NativeMtpServer::preparePendingObject(const std::string& name, uint64_t size, bool sizeKnown,
                                           uint16_t format, uint32_t trans)
{
    if (installerBusy.load(std::memory_order_acquire) || pendingValid) {
        sendResponse(RES_DEVICE_BUSY, trans);
        return false;
    }

    if (format == FORMAT_ASSOCIATION || !isSafeNspName(name)) {
        const auto d = installer::makeDiagnostic(installer::ErrorCode::UnsupportedFormat,
            "MTP object rejected: " + name);
        setDiagnostic(d);
        sendResponse(format == FORMAT_ASSOCIATION ? RES_INVALID_OBJECT_FORMAT : RES_INVALID_OBJECT_INFO, trans);
        return false;
    }

    // This is a virtual install storage, not an SD staging folder. Large NSPs
    // are consumed directly from USB and written to NCM ContentStorage, so FAT32
    // never has to hold a 4+ GiB temporary file.
    pendingName = name;
    pendingSize = size;
    pendingSizeKnown = sizeKnown;
    pendingFormat = format;
    pendingFinalPath.clear();
    pendingPartPath.clear();
    pendingPartCount = 0;

    // Every MTP object creation must receive a fresh handle. Reusing handle 1
    // makes Windows cache the previous virtual install object and can confuse
    // back-to-back uploads (especially batches of DLCs).
    pendingObjectHandle = nextObjectHandle++;
    if (pendingObjectHandle == 0 || pendingObjectHandle == 0xFFFFFFFFU) {
        pendingObjectHandle = 1;
        nextObjectHandle = 2;
    }

    pendingValid = true;
    transferCancelRequested.store(false, std::memory_order_release);

    {
        std::scoped_lock lock(statusMutex);
        status.currentFile = name;
        status.lastStagedPath.clear();
        status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
        status.transfer = {};
        status.installStage = "Waiting for NSP data";
        status.installCurrent = 0;
        status.installTotal = sizeKnown ? size : 0;
        status.installContentIndex = 0;
        status.installContentCount = 0;
        status.lastInstallSucceeded = false;
    }
    setState(ServerState::Connected);
    sendResponse(RES_OK, trans, {STORAGE_ID, ROOT_PARENT, currentObjectHandle()});
    return true;
}

void NativeMtpServer::removePendingParts()
{
    if (pendingFinalPath.empty())
        return;

    const std::filesystem::path base(pendingFinalPath);
    const auto dir = base.parent_path();
    const std::string name = base.filename().string();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec)
        return;

    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::string candidate = it->path().filename().string();
        const bool first = candidate == name + ".partial";
        const bool split = candidate.size() > name.size() + 9 &&
                           candidate.compare(0, name.size() + 1, name + ".") == 0 &&
                           candidate.size() >= 8 &&
                           candidate.compare(candidate.size() - 8, 8, ".partial") == 0;
        if (first || split) {
            std::error_code removeEc;
            std::filesystem::remove(it->path(), removeEc);
        }
    }
}

bool NativeMtpServer::commitPendingParts(uint32_t partCount)
{
    if (pendingFinalPath.empty() || partCount == 0)
        return false;

    // Remove a previous logical package first, including all contiguous split parts.
    installer::PackageReader::removePackage(pendingFinalPath);

    std::vector<std::string> committed;
    committed.reserve(partCount);
    for (uint32_t i = 0; i < partCount; ++i) {
        const std::string source = splitTempPath(pendingFinalPath, i);
        const std::string destination = splitFinalPath(pendingFinalPath, i);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec) || ec) {
            for (const auto& path : committed) {
                std::error_code rollbackEc;
                std::filesystem::remove(path, rollbackEc);
            }
            removePendingParts();
            return false;
        }

        ec.clear();
        std::filesystem::remove(destination, ec);
        ec.clear();
        std::filesystem::rename(source, destination, ec);
        if (ec) {
            for (const auto& path : committed) {
                std::error_code rollbackEc;
                std::filesystem::remove(path, rollbackEc);
            }
            removePendingParts();
            return false;
        }
        committed.push_back(destination);
    }

    // Remove stale trailing parts from an interrupted/older copy with the same name.
    // The loop starts immediately after the new package and stops at the first gap.
    for (uint32_t i = partCount; i < partCount + 1024; ++i) {
        const std::string extra = splitFinalPath(pendingFinalPath, i);
        std::error_code ec;
        if (!std::filesystem::exists(extra, ec) || ec)
            break;
        std::filesystem::remove(extra, ec);
        if (ec)
            return false;
    }
    return true;
}

void NativeMtpServer::handleSendObjectInfo(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (installerBusy.load(std::memory_order_acquire)) {
        sendResponse(RES_DEVICE_BUSY, trans);
        return;
    }
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
        (parent != 0 && parent != 0xFFFFFFFF) || !nameOk) {
        setDiagnostic(installer::makeDiagnostic(installer::ErrorCode::UnsupportedFormat,
            "MTP object rechazado: " + name));
        sendResponse(format == FORMAT_ASSOCIATION ? RES_INVALID_OBJECT_FORMAT : RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    preparePendingObject(name, size32 == 0xFFFFFFFFU ? 0 : static_cast<uint64_t>(size32),
                         size32 != 0xFFFFFFFFU, format, trans);
}

void NativeMtpServer::handleSendObjectPropList(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (installerBusy.load(std::memory_order_acquire)) {
        sendResponse(RES_DEVICE_BUSY, trans);
        return;
    }
    if (params.size() < 5) {
        sendResponse(RES_INVALID_PARAMETER, trans);
        return;
    }
    const uint32_t storage = params[0];
    const uint32_t parent = params[1];
    const uint16_t format = static_cast<uint16_t>(params[2]);
    const uint64_t objectSize = (static_cast<uint64_t>(params[3]) << 32) | params[4];
    if ((storage != 0 && storage != STORAGE_ID) ||
        (parent != 0 && parent != 0xFFFFFFFFU)) {
        sendResponse(storage != 0 && storage != STORAGE_ID ? RES_INVALID_STORAGE_ID : RES_INVALID_PARENT, trans);
        return;
    }

    ContainerHeader dataHeader{};
    if (!readExact(&dataHeader, sizeof(dataHeader), UINT64_MAX, false))
        return;
    if (dataHeader.type != CONTAINER_DATA || dataHeader.code != OP_SEND_OBJECT_PROP_LIST ||
        dataHeader.transaction != trans || dataHeader.length < sizeof(ContainerHeader) + 4 ||
        dataHeader.length > sizeof(ContainerHeader) + 256 * 1024) {
        if (dataHeader.length > sizeof(ContainerHeader) && dataHeader.length < 1024 * 1024)
            drainExact(dataHeader.length - sizeof(ContainerHeader), false);
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    std::vector<uint8_t> data(dataHeader.length - sizeof(ContainerHeader));
    if (!readExact(data.data(), data.size(), UINT64_MAX, false))
        return;
    if (data.size() < 4) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    const uint32_t count = read32(data.data());
    if (count > 256) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }
    size_t offset = 4;
    std::string name;
    uint64_t sizeFromProp = 0;
    bool haveSizeProp = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 8 > data.size()) {
            sendResponse(RES_INVALID_OBJECT_INFO, trans);
            return;
        }
        const uint32_t objectHandleInList = read32(data.data() + offset);
        (void)objectHandleInList; // creation property lists may use a provisional/non-zero handle
        const uint16_t prop = read16(data.data() + offset + 4);
        const uint16_t type = read16(data.data() + offset + 6);
        offset += 8;
        bool valueOk = false;
        const size_t valueSize = mtpValueSize(type, data.data() + offset, data.size() - offset, valueOk);
        if (!valueOk || valueSize == 0 || offset + valueSize > data.size()) {
            sendResponse(RES_INVALID_OBJECT_INFO, trans);
            return;
        }

        if ((prop == PROP_OBJECT_FILE_NAME) && type == TYPE_STRING) {
            bool stringOk = false;
            const std::string candidate = utf16Name(data.data() + offset, valueSize, stringOk);
            if (stringOk && !candidate.empty())
                name = candidate;
        } else if (prop == PROP_OBJECT_SIZE && type == TYPE_UINT64 && valueSize == 8) {
            sizeFromProp = read64(data.data() + offset);
            haveSizeProp = true;
        }
        offset += valueSize;
    }
    if (offset != data.size() || name.empty()) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }
    if (haveSizeProp && sizeFromProp != objectSize) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    preparePendingObject(name, objectSize, true, format, trans);
}


void NativeMtpServer::handleSetObjectPropValue(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (params.size() < 2 || params[0] != currentObjectHandle() || !currentObjectExists()) {
        sendResponse(RES_INVALID_OBJECT_HANDLE, trans);
        return;
    }
    const uint16_t prop = static_cast<uint16_t>(params[1]);
    if (prop != PROP_OBJECT_FILE_NAME) {
        sendResponse(RES_INVALID_OBJECT_PROP, trans);
        return;
    }
    if (installerBusy.load(std::memory_order_acquire) || snapshot().state == ServerState::Receiving ||
        snapshot().state == ServerState::Installing) {
        sendResponse(RES_DEVICE_BUSY, trans);
        return;
    }

    ContainerHeader dataHeader{};
    if (!readExact(&dataHeader, sizeof(dataHeader), UINT64_MAX, false)) return;
    if (dataHeader.type != CONTAINER_DATA || dataHeader.code != OP_SET_OBJECT_PROP_VALUE ||
        dataHeader.transaction != trans || dataHeader.length <= sizeof(ContainerHeader) ||
        dataHeader.length > sizeof(ContainerHeader) + 4096) {
        if (dataHeader.length > sizeof(ContainerHeader) && dataHeader.length < 1024 * 1024)
            drainExact(dataHeader.length - sizeof(ContainerHeader), false);
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }
    std::vector<uint8_t> data(dataHeader.length - sizeof(ContainerHeader));
    if (!readExact(data.data(), data.size(), UINT64_MAX, false)) return;
    bool ok = false;
    const std::string renamed = utf16Name(data.data(), data.size(), ok);
    if (!ok || !isSafeNspName(renamed)) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }
    pendingName = renamed;
    {
        std::scoped_lock lock(statusMutex);
        status.currentFile = renamed;
    }
    sendResponse(RES_OK, trans);
}

void NativeMtpServer::handleGetObjectPropList(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (params.size() < 5) {
        sendResponse(RES_INVALID_PARAMETER, trans);
        return;
    }
    const uint32_t handle = params[0];
    const uint16_t format = static_cast<uint16_t>(params[1]);
    const uint16_t property = static_cast<uint16_t>(params[2]);
    const uint32_t group = params[3];
    const uint32_t depth = params[4];

    if (format != 0 && format != FORMAT_UNDEFINED) {
        sendResponse(RES_INVALID_OBJECT_FORMAT, trans);
        return;
    }
    if (group != 0 || depth != 0) {
        sendResponse(RES_INVALID_PARAMETER, trans);
        return;
    }
    if (handle != currentObjectHandle() && handle != 0xFFFFFFFFU) {
        sendResponse(RES_INVALID_OBJECT_HANDLE, trans);
        return;
    }

    bool valid = false;
    auto data = makeObjectPropList(handle, property, valid);
    if (!valid) {
        sendResponse(property != 0 && property != 0xFFFFU ? RES_INVALID_OBJECT_PROP : RES_INVALID_OBJECT_HANDLE, trans);
        return;
    }
    sendData(OP_GET_OBJECT_PROP_LIST, trans, data);
    sendResponse(RES_OK, trans);
}

void NativeMtpServer::handleSendObject(uint32_t trans)
{
    if (installerBusy.load(std::memory_order_acquire)) {
        sendResponse(RES_DEVICE_BUSY, trans);
        return;
    }
    if (!pendingValid) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    ContainerHeader dataHeader{};
    if (!readExact(&dataHeader, sizeof(dataHeader), UINT64_MAX, false)) return;

    // MTP's USB data-container length field is only 32-bit.  Windows/libmtp
    // clients do not consistently use 0xFFFFFFFF for objects above 4 GiB; in
    // practice the value can simply wrap/truncate to 32 bits.  The authoritative
    // length for a modern large upload is the 64-bit size from SendObjectPropList
    // (operation 0x9808), so never reject a known >4 GiB object because of the
    // legacy container-length field.  We still consume exactly pendingSize bytes.
    const bool knownLargeObject = pendingSizeKnown &&
        pendingSize > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());

    if (dataHeader.type != CONTAINER_DATA || dataHeader.code != OP_SEND_OBJECT ||
        dataHeader.transaction != trans ||
        (!knownLargeObject && dataHeader.length != 0xFFFFFFFFU &&
         dataHeader.length < sizeof(ContainerHeader))) {
        sendResponse(RES_INVALID_OBJECT_INFO, trans);
        return;
    }

    const bool extendedLength = dataHeader.length == 0xFFFFFFFFU;
    const uint64_t headerPayloadSize = (!extendedLength && dataHeader.length >= sizeof(ContainerHeader))
        ? static_cast<uint64_t>(dataHeader.length - sizeof(ContainerHeader)) : 0;

    // For <=4 GiB objects the container size is still useful corruption
    // detection.  For >4 GiB it is inherently unable to represent the real
    // transfer and must be ignored in favour of the 64-bit object property.
    if (pendingSizeKnown && !knownLargeObject && !extendedLength && headerPayloadSize != pendingSize) {
        if (headerPayloadSize && headerPayloadSize < (8ULL * 1024ULL * 1024ULL * 1024ULL))
            drainExact(headerPayloadSize, true);
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::PackageTruncated,
            "MTP SendObject length=" + std::to_string(headerPayloadSize) +
            ", expected=" + std::to_string(pendingSize)), RES_INCOMPLETE_TRANSFER, trans);
        return;
    }
    if (!pendingSizeKnown && !extendedLength) {
        pendingSize = headerPayloadSize;
        pendingSizeKnown = true;
    }

    health.reset(pendingSizeKnown ? pendingSize : 0);
    transferCancelRequested.store(false, std::memory_order_release);
    installerBusy.store(true, std::memory_order_release);
    {
        std::scoped_lock lock(statusMutex);
        status.installStage = "Starting direct install";
        status.installCurrent = 0;
        status.installTotal = pendingSizeKnown ? pendingSize : 0;
        status.installContentIndex = 0;
        status.installContentCount = 0;
        status.lastInstallSucceeded = false;
        status.lastDiagnostic = installer::makeDiagnostic(installer::ErrorCode::None);
    }
    setState(ServerState::Receiving);

    appletSetMediaPlaybackState(true);
    if (R_SUCCEEDED(appletBeginBlockingHomeButton(0))) homeBlocked = true;

    InstallStream stream(4 * 1024 * 1024);
    installer::RealInstallResult installResult;
    std::thread installWorker([&] {
        setState(ServerState::Installing);
        installResult = installer::installNspStreamToSdTransactional(
            pendingName,
            pendingSizeKnown ? pendingSize : 0,
            [&](void* out, size_t size) {
                return stream.readExact(out, size);
            },
            [&](uint64_t current, uint64_t total, const std::string& stage,
                uint32_t contentIndex, uint32_t contentCount) {
                std::scoped_lock lock(statusMutex);
                status.installCurrent = current;
                status.installTotal = total;
                status.installStage = stage;
                status.installContentIndex = contentIndex;
                status.installContentCount = contentCount;
            });
        stream.finishConsumer();
    });

    uint64_t totalReceived = 0;
    bool readFailed = false;
    bool pushFailed = false;

    while (!stopRequested.load(std::memory_order_acquire) &&
           !transferCancelRequested.load(std::memory_order_acquire)) {
        if (pendingSizeKnown && totalReceived >= pendingSize) break;

        if (rxOffset >= rxAvailable) {
            // Unknown-size legacy SendObjectInfo ends on a short USB transfer/ZLP.
            if (!pendingSizeKnown && rxShortPacket && rxAvailable > 0) break;
            if (!receiveIntoBuffer(UINT64_MAX, true)) {
                readFailed = true;
                break;
            }
            if (rxAvailable == 0) {
                if (!pendingSizeKnown && rxShortPacket) break;
                readFailed = true;
                break;
            }
        }

        uint64_t available = static_cast<uint64_t>(rxAvailable - rxOffset);
        if (pendingSizeKnown) available = std::min<uint64_t>(available, pendingSize - totalReceived);
        if (!available) continue;

        const size_t chunk = static_cast<size_t>(available);
        auto* src = static_cast<uint8_t*>(rxBuffer) + rxOffset;
        health.addReceived(chunk);
        if (!stream.push(src, chunk)) {
            pushFailed = true;
            break;
        }
        rxOffset += chunk;
        totalReceived += chunk;

        if (!pendingSizeKnown && rxOffset >= rxAvailable && rxShortPacket) break;
    }

    if (stopRequested.load(std::memory_order_acquire) ||
        transferCancelRequested.load(std::memory_order_acquire) || pushFailed) {
        stream.cancel();
    } else {
        stream.closeProducer();
    }
    if (installWorker.joinable()) installWorker.join();
    unlockHome();
    installerBusy.store(false, std::memory_order_release);

    if (stopRequested.load(std::memory_order_acquire) ||
        transferCancelRequested.load(std::memory_order_acquire)) {
        failTransfer(installer::makeDiagnostic(installer::ErrorCode::Cancelled,
            "MTP streaming install cancelled"), RES_INCOMPLETE_TRANSFER, trans, false);
        return;
    }

    if (readFailed || pushFailed || (pendingSizeKnown && totalReceived != pendingSize)) {
        installer::Diagnostic d = installResult.diagnostic;
        if (d.ok()) {
            UsbState usbState = UsbState_Detached;
            if (R_FAILED(usbDsGetState(&usbState)) || usbState != UsbState_Configured)
                d = installer::makeDiagnostic(installer::ErrorCode::UsbDisconnected,
                    "received=" + std::to_string(totalReceived) +
                    ", expected=" + std::to_string(pendingSize));
            else
                d = installer::makeDiagnostic(installer::ErrorCode::PackageTruncated,
                    "received=" + std::to_string(totalReceived) +
                    ", expected=" + std::to_string(pendingSize));
        }
        failTransfer(d, RES_INCOMPLETE_TRANSFER, trans, false);
        return;
    }

    const auto transfer = health.snapshot();
    if (!installResult.ok()) {
        setDiagnostic(installResult.diagnostic);
        installer::writeDiagnosticLog(installResult.diagnostic, installer::getSystemSnapshot(), pendingName,
                                      nullptr, &transfer);
        {
            std::scoped_lock lock(statusMutex);
            status.transfer = transfer;
            status.installStage = installResult.diagnostic.title.empty()
                ? "Installation failed" : installResult.diagnostic.title;
            status.lastInstallSucceeded = false;
        }
        setState(ServerState::Error);
        uint16_t response = RES_GENERAL_ERROR;
        if (installResult.diagnostic.code == installer::ErrorCode::NotEnoughSpace) response = RES_STORE_FULL;
        else if (installResult.diagnostic.code == installer::ErrorCode::UnsupportedFormat ||
                 installResult.diagnostic.code == installer::ErrorCode::InvalidPfs0 ||
                 installResult.diagnostic.code == installer::ErrorCode::InvalidNca)
            response = RES_INVALID_OBJECT_INFO;
        else if (installResult.diagnostic.code == installer::ErrorCode::PackageTruncated ||
                 installResult.diagnostic.code == installer::ErrorCode::UsbDisconnected)
            response = RES_INCOMPLETE_TRANSFER;
        const uint32_t failedHandle = currentObjectHandle();
        const std::string failedName = pendingName;
        sendResponse(response, trans);
        resetPendingObject();
        {
            std::scoped_lock lock(statusMutex);
            status.currentFile = failedName;
            status.lastFailedFile = failedName;
            ++status.sessionFailedCount;
        }
        if (failedHandle != 0)
            sendEvent(EVENT_OBJECT_REMOVED, {failedHandle});
        return;
    }

    const auto transferResult = health.evaluate(true);
    const std::string completedName = pendingName;
    {
        std::scoped_lock lock(statusMutex);
        status.transfer = transfer;
        status.installStage = completedName.empty()
            ? "Installation completed" : "Installed: " + completedName;
        status.installCurrent = pendingSizeKnown ? pendingSize : totalReceived;
        status.installTotal = pendingSizeKnown ? pendingSize : totalReceived;
        status.lastInstallSucceeded = true;
        status.lastDiagnostic = transferResult.code == installer::ErrorCode::UsbSlow
            ? transferResult : installResult.diagnostic;
        status.lastStagedPath.clear();
        status.currentFile = completedName;
        status.lastCompletedFile = completedName;
        ++status.sessionInstalledCount;
    }
    const uint32_t completedHandle = currentObjectHandle();
    resetPendingObject();
    setState(ServerState::Connected, "Installation completed");

    // AIO's Install storage is a write-only virtual sink: after SendObject has
    // completed there is no file to enumerate. Tell the host to drop the
    // provisional object it cached from SendObjectInfo/SendObjectPropList.
    sendResponse(RES_OK, trans);
    if (completedHandle != 0)
        sendEvent(EVENT_OBJECT_REMOVED, {completedHandle});
}

void NativeMtpServer::handleDeleteObject(uint32_t trans, const std::vector<uint32_t>& params)
{
    if (installerBusy.load(std::memory_order_acquire) || snapshot().state == ServerState::Receiving ||
        snapshot().state == ServerState::Installing) {
        sendResponse(RES_DEVICE_BUSY, trans);
        return;
    }
    if (params.empty() || params[0] != currentObjectHandle() || !currentObjectExists()) {
        sendResponse(RES_INVALID_OBJECT_HANDLE, trans);
        return;
    }
    // Virtual install objects only exist between SendObjectInfo/PropList and
    // SendObject. Deleting one simply cancels that pending host object.
    const uint32_t removedHandle = currentObjectHandle();
    resetPendingObject();
    {
        std::scoped_lock lock(statusMutex);
        status.currentFile.clear();
    }
    sendResponse(RES_OK, trans);
    if (removedHandle != 0)
        sendEvent(EVENT_OBJECT_REMOVED, {removedHandle});
}

void NativeMtpServer::failTransfer(const installer::Diagnostic& diagnostic, uint16_t mtpResponse,
                                   uint32_t transaction, bool deletePartial)
{
    const uint32_t failedHandle = currentObjectHandle();
    const std::string failedName = pendingName;
    unlockHome();
    const auto transfer = health.snapshot();
    setDiagnostic(diagnostic);
    if (deletePartial)
        removePendingParts();
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
    {
        std::scoped_lock lock(statusMutex);
        status.currentFile = failedName;
        status.lastFailedFile = failedName;
        status.installStage = diagnostic.title.empty() ? "Transfer/install failed" : diagnostic.title;
        status.lastInstallSucceeded = false;
        ++status.sessionFailedCount;
    }
    if (failedHandle != 0 && usbInitialized && usbState == UsbState_Configured)
        sendEvent(EVENT_OBJECT_REMOVED, {failedHandle});
}

void NativeMtpServer::resetPendingObject()
{
    pendingName.clear();
    pendingPartPath.clear();
    pendingFinalPath.clear();
    pendingSize = 0;
    pendingSizeKnown = false;
    pendingFormat = 0;
    pendingValid = false;
    pendingPartCount = 0;
    pendingObjectHandle = 0;
    rxShortPacket = false;
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
    return pendingValid;
}

std::string NativeMtpServer::currentObjectName() const
{
    return pendingValid ? pendingName : std::string{};
}

uint64_t NativeMtpServer::currentObjectSize() const
{
    return pendingValid && pendingSizeKnown ? pendingSize : 0;
}

uint32_t NativeMtpServer::currentObjectHandle() const
{
    return pendingValid ? pendingObjectHandle : 0;
}

CleanupResult NativeMtpServer::cleanupTemporaryFiles()
{
    CleanupResult result{};
    const std::array<const char*, 2> dirs = {
        INBOX_DIR,
        "/switch/aio-switch-updater-mod/config/install_work",
    };

    // Never race a live SendObject writer. The UI disables this action conceptually,
    // but guard it here as well because the server API may be called elsewhere later.
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

    const bool removed = installer::PackageReader::removePackage(path);
    if (removed) {
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

} // namespace mtp
