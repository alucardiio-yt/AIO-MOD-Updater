#include "diagnostic_log.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "storage_error.hpp"

namespace diagnostic_log {
namespace {
std::mutex logMutex;
constexpr std::uintmax_t MaxLogBytes = 64 * 1024;

std::string oneLine(const std::string& input)
{
    std::string output;
    output.reserve(std::min<size_t>(input.size(), 500));
    for (char c : input) {
        if (output.size() == 500) break;
        output += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    return output;
}
}

void record(const std::string& area, const std::string& message) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(logMutex);
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(Path).parent_path(), ec) || ec)
            return;
        if (!std::filesystem::exists(Path, ec) && !ec &&
            std::filesystem::is_regular_file(LegacyPath, ec) && !ec) {
            std::filesystem::copy_file(LegacyPath, Path, std::filesystem::copy_options::none, ec);
        }
        ec.clear();
        const auto size = std::filesystem::file_size(Path, ec);
        const bool rotate = !ec && size >= MaxLogBytes;
        std::deque<std::string> retained;
        if (rotate) {
            std::ifstream old(Path);
            if (size > MaxLogBytes) {
                old.seekg(-static_cast<std::streamoff>(MaxLogBytes), std::ios::end);
                std::string partial;
                std::getline(old, partial);
            }
            std::string line;
            while (std::getline(old, line)) {
                retained.push_back(std::move(line));
                if (retained.size() > 20) retained.pop_front();
            }
        }
        std::ofstream out(Path, rotate ? std::ios::trunc : std::ios::app);
        if (!out) return;
        for (const auto& line : retained) out << line << '\n';
        std::array<char, 32> date{};
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(date.data(), date.size(), "%Y-%m-%d %H:%M:%S", &local);
        out << date.data() << " | " << oneLine(area) << " | " << oneLine(message) << '\n';
    }
    catch (...) {}
}

std::vector<std::string> recent(size_t limit)
{
    std::lock_guard<std::mutex> lock(logMutex);
    std::ifstream in(Path);
    if (!in) {
        std::error_code ec;
        if (!std::filesystem::is_directory(std::filesystem::path(Path).parent_path(), ec))
            return {};
        in.clear();
        in.open(LegacyPath);
    }
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    if (length > static_cast<std::streamoff>(MaxLogBytes)) {
        in.seekg(-static_cast<std::streamoff>(MaxLogBytes), std::ios::end);
        std::string partial;
        std::getline(in, partial);
    }
    else in.seekg(0, std::ios::beg);
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        tail.push_back(std::move(line));
        if (tail.size() > limit) tail.pop_front();
    }
    return {tail.rbegin(), tail.rend()};
}

bool exportFile(std::string& error)
{
    std::lock_guard<std::mutex> lock(logMutex);
    error.clear();
    std::error_code ec;
    if (std::filesystem::is_regular_file(Path, ec)) return true;
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = storage::describeError(Path, ec);
        return false;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(LegacyPath, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory)
            error = storage::describeError(LegacyPath, ec);
        return false;
    }
    ec.clear();
    if (!std::filesystem::is_directory(std::filesystem::path(Path).parent_path(), ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory)
            error = storage::describeError(Path, ec);
        return false;
    }
    std::filesystem::copy_file(LegacyPath, Path, std::filesystem::copy_options::none, ec);
    if (ec) error = storage::describeError(Path, ec);
    return !ec;
}
}
