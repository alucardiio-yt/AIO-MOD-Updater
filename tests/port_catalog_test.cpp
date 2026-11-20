#include "port_catalog.hpp"

#include <cassert>
#include <string>
#include <vector>

using port_catalog::Entry;

static Entry item(const std::string& label, const std::string& url)
{
    return {label, "icon.png", {{label, url}}};
}

int main()
{
    const auto grouped = port_catalog::group({
        item("Solo", "solo.zip"),
        item("Game (PARTE 2)", "part2.zip"),
        item("Other", "other.nro"),
        item("Game (PARTE 1)", "part1.zip"),
        item("Game (PARTE 3)", "part3.zip"),
    });
    assert(grouped.size() == 3);
    assert(grouped[0].label == "Solo");
    assert(grouped[1].label == "Game");
    assert(grouped[1].parts.size() == 3);
    assert(grouped[1].parts[0].url == "part1.zip");
    assert(grouped[1].parts[1].url == "part2.zip");
    assert(grouped[1].parts[2].url == "part3.zip");
    assert(grouped[2].label == "Other");

    const auto missing = port_catalog::group({
        item("Incomplete (PARTE 1)", "first.zip"),
        item("Incomplete (PARTE 3)", "third.zip"),
    });
    assert(missing.size() == 2);
    assert(missing[0].label == "Incomplete (PARTE 1)");

    const auto mixed = port_catalog::group({
        item("Mixed (PARTE 1)", "first.zip"),
        item("Mixed (PARTE 2)", "second.nro"),
    });
    assert(mixed.size() == 2);

    const auto duplicate = port_catalog::group({
        item("Duplicate (PARTE 1)", "first.zip"),
        item("Duplicate (PARTE 1)", "other.zip"),
        item("Duplicate (PARTE 2)", "second.zip"),
    });
    assert(duplicate.size() == 3);
}
