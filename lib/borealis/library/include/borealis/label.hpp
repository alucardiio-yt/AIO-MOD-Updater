/*
    Borealis, a Nintendo Switch UI Library
    Copyright (C) 2019-2020  natinusala
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

#pragma once

#include <cstdint>
#include <borealis/view.hpp>
#include <borealis/animations.hpp>

namespace brls
{

enum class LabelStyle : std::uint32_t
{
    REGULAR = 0,
    MEDIUM,
    SMALL,
    DESCRIPTION,
    FPS,
    CRASH,
    BUTTON_PRIMARY,
    BUTTON_PRIMARY_DISABLED,
    BUTTON_BORDERLESS,
    LIST_ITEM,
    LIST_ITEM_VALUE,
    LIST_ITEM_VALUE_FAINT,
    NOTIFICATION,
    DIALOG,
    BUTTON_DIALOG,
    HINT,
    BUTTON_BORDERED,
    BUTTON_REGULAR
};

enum class LabelAnimation
{
    EASE_IN,
    EASE_OUT
};

// A Label, multiline or with a ticker
class Label : public View
{
  private:
    std::string text = "";
    std::string textTicker = "";
    std::string textEllipsis = "";

    bool multiline;
    unsigned fontSize;
    float lineHeight;
    LabelStyle labelStyle, oldLabelStyle;

    NVGalign horizontalAlign = NVG_ALIGN_LEFT;
    NVGalign verticalAlign   = NVG_ALIGN_MIDDLE;

    NVGcolor customColor;
    bool useCustomColor = false;

    int customFont;
    bool useCustomFont = false;

    unsigned textWidth = 0, textHeight = 0;
    unsigned oldWidth = 0;

    unsigned textTickerWidth = 0;
    float tickerOffset = 0.0f;

    bool tickerActive = false;
    menu_timer_t tickerWaitTimer;
    menu_timer_ctx_entry_t tickerWaitTimerCtx;

    float textAnimation = 1.0f;

    GenericEvent::Subscription parentFocusSubscription;

    unsigned getFontSize(LabelStyle labelStyle);
    float getLineHeight(LabelStyle labelStyle);

    void onParentFocus();
    void onParentUnfocus();

    void updateTextDimensions();

    size_t getUtf8StringLength(const std::string& str);
    std::string getUtf8SubString(const std::string& str, size_t start, size_t len = std::string::npos);

  public:
    Label(LabelStyle labelStyle, std::string text, bool multiline = false);
    ~Label();

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style, FrameContext* ctx) override;
    void layout(NVGcontext* vg, Style* style, FontStash* stash) override;

    void setVerticalAlign(NVGalign align);
    void setHorizontalAlign(NVGalign align);
    void setText(std::string text);
    void setStyle(LabelStyle style);
    void setFontSize(unsigned size);

    void startTickerAnimation();
    void stopTickerAnimation();

    const std::string& getText();

    /**
     * Only useful for single line labels.
     */
    unsigned getTextWidth();
    unsigned getTextHeight();

    /**
     * Sets the label color
     */
    void setColor(NVGcolor color);

    /**
     * Unsets the label color - it
     * will now use the default one
     * for the label style
     */
    void unsetColor();

    /**
     * Returns the effective label color
     * = custom or the style default
     */
    NVGcolor getColor(Theme* theme);

    /**
     * Sets the font id
     */
    void setFont(int fontId);

    /**
     * Unsets the font id - it
     * will now use the regular one
     */
    void unsetFont();

    /**
     * Returns the font used
     * = custom or the regular font
     */
    int getFont(FontStash* stash);

    /**
     * Sets the ticker state to active (scrolling) or inactive (ellipsis)
     */
    void setTickerState(bool active);

    float getTextAnimation();
    void resetTextAnimation();
    void animate(LabelAnimation animation);
};

} // namespace brls
