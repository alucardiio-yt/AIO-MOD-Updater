#pragma once

#include <string>
#include <vector>

namespace port_catalog {

struct Part {
    std::string label;
    std::string url;
};

struct Entry {
    std::string label;
    std::string iconUrl;
    std::vector<Part> parts;
};

// Groups only complete, consecutively numbered ZIP parts. Other entries keep
// their original name, order, and download URL.
std::vector<Entry> group(const std::vector<Entry>& entries);

} // namespace port_catalog
