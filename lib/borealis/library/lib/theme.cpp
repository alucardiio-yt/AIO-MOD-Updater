/*
    Borealis, a Nintendo Switch UI Library
    Copyright (C) 2019  natinusala
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

#include <borealis/theme.hpp>

namespace brls
{

HorizonLightTheme::HorizonLightTheme()
{
    this->backgroundColor[0] = 0.922f;
    this->backgroundColor[1] = 0.922f;
    this->backgroundColor[2] = 0.922f;
    this->backgroundColorRGB = nvgRGB(235, 235, 235);

    this->textColor        = nvgRGB(51, 51, 51);
    this->descriptionColor = nvgRGB(140, 140, 140);

    this->notificationTextColor = nvgRGB(255, 255, 255);
    this->backdropColor         = nvgRGBA(0, 0, 0, 178);

    this->separatorColor = nvgRGB(45, 45, 45);

    this->sidebarColor          = nvgRGB(240, 240, 240);
    this->activeTabColor        = nvgRGB(49, 79, 235);
    this->sidebarSeparatorColor = nvgRGB(208, 208, 208);

    this->highlightBackgroundColor = nvgRGB(252, 255, 248);
    this->highlightColor1          = nvgRGB(13, 182, 213);
    this->highlightColor2          = nvgRGB(80, 239, 217);

    this->listItemSeparatorColor  = nvgRGB(207, 207, 207);
    this->listItemValueColor      = nvgRGB(43, 81, 226);
    this->listItemFaintValueColor = nvgRGB(181, 184, 191);

    this->tableEvenBackgroundColor = nvgRGB(240, 240, 240);
    this->tableBodyTextColor       = nvgRGB(131, 131, 131);

    this->dropdownBackgroundColor = nvgRGBA(0, 0, 0, 178);

    this->nextStageBulletColor = nvgRGB(165, 165, 165);

    this->spinnerBarColor = nvgRGBA(131, 131, 131, 102);

    this->scrollBarColor = nvgRGB(138, 138, 138);
    this->scrollBarAlphaNormal = 0.2f;
    this->scrollBarAlphaFull = 0.5f;

    this->clickAnimationAlpha = 0.3f;

    this->headerRectangleColor = nvgRGB(127, 127, 127);

    this->buttonPrimaryEnabledBackgroundColor  = nvgRGB(50, 79, 241);
    this->buttonPrimaryDisabledBackgroundColor = nvgRGB(201, 201, 209);
    this->buttonPrimaryEnabledTextColor        = nvgRGB(255, 255, 255);
    this->buttonPrimaryDisabledTextColor       = nvgRGB(220, 220, 228);
    this->buttonBorderedBorderColor            = nvgRGB(45, 45, 45);
    this->buttonBorderedTextColor              = nvgRGB(45, 45, 45);
    this->buttonRegularBackgroundColor         = nvgRGB(255, 255, 255);
    this->buttonRegularTextColor               = nvgRGB(46, 46, 46);
    this->buttonRegularBorderColor             = nvgRGB(223, 223, 223);

    this->dialogColor                = nvgRGB(240, 240, 240);
    this->dialogBackdrop             = nvgRGBA(0, 0, 0, 100);
    this->dialogButtonColor          = nvgRGB(46, 78, 255);
    this->dialogButtonSeparatorColor = nvgRGB(210, 210, 210);
}

HorizonDarkTheme::HorizonDarkTheme()
{
    this->backgroundColor[0] = 0.015f;
    this->backgroundColor[1] = 0.015f;
    this->backgroundColor[2] = 0.025f;
    this->backgroundColorRGB = nvgRGB(4, 4, 8);

    this->textColor        = nvgRGB(255, 255, 255);
    this->descriptionColor = nvgRGB(190, 180, 210);

    this->notificationTextColor = nvgRGB(255, 255, 255);
    this->backdropColor         = nvgRGBA(0, 0, 0, 178);

    this->separatorColor = nvgRGB(110, 70, 180);

    this->sidebarColor          = nvgRGB(16, 10, 26);
    this->activeTabColor        = nvgRGB(170, 90, 255);
    this->sidebarSeparatorColor = nvgRGB(40, 24, 60);

    this->highlightBackgroundColor = nvgRGB(60, 30, 90);
    this->highlightColor1          = nvgRGB(170, 70, 255);
    this->highlightColor2          = nvgRGB(230, 160, 255);

    this->listItemSeparatorColor  = nvgRGB(45, 28, 65);
    this->listItemValueColor      = nvgRGB(195, 140, 255);
    this->listItemFaintValueColor = nvgRGB(110, 95, 130);

    this->tableEvenBackgroundColor = nvgRGB(22, 14, 34);
    this->tableBodyTextColor       = nvgRGB(185, 175, 205);

    this->dropdownBackgroundColor = nvgRGBA(8, 5, 15, 230);

    this->nextStageBulletColor = nvgRGB(180, 150, 220);

    this->spinnerBarColor = nvgRGBA(170, 120, 255, 102);

    this->scrollBarColor = nvgRGB(160, 120, 220);
    this->scrollBarAlphaNormal = 0.2f;
    this->scrollBarAlphaFull = 0.5f;

    this->clickAnimationAlpha = 0.3f;

    this->headerRectangleColor = nvgRGB(180, 140, 230);

    this->buttonPrimaryEnabledBackgroundColor  = nvgRGB(160, 60, 255);
    this->buttonPrimaryDisabledBackgroundColor = nvgRGB(65, 50, 85);
    this->buttonPrimaryEnabledTextColor        = nvgRGB(255, 255, 255);
    this->buttonPrimaryDisabledTextColor       = nvgRGB(150, 140, 165);
    this->buttonBorderedBorderColor            = nvgRGB(190, 150, 255);
    this->buttonBorderedTextColor              = nvgRGB(255, 255, 255);
    this->buttonRegularBackgroundColor         = nvgRGB(28, 18, 42);
    this->buttonRegularTextColor               = nvgRGB(255, 255, 255);
    this->buttonRegularBorderColor             = nvgRGB(55, 35, 75);

    this->dialogColor                = nvgRGB(24, 16, 36);
    this->dialogBackdrop             = nvgRGBA(0, 0, 0, 120);
    this->dialogButtonColor          = nvgRGB(180, 90, 255);
    this->dialogButtonSeparatorColor = nvgRGB(70, 50, 95);
}

} // namespace brls