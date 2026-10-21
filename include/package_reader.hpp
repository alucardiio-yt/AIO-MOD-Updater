#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace installer {

// Logical reader for NSP packages staged by AIO MOD. A normal NSP is one file.
// Large MTP uploads are stored as FAT32-safe contiguous parts:
//   game.nsp, game.nsp.01, game.nsp.02, ...
// Callers see one 64-bit logical stream regardless of the backing layout.
class PackageReader {
public:
    PackageReader() = default;
    explicit PackageReader(std::string basePath);

    bool open(const std::string& basePath);
    bool isOpen() const;
    uint64_t size() const;
    uint64_t tell() const;
    const std::string& basePath() const;
    const std::vector<std::string>& parts() const;

    bool seek(uint64_t logicalOffset);
    bool read(void* buffer, size_t bytes);

    static std::vector<std::string> discoverParts(const std::string& basePath);
    static uint64_t logicalSize(const std::string& basePath, bool* exists = nullptr);
    static bool removePackage(const std::string& basePath, uint64_t* removedBytes = nullptr);

private:
    bool openPart(size_t index, uint64_t localOffset);
    size_t locatePart(uint64_t logicalOffset, uint64_t& localOffset) const;

    std::string path;
    std::vector<std::string> partPaths;
    std::vector<uint64_t> partSizes;
    std::vector<uint64_t> partStarts;
    uint64_t totalSize = 0;
    uint64_t position = 0;
    size_t currentPart = static_cast<size_t>(-1);
    std::ifstream stream;
};

} // namespace installer
