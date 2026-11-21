#pragma once

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <thread>

class CatalogRefreshDialog final : public brls::Dialog
{
public:
    CatalogRefreshDialog(std::function<void()> fetch, std::function<void()> complete);
    ~CatalogRefreshDialog() override;

    void onShowAnimationEnd() override;
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override;

private:
    std::function<void()> fetch;
    std::function<void()> complete;
    std::thread worker;
    std::atomic_bool finished{false};
    bool started = false;
    bool shown = false;
    bool closing = false;
};

class CatalogRefreshButtonRow final : public brls::BoxLayout
{
public:
    explicit CatalogRefreshButtonRow(brls::Button* button);

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override;
};
