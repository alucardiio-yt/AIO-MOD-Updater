#include "file_explorer_ops.hpp"

#include <minizip/unzip.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>

namespace explorer {
namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string error(const fs::path& p, const std::error_code& ec) {
    return p.filename().string() + ": " + ec.message();
}

bool inside(const fs::path& child, const fs::path& parent) {
    const auto c = clean(child).generic_string();
    const auto p = clean(parent).generic_string();
    return p == "/" || c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/');
}

void validSource(const fs::path& p) {
    require(isSdPath(p) && clean(p) != fs::path("/"), "Ruta fuera de la microSD");
    std::error_code ec;
    const bool exists = fs::exists(p, ec);
    require(!ec, p.string() + ": " + ec.message());
    require(exists, "Archivo no encontrado: " + p.string());
    require(!fs::is_symlink(p, ec), "No se permiten enlaces simbolicos");
}

fs::path tempFor(const fs::path& target) {
    for (unsigned n = 0; n < 1000; ++n) {
        const auto tmp = target.parent_path() / (".aio-copy-" + std::to_string(n) + ".tmp");
        if (tmp == target) continue;
        std::error_code ec;
        const bool exists = fs::exists(tmp, ec);
        require(!ec, error(tmp, ec));
        if (!exists) return tmp;
    }
    throw std::runtime_error("No se pudo reservar un archivo temporal");
}

void copyFile(const fs::path& src, const fs::path& dst, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed) {
    std::ifstream input(src, std::ios::binary);
    require(input.good(), "No se pudo leer: " + src.string());
    std::error_code sizeError;
    const auto expectedSize = fs::file_size(src, sizeError);
    require(!sizeError, error(src, sizeError));
    const auto tmp = tempFor(dst);
    try {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        require(output.good(), "No se pudo crear: " + dst.string());
        // std::thread uses a small stack on Switch. Keep the transfer buffer
        // on the heap so copying a file cannot overflow the worker stack.
        std::vector<char> buffer(64 * 1024);
        std::uintmax_t copied = 0;
        while (input && !cancel.load()) {
            input.read(buffer.data(), buffer.size());
            const auto count = input.gcount();
            if (!count) break;
            output.write(buffer.data(), count);
            require(output.good(), "Error de escritura: " + dst.string());
            copied += static_cast<std::uintmax_t>(count);
            completed.fetch_add(static_cast<std::uintmax_t>(count));
        }
        require(!cancel.load(), "Operacion cancelada");
        require(input.eof(), "Archivo de origen ilegible: " + src.string());
        require(copied == expectedSize, "El archivo de origen cambio durante la copia: " + src.string());
        output.flush();
        require(output.good(), "Error al guardar: " + dst.string());
        output.close();
        require(output.good(), "Error al cerrar: " + dst.string());
        std::error_code ec;
        require(!fs::exists(dst, ec) && !ec, "Ya existe en el destino: " + dst.filename().string());
        fs::rename(tmp, dst, ec);
        require(!ec, error(dst, ec));
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

void copyTree(const fs::path& src, const fs::path& dst, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, bool destinationCreated = false) {
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
        require(!ec, error(src, ec));
        if (destinationCreated) {
            require(fs::is_directory(dst, ec) && !ec, error(dst, ec));
        } else {
            const bool created = fs::create_directory(dst, ec);
            require(created && !ec, "No se pudo crear la carpeta de copia: " + dst.string());
        }
        for (fs::directory_iterator it(src, ec), end; it != end && !cancel.load(); it.increment(ec)) {
            require(!ec, error(src, ec));
            require(!it->is_symlink(ec) && !ec, "Enlace simbolico no admitido: " + it->path().string());
            copyTree(it->path(), dst / it->path().filename(), cancel, completed);
        }
        require(!ec, error(src, ec));
        require(!cancel.load(), "Operacion cancelada");
    } else {
        require(!ec && fs::is_regular_file(src, ec), "Tipo de archivo no admitido: " + src.string());
        copyFile(src, dst, cancel, completed);
    }
}

void removeTree(const fs::path& source, std::atomic_bool& cancel) {
    require(!cancel.load(), "Operacion cancelada");
    std::error_code ec;
    const auto type = fs::symlink_status(source, ec);
    require(!ec, source.string() + ": " + ec.message());
    if (fs::is_directory(type)) {
        std::vector<fs::path> children;
        {
            // Close the directory handle before deleting its entries. Some SD
            // filesystems reject removal while the directory is still open.
            fs::directory_iterator it(source, ec), end;
            require(!ec, source.string() + ": " + ec.message());
            for (; it != end; it.increment(ec)) {
                require(!ec, source.string() + ": " + ec.message());
                children.push_back(it->path());
            }
            require(!ec, source.string() + ": " + ec.message());
        }
        for (const auto& child : children) removeTree(child, cancel);
    }
    require(!cancel.load(), "Operacion cancelada");
    fs::remove(source, ec);
    require(!ec, source.string() + ": " + ec.message());
}

void extractZip(const fs::path& source, const fs::path& destination, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, std::atomic<std::uintmax_t>& total) {
    struct Archive {
        unzFile file;
        explicit Archive(const fs::path& p) : file(unzOpen64(p.string().c_str())) {}
        ~Archive() { if (file) unzClose(file); }
    } archive(source);
    require(archive.file != nullptr, "ZIP ilegible");
    unz_global_info64 global{};
    require(unzGetGlobalInfo64(archive.file, &global) == UNZ_OK, "No se pudo leer el indice ZIP");
    require(global.number_entry <= 100000, "ZIP con demasiadas entradas");

    // Preflight names and collisions before writing any member.
    std::vector<std::pair<fs::path, bool>> names;
    std::set<std::string> unique;
    std::uintmax_t expandedSize{};
    for (ZPOS64_T i = 0; i < global.number_entry; ++i) {
        unz_file_info64 info{};
        require(unzGetCurrentFileInfo64(archive.file, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK,
            "Entrada ZIP ilegible");
        require(info.size_filename > 0 && info.size_filename <= 768, "Nombre ZIP invalido");
        std::vector<char> buffer(static_cast<size_t>(info.size_filename) + 1, 0);
        require(unzGetCurrentFileInfo64(archive.file, &info, buffer.data(), buffer.size(), nullptr, 0, nullptr, 0) == UNZ_OK,
            "Entrada ZIP ilegible");
        const std::string name(buffer.data(), info.size_filename);
        require(name.find('\0') == std::string::npos && name.find('\\') == std::string::npos &&
                name.find(':') == std::string::npos,
            "Nombre ZIP invalido");
        const auto mode = static_cast<unsigned>(info.external_fa >> 16);
        require((mode & S_IFMT) != S_IFLNK, "ZIP contiene un enlace simbolico");
        const fs::path relative(name);
        require(!relative.is_absolute() && !relative.empty(), "Ruta absoluta dentro del ZIP");
        for (const auto& part : relative) require(part != ".." && part != ".", "Ruta insegura dentro del ZIP");
        const auto target = clean(destination / relative);
        require(inside(target, destination) && target != destination, "Ruta ZIP fuera del destino");
        const bool directory = name.back() == '/';
        auto caseInsensitiveTarget = target.string();
        std::transform(caseInsensitiveTarget.begin(), caseInsensitiveTarget.end(), caseInsensitiveTarget.begin(),
            [](unsigned char c) { return std::tolower(c); });
        require(unique.insert(caseInsensitiveTarget).second, "Entradas ZIP duplicadas");
        std::error_code ec;
        const bool exists = fs::exists(target, ec);
        require(!ec && (!exists || (directory && fs::is_directory(target, ec))),
            "Ya existe: " + target.string());
        names.emplace_back(target, directory);
        if (!directory) {
            require(info.uncompressed_size <= std::numeric_limits<std::uintmax_t>::max() - expandedSize,
                "ZIP demasiado grande");
            expandedSize += info.uncompressed_size;
        }
        if (i + 1 < global.number_entry)
            require(unzGoToNextFile(archive.file) == UNZ_OK, "ZIP truncado");
    }

    std::error_code spaceError;
    const auto available = fs::space(destination, spaceError).available;
    require(!spaceError && expandedSize <= available, "Espacio insuficiente para descomprimir el ZIP");
    total.store(expandedSize);

    require(unzGoToFirstFile(archive.file) == UNZ_OK || names.empty(), "ZIP truncado");
    std::vector<char> buffer(64 * 1024);
    std::vector<fs::path> createdFiles;
    std::set<fs::path> createdDirs;
    try {
        for (size_t i = 0; i < names.size(); ++i) {
            require(!cancel.load(), "Operacion cancelada");
            const auto& [target, directory] = names[i];
            std::error_code ec;
            const auto folder = directory ? target : target.parent_path();
            for (auto parent = folder; parent != destination && inside(parent, destination);
                 parent = parent.parent_path()) {
                const bool exists = fs::exists(parent, ec);
                require(!ec, error(parent, ec));
                if (!exists) createdDirs.insert(parent);
            }
            fs::create_directories(folder, ec);
            require(!ec, error(folder, ec));
            if (!directory) {
                require(unzOpenCurrentFile(archive.file) == UNZ_OK, "No se pudo abrir la entrada ZIP");
                bool memberOpen = true;
                const auto tmp = tempFor(target);
                try {
                    std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
                    require(output.good(), "No se pudo crear: " + target.string());
                    for (;;) {
                        require(!cancel.load(), "Operacion cancelada");
                        const int read = unzReadCurrentFile(archive.file, buffer.data(), buffer.size());
                        require(read >= 0, "ZIP dañado o error de lectura");
                        if (!read) break;
                        output.write(buffer.data(), read);
                        require(output.good(), "Error de escritura: " + target.string());
                        completed.fetch_add(static_cast<std::uintmax_t>(read));
                    }
                    output.flush();
                    require(output.good(), "Error al guardar: " + target.string());
                    output.close();
                    require(output.good(), "Error al cerrar: " + target.string());
                    const int closeResult = unzCloseCurrentFile(archive.file);
                    memberOpen = false;
                    require(closeResult == UNZ_OK, "CRC incorrecto en ZIP");
                    require(!fs::exists(target, ec) && !ec, "Ya existe: " + target.string());
                    fs::rename(tmp, target, ec);
                    require(!ec, error(target, ec));
                    createdFiles.push_back(target);
                } catch (...) {
                    if (memberOpen) unzCloseCurrentFile(archive.file);
                    fs::remove(tmp, ec);
                    throw;
                }
            }
            if (i + 1 < names.size()) require(unzGoToNextFile(archive.file) == UNZ_OK, "ZIP truncado");
        }
    } catch (...) {
        std::error_code ec;
        for (auto it = createdFiles.rbegin(); it != createdFiles.rend(); ++it) fs::remove(*it, ec);
        for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) fs::remove(*it, ec);
        throw;
    }
}

} // namespace

fs::path clean(const fs::path& p) { return p.lexically_normal(); }

bool isSdPath(const fs::path& p) {
    const auto normalized = clean(p);
    return normalized.is_absolute() && normalized.string().rfind("/", 0) == 0 &&
        normalized.string().find(':') == std::string::npos;
}

std::string formatSize(std::uintmax_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    unsigned unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    char text[40]{};
    std::snprintf(text, sizeof(text), unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
    return text;
}

std::vector<Entry> list(const fs::path& directory) {
    require(isSdPath(directory), "Ruta fuera de la microSD");
    std::error_code ec;
    std::vector<Entry> entries;
    for (fs::directory_iterator it(directory, ec), end; it != end; it.increment(ec)) {
        require(!ec, error(directory, ec));
        const auto p = it->path();
        if (it->is_symlink(ec) || ec) continue;
        const bool dir = it->is_directory(ec);
        if (ec) continue;
        Entry entry{p, p.filename().string(), dir};
        if (!dir) {
            entry.size = it->file_size(ec);
            entry.sizeKnown = !ec;
        }
        ec.clear();
        struct stat st{};
        if (::stat(p.string().c_str(), &st) == 0 && st.st_mtime > 0) {
            char date[32]{};
            const auto* tm = std::localtime(&st.st_mtime);
            if (tm && std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm)) entry.modified = date;
        }
        entries.push_back(std::move(entry));
    }
    require(!ec, error(directory, ec));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        std::string an = a.name, bn = b.name;
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return std::tolower(c); });
        return an < bn;
    });
    return entries;
}

std::uintmax_t directorySize(const fs::path& directory, const std::atomic_bool& cancel) {
    std::uintmax_t size = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !cancel.load(); it.increment(ec)) {
        if (ec) throw std::runtime_error(error(directory, ec));
        if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec)) {
            const auto bytes = it->file_size(ec);
            if (!ec) size += bytes;
        }
        ec.clear();
    }
    if (ec) throw std::runtime_error(error(directory, ec));
    return size;
}

std::string run(const Job& job, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, std::atomic<std::uintmax_t>& total) {
    try {
        require(!job.sources.empty(), "No hay archivos seleccionados");
        if (job.operation != Operation::Delete) require(isSdPath(job.destination), "Destino fuera de la microSD");
        std::set<std::string> destinations;
        for (const auto& source : job.sources) {
            validSource(source);
            if (job.operation == Operation::Copy || job.operation == Operation::Move) {
                const auto target = clean(job.destination / source.filename());
                require(!inside(target, source), "No se puede copiar o mover una carpeta dentro de si misma");
                require(destinations.insert(target.string()).second, "Destino duplicado");
                std::error_code ec;
                require(!fs::exists(target, ec) && !ec, "Ya existe en el destino: " + target.filename().string());
                if (job.operation == Operation::Copy) {
                    const auto bytes = fs::is_directory(source) ? directorySize(source, cancel) : fs::file_size(source);
                    total.fetch_add(bytes);
                }
            }
        }
        if (job.operation == Operation::ExtractZip) {
            require(job.sources.size() == 1, "Selecciona un solo ZIP");
            require(fs::is_directory(job.destination), "Selecciona una carpeta de destino");
            extractZip(job.sources.front(), job.destination, cancel, completed, total);
            return "ZIP descomprimido";
        }
        for (const auto& source : job.sources) {
            require(!cancel.load(), "Operacion cancelada");
            std::error_code ec;
            if (job.operation == Operation::Delete) {
                removeTree(source, cancel);
            } else if (job.operation == Operation::Move) {
                const auto target = job.destination / source.filename();
                require(!fs::exists(target, ec) && !ec, "Ya existe en el destino: " + target.filename().string());
                fs::rename(source, target, ec);
                require(!ec, error(source, ec));
            } else {
                const auto target = job.destination / source.filename();
                if (fs::is_directory(source)) {
                    const auto staging = tempFor(target);
                    bool stagingCreated = false;
                    try {
                        stagingCreated = fs::create_directory(staging, ec);
                        require(stagingCreated && !ec,
                            "No se pudo crear la carpeta temporal: " + staging.string());
                        copyTree(source, staging, cancel, completed, true);
                        require(!fs::exists(target, ec) && !ec, "Ya existe en el destino: " + target.filename().string());
                        fs::rename(staging, target, ec);
                        require(!ec, error(target, ec));
                    } catch (...) {
                        if (stagingCreated) fs::remove_all(staging, ec);
                        throw;
                    }
                } else {
                    copyFile(source, target, cancel, completed);
                }
            }
        }
        return job.operation == Operation::Delete ? "Elementos eliminados" :
            job.operation == Operation::Move ? "Elementos movidos" : "Copia completada";
    } catch (const std::exception& e) {
        return std::string("ERROR: ") + e.what();
    }
}

} // namespace explorer
