#pragma once

#include <borealis.hpp>
#include "port_catalog.hpp"
#include <functional>
#include <future>
#include <map>
#include <optional>
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

    using Revisions = std::map<std::string, std::string>;
    std::future<std::optional<Revisions>> pendingUpdates;
    Revisions revisions;
    bool requestUpdates = true;
    bool updatesLoaded = false;
    std::vector<std::function<void()>> updateBadges;
    void willAppear(bool resetState) override;
    void draw(NVGcontext*, int, int, unsigned, unsigned, brls::Style*, brls::FrameContext*) override;
    void loadCatalog();
    static CatalogResult fetchCatalog(long timeoutSeconds);
    void refreshCatalog();
    void rebuildCatalog();
    void openPort(const Port& port);
    bool markPortSeen(const Port& port);
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
