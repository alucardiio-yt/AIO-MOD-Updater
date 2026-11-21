#pragma once

#include <string>
#include <vector>

namespace catalog_news {

enum class Catalog { Apps, Ports };

void observe(Catalog catalog, const std::vector<std::string>& names);
bool hasNew(Catalog catalog);
bool markSeen(Catalog catalog);
bool isNew(Catalog catalog, const std::string& name);
bool markItemsSeen(Catalog catalog, const std::vector<std::string>& names);

} // namespace catalog_news
