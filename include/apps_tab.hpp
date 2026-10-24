#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>

class AppsTab : public brls::BoxLayout
{
public:
    AppsTab();

private:
    struct App {
        std::string label;
        std::string url;
    };

    void loadCatalog();
    void rebuildCatalog();
    void updateControls(size_t visibleCount);
    void openApp(const App& app);

    std::vector<App> apps;
    std::string searchText;
    bool emulators = false;
    bool loadFailed = false;

    brls::Button* applicationsButton = nullptr;
    brls::Button* emulatorsButton = nullptr;
    brls::Button* searchButton = nullptr;
    brls::Label* countLabel = nullptr;
    brls::ScrollView* catalogView = nullptr;
    brls::View* catalogContent = nullptr;
};
