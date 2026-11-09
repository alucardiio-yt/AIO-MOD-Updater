#include "file_explorer_ops.hpp"

#include <minizip/unzip.h>
#include <borealis.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>

#include "storage_error.hpp"

using namespace brls::i18n::literals;

namespace explorer {
namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string error(const fs::path& p, const std::error_code& ec) {
    return storage::describeError(p.string(), ec);
}

std::string writeError(const fs::path& path, const std::string& fallback) {
    return errno ? storage::describeError(path.string(),
        std::error_code(errno, std::generic_category())) : fallback;
}

bool inside(const fs::path& child, const fs::path& parent) {
    const auto c = clean(child).generic_string();
    const auto p = clean(parent).generic_string();
    return p == "/" || c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/');
}

void validSource(const fs::path& p) {
    require(isSdPath(p) && clean(p) != fs::path("/"), "explorer/errors/outside_sd"_i18n);
    std::error_code ec;
    const bool exists = fs::exists(p, ec);
    require(!ec, error(p, ec));
    require(exists, storage::describeError(p.string(), std::make_error_code(std::errc::no_such_file_or_directory)));
    require(!fs::is_symlink(p, ec), "explorer/errors/symlink"_i18n);
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
    throw std::runtime_error("explorer/errors/temp_file"_i18n);
}

void copyFile(const fs::path& src, const fs::path& dst, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, const ProgressCallback& onCurrent) {
    if (onCurrent) onCurrent(src);
    errno = 0;
    std::ifstream input(src, std::ios::binary);
    require(input.good(), writeError(src, std::string("explorer/errors/read"_i18n) + src.string()));
    std::error_code sizeError;
    const auto expectedSize = fs::file_size(src, sizeError);
    require(!sizeError, error(src, sizeError));
    const auto tmp = tempFor(dst);
    try {
        errno = 0;
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        require(output.good(), writeError(dst, std::string("explorer/errors/create"_i18n) + dst.string()));
        // std::thread uses a small stack on Switch. Keep the transfer buffer
        // on the heap so copying a file cannot overflow the worker stack.
        std::vector<char> buffer(64 * 1024);
        std::uintmax_t copied = 0;
        while (input && !cancel.load()) {
            input.read(buffer.data(), buffer.size());
            const auto count = input.gcount();
            if (!count) break;
            errno = 0;
            output.write(buffer.data(), count);
            require(output.good(), writeError(dst, std::string("explorer/errors/write"_i18n) + dst.string()));
            copied += static_cast<std::uintmax_t>(count);
            completed.fetch_add(static_cast<std::uintmax_t>(count));
        }
        require(!cancel.load(), "explorer/errors/cancelled"_i18n);
        require(input.eof(), std::string("explorer/errors/source_unreadable"_i18n) + src.string());
        require(copied == expectedSize, std::string("explorer/errors/source_changed"_i18n) + src.string());
        errno = 0;
        output.flush();
        require(output.good(), writeError(dst, std::string("explorer/errors/save"_i18n) + dst.string()));
        errno = 0;
        output.close();
        require(output.good(), writeError(dst, std::string("explorer/errors/close"_i18n) + dst.string()));
        std::error_code ec;
        require(!fs::exists(dst, ec) && !ec, std::string("explorer/errors/exists"_i18n) + dst.filename().string());
        fs::rename(tmp, dst, ec);
        require(!ec, error(dst, ec));
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

void copyTree(const fs::path& src, const fs::path& dst, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, const ProgressCallback& onCurrent,
    bool destinationCreated = false) {
    if (onCurrent) onCurrent(src);
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
        require(!ec, error(src, ec));
        if (destinationCreated) {
            require(fs::is_directory(dst, ec) && !ec, error(dst, ec));
        } else {
            const bool created = fs::create_directory(dst, ec);
            require(created && !ec, std::string("explorer/errors/create_folder"_i18n) + dst.string());
        }
        for (fs::directory_iterator it(src, ec), end; it != end && !cancel.load(); it.increment(ec)) {
            require(!ec, error(src, ec));
            require(!it->is_symlink(ec) && !ec, std::string("explorer/errors/symlink_path"_i18n) + it->path().string());
            copyTree(it->path(), dst / it->path().filename(), cancel, completed, onCurrent);
        }
        require(!ec, error(src, ec));
        require(!cancel.load(), "explorer/errors/cancelled"_i18n);
    } else {
        require(!ec && fs::is_regular_file(src, ec), std::string("explorer/errors/file_type"_i18n) + src.string());
        copyFile(src, dst, cancel, completed, onCurrent);
    }
}

std::uintmax_t countTree(const fs::path& source, const std::atomic_bool& cancel,
    const ProgressCallback& onCurrent) {
    require(!cancel.load(), "explorer/errors/cancelled"_i18n);
    if (onCurrent) onCurrent(source);
    std::error_code ec;
    const auto type = fs::symlink_status(source, ec);
    require(!ec, error(source, ec));
    std::uintmax_t count = 1;
    if (fs::is_directory(type)) {
        for (fs::directory_iterator it(source, ec), end; it != end; it.increment(ec)) {
            require(!ec, error(source, ec));
            const auto childCount = countTree(it->path(), cancel, onCurrent);
            require(childCount <= std::numeric_limits<std::uintmax_t>::max() - count,
                "explorer/errors/too_many_entries"_i18n);
            count += childCount;
        }
        require(!ec, error(source, ec));
    }
    return count;
}

void removeTree(const fs::path& source, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, const ProgressCallback& onCurrent) {
    require(!cancel.load(), "explorer/errors/cancelled"_i18n);
    std::error_code ec;
    const auto type = fs::symlink_status(source, ec);
    require(!ec, error(source, ec));
    if (fs::is_directory(type)) {
        std::vector<fs::path> children;
        {
            // Close the directory handle before deleting its entries. Some SD
            // filesystems reject removal while the directory is still open.
            fs::directory_iterator it(source, ec), end;
            require(!ec, error(source, ec));
            for (; it != end; it.increment(ec)) {
                require(!ec, error(source, ec));
                children.push_back(it->path());
            }
            require(!ec, error(source, ec));
        }
        for (const auto& child : children) removeTree(child, cancel, completed, onCurrent);
    }
    require(!cancel.load(), "explorer/errors/cancelled"_i18n);
    if (onCurrent) onCurrent(source);
    const bool removed = fs::remove(source, ec);
    require(!ec, error(source, ec));
    require(removed, storage::describeError(source.string(),
        std::make_error_code(std::errc::no_such_file_or_directory)));
    completed.fetch_add(1);
}

void extractZip(const fs::path& source, const fs::path& destination, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, std::atomic<std::uintmax_t>& total,
    const ProgressCallback& onCurrent) {
    if (onCurrent) onCurrent(source);
    struct Archive {
        unzFile file;
        explicit Archive(const fs::path& p) : file(unzOpen64(p.string().c_str())) {}
        ~Archive() { if (file) unzClose(file); }
    } archive(source);
    require(archive.file != nullptr, "explorer/errors/zip_unreadable"_i18n);
    unz_global_info64 global{};
    require(unzGetGlobalInfo64(archive.file, &global) == UNZ_OK, "explorer/errors/zip_index"_i18n);
    require(global.number_entry <= 100000, "explorer/errors/zip_too_many"_i18n);

    // Preflight names and collisions before writing any member.
    std::vector<std::pair<fs::path, bool>> names;
    std::set<std::string> unique;
    std::uintmax_t expandedSize{};
    for (ZPOS64_T i = 0; i < global.number_entry; ++i) {
        unz_file_info64 info{};
        require(unzGetCurrentFileInfo64(archive.file, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK,
            "explorer/errors/zip_entry"_i18n);
        require(info.size_filename > 0 && info.size_filename <= 768, "explorer/errors/zip_name"_i18n);
        std::vector<char> buffer(static_cast<size_t>(info.size_filename) + 1, 0);
        require(unzGetCurrentFileInfo64(archive.file, &info, buffer.data(), buffer.size(), nullptr, 0, nullptr, 0) == UNZ_OK,
            "explorer/errors/zip_entry"_i18n);
        const std::string name(buffer.data(), info.size_filename);
        require(name.find('\0') == std::string::npos && name.find('\\') == std::string::npos &&
                name.find(':') == std::string::npos,
            "explorer/errors/zip_name"_i18n);
        const auto mode = static_cast<unsigned>(info.external_fa >> 16);
        require((mode & S_IFMT) != S_IFLNK, "explorer/errors/zip_symlink"_i18n);
        const fs::path relative(name);
        require(!relative.is_absolute() && !relative.empty(), "explorer/errors/zip_path"_i18n);
        for (const auto& part : relative) require(part != ".." && part != ".", "explorer/errors/zip_path"_i18n);
        const auto target = clean(destination / relative);
        require(inside(target, destination) && target != destination, "explorer/errors/zip_path"_i18n);
        const bool directory = name.back() == '/';
        auto caseInsensitiveTarget = target.string();
        std::transform(caseInsensitiveTarget.begin(), caseInsensitiveTarget.end(), caseInsensitiveTarget.begin(),
            [](unsigned char c) { return std::tolower(c); });
        require(unique.insert(caseInsensitiveTarget).second, "explorer/errors/zip_duplicate"_i18n);
        std::error_code ec;
        const bool exists = fs::exists(target, ec);
        require(!ec && (!exists || (directory && fs::is_directory(target, ec))),
            std::string("explorer/errors/exists"_i18n) + target.string());
        names.emplace_back(target, directory);
        if (!directory) {
            require(info.uncompressed_size <= std::numeric_limits<std::uintmax_t>::max() - expandedSize,
                "explorer/errors/zip_too_large"_i18n);
            expandedSize += info.uncompressed_size;
        }
        if (i + 1 < global.number_entry)
            require(unzGoToNextFile(archive.file) == UNZ_OK, "explorer/errors/zip_truncated"_i18n);
    }

    std::error_code spaceError;
    const auto available = fs::space(destination, spaceError).available;
    require(!spaceError, error(destination, spaceError));
    require(expandedSize <= available, "explorer/errors/zip_no_space"_i18n);
    total.store(expandedSize);

    require(unzGoToFirstFile(archive.file) == UNZ_OK || names.empty(), "explorer/errors/zip_truncated"_i18n);
    std::vector<char> buffer(64 * 1024);
    std::vector<fs::path> createdFiles;
    std::set<fs::path> createdDirs;
    try {
        for (size_t i = 0; i < names.size(); ++i) {
            require(!cancel.load(), "explorer/errors/cancelled"_i18n);
            const auto& [target, directory] = names[i];
            if (onCurrent) onCurrent(target);
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
                require(unzOpenCurrentFile(archive.file) == UNZ_OK, "explorer/errors/zip_entry"_i18n);
                bool memberOpen = true;
                const auto tmp = tempFor(target);
                try {
                    std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
                    require(output.good(), std::string("explorer/errors/create"_i18n) + target.string());
                    for (;;) {
                        require(!cancel.load(), "explorer/errors/cancelled"_i18n);
                        const int read = unzReadCurrentFile(archive.file, buffer.data(), buffer.size());
                        require(read >= 0, "explorer/errors/zip_damaged"_i18n);
                        if (!read) break;
                        output.write(buffer.data(), read);
                        require(output.good(), std::string("explorer/errors/write"_i18n) + target.string());
                        completed.fetch_add(static_cast<std::uintmax_t>(read));
                    }
                    output.flush();
                    require(output.good(), std::string("explorer/errors/save"_i18n) + target.string());
                    output.close();
                    require(output.good(), std::string("explorer/errors/close"_i18n) + target.string());
                    const int closeResult = unzCloseCurrentFile(archive.file);
                    memberOpen = false;
                    require(closeResult == UNZ_OK, "explorer/errors/zip_crc"_i18n);
                    require(!fs::exists(target, ec) && !ec, std::string("explorer/errors/exists"_i18n) + target.string());
                    fs::rename(tmp, target, ec);
                    require(!ec, error(target, ec));
                    createdFiles.push_back(target);
                } catch (...) {
                    if (memberOpen) unzCloseCurrentFile(archive.file);
                    fs::remove(tmp, ec);
                    throw;
                }
            }
            if (i + 1 < names.size()) require(unzGoToNextFile(archive.file) == UNZ_OK, "explorer/errors/zip_truncated"_i18n);
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
    require(isSdPath(directory), "explorer/errors/outside_sd"_i18n);
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

std::uintmax_t directorySize(const fs::path& directory, const std::atomic_bool& cancel,
    const ProgressCallback& onCurrent) {
    std::uintmax_t size = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !cancel.load(); it.increment(ec)) {
        if (ec) throw std::runtime_error(error(directory, ec));
        if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec)) {
            if (onCurrent) onCurrent(it->path());
            const auto bytes = it->file_size(ec);
            if (ec) throw std::runtime_error(error(it->path(), ec));
            size += bytes;
        }
        ec.clear();
    }
    if (ec) throw std::runtime_error(error(directory, ec));
    return size;
}

std::string run(const Job& job, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, std::atomic<std::uintmax_t>& total,
    const ProgressCallback& onCurrent) {
    try {
        require(!job.sources.empty(), "explorer/errors/no_selection"_i18n);
        if (job.operation != Operation::Delete) {
            require(isSdPath(job.destination), "explorer/errors/outside_sd"_i18n);
            std::error_code destinationError;
            const bool destinationExists = fs::is_directory(job.destination, destinationError);
            require(!destinationError, error(job.destination, destinationError));
            require(destinationExists, storage::describeError(job.destination.string(),
                std::make_error_code(std::errc::no_such_file_or_directory)));
        }
        std::set<std::string> destinations;
        std::uintmax_t computedTotal = 0;
        for (const auto& source : job.sources) {
            if (onCurrent) onCurrent(source);
            validSource(source);
            if (job.operation == Operation::Copy || job.operation == Operation::Move) {
                const auto target = clean(job.destination / source.filename());
                require(!inside(target, source), "explorer/errors/into_self"_i18n);
                require(destinations.insert(target.string()).second, "explorer/errors/duplicate_target"_i18n);
                std::error_code ec;
                require(!fs::exists(target, ec) && !ec, std::string("explorer/errors/exists"_i18n) + target.filename().string());
                if (job.operation == Operation::Copy) {
                    const auto bytes = fs::is_directory(source) ? directorySize(source, cancel, onCurrent) : fs::file_size(source);
                    require(bytes <= std::numeric_limits<std::uintmax_t>::max() - computedTotal,
                        "explorer/errors/copy_too_large"_i18n);
                    computedTotal += bytes;
                }
            }
            else if (job.operation == Operation::Delete) {
                const auto items = countTree(source, cancel, onCurrent);
                require(items <= std::numeric_limits<std::uintmax_t>::max() - computedTotal,
                    "explorer/errors/too_many_entries"_i18n);
                computedTotal += items;
            }
        }
        if (job.operation == Operation::Move)
            total.store(job.sources.size());
        else if (job.operation == Operation::Copy || job.operation == Operation::Delete)
            total.store(computedTotal);
        if (job.operation == Operation::Copy) {
            std::error_code spaceError;
            const auto available = fs::space(job.destination, spaceError).available;
            if (!spaceError && computedTotal > available)
                throw std::runtime_error(storage::describeError(job.destination.string(),
                    std::make_error_code(std::errc::no_space_on_device)));
        }
        if (job.operation == Operation::ExtractZip) {
            require(job.sources.size() == 1, "explorer/errors/one_zip"_i18n);
            require(fs::is_directory(job.destination), "explorer/errors/destination_folder"_i18n);
            extractZip(job.sources.front(), job.destination, cancel, completed, total, onCurrent);
            return "explorer/extracted"_i18n;
        }
        for (const auto& source : job.sources) {
            require(!cancel.load(), "explorer/errors/cancelled"_i18n);
            std::error_code ec;
            if (job.operation == Operation::Delete) {
                removeTree(source, cancel, completed, onCurrent);
            } else if (job.operation == Operation::Move) {
                const auto target = job.destination / source.filename();
                require(!fs::exists(target, ec) && !ec, std::string("explorer/errors/exists"_i18n) + target.filename().string());
                if (onCurrent) onCurrent(source);
                fs::rename(source, target, ec);
                require(!ec, error(source, ec));
                completed.fetch_add(1);
            } else {
                const auto target = job.destination / source.filename();
                if (fs::is_directory(source)) {
                    const auto staging = tempFor(target);
                    bool stagingCreated = false;
                    try {
                        stagingCreated = fs::create_directory(staging, ec);
                        require(stagingCreated && !ec,
                            std::string("explorer/errors/create_folder"_i18n) + staging.string());
                        copyTree(source, staging, cancel, completed, onCurrent, true);
                        require(!fs::exists(target, ec) && !ec, std::string("explorer/errors/exists"_i18n) + target.filename().string());
                        fs::rename(staging, target, ec);
                        require(!ec, error(target, ec));
                    } catch (...) {
                        if (stagingCreated) fs::remove_all(staging, ec);
                        throw;
                    }
                } else {
                    copyFile(source, target, cancel, completed, onCurrent);
                }
            }
        }
        return job.operation == Operation::Delete ? "explorer/deleted"_i18n :
            job.operation == Operation::Move ? "explorer/moved"_i18n : "explorer/copied"_i18n;
    } catch (const std::exception& e) {
        return std::string("ERROR: ") + e.what();
    }
}

} // namespace explorer
