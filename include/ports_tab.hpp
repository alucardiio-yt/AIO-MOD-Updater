#pragma once

#include <borealis.hpp>
#include "port_catalog.hpp"
#include <string>
#include <vector>

class PortsTab : public brls::BoxLayout
{
public:
    PortsTab();

private:
    struct Port {
        std::string label;
        std::string iconPath;
        std::vector<port_catalog::Part> parts;
    };

    struct CatalogResult {
        std::vector<Port> entries;
        std::vector<std::string> names;
    };

    void loadCatalog();
    static CatalogResult fetchCatalog(long timeoutSeconds);
    void refreshCatalog();
    void rebuildCatalog();
    void openPort(const Port& port);
    void updateControls(size_t visibleCount);

    std::vector<Port> ports;
    std::string searchText;
    bool gridView = false;
    bool loadFailed = false;

    brls::Button* searchButton = nullptr;
    brls::Button* viewButton = nullptr;
    brls::Label* countLabel = nullptr;
    brls::ScrollView* catalogView = nullptr;
    brls::View* catalogContent = nullptr;
};
