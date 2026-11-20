#include "port_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace port_catalog {
namespace {

bool zipUrl(const std::string& url)
{
    const size_t end = url.find_first_of("?#");
    const std::string path = url.substr(0, end);
    if (path.size() < 4)
        return false;
    std::string extension = path.substr(path.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".zip";
}

bool numberedPart(const std::string& label, std::string& base, unsigned& number)
{
    constexpr const char marker[] = " (PARTE ";
    const size_t start = label.rfind(marker);
    if (start == std::string::npos || start == 0 || label.back() != ')')
        return false;
    const size_t digit = start + sizeof(marker) - 1;
    if (digit + 2 != label.size() || label[digit] < '1' || label[digit] > '8')
        return false;
    base = label.substr(0, start);
    number = static_cast<unsigned>(label[digit] - '0');
    return true;
}

} // namespace

std::vector<Entry> group(const std::vector<Entry>& entries)
{
    std::map<std::string, std::map<unsigned, size_t>> candidates;
    std::set<std::string> invalid;
    std::set<std::string> originalLabels;
    for (const auto& entry : entries)
        originalLabels.insert(entry.label);

    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        std::string base;
        unsigned number = 0;
        if (!numberedPart(entry.label, base, number))
            continue;
        if (entry.parts.size() != 1 || !zipUrl(entry.parts.front().url)) {
            invalid.insert(base);
            continue;
        }
        if (!candidates[base].emplace(number, index).second)
            invalid.insert(base);
    }

    std::map<size_t, Entry> grouped;
    std::set<size_t> hidden;
    for (const auto& [base, parts] : candidates) {
        if (invalid.count(base) || originalLabels.count(base) || parts.size() < 2 ||
            parts.size() > 8 || parts.begin()->first != 1 || parts.rbegin()->first != parts.size())
            continue;
        size_t firstIndex = entries.size();
        for (const auto& [number, index] : parts)
            firstIndex = std::min(firstIndex, index);
        Entry merged{base, entries[parts.begin()->second].iconUrl, {}};
        for (const auto& [number, index] : parts) {
            merged.parts.push_back(entries[index].parts.front());
            hidden.insert(index);
        }
        grouped.emplace(firstIndex, std::move(merged));
    }

    std::vector<Entry> result;
    result.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto merged = grouped.find(index);
        if (merged != grouped.end())
            result.push_back(merged->second);
        else if (!hidden.count(index))
            result.push_back(entries[index]);
    }
    return result;
}

} // namespace port_catalog
