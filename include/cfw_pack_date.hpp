#pragma once

#include <string>

namespace cfw_pack_date {

// Empty when GitHub is unavailable or the release has no NX.latest.zip asset.
std::string latestZipDateUtc();

} // namespace cfw_pack_date
