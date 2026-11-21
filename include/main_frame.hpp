#pragma once

#include <borealis.hpp>

class MainFrame : public brls::TabFrame
{
private:
    //RefreshTask *refreshTask;
    bool appletMode = false;
    brls::SidebarItem* portsSidebarItem = nullptr;
    bool portsBadgeShown = false;

public:
    MainFrame();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override;
};
