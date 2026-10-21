#pragma once

#include <switch.h>
#include <unordered_map>
#include <vector>

namespace sphaira::ui {
struct ProgressBox;
}

namespace sphaira::nx_versions {

enum class CatalogState {
    Missing,
    Stale,
    Current,
};

struct AvailableEntry {
    u64 application_id{};
    u32 version{};
    u8 meta_type{};
    bool installed{};
};

using InstalledVersions = std::unordered_map<u64, u32>;

auto GetAvailable(u64 app_id, const InstalledVersions& installed) -> std::vector<AvailableEntry>;
auto HasEntries(u64 app_id) -> bool;
auto HasAvailable(u64 app_id, const InstalledVersions& installed) -> bool;
auto GetCatalogState() -> CatalogState;
auto GetCatalogRevision() -> u64;
auto ShouldPromptForUpdate() -> bool;
auto Download(ui::ProgressBox* pbox) -> Result;

}
