#include "installer_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace installer {
namespace {

constexpr const char LOG_DIR[] = "/switch/aio-switch-updater-mod/config/install_logs";
constexpr const char LAST_LOG[] = "/switch/aio-switch-updater-mod/config/install_logs/last.log";
constexpr uint32_t MAX_PFS0_FILES = 0x10000;
constexpr uint32_t MAX_STRING_TABLE = 32 * 1024 * 1024;

#pragma pack(push, 1)
struct Pfs0Header {
    char magic[4];
    uint32_t fileCount;
    uint32_t stringTableSize;
    uint32_t reserved;
};

struct Pfs0Entry {
    uint64_t offset;
    uint64_t size;
    uint32_t stringOffset;
    uint32_t reserved;
};
#pragma pack(pop)

uint64_t monotonicNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool checkedAdd(uint64_t a, uint64_t b, uint64_t& out)
{
    if (a > std::numeric_limits<uint64_t>::max() - b)
        return false;
    out = a + b;
    return true;
}

std::string lowerExt(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::string resultHex(Result rc)
{
    std::ostringstream ss;
    ss << "0x" << std::uppercase << std::hex << static_cast<uint32_t>(rc);
    return ss.str();
}

std::string sanitizeOneLine(std::string value)
{
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

} 

Diagnostic makeDiagnostic(ErrorCode code, const std::string& technical, Result result)
{
    Diagnostic d;
    d.code = code;
    d.technical = technical;
    d.result = result;

    switch (code) {
        case ErrorCode::None:
            d.aioCode = "AIO-INSTALL-OK";
            d.title = "Sin errores detectados";
            d.message = "La comprobación terminó correctamente.";
            break;
        case ErrorCode::NotEnoughSpace:
            d.aioCode = "AIO-INSTALL-STG-001";
            d.title = "Espacio insuficiente";
            d.message = "No hay suficiente espacio disponible en el destino para completar la operación.";
            d.advice = "Libera espacio en la tarjeta SD o selecciona otro destino y vuelve a intentarlo.";
            break;
        case ErrorCode::StorageSlow:
            d.aioCode = "AIO-INSTALL-STG-003";
            d.title = "Almacenamiento demasiado lento";
            d.message = "La velocidad de escritura no puede seguir el ritmo de la transferencia USB.";
            d.advice = "Comprueba la tarjeta SD, su formato y su estado. Una SD dañada o muy lenta puede provocar interrupciones.";
            break;
        case ErrorCode::WriteError:
            d.aioCode = "AIO-INSTALL-STG-002";
            d.title = "Error de escritura";
            d.message = "Se produjo un error al escribir los datos en el almacenamiento.";
            d.advice = "Comprueba la tarjeta SD y reinicia la consola antes de volver a intentarlo.";
            break;
        case ErrorCode::UnsupportedFormat:
            d.aioCode = "AIO-INSTALL-PKG-000";
            d.title = "Formato no compatible";
            d.message = "El archivo seleccionado no es un paquete compatible con esta etapa del instalador.";
            d.advice = "El instalador actual valida paquetes NSP. Otros formatos de contenedor no están soportados.";
            break;
        case ErrorCode::InvalidPfs0:
            d.aioCode = "AIO-INSTALL-PKG-001";
            d.title = "NSP no válido";
            d.message = "La estructura PFS0 del paquete no es válida.";
            d.advice = "Vuelve a obtener o extraer el archivo. Si proviene de un RAR/ZIP, comprueba que la extracción haya terminado sin errores.";
            break;
        case ErrorCode::PackageTruncated:
            d.aioCode = "AIO-INSTALL-PKG-003";
            d.title = "Paquete incompleto";
            d.message = "El archivo termina antes de los tamaños declarados por el contenedor.";
            d.advice = "Es muy probable que la descarga o la extracción del RAR/ZIP esté incompleta. Vuelve a extraer el archivo original.";
            break;
        case ErrorCode::MissingContent:
            d.aioCode = "AIO-INSTALL-PKG-004";
            d.title = "Contenido faltante";
            d.message = "El paquete no contiene todos los contenidos requeridos por sus metadatos.";
            d.advice = "Obtén de nuevo el paquete completo y verifica su integridad.";
            break;
        case ErrorCode::InvalidNca:
            d.aioCode = "AIO-INSTALL-PKG-002";
            d.title = "Contenido NCA no válido";
            d.message = "Uno de los contenidos NCA no pudo validarse correctamente.";
            d.advice = "El archivo puede estar dañado o ser incompatible con la versión actual del sistema.";
            break;
        case ErrorCode::FirmwareTooOld:
            d.aioCode = "AIO-INSTALL-SYS-001";
            d.title = "Firmware demasiado antiguo";
            d.message = "El contenido requiere una versión de sistema más reciente.";
            d.advice = "Actualiza el firmware y el CFW a versiones compatibles antes de volver a instalar.";
            break;
        case ErrorCode::AuthorizationRejected:
            d.aioCode = "AIO-INSTALL-AUTH-001";
            d.title = "Validación rechazada";
            d.message = "Horizon rechazó la autorización o validación necesaria para registrar el contenido.";
            d.advice = "Comprueba que tu configuración de CFW sea compatible con tu firmware. AIO MOD no omite las validaciones criptográficas del sistema.";
            break;
        case ErrorCode::UsbDisconnected:
            d.aioCode = "AIO-INSTALL-USB-001";
            d.title = "USB desconectado";
            d.message = "La conexión USB se perdió antes de terminar la transferencia.";
            d.advice = "Comprueba el cable USB-C y el puerto del PC. Evita hubs de baja calidad.";
            break;
        case ErrorCode::UsbTimeout:
            d.aioCode = "AIO-INSTALL-USB-002";
            d.title = "Tiempo de espera USB agotado";
            d.message = "El PC dejó de enviar datos durante demasiado tiempo.";
            d.advice = "Prueba otro cable o puerto USB y evita que el PC entre en suspensión durante la transferencia.";
            break;
        case ErrorCode::UsbUnstable:
            d.aioCode = "AIO-INSTALL-USB-003";
            d.title = "Conexión USB inestable";
            d.message = "Se detectaron errores o interrupciones repetidas durante la transferencia.";
            d.advice = "Prueba otro cable USB-C o conecta la consola directamente a otro puerto del PC.";
            break;
        case ErrorCode::UsbSlow:
            d.aioCode = "AIO-INSTALL-USB-004";
            d.title = "Velocidad USB baja";
            d.message = "La transferencia USB es inusualmente lenta.";
            d.advice = "Esto es una advertencia, no un fallo por sí mismo. Un cable USB 2.0 o un hub puede limitar la velocidad.";
            break;
        case ErrorCode::PlaceholderError:
            d.aioCode = "AIO-INSTALL-NCM-001";
            d.title = "No se pudo reservar la instalación";
            d.message = "Horizon no pudo crear el placeholder necesario para instalar el contenido.";
            d.advice = "Comprueba el espacio libre y reinicia la consola si existe una instalación incompleta pendiente.";
            break;
        case ErrorCode::RegistrationError:
            d.aioCode = "AIO-INSTALL-NCM-002";
            d.title = "No se pudo registrar el contenido";
            d.message = "Los datos se transfirieron, pero Horizon no pudo registrar correctamente la aplicación.";
            d.advice = "Reinicia la consola y vuelve a intentar la instalación.";
            break;
        case ErrorCode::AlreadyInstalled:
            d.aioCode = "AIO-INSTALL-NCM-003";
            d.title = "Contenido ya instalado";
            d.message = "La misma versión, una versión más nueva o una instalación base que no debe reemplazarse ya existe en la consola.";
            d.advice = "Para updates/DLC usa una versión superior. La sustitución completa del juego base permanece bloqueada por seguridad.";
            break;
        case ErrorCode::MtpObjectTooLarge:
            d.aioCode = "AIO-INSTALL-MTP-001";
            d.title = "NSP demasiado grande para MTP";
            d.message = "El transporte MTP actual admite paquetes de hasta 4 GiB.";
            d.advice = "Usa un NSP menor de 4 GiB con el transporte MTP actual.";
            break;
        case ErrorCode::Cancelled:
            d.aioCode = "AIO-INSTALL-CANCELLED";
            d.title = "Instalación cancelada";
            d.message = "La operación fue cancelada por el usuario.";
            break;
        case ErrorCode::InternalError:
        default:
            d.aioCode = "AIO-INSTALL-INT-001";
            d.title = "Error interno del instalador";
            d.message = "Se produjo un error inesperado durante la operación.";
            d.advice = "Guarda el código y el log para poder diagnosticar el problema.";
            break;
    }

    if (result != 0) {
        if (!d.technical.empty())
            d.technical += " | ";
        d.technical += "Result=" + resultHex(result);
    }
    return d;
}

std::string formatDiagnostic(const Diagnostic& d)
{
    std::string out = d.title + "\n\n" + d.message;
    if (!d.advice.empty())
        out += "\n\n" + d.advice;
    out += "\n\nCódigo: " + d.aioCode;
    if (!d.technical.empty())
        out += "\nDetalle: " + d.technical;
    return out;
}

SystemSnapshot getSystemSnapshot()
{
    SystemSnapshot out;

    SetSysFirmwareVersion fw{};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
        out.firmware = fw.display_version;
    if (out.firmware.empty())
        out.firmware = "Unknown";

    s64 freeBytes = 0;
    if (R_SUCCEEDED(nsGetFreeSpaceSize(NcmStorageId_SdCard, &freeBytes)))
        out.sdFreeBytes = freeBytes;

    return out;
}

PackagePreflight preflightPackage(const std::string& path, bool checkSdSpace)
{
    PackagePreflight out;
    const std::filesystem::path fsPath(path);
    const std::string ext = lowerExt(fsPath);
    out.packageType = ext.empty() ? "unknown" : ext.substr(1);

    if (ext != ".nsp") {
        out.diagnostic = makeDiagnostic(ErrorCode::UnsupportedFormat, "extension=" + ext);
        return out;
    }

    std::error_code ec;
    const uint64_t fileSize = std::filesystem::file_size(fsPath, ec);
    if (ec || fileSize < sizeof(Pfs0Header)) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "No se pudo leer el tamaño del NSP o es demasiado pequeño");
        return out;
    }
    out.packageSize = fileSize;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        out.diagnostic = makeDiagnostic(ErrorCode::InternalError, "No se pudo abrir el archivo: " + path);
        return out;
    }

    Pfs0Header header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.magic, "PFS0", 4) != 0) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Magic PFS0 ausente");
        return out;
    }

    if (header.fileCount == 0 || header.fileCount > MAX_PFS0_FILES || header.stringTableSize > MAX_STRING_TABLE) {
        out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0,
            "file_count=" + std::to_string(header.fileCount) + ", string_table=" + std::to_string(header.stringTableSize));
        return out;
    }
    out.fileCount = header.fileCount;

    uint64_t entriesBytes = static_cast<uint64_t>(header.fileCount) * sizeof(Pfs0Entry);
    uint64_t tableStart = 0;
    uint64_t dataStart = 0;
    if (!checkedAdd(sizeof(Pfs0Header), entriesBytes, tableStart) ||
        !checkedAdd(tableStart, header.stringTableSize, dataStart) ||
        dataStart > fileSize) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "La tabla PFS0 excede el tamaño real del archivo");
        return out;
    }

    std::vector<Pfs0Entry> entries(header.fileCount);
    file.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entriesBytes));
    if (!file) {
        out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "Tabla de archivos PFS0 incompleta");
        return out;
    }

    std::vector<char> strings(header.stringTableSize + 1, '\0');
    if (header.stringTableSize > 0) {
        file.read(strings.data(), static_cast<std::streamsize>(header.stringTableSize));
        if (!file) {
            out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated, "Tabla de nombres PFS0 incompleta");
            return out;
        }
    }

    uint64_t payloadEnd = 0;
    for (uint32_t i = 0; i < header.fileCount; ++i) {
        const auto& e = entries[i];
        if (e.stringOffset >= header.stringTableSize) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Índice de nombre fuera de rango en entrada " + std::to_string(i));
            return out;
        }

        const char* name = strings.data() + e.stringOffset;
        const size_t remaining = header.stringTableSize - e.stringOffset;
        if (std::memchr(name, '\0', remaining) == nullptr) {
            out.diagnostic = makeDiagnostic(ErrorCode::InvalidPfs0, "Nombre PFS0 sin terminador en entrada " + std::to_string(i));
            return out;
        }

        uint64_t entryEnd = 0;
        if (!checkedAdd(e.offset, e.size, entryEnd) || entryEnd > fileSize - dataStart) {
            out.diagnostic = makeDiagnostic(ErrorCode::PackageTruncated,
                "Contenido fuera del archivo: " + sanitizeOneLine(name) +
                " (offset=" + std::to_string(e.offset) + ", size=" + std::to_string(e.size) + ")");
            return out;
        }
        payloadEnd = std::max(payloadEnd, entryEnd);
    }

    out.declaredPayloadSize = payloadEnd;

    if (checkSdSpace) {
        const auto system = getSystemSnapshot();

        if (system.sdFreeBytes >= 0 && payloadEnd > static_cast<uint64_t>(system.sdFreeBytes)) {
            out.diagnostic = makeDiagnostic(ErrorCode::NotEnoughSpace,
                "required=" + std::to_string(payloadEnd) + ", free=" + std::to_string(system.sdFreeBytes));
            return out;
        }
    }

    out.diagnostic = makeDiagnostic(ErrorCode::None,
        "PFS0 OK; files=" + std::to_string(header.fileCount) + ", payload=" + std::to_string(payloadEnd));
    return out;
}

void TransferHealthMonitor::reset(uint64_t expected)
{
    const uint64_t now = monotonicNs();
    receivedBytes.store(0, std::memory_order_relaxed);
    writtenBytes.store(0, std::memory_order_relaxed);
    expectedBytes.store(expected, std::memory_order_relaxed);
    usbStalls.store(0, std::memory_order_relaxed);
    usbErrors.store(0, std::memory_order_relaxed);
    disconnected.store(false, std::memory_order_relaxed);
    startedNs.store(now, std::memory_order_relaxed);
    lastReceivedNs.store(now, std::memory_order_relaxed);
}

void TransferHealthMonitor::addReceived(uint64_t bytes)
{
    receivedBytes.fetch_add(bytes, std::memory_order_relaxed);
    lastReceivedNs.store(monotonicNs(), std::memory_order_relaxed);
}

void TransferHealthMonitor::addWritten(uint64_t bytes)
{
    writtenBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void TransferHealthMonitor::addUsbStall()
{
    usbStalls.fetch_add(1, std::memory_order_relaxed);
}

void TransferHealthMonitor::addUsbError()
{
    usbErrors.fetch_add(1, std::memory_order_relaxed);
}

void TransferHealthMonitor::markDisconnected()
{
    disconnected.store(true, std::memory_order_relaxed);
}

TransferSnapshot TransferHealthMonitor::snapshot() const
{
    TransferSnapshot out;
    out.receivedBytes = receivedBytes.load(std::memory_order_relaxed);
    out.writtenBytes = writtenBytes.load(std::memory_order_relaxed);
    out.expectedBytes = expectedBytes.load(std::memory_order_relaxed);
    out.usbStalls = usbStalls.load(std::memory_order_relaxed);
    out.usbErrors = usbErrors.load(std::memory_order_relaxed);

    const uint64_t start = startedNs.load(std::memory_order_relaxed);
    const uint64_t elapsedNs = std::max<uint64_t>(1, monotonicNs() - start);
    const double seconds = static_cast<double>(elapsedNs) / 1'000'000'000.0;
    out.usbBytesPerSecond = static_cast<double>(out.receivedBytes) / seconds;
    out.storageBytesPerSecond = static_cast<double>(out.writtenBytes) / seconds;
    return out;
}

Diagnostic TransferHealthMonitor::evaluate(bool transferFinished) const
{
    const auto s = snapshot();
    if (disconnected.load(std::memory_order_relaxed))
        return makeDiagnostic(ErrorCode::UsbDisconnected,
            "received=" + std::to_string(s.receivedBytes) + ", expected=" + std::to_string(s.expectedBytes));

    if (s.usbErrors > 0)
        return makeDiagnostic(ErrorCode::UsbUnstable, "usb_errors=" + std::to_string(s.usbErrors));

    if (transferFinished && s.expectedBytes > 0 && s.receivedBytes < s.expectedBytes)
        return makeDiagnostic(ErrorCode::UsbDisconnected,
            "received=" + std::to_string(s.receivedBytes) + ", expected=" + std::to_string(s.expectedBytes));

    
    
    constexpr uint64_t USB_TIMEOUT_NS = 20ULL * 1'000'000'000ULL;
    const uint64_t lastRx = lastReceivedNs.load(std::memory_order_relaxed);
    if (!transferFinished && s.receivedBytes > 0 && monotonicNs() - lastRx >= USB_TIMEOUT_NS)
        return makeDiagnostic(ErrorCode::UsbTimeout,
            "idle_ns=" + std::to_string(monotonicNs() - lastRx));

    constexpr double SLOW_USB = 3.0 * 1024.0 * 1024.0;
    if (s.receivedBytes > 64 * 1024 * 1024ULL && s.storageBytesPerSecond > 0.0 &&
        s.usbBytesPerSecond > s.storageBytesPerSecond * 2.5 && s.usbStalls >= 3)
        return makeDiagnostic(ErrorCode::StorageSlow,
            "usb_bps=" + std::to_string(static_cast<uint64_t>(s.usbBytesPerSecond)) +
            ", storage_bps=" + std::to_string(static_cast<uint64_t>(s.storageBytesPerSecond)));

    if (s.receivedBytes > 64 * 1024 * 1024ULL && s.usbBytesPerSecond > 0.0 && s.usbBytesPerSecond < SLOW_USB)
        return makeDiagnostic(ErrorCode::UsbSlow,
            "avg_usb_bps=" + std::to_string(static_cast<uint64_t>(s.usbBytesPerSecond)));

    return makeDiagnostic(ErrorCode::None);
}

std::string getLogDirectory() { return LOG_DIR; }
std::string getLastLogPath() { return LAST_LOG; }

std::string writeDiagnosticLog(const Diagnostic& diagnostic,
                               const SystemSnapshot& system,
                               const std::string& packagePath,
                               const PackagePreflight* preflight,
                               const TransferSnapshot* transfer)
{
    std::error_code ec;
    std::filesystem::create_directories(LOG_DIR, ec);

    std::ofstream out(LAST_LOG, std::ios::trunc);
    if (!out)
        return {};

    out << "AIO MOD Installer diagnostics\n";
    out << "Firmware: " << system.firmware << "\n";
    out << "SD free bytes: " << system.sdFreeBytes << "\n";
    if (!packagePath.empty())
        out << "Package: " << packagePath << "\n";

    if (preflight) {
        out << "Package type: " << preflight->packageType << "\n";
        out << "Package size: " << preflight->packageSize << "\n";
        out << "PFS0 files: " << preflight->fileCount << "\n";
        out << "Declared payload: " << preflight->declaredPayloadSize << "\n";
    }

    if (transfer) {
        out << "USB received: " << transfer->receivedBytes << "\n";
        out << "Storage written: " << transfer->writtenBytes << "\n";
        out << "Expected: " << transfer->expectedBytes << "\n";
        out << "USB avg B/s: " << static_cast<uint64_t>(transfer->usbBytesPerSecond) << "\n";
        out << "Storage avg B/s: " << static_cast<uint64_t>(transfer->storageBytesPerSecond) << "\n";
        out << "USB stalls: " << transfer->usbStalls << "\n";
        out << "USB errors: " << transfer->usbErrors << "\n";
    }

    out << "\nResult: " << diagnostic.aioCode << "\n";
    out << "Title: " << diagnostic.title << "\n";
    out << "Message: " << diagnostic.message << "\n";
    out << "Advice: " << diagnostic.advice << "\n";
    out << "Technical: " << diagnostic.technical << "\n";
    if (diagnostic.result)
        out << "Horizon result: " << resultHex(diagnostic.result) << "\n";

    return LAST_LOG;
}

} 
