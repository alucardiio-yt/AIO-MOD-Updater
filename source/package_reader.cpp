#include "package_reader.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace installer {
namespace {

std::string partPath(const std::string& base, unsigned index)
{
    if (index == 0)
        return base;
    std::ostringstream ss;
    ss << base << '.' << std::setfill('0') << std::setw(2) << index;
    return ss.str();
}

} // namespace

PackageReader::PackageReader(std::string basePath)
{
    open(basePath);
}

std::vector<std::string> PackageReader::discoverParts(const std::string& basePath)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(basePath, ec) || ec)
        return out;

    out.push_back(basePath);
    // More than 99 GiB at 1 GiB/part is already far beyond normal NSP sizes,
    // but keep a generous bound for future-proofing.
    for (unsigned i = 1; i < 10000; ++i) {
        const std::string candidate = partPath(basePath, i);
        ec.clear();
        if (!std::filesystem::is_regular_file(candidate, ec) || ec)
            break;
        out.push_back(candidate);
    }
    return out;
}

uint64_t PackageReader::logicalSize(const std::string& basePath, bool* exists)
{
    const auto parts = discoverParts(basePath);
    if (exists)
        *exists = !parts.empty();
    uint64_t total = 0;
    for (const auto& p : parts) {
        std::error_code ec;
        const auto s = std::filesystem::file_size(p, ec);
        if (ec)
            return 0;
        total += static_cast<uint64_t>(s);
    }
    return total;
}

bool PackageReader::removePackage(const std::string& basePath, uint64_t* removedBytes)
{
    if (removedBytes)
        *removedBytes = 0;
    const auto parts = discoverParts(basePath);
    if (parts.empty())
        return true;

    bool ok = true;
    for (const auto& p : parts) {
        std::error_code ec;
        const auto s = std::filesystem::file_size(p, ec);
        if (ec)
            ec.clear();
        if (!std::filesystem::remove(p, ec) || ec) {
            ok = false;
            continue;
        }
        if (removedBytes && !ec)
            *removedBytes += static_cast<uint64_t>(s);
    }
    return ok;
}

bool PackageReader::open(const std::string& basePath)
{
    stream.close();
    path = basePath;
    partPaths = discoverParts(basePath);
    partSizes.clear();
    partStarts.clear();
    totalSize = 0;
    position = 0;
    currentPart = static_cast<size_t>(-1);

    if (partPaths.empty())
        return false;

    for (const auto& p : partPaths) {
        std::error_code ec;
        const auto s = std::filesystem::file_size(p, ec);
        if (ec)
            return false;
        partStarts.push_back(totalSize);
        partSizes.push_back(static_cast<uint64_t>(s));
        totalSize += static_cast<uint64_t>(s);
    }
    return openPart(0, 0);
}

bool PackageReader::isOpen() const
{
    return !partPaths.empty() && stream.is_open();
}

uint64_t PackageReader::size() const { return totalSize; }
uint64_t PackageReader::tell() const { return position; }
const std::string& PackageReader::basePath() const { return path; }
const std::vector<std::string>& PackageReader::parts() const { return partPaths; }

size_t PackageReader::locatePart(uint64_t logicalOffset, uint64_t& localOffset) const
{
    if (logicalOffset > totalSize || partPaths.empty())
        return static_cast<size_t>(-1);
    if (logicalOffset == totalSize) {
        localOffset = partSizes.back();
        return partPaths.size() - 1;
    }
    auto it = std::upper_bound(partStarts.begin(), partStarts.end(), logicalOffset);
    size_t idx = it == partStarts.begin() ? 0 : static_cast<size_t>((it - partStarts.begin()) - 1);
    if (idx >= partPaths.size())
        return static_cast<size_t>(-1);
    localOffset = logicalOffset - partStarts[idx];
    return idx;
}

bool PackageReader::openPart(size_t index, uint64_t localOffset)
{
    if (index >= partPaths.size())
        return false;
    if (currentPart != index || !stream.is_open()) {
        stream.close();
        stream.clear();
        stream.open(partPaths[index], std::ios::binary);
        if (!stream)
            return false;
        currentPart = index;
    }
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(localOffset), std::ios::beg);
    return static_cast<bool>(stream);
}

bool PackageReader::seek(uint64_t logicalOffset)
{
    uint64_t local = 0;
    const size_t idx = locatePart(logicalOffset, local);
    if (idx == static_cast<size_t>(-1))
        return false;
    if (!openPart(idx, local))
        return false;
    position = logicalOffset;
    return true;
}

bool PackageReader::read(void* buffer, size_t bytes)
{
    if (bytes == 0)
        return true;
    if (!buffer || position > totalSize || static_cast<uint64_t>(bytes) > totalSize - position)
        return false;

    auto* dst = static_cast<unsigned char*>(buffer);
    size_t remaining = bytes;
    while (remaining > 0) {
        uint64_t local = 0;
        const size_t idx = locatePart(position, local);
        if (idx == static_cast<size_t>(-1) || !openPart(idx, local))
            return false;
        const uint64_t available = partSizes[idx] - local;
        const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining, available));
        if (take == 0)
            return false;
        stream.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(take));
        if (static_cast<size_t>(stream.gcount()) != take)
            return false;
        dst += take;
        remaining -= take;
        position += take;
    }
    return true;
}

} // namespace installer
