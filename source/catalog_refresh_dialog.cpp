#include "catalog_refresh_dialog.hpp"

#include <algorithm>
#include <utility>

namespace i18n = brls::i18n;
using namespace i18n::literals;

CatalogRefreshDialog::CatalogRefreshDialog(std::function<void()> fetch,
                                           std::function<void()> complete)
    : brls::Dialog("menus/common/catalog_refreshing"_i18n),
      fetch(std::move(fetch)), complete(std::move(complete))
{
    setCancelable(false);
}

CatalogRefreshDialog::~CatalogRefreshDialog()
{
    if (worker.joinable())
        worker.join();
}

void CatalogRefreshDialog::onShowAnimationEnd()
{
    shown = true;
}

void CatalogRefreshDialog::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
                                brls::Style* style, brls::FrameContext* ctx)
{
    brls::Dialog::draw(vg, x, y, width, height, style, ctx);
    if (!started) {
        started = true;
        try {
            worker = std::thread([this] {
                try { fetch(); }
                catch (...) {}
                finished.store(true, std::memory_order_release);
            });
        }
        catch (...) {
            finished.store(true, std::memory_order_release);
        }
    }
    else if (shown && finished.load(std::memory_order_acquire) && !closing) {
        closing = true;
        if (worker.joinable())
            worker.join();
        auto onComplete = std::move(complete);
        close([onComplete = std::move(onComplete)]() mutable { onComplete(); });
    }
}

CatalogRefreshButtonRow::CatalogRefreshButtonRow(brls::Button* button)
    : brls::BoxLayout(brls::BoxLayoutOrientation::HORIZONTAL)
{
    setHeight(68);
    addView(button);
}

void CatalogRefreshButtonRow::layout(NVGcontext* vg, brls::Style* style,
                                     brls::FontStash* stash)
{
    const unsigned width = std::min(getWidth(), 300u);
    getChild(0)->setBoundaries(getX() + static_cast<int>((getWidth() - width) / 2),
                               getY() + 8, width, getHeight() - 16);
    getChild(0)->invalidate(true);
}
