#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hb_store {
struct Package {
    std::string name, title, author, version, description, details, md5;
    std::string category, updated, binary, changelog;
    std::uint64_t downloads = 0;
    std::uint64_t downloadKiB = 0, installedKiB = 0;
};

struct Catalog {
    std::vector<Package> packages;
    bool offline = false;
};

void open();
Catalog load();
std::string artwork(const std::string& name, bool banner, const std::atomic<bool>& stop);
void install(const Package& package);
}
