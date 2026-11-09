/*
    Borealis, a Nintendo Switch UI Library
    Copyright (C) 2019  natinusala
    Copyright (C) 2019  WerWolv
    Copyright (C) 2019  p-sam

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <borealis/application.hpp>
#include <borealis/i18n.hpp>
#include <borealis/sidebar.hpp>

#include <algorithm>
#include <cmath>

using namespace brls::i18n::literals;

namespace brls
{

Sidebar::Sidebar()
    : BoxLayout(BoxLayoutOrientation::VERTICAL)
{
    Style* style = Application::getStyle();

    this->setWidth(style->Sidebar.width);
    this->setSpacing(style->Sidebar.spacing);
    this->setMargins(style->Sidebar.marginTop, style->Sidebar.marginRight, style->Sidebar.marginBottom, style->Sidebar.marginLeft);
    this->setBackground(ViewBackground::SIDEBAR);
}

View* Sidebar::getDefaultFocus()
{
    // Sanity check
    if (this->lastFocus >= this->children.size())
        this->lastFocus = 0;

    View* toFocus { nullptr };
    // Try to focus last focused one
    if (this->children.size() != 0)
        toFocus = this->children[this->lastFocus]->view->getDefaultFocus();

    if (toFocus)
        return toFocus;

    // Otherwise just get the first available item
    return BoxLayout::getDefaultFocus();
}

void Sidebar::onChildFocusGained(View* child)
{
    size_t position = *((size_t*)child->getParentUserData());

    this->lastFocus = position;

    BoxLayout::onChildFocusGained(child);
}

SidebarItem* Sidebar::addItem(std::string label, View* view)
{
    SidebarItem* item = new SidebarItem(label, this);
    item->setAssociatedView(view);

    if (this->isEmpty())
        setActive(item);

    this->addView(item);

    return item;
}

void Sidebar::addSeparator()
{
    SidebarSeparator* separator = new SidebarSeparator();
    this->addView(separator);
}

void Sidebar::setActive(SidebarItem* active)
{
    if (currentActive)
        currentActive->setActive(false);

    currentActive = active;
    active->setActive(true);
}

SidebarItem::SidebarItem(std::string label, Sidebar* sidebar)
    : label(label)
    , sidebar(sidebar)
{
    Style* style = Application::getStyle();
    this->setHeight(style->Sidebar.Item.height);

    this->registerAction("brls/hints/ok"_i18n, Key::A, [this] { return this->onClick(); });
}

void SidebarItem::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style, FrameContext* ctx)
{
    const int labelX = x + style->Sidebar.Item.textOffsetX + style->Sidebar.Item.padding;
    int textX = labelX;
    float badgeWidth = 0.f;
    if (!this->badge.empty())
    {
        nvgFontFaceId(vg, ctx->fontStash->regular);
        nvgFontSize(vg, 13.f);
        badgeWidth = nvgTextBounds(vg, 0, 0, this->badge.c_str(), nullptr, nullptr) + 18.f;
    }
    nvgFillColor(vg, a(this->active ? ctx->theme->activeTabColor : ctx->theme->textColor));
    if (!this->icon.empty())
    {
        nvgFontSize(vg, 23);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, ctx->fontStash->material);
        nvgText(vg, labelX, y + height / 2, this->icon.c_str(), nullptr);
        textX += 32;
    }

    // Label
    nvgFontSize(vg, style->Sidebar.Item.textSize);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, ctx->fontStash->regular);
    if (!this->icon.empty())
    {
        const float available = static_cast<float>(width) - (textX - x) - style->Sidebar.Item.padding
            - (badgeWidth > 0.f ? badgeWidth + 8.f : 0.f);
        const float labelWidth = nvgTextBounds(vg, 0, 0, this->label.c_str(), nullptr, nullptr);
        if (available > 0 && labelWidth > available)
            nvgFontSize(vg, std::max(16.0f, style->Sidebar.Item.textSize * available / labelWidth));
    }
    nvgBeginPath(vg);
    if (badgeWidth > 0.f)
    {
        nvgSave(vg);
        nvgIntersectScissor(vg, textX, y, std::max(0.f,
            static_cast<float>(width) - (textX - x) - style->Sidebar.Item.padding - badgeWidth - 8.f), height);
    }
    nvgText(vg, textX, y + height / 2, this->label.c_str(), nullptr);
    if (badgeWidth > 0.f)
    {
        nvgRestore(vg);
        const float left = x + width - style->Sidebar.Item.padding - badgeWidth;
        const float top = y + (height - 22.f) / 2.f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, left, top, badgeWidth, 22.f, 8.f);
        nvgFillColor(vg, a(ctx->theme->activeTabColor));
        nvgFill(vg);
        nvgFontFaceId(vg, ctx->fontStash->regular);
        nvgFontSize(vg, 13.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        const auto linear = [](float channel) {
            return channel <= .04045f ? channel / 12.92f
                : std::pow((channel + .055f) / 1.055f, 2.4f);
        };
        const auto accent = ctx->theme->activeTabColor;
        const float luminance = .2126f * linear(accent.r) + .7152f * linear(accent.g)
            + .0722f * linear(accent.b);
        nvgFillColor(vg, luminance > .179f ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgText(vg, left + badgeWidth / 2.f, y + height / 2.f, this->badge.c_str(), nullptr);
    }

    // Active marker
    if (this->active)
    {
        nvgFillColor(vg, a(ctx->theme->activeTabColor));
        nvgBeginPath(vg);
        nvgRect(vg, x + style->Sidebar.Item.padding, y + style->Sidebar.Item.padding, style->Sidebar.Item.activeMarkerWidth, style->Sidebar.Item.height - style->Sidebar.Item.padding * 2);
        nvgFill(vg);
    }
}

bool SidebarItem::onClick()
{
    Application::onGamepadButtonPressed(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    return true;
}

void SidebarItem::setActive(bool active)
{
    this->active = active;
}

void SidebarItem::setIcon(std::string icon)
{
    this->icon = std::move(icon);
}

void SidebarItem::setBadge(std::string badge)
{
    this->badge = std::move(badge);
}

SidebarSeparator::SidebarSeparator()
{
    Style* style = Application::getStyle();
    this->setHeight(style->Sidebar.Separator.height);
}

void SidebarSeparator::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style, FrameContext* ctx)
{
    nvgFillColor(vg, a(ctx->theme->sidebarSeparatorColor));
    nvgBeginPath(vg);
    nvgRect(vg, x, y + height / 2, width, 1);
    nvgFill(vg);
}

void SidebarItem::setAssociatedView(View* view)
{
    this->associatedView = view;
}

bool SidebarItem::isActive()
{
    return this->active;
}

void SidebarItem::onFocusGained()
{
    this->sidebar->setActive(this);
    View::onFocusGained();
}

View* SidebarItem::getAssociatedView()
{
    return this->associatedView;
}


ViewType SidebarItem::getViewType()
{
    return this->viewType;
}

SidebarItem::~SidebarItem()
{
    if (this->associatedView)
        delete this->associatedView;
}

} // namespace brls
