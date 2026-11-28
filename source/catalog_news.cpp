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
    std::set<std::string> unseen;
    bool pending = false;
};

std::array<State, 2> states;

std::size_t index(Catalog catalog) { return catalog == Catalog::Apps ? 0u : 1u; }

std::filesystem::path file(Catalog catalog)
{
    return std::string(CONFIG_PATH) + (catalog == Catalog::Apps
        ? "apps_seen.json" : "ports_seen.json");
}

std::filesystem::path portsItemsFile()
{
    return std::string(CONFIG_PATH) + "ports_items_seen.json";
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

bool writeNames(const std::filesystem::path& target, const std::vector<std::string>& names)
{
    if (names.empty()) return false;
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

bool writeNames(Catalog catalog, const std::vector<std::string>& names)
{
    return writeNames(file(catalog), names);
}

} // namespace

void observe(Catalog catalog, const std::vector<std::string>& names)
{
    if (names.empty()) return; // A failed or empty download must not alter the baseline.
    auto& state = states[index(catalog)];
    state.current = names;
    state.unseen.clear();
    state.pending = false;

    std::set<std::string> seen;
    const auto primary = file(catalog);
    const auto backup = primary.string() + ".bak";
    if (!readNames(primary, seen) && !readNames(backup, seen)) {
        writeNames(catalog, names); // First successful catalog becomes the baseline.
        seen.insert(names.begin(), names.end());
    }
    for (const auto& name : names) {
        if (!seen.count(name)) state.pending = true;
    }
    if (catalog == Catalog::Ports) {
        std::set<std::string> itemsSeen;
        const auto items = portsItemsFile();
        if (!readNames(items, itemsSeen) && !readNames(items.string() + ".bak", itemsSeen)) {
            itemsSeen = seen;
            writeNames(items, std::vector<std::string>(itemsSeen.begin(), itemsSeen.end()));
        }
        for (const auto& name : names)
            if (!itemsSeen.count(name)) state.unseen.insert(name);
    }
    else {
        for (const auto& name : names)
            if (!seen.count(name)) state.unseen.insert(name);
    }
}

namespace {
const auto updatesFile = std::string(CONFIG_PATH) + "ports_updates_seen.json";
std::set<std::string>& seenUpdates()
{
    static std::set<std::string> seen = [] {
        std::set<std::string> result;
        if (!readNames(updatesFile, result)) readNames(updatesFile + ".bak", result);
        return result;
    }();
    return seen;
}
}

bool hasUnreadUpdate(const std::string& name, const std::string& revision)
{
    return !revision.empty() && !seenUpdates().count(name + '\n' + revision);
}

bool markUpdateSeen(const std::string& name, const std::string& revision)
{
    if (!hasUnreadUpdate(name, revision)) return true;
    auto next = seenUpdates();
    const auto prefix = name + '\n';
    std::erase_if(next, [&](const auto& value) { return value.starts_with(prefix); });
    next.insert(prefix + revision);
    if (!writeNames(updatesFile, std::vector<std::string>(next.begin(), next.end()))) return false;
    seenUpdates().swap(next);
    return true;
}

bool hasNew(Catalog catalog) { return states[index(catalog)].pending; }

bool markSeen(Catalog catalog)
{
    auto& state = states[index(catalog)];
    if (state.current.empty() || !writeNames(catalog, state.current)) return false;
    if (catalog == Catalog::Apps) state.unseen.clear();
    state.pending = false;
    return true;
}

bool isNew(Catalog catalog, const std::string& name)
{
    return states[index(catalog)].unseen.count(name) != 0;
}

bool markItemsSeen(Catalog catalog, const std::vector<std::string>& names)
{
    if (catalog != Catalog::Ports) return false;
    auto& state = states[index(catalog)];
    auto remaining = state.unseen;
    for (const auto& name : names) remaining.erase(name);
    if (remaining.size() == state.unseen.size()) return true;

    std::vector<std::string> seen;
    seen.reserve(state.current.size());
    for (const auto& name : state.current)
        if (!remaining.count(name)) seen.push_back(name);
    if (!writeNames(portsItemsFile(), seen)) return false;

    state.unseen = std::move(remaining);
    return true;
}

} // namespace catalog_news
