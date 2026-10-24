#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace explorer {

namespace path = std::filesystem;

struct Entry {
    path::path path;
    std::string name;
    bool directory{};
    std::uintmax_t size{};
    bool sizeKnown{};
    std::string modified;
};

enum class Operation { Copy, Move, Delete, ExtractZip };

struct Job {
    Operation operation{};
    std::vector<path::path> sources;
    path::path destination;
};

using ProgressCallback = std::function<void(const path::path&)>;

// All paths are confined to the SD root. Existing destinations are never
// replaced by a batch operation, so partial failure cannot destroy a file.
path::path clean(const path::path& p);
bool isSdPath(const path::path& p);
std::vector<Entry> list(const path::path& directory);
std::uintmax_t directorySize(const path::path& directory, const std::atomic_bool& cancel,
    const ProgressCallback& onCurrent = {});
std::string formatSize(std::uintmax_t bytes);
std::string run(const Job& job, std::atomic_bool& cancel,
    std::atomic<std::uintmax_t>& completed, std::atomic<std::uintmax_t>& total,
    const ProgressCallback& onCurrent = {});

} // namespace explorer
