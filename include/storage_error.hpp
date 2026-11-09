#pragma once

#include <string>
#include <system_error>

namespace storage {
    std::string describeError(const std::string& path, const std::error_code& error);
}
