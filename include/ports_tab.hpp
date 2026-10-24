#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

class PortsTab : public brls::BoxLayout
{
public:
    PortsTab();

private:
    struct Port {
        std::string label;
        std::string url;
        std::string iconPath;
    };

    void loadCatalog();
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
