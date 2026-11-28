#pragma once

#include <filesystem>
#include <stdexcept>
#include "storage_error.hpp"

namespace hb_store {
// Horizon cannot rename over an existing file. Keep the previous contents
// until the complete replacement has been moved into its final location.
inline void replaceFile(const std::filesystem::path& staged, const std::filesystem::path& target)
{
    namespace disk = std::filesystem;
    const disk::path backup = target.string() + ".aio-hb-backup";
    if (disk::exists(backup)) {
        if (disk::exists(target))
            throw std::runtime_error(storage::describeError(backup.string(),std::make_error_code(std::errc::file_exists)));
        disk::rename(backup,target);
    }
    const bool replacing = disk::exists(target);
    if (replacing) {
        if (!disk::is_regular_file(target))
            throw std::runtime_error(storage::describeError(target.string(),std::make_error_code(std::errc::is_a_directory)));
        disk::rename(target,backup);
    }
    std::error_code error;
    disk::rename(staged,target,error);
    if (error) {
        if (replacing) disk::rename(backup,target);
        throw std::runtime_error(storage::describeError(target.string(),error));
    }
    if (replacing) disk::remove(backup);
}
} // namespace hb_store
