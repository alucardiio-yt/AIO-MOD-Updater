#pragma once

#include <string>
#include <vector>

namespace diagnostic_log {

constexpr const char* Path = "/switch/aio-switch-updater-mod/error-log.txt";
constexpr const char* LegacyPath = "/config/aio-switch-updater-mod/error-log.txt";
constexpr const char* ExportPath = Path;

void record(const std::string& area, const std::string& message) noexcept;
std::vector<std::string> recent(size_t limit = 20);
bool exportFile(std::string& error);

}
