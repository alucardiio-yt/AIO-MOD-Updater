#include "catalog_news.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

#include <json.hpp>

#include "constants.hpp"

namespace catalog_news {
namespace {

struct State {
    std::vector<std::string> current;
    bool pending = false;
};

std::array<State, 2> states;

std::size_t index(Catalog catalog) { return catalog == Catalog::Apps ? 0u : 1u; }

std::filesystem::path file(Catalog catalog)
{
    return std::string(CONFIG_PATH) + (catalog == Catalog::Apps
        ? "apps_seen.json" : "ports_seen.json");
}

bool readNames(const std::filesystem::path& path, std::set<std::string>& names)
{
    std::ifstream input(path);
    if (!input) return false;
    try {
        std::set<std::string> parsed;
        const auto data = nlohmann::ordered_json::parse(input);
        if (!data.is_array()) return false;
        for (const auto& value : data) {
            if (!value.is_string()) return false;
            parsed.insert(value.get<std::string>());
        }
        if (parsed.empty()) return false;
        names = std::move(parsed);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool writeNames(Catalog catalog, const std::vector<std::string>& names)
{
    if (names.empty()) return false;
    const auto target = file(catalog);
    const auto temporary = target.string() + ".tmp";
    const auto backup = target.string() + ".bak";
    std::error_code ec;
    std::filesystem::create_directories(CONFIG_PATH, ec);
    if (ec) return false;
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << nlohmann::ordered_json(names).dump() << '\n';
        output.close();
        if (!output) return false;
    }
    const bool hadTarget = std::filesystem::exists(target, ec);
    if (ec) return false;
    if (hadTarget) {
        std::filesystem::remove(backup, ec);
        if (ec) return false;
        std::filesystem::rename(target, backup, ec);
        if (ec) return false;
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        if (hadTarget) {
            std::error_code restore;
            std::filesystem::rename(backup, target, restore);
        }
        return false;
    }
    if (hadTarget) std::filesystem::remove(backup, ec);
    return true;
}

} // namespace

void observe(Catalog catalog, const std::vector<std::string>& names)
{
    if (names.empty()) return; // A failed or empty download must not alter the baseline.
    auto& state = states[index(catalog)];
    state.current = names;
    state.pending = false;

    std::set<std::string> seen;
    const auto primary = file(catalog);
    const auto backup = primary.string() + ".bak";
    if (!readNames(primary, seen) && !readNames(backup, seen)) {
        writeNames(catalog, names); // First successful catalog becomes the baseline.
        return;
    }
    for (const auto& name : names) {
        if (!seen.count(name)) {
            state.pending = true;
            break;
        }
    }
}

bool hasNew(Catalog catalog) { return states[index(catalog)].pending; }

bool markSeen(Catalog catalog)
{
    auto& state = states[index(catalog)];
    if (state.current.empty() || !writeNames(catalog, state.current)) return false;
    state.pending = false;
    return true;
}

} // namespace catalog_news
