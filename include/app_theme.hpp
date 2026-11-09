#pragma once

#include <borealis/theme.hpp>

#include <cstddef>

namespace brls { class View; }

namespace app_theme {

constexpr std::size_t Count = 9;

std::size_t selected();
void load();
bool select(std::size_t index);
void apply(brls::LibraryViewsThemeVariantsWrapper* variants);
const char* labelKey(std::size_t index);
brls::View* createTab();
void openPicker();

} // namespace app_theme
