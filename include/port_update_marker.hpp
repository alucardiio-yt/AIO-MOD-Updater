#pragma once

#include <json.hpp>
#include <regex>
#include <string>

namespace port_update {

inline std::string marker(const nlohmann::ordered_json& text)
{
    if (!text.is_string()) return {};
    const auto& description = text.get_ref<const std::string&>();
    static const std::regex pattern(
        R"(\b(?:actualizaci(?:ó|Ó|o)n|update)(?:[ \t]+|[ \t]*:[ \t]*)v?(\d{1,8}(?:\.\d{1,8}){0,4})(?![\w]|\.\d)(?:[ \t]+(\d{2}-\d{2}-\d{2}(?:\d{2})?)\b)?)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(description, match, pattern)) return {};
    return match[2].matched ? match[1].str() + " " + match[2].str() : match[1].str();
}

inline std::string revision(const nlohmann::ordered_json& entry)
{
    const auto desc = entry.find("description");
    if (desc == entry.end()) return {};
    if (!desc->is_object()) return marker(*desc);
    // A single revision applies in every UI language.
    const auto spanish = desc->find("es");
    if (spanish != desc->end()) {
        auto value = marker(*spanish);
        if (!value.empty()) return value;
    }
    for (auto it = desc->begin(); it != desc->end(); ++it) {
        if (it == spanish) continue;
        auto value = marker(it.value());
        if (!value.empty()) return value;
    }
    return {};
}

} // namespace port_update
