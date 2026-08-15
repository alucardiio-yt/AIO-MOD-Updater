#include "language_tab.hpp"

#include <json.hpp>

#include "constants.hpp"
#include "fs.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;
using json = nlohmann::ordered_json;

static void setLanguage(const std::string& code)
{
    fs::createTree(CONFIG_PATH);

    json data = json::object();
    data["language"] = code;
    fs::writeJsonToFile(data, LANGUAGE_JSON);

    util::showDialogBoxInfo(
        "Idioma guardado. Reinicia la aplicación para aplicar.\n"
        "Language saved. Restart the app to apply.\n"
        "Langue enregistrée. Redémarrez l'application pour appliquer.");
}

LanguageTab::LanguageTab() : brls::List()
{
    brls::Label* desc = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        "Selecciona el idioma de la aplicación.\n"
        "Select the application language.\n"
        "Sélectionnez la langue de l'application.",
        true);
    this->addView(desc);

    brls::ListItem* spanish = new brls::ListItem("Español");
    spanish->getClickEvent()->subscribe([](brls::View* view) {
        setLanguage("es");
    });
    this->addView(spanish);

    brls::ListItem* english = new brls::ListItem("English");
    english->getClickEvent()->subscribe([](brls::View* view) {
        setLanguage("en-US");
    });
    this->addView(english);

    brls::ListItem* french = new brls::ListItem("Français");
    french->getClickEvent()->subscribe([](brls::View* view) {
        setLanguage("fr");
    });
    this->addView(french);
}
