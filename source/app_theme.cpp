#include "app_theme.hpp"

#include <borealis.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "constants.hpp"

using namespace brls::i18n::literals;

namespace app_theme {
namespace {

struct RGB { unsigned char r, g, b; };
struct Palette {
    const char* id;
    const char* label;
    RGB background, gradient, sidebar, panel, selection, accent, accentLight, separator;
};

constexpr std::array<Palette, Count> palettes{{
    {"classic", "menus/themes/classic", {4,4,8}, {20,10,35}, {16,10,26}, {22,14,34}, {60,30,90}, {170,70,255}, {230,160,255}, {45,28,65}},
    {"cyan", "menus/themes/cyan", {3,12,19}, {8,35,48}, {6,24,34}, {10,32,44}, {16,62,76}, {39,204,225}, {152,239,246}, {28,71,82}},
    {"dark", "menus/themes/dark", {9,10,14}, {22,24,32}, {15,17,22}, {24,27,35}, {48,52,68}, {156,169,217}, {212,220,246}, {51,56,70}},
    {"oled", "menus/themes/oled", {0,0,0}, {0,0,0}, {0,0,0}, {9,9,12}, {31,32,42}, {101,180,255}, {185,222,255}, {46,46,55}},
    {"white", "menus/themes/white", {245,246,249}, {229,234,240}, {234,237,243}, {255,255,255}, {214,232,247}, {38,104,173}, {25,79,141}, {190,200,214}},
    {"gray", "menus/themes/gray", {24,26,30}, {44,47,53}, {33,36,41}, {42,45,51}, {69,73,82}, {184,192,206}, {230,234,241}, {74,79,89}},
    {"blue", "menus/themes/blue", {5,11,27}, {13,29,58}, {9,21,43}, {14,27,53}, {24,52,93}, {67,144,255}, {161,206,255}, {39,65,100}},
    {"green", "menus/themes/green", {5,18,15}, {12,43,34}, {9,29,24}, {15,37,30}, {25,64,50}, {56,201,137}, {167,240,200}, {39,75,60}},
    {"amber", "menus/themes/amber", {21,14,7}, {48,29,10}, {31,21,10}, {41,28,14}, {70,47,22}, {245,174,69}, {255,222,159}, {82,59,32}},
}};

constexpr const char* ThemeFile = "/config/aio-switch-updater-mod/theme.txt";
constexpr const char* TempFile = "/config/aio-switch-updater-mod/theme.txt.tmp";
constexpr const char* BackupFile = "/config/aio-switch-updater-mod/theme.txt.bak";

std::size_t current = 0;

NVGcolor color(RGB value) { return nvgRGB(value.r, value.g, value.b); }
NVGcolor translucent(RGB value, unsigned char alpha) { return nvgRGBA(value.r, value.g, value.b, alpha); }

std::size_t parseFile(const char* path)
{
    std::ifstream input(path);
    std::string id;
    if (!(input >> id)) return Count;
    for (std::size_t i = 0; i < Count; ++i)
        if (id == palettes[i].id) return i;
    return Count;
}

void applyOne(brls::Theme* theme)
{
    if (current == 0) {
        *theme = brls::HorizonDarkTheme();
        return;
    }

    const auto& p = palettes[current];
    const bool light = current == 4;
    if (light) *theme = brls::HorizonLightTheme();
    else *theme = brls::HorizonDarkTheme();

    theme->backgroundColor[0] = p.background.r / 255.f;
    theme->backgroundColor[1] = p.background.g / 255.f;
    theme->backgroundColor[2] = p.background.b / 255.f;
    theme->backgroundColorRGB = color(p.background);
    theme->backgroundGradientColor = color(p.gradient);
    theme->backgroundGlowColor1 = translucent(p.accent, light ? 12 : 28);
    theme->backgroundGlowColor2 = translucent(p.accent, light ? 7 : 16);
    theme->descriptionColor = light ? nvgRGB(91, 101, 116) : nvgRGB(184, 193, 205);
    theme->sidebarColor = color(p.sidebar);
    theme->sidebarSeparatorColor = color(p.separator);
    theme->separatorColor = color(p.separator);
    theme->activeTabColor = color(p.accent);
    theme->highlightBackgroundColor = color(p.selection);
    theme->highlightColor1 = color(p.accent);
    theme->highlightColor2 = color(p.accentLight);
    theme->listItemSeparatorColor = color(p.separator);
    theme->listItemValueColor = color(p.accent);
    theme->listItemFaintValueColor = light ? nvgRGB(110, 119, 132) : nvgRGB(130, 139, 157);
    theme->tableEvenBackgroundColor = color(p.panel);
    theme->tableBodyTextColor = light ? nvgRGB(73, 83, 99) : nvgRGB(199, 205, 217);
    theme->dropdownBackgroundColor = light ? nvgRGBA(25, 35, 48, 150) : nvgRGBA(0, 0, 0, 205);
    theme->nextStageBulletColor = color(p.accentLight);
    theme->spinnerBarColor = translucent(p.accent, 105);
    theme->scrollBarColor = color(p.accent);
    theme->headerRectangleColor = color(p.accent);
    theme->buttonPrimaryEnabledBackgroundColor = color(p.accent);
    theme->buttonPrimaryEnabledTextColor = light ? nvgRGB(255, 255, 255) : nvgRGB(0, 0, 0);
    theme->buttonPrimaryDisabledBackgroundColor = color(p.separator);
    theme->buttonBorderedBorderColor = color(p.accent);
    theme->buttonBorderedTextColor = theme->textColor;
    theme->buttonRegularBackgroundColor = color(p.panel);
    theme->buttonRegularTextColor = theme->textColor;
    theme->buttonRegularBorderColor = color(p.separator);
    theme->dialogColor = color(p.panel);
    theme->dialogButtonColor = color(p.accent);
    theme->dialogButtonSeparatorColor = color(p.separator);
}

class Picker final : public brls::AppletFrame {
public:
    Picker() : brls::AppletFrame(true, true)
    {
        setTitle("menus/themes/title"_i18n);
        auto* list = new brls::List();
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "menus/themes/help"_i18n, true));
        for (std::size_t i = 0; i < Count; ++i) {
            auto* item = new brls::ListItem(brls::i18n::getStr(labelKey(i)));
            item->setHeight(LISTITEM_HEIGHT);
            if (i == current) item->setValue("menus/themes/selected"_i18n, false, false);
            item->getClickEvent()->subscribe([i](brls::View*) {
                if (select(i)) brls::Application::popView();
                else {
                    auto* dialog = new brls::Dialog("menus/themes/save_error"_i18n);
                    dialog->addButton("menus/common/ok"_i18n, [dialog](brls::View*) { dialog->close(); });
                    dialog->open();
                }
            });
            list->addView(item);
        }
        setContentView(list);
    }
};

class ThemeTab final : public brls::List {
    std::array<brls::ListItem*, Count> items{};

public:
    ThemeTab()
    {
        addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "menus/themes/help"_i18n, true));
        for (std::size_t i = 0; i < Count; ++i) {
            auto* item = new brls::ListItem(brls::i18n::getStr(labelKey(i)));
            item->setHeight(LISTITEM_HEIGHT);
            if (i == selected()) item->setValue("menus/themes/selected"_i18n, false, false);
            item->getClickEvent()->subscribe([this, i](brls::View*) {
                if (!select(i)) {
                    auto* dialog = new brls::Dialog("menus/themes/save_error"_i18n);
                    dialog->addButton("menus/common/ok"_i18n, [dialog](brls::View*) { dialog->close(); });
                    dialog->open();
                    return;
                }
                for (std::size_t j = 0; j < Count; ++j)
                    items[j]->setValue(j == selected() ? "menus/themes/selected"_i18n : "", false, false);
            });
            items[i] = item;
            addView(item);
        }
    }
};

} // namespace

std::size_t selected() { return current; }

const char* labelKey(std::size_t index) { return index < Count ? palettes[index].label : palettes[0].label; }

void load()
{
    const auto primary = parseFile(ThemeFile);
    const auto backup = primary == Count ? parseFile(BackupFile) : Count;
    current = primary != Count ? primary : (backup != Count ? backup : 0);
}

bool select(std::size_t index)
{
    if (index >= Count) return false;
    if (index == current) return true;
    std::error_code ec;
    std::filesystem::create_directories(CONFIG_PATH, ec);
    if (ec) return false;
    {
        std::ofstream output(TempFile, std::ios::trunc);
        if (!output) return false;
        output << palettes[index].id << '\n';
        output.close();
        if (!output) return false;
    }
    const bool hadOriginal = std::filesystem::exists(ThemeFile, ec);
    if (ec) return false;
    if (hadOriginal) {
        std::filesystem::remove(BackupFile, ec);
        if (ec) return false;
        std::filesystem::rename(ThemeFile, BackupFile, ec);
        if (ec) return false;
    }
    std::filesystem::rename(TempFile, ThemeFile, ec);
    if (ec) {
        if (hadOriginal) {
            std::error_code restore;
            std::filesystem::rename(BackupFile, ThemeFile, restore);
        }
        return false;
    }
    if (hadOriginal) std::filesystem::remove(BackupFile, ec);
    current = index;
    apply(brls::Application::getThemeVariantsWrapper());
    return true;
}

void apply(brls::LibraryViewsThemeVariantsWrapper* variants)
{
    if (!variants) return;
    applyOne(variants->getLightTheme());
    applyOne(variants->getDarkTheme());
}

brls::View* createTab() { return new ThemeTab(); }

void openPicker() { brls::Application::pushView(new Picker()); }

} // namespace app_theme
