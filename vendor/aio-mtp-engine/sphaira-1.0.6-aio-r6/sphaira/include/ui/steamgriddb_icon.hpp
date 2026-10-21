#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>
#include <switch.h>

namespace sphaira::ui::steamgriddb {

using IconCallback = std::function<void(std::vector<u8>)>;

// Converts supported image data into a 256x256 JPEG suitable for a Control
// NCA icon. Returns an empty vector if the image cannot be decoded.
auto NormalizeIcon(std::span<const u8> icon) -> std::vector<u8>;

// Searches SteamGridDB using the supplied title, then displays a grid of
// suitable HOME Menu icons. The API key is requested on first use and saved
// in Sphaira's configuration file.
void ShowIconPicker(const std::string& title, const IconCallback& callback);

} // namespace sphaira::ui::steamgriddb
