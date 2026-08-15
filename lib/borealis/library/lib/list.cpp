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

#include <math.h>

#include <borealis/animations.hpp>
#include <borealis/application.hpp>
#include <borealis/dropdown.hpp>
#include <borealis/header.hpp>
#include <borealis/i18n.hpp>
#include <borealis/list.hpp>
#include <borealis/logger.hpp>
#include <borealis/swkbd.hpp>
#include <borealis/table.hpp>

using namespace brls::i18n::literals;

namespace brls
{

ListContentView::ListContentView(List* list, size_t defaultFocus)
    : BoxLayout(BoxLayoutOrientation::VERTICAL, defaultFocus)
    , list(list)
{
    Style* style = Application::getStyle();
    this->setMargins(style->List.marginTopBottom, style->List.marginLeftRight, style->List.marginTopBottom, style->List.marginLeftRight);
    this->setSpacing(style->List.spacing);
    this->setRememberFocus(true);
}

void ListContentView::customSpacing(View* current, View* next, int* spacing)
{
    // Don't add spacing to the first list item
    // if it doesn't have a description and the second one is a
    // list item too
    // Or if the next item is a ListItemGroupSpacing
    if (ListItem* currentItem = dynamic_cast<ListItem*>(current))
    {
        if (currentItem->getReduceDescriptionSpacing())
        {
            if (next != nullptr)
                *spacing /= 2;
        }
        else if (ListItem* nextItem = dynamic_cast<ListItem*>(next))
        {
            if (!currentItem->hasDescription())
            {
                *spacing = 2;

                nextItem->setDrawTopSeparator(currentItem->isCollapsed());
            }
        }
        else if (dynamic_cast<ListItemGroupSpacing*>(next))
        {
            *spacing = 0;
        }
        else if (dynamic_cast<Table*>(next))
        {
            *spacing /= 2;
        }
    }
    // Table custom spacing
    else if (dynamic_cast<Table*>(current))
    {
        *spacing /= 2;
    }
    // ListItemGroupSpacing custom spacing
    else if (dynamic_cast<ListItemGroupSpacing*>(current))
    {
        *spacing /= 2;
    }
    // Header custom spacing
    else if (dynamic_cast<Header*>(current) || dynamic_cast<Header*>(next))
    {
        if (dynamic_cast<Header*>(current) && dynamic_cast<ListItem*>(next))
        {
            *spacing = 1;
        }
        else if (dynamic_cast<Label*>(current) && dynamic_cast<Header*>(next))
        {
            // Keep default spacing
        }
        else
        {
            Style* style = Application::getStyle();
            *spacing     = style->Header.padding;
        }
    }

    // Call list custom spacing
    if (this->list)
        this->list->customSpacing(current, next, spacing);
}

ListItem::ListItem(std::string label, std::string description, std::string subLabel)
{
    Style* style = Application::getStyle();

    this->setHeight(subLabel != "" ? style->List.Item.heightWithSubLabel : style->List.Item.height);
    this->setTextSize(style->Label.listItemFontSize);

    this->labelView = new Label(LabelStyle::LIST_ITEM, label, false);
    this->labelView->setParent(this);

    if (description != "")
    {
        this->descriptionView = new Label(LabelStyle::DESCRIPTION, description, true);
        this->descriptionView->setParent(this);
    }

    if (subLabel != "")
    {
        this->subLabelView = new Label(LabelStyle::DESCRIPTION, subLabel, false);
        this->subLabelView->setParent(this);
    }

    this->registerAction("brls/hints/ok"_i18n, Key::A, [this] { return this->onClick(); });
}

void ListItem::setThumbnail(Image* image)
{
    if (this->thumbnailView)
        delete this->thumbnailView;
    if (image != NULL)
    {
        this->thumbnailView = image;
        this->thumbnailView->setParent(this);
        this->invalidate();
    }
}

void ListItem::setThumbnail(std::string imagePath)
{
    if (this->thumbnailView)
        this->thumbnailView->setImage(imagePath);
    else
        this->thumbnailView = new Image(imagePath);

    this->thumbnailView->setParent(this);
    this->thumbnailView->setScaleType(ImageScaleType::FIT);
    this->invalidate();
}

void ListItem::setThumbnail(unsigned char* buffer, size_t bufferSize)
{
    if (this->thumbnailView)
        this->thumbnailView->setImage(buffer, bufferSize);
    else
        this->thumbnailView = new Image(buffer, bufferSize);

    this->thumbnailView->setParent(this);
    this->thumbnailView->setScaleType(ImageScaleType::FIT);
    this->invalidate();
}

bool ListItem::getReduceDescriptionSpacing()
{
    return this->reduceDescriptionSpacing;
}

void ListItem::setReduceDescriptionSpacing(bool value)
{
    this->reduceDescriptionSpacing = value;
}

void ListItem::setIndented(bool indented)
{
    this->indented = indented;
}

void ListItem::setTextSize(unsigned textSize)
{
    this->textSize = textSize;
}

void ListItem::setChecked(bool checked)
{
    this->checked = checked;
}

bool ListItem::onClick()
{
    return this->clickEvent.fire(this);
}

GenericEvent* ListItem::getClickEvent()
{
    return &this->clickEvent;
}

void ListItem::layout(NVGcontext* vg, Style* style, FontStash* stash)
{
    unsigned baseHeight = this->height;
    bool hasSubLabel    = this->subLabelView && this->subLabelView->getText() != "";
    bool hasThumbnail   = this->thumbnailView;
    bool hasValue       = this->valueView && this->valueView->getText() != "";

    if (this->descriptionView)
        baseHeight -= this->descriptionView->getHeight() + style->List.Item.descriptionSpacing;

    unsigned leftPadding = hasThumbnail ? this->thumbnailView->getWidth() + style->List.Item.thumbnailPadding * 2 : style->List.Item.padding;
    unsigned rightPadding = (hasSubLabel || !hasValue) ? style->List.Item.padding : this->valueView->getTextWidth() + style->List.Item.padding * 2;

    this->labelView->setBoundaries(x + leftPadding, y + (baseHeight / (hasSubLabel ? 3 : 2)), width - leftPadding - rightPadding, 0);
    this->labelView->invalidate();

    // Value
    if (hasValue) {
        unsigned valueX = x + width - style->List.Item.padding;
        unsigned valueY = y + (hasSubLabel ? baseHeight - baseHeight / 3 : baseHeight / 2);

        this->valueView->setBoundaries(valueX, valueY, 0, 0);
        if (hasSubLabel) this->valueView->setVerticalAlign(NVG_ALIGN_TOP);
        this->valueView->setHorizontalAlign(NVG_ALIGN_RIGHT);
        this->valueView->invalidate();

        this->oldValueView->setBoundaries(valueX, valueY, 0, 0);
        if (hasSubLabel) this->oldValueView->setVerticalAlign(NVG_ALIGN_TOP);
        this->valueView->setHorizontalAlign(NVG_ALIGN_RIGHT);
        this->oldValueView->invalidate();
    }

    // Sub Label
    if (hasSubLabel)
    {
        rightPadding = hasValue ? this->valueView->getTextWidth() + style->List.Item.padding * 2 : style->List.Item.padding;

        this->subLabelView->setBoundaries(x + leftPadding, y + baseHeight - baseHeight / 3, width - leftPadding - rightPadding, 0);
        this->subLabelView->setVerticalAlign(NVG_ALIGN_TOP);
        this->subLabelView->invalidate();
    }

    // Description
    if (this->descriptionView)
    {
        unsigned indent = style->List.Item.descriptionIndent;

        if (this->indented)
            indent += style->List.Item.indent;

        this->height = style->List.Item.height;
        this->descriptionView->setBoundaries(this->x + indent, this->y + this->height + style->List.Item.descriptionSpacing, this->width - indent * 2, 0);
        this->descriptionView->invalidate(true); // we must call layout directly
        this->height += this->descriptionView->getHeight() + style->List.Item.descriptionSpacing;
    }

    // Thumbnail
    if (this->thumbnailView)
    {
        Style* style           = Application::getStyle();
        unsigned thumbnailSize = height - style->List.Item.thumbnailPadding * 2;

        this->thumbnailView->setBoundaries(
            x + style->List.Item.thumbnailPadding,
            y + style->List.Item.thumbnailPadding,
            thumbnailSize,
            thumbnailSize);
        this->thumbnailView->invalidate();
    }
}

void ListItem::getHighlightInsets(unsigned* top, unsigned* right, unsigned* bottom, unsigned* left)
{
    Style* style = Application::getStyle();
    View::getHighlightInsets(top, right, bottom, left);

    if (descriptionView)
        *bottom = -(descriptionView->getHeight() + style->List.Item.descriptionSpacing);

    if (indented)
        *left = -style->List.Item.indent;
}

void ListItem::setValue(std::string value, bool faint, bool animate)
{
    this->oldValueFaint = this->valueFaint;
    this->valueFaint    = faint;

    if (!this->valueView) {
        this->valueView = new Label(this->valueFaint ? LabelStyle::LIST_ITEM_VALUE_FAINT : LabelStyle::LIST_ITEM_VALUE, value, false);
        this->valueView->setParent(this);

        this->oldValueView = new Label(this->oldValueFaint ? LabelStyle::LIST_ITEM_VALUE_FAINT : LabelStyle::LIST_ITEM_VALUE, "", false);
        this->oldValueView->setParent(this);
    } else {
        this->oldValueView->setText(this->valueView->getText());
        this->oldValueView->setStyle(this->oldValueFaint ? LabelStyle::LIST_ITEM_VALUE_FAINT : LabelStyle::LIST_ITEM_VALUE);

        this->valueView->setText(value);
        this->valueView->setStyle(this->valueFaint ? LabelStyle::LIST_ITEM_VALUE_FAINT : LabelStyle::LIST_ITEM_VALUE);

        if (animate && this->oldValueView->getText() != "")
        {
            this->oldValueView->animate(LabelAnimation::EASE_OUT);
            this->valueView->animate(LabelAnimation::EASE_IN);
        } else {
            this->valueView->resetTextAnimation();
            this->oldValueView->resetTextAnimation();
        }
    }
}

std::string ListItem::getValue()
{
    return this->valueView ? this->valueView->getText() : "";
}

void ListItem::setDrawTopSeparator(bool draw)
{
    this->drawTopSeparator = draw;
}

View* ListItem::getDefaultFocus()
{
    if (this->collapseState != 1.0f)
        return nullptr;

    return this;
}

void ListItem::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, Style* style, FrameContext* ctx)
{
    unsigned baseHeight = this->height;
    bool hasSubLabel    = this->subLabelView && this->subLabelView->getText() != "";
    bool hasThumbnail   = this->thumbnailView;
    bool hasValue       = this->valueView && this->valueView->getText() != "";

    if (this->indented)
    {
        x += style->List.Item.indent;
        width -= style->List.Item.indent;
    }

    // Description
    if (this->descriptionView)
    {
        // Don't count description as part of list item
        baseHeight -= this->descriptionView->getHeight() + style->List.Item.descriptionSpacing;
        this->descriptionView->frame(ctx);
    }

    // Value
    if (hasValue) {
        if (this->valueView->getTextAnimation() != 1.0f)
        {
            this->valueView->frame(ctx);
            this->oldValueView->frame(ctx);
        } else {
            this->valueView->frame(ctx);
        }
    }

    // Checked marker
    if (this->checked)
    {
        unsigned radius  = style->List.Item.selectRadius;
        unsigned centerX = x + width - radius - style->List.Item.padding;
        unsigned centerY = y + baseHeight / 2;

        float radiusf = (float)radius;

        int thickness = roundf(radiusf * 0.10f);

        // Background
        nvgFillColor(vg, a(ctx->theme->listItemValueColor));
        nvgBeginPath(vg);
        nvgCircle(vg, centerX, centerY, radiusf);
        nvgFill(vg);

        // Check mark
        nvgFillColor(vg, a(ctx->theme->backgroundColorRGB));

        // Long stroke
        nvgSave(vg);
        nvgTranslate(vg, centerX, centerY);
        nvgRotate(vg, -NVG_PI / 4.0f);

        nvgBeginPath(vg);
        nvgRect(vg, -(radiusf * 0.55f), 0, radiusf * 1.3f, thickness);
        nvgFill(vg);
        nvgRestore(vg);

        // Short stroke
        nvgSave(vg);
        nvgTranslate(vg, centerX - (radiusf * 0.65f), centerY);
        nvgRotate(vg, NVG_PI / 4.0f);

        nvgBeginPath(vg);
        nvgRect(vg, 0, -(thickness / 2), radiusf * 0.53f, thickness);
        nvgFill(vg);

        nvgRestore(vg);
    }

    // Label
    this->labelView->frame(ctx);

    // Sub Label
    if (hasSubLabel)
        this->subLabelView->frame(ctx);

    // Thumbnail
    if (hasThumbnail)
        this->thumbnailView->frame(ctx);

    // Separators
    // Offset by one to be hidden by highlight
    nvgFillColor(vg, a(ctx->theme->listItemSeparatorColor));

    // Top
    if (this->drawTopSeparator)
    {
        nvgBeginPath(vg);
        nvgRect(vg, x, y - 1, width, 1);
        nvgFill(vg);
    }

    // Bottom
    nvgBeginPath(vg);
    nvgRect(vg, x, y + 1 + baseHeight, width, 1);
    nvgFill(vg);
}

bool ListItem::hasDescription()
{
    return this->descriptionView;
}

void ListItem::setLabel(std::string label)
{
    this->labelView->setText(label);
}

std::string ListItem::getLabel()
{
    return this->labelView->getText();
}

void ListItem::setSubLabel(std::string subLabel)
{
    if (!this->subLabelView) {
        this->subLabelView = new Label(LabelStyle::DESCRIPTION, subLabel, false);
        this->subLabelView->setParent(this);
    } else {
        this->subLabelView->setText(subLabel);
    }

    Style* style = Application::getStyle();
    this->setHeight(subLabel != "" ? style->List.Item.heightWithSubLabel : style->List.Item.height);
}

std::string ListItem::getSubLabel()
{
    return this->subLabelView ? this->subLabelView->getText() : "";
}

void ListItem::setDescription(std::string description)
{
    if (!this->descriptionView) {
        this->descriptionView = new Label(LabelStyle::DESCRIPTION, description, true);
        this->descriptionView->setParent(this);
    } else {
        this->descriptionView->setText(description);
    }
}

std::string ListItem::getDescription()
{
    return this->descriptionView ? this->descriptionView->getText() : "";
}

ListItem::~ListItem()
{
    delete this->labelView;

    if (this->valueView)
        delete this->valueView;

    if (this->oldValueView)
        delete this->oldValueView;

    if (this->descriptionView)
        delete this->descriptionView;

    if (this->subLabelView)
        delete this->subLabelView;

    if (this->thumbnailView)
        delete this->thumbnailView;
}

ToggleListItem::ToggleListItem(std::string label, bool initialValue, std::string description, std::string onValue, std::string offValue)
    : ListItem(label, description)
    , toggleState(initialValue)
    , onValue(onValue)
    , offValue(offValue)
{
    this->updateValue();
}

void ToggleListItem::updateValue()
{
    if (this->toggleState)
        this->setValue(this->onValue, false);
    else
        this->setValue(this->offValue, true);
}

bool ToggleListItem::onClick()
{
    this->toggleState = !this->toggleState;
    this->updateValue();

    ListItem::onClick();
    return true;
}

bool ToggleListItem::getToggleState()
{
    return this->toggleState;
}

InputListItem::InputListItem(std::string label, std::string initialValue, std::string helpText, std::string description, int maxInputLength, int kbdDisableBitmask)
    : ListItem(label, description)
    , helpText(helpText)
    , maxInputLength(maxInputLength)
    , kbdDisableBitmask(kbdDisableBitmask)
{
    this->setValue(initialValue, false);
}

bool InputListItem::onClick()
{
    Swkbd::openForText([&](std::string text) {
        this->setValue(text, false);
    },
        this->helpText, "", this->maxInputLength, this->getValue(), this->kbdDisableBitmask);

    ListItem::onClick();
    return true;
}

IntegerInputListItem::IntegerInputListItem(std::string label, int initialValue, std::string helpText, std::string description, int maxInputLength, int kbdDisableBitmask)
    : InputListItem(label, std::to_string(initialValue), helpText, description, maxInputLength, kbdDisableBitmask)
{
}

bool IntegerInputListItem::onClick()
{
    Swkbd::openForNumber([&](int number) {
        this->setValue(std::to_string(number), false);
    },
        this->helpText, "", this->maxInputLength, this->getValue(), "", "", this->kbdDisableBitmask);

    ListItem::onClick();
    return true;
}

ListItemGroupSpacing::ListItemGroupSpacing(bool separator)
    : Rectangle(nvgRGBA(0, 0, 0, 0))
{
    Theme* theme = Application::getTheme();

    if (separator)
        this->setColor(theme->listItemSeparatorColor);
}

SelectListItem::SelectListItem(std::string label, std::vector<std::string> values, unsigned selectedValue, std::string description)
    : ListItem(label, description)
    , values(values)
    , selectedValue(selectedValue)
{
    this->setValue(values[selectedValue], false, false);

    this->getClickEvent()->subscribe([this](View* view) {
        ValueSelectedEvent::Callback valueCallback = [this](int result) {
            if (result == -1)
                return;

            this->setValue(this->values[result], false, false);
            this->selectedValue = result;

            this->valueEvent.fire(result);
        };
        Dropdown::open(this->getLabel(), this->values, valueCallback, this->selectedValue);
    });
}

void SelectListItem::setSelectedValue(unsigned value)
{
    if (value >= 0 && value < this->values.size())
    {
        this->selectedValue = value;
        this->setValue(this->values[value], false, false);
    }
}

unsigned SelectListItem::getSelectedValue()
{
    return this->selectedValue;
}

ValueSelectedEvent* SelectListItem::getValueSelectedEvent()
{
    return &this->valueEvent;
}

List::List(size_t defaultFocus)
{
    this->layout = new ListContentView(this, defaultFocus);

    this->layout->setResize(true);
    this->layout->setParent(this);

    this->setContentView(this->layout);
}

// Wrapped BoxLayout methods

void List::addView(View* view, bool fill)
{
    this->layout->addView(view, fill);
}

void List::removeView(int index, bool free)
{
    this->layout->removeView(index, free);
}

void List::clear(bool free)
{
    this->layout->clear(free);
}

size_t List::getViewsCount()
{
    return this->layout->getViewsCount();
}

View* List::getChild(size_t i)
{
    return this->layout->getChild(i);
}

void List::setMargins(unsigned top, unsigned right, unsigned bottom, unsigned left)
{
    this->layout->setMargins(
        top,
        right,
        bottom,
        left);
}

void List::setSpacing(unsigned spacing)
{
    this->layout->setSpacing(spacing);
}

unsigned List::getSpacing()
{
    return this->layout->getSpacing();
}

void List::setMarginBottom(unsigned bottom)
{
    this->layout->setMarginBottom(bottom);
}

void List::customSpacing(View* current, View* next, int* spacing)
{
    // Nothing to do by default
}

List::~List()
{
    // ScrollView already deletes the content view
}

} // namespace brls
