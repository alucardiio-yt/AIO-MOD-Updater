#include "apps_tab.hpp"

#include <filesystem>

#include "confirm_page.hpp"
#include "constants.hpp"
#include "download.hpp"
#include "extract.hpp"
#include "fs.hpp"
#include "worker_page.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

// ⚠️ Cambia esta URL por la URL raw de tu JSON en GitHub
constexpr const char APPS_JSON_URL[] = "https://raw.githubusercontent.com/TU_USUARIO/TU_REPO/main/apps.json";

// Archivo temporal para descargas ZIP
constexpr const char APPS_TEMP_PATH[] = "/config/aio-switch-updater-mod/apps_temp_download";

AppsTab::AppsTab() : brls::List()
{
    brls::Label* description = new brls::Label(
        brls::LabelStyle::DESCRIPTION,
        "Homebrew Apps — Selecciona una app para descargarla a tu SD",
        true);
    this->addView(description);

    this->createList();
}

void AppsTab::createList()
{
    nlohmann::ordered_json appsJson;
    download::getRequest(APPS_JSON_URL, appsJson);

    std::vector<std::pair<std::string, std::string>> links = download::getLinksFromJson(appsJson);

    if (links.empty()) {
        this->displayNotFound();
        return;
    }

    for (const auto& link : links) {
        const std::string label = link.first;   // Lo que escribiste en el JSON
        const std::string url   = link.second;  // URL oculta al usuario

        brls::ListItem* item = new brls::ListItem(label);
        item->setHeight(LISTITEM_HEIGHT);

        item->getClickEvent()->subscribe([label, url](brls::View* view) {

            // Detectar automáticamente si es .zip o .nro
            bool isZip = url.size() >= 4 &&
                         url.substr(url.size() - 4) == ".zip";

            std::string confirmText = isZip
                ? "¿Descargar e instalar " + label + "?\nSe extraerá en la raíz de la SD."
                : "¿Descargar " + label + " a /switch/?";

            brls::StagedAppletFrame* stagedFrame = new brls::StagedAppletFrame();
            stagedFrame->setTitle("Homebrew Apps");

            // Pantalla de confirmación
            stagedFrame->addStage(new ConfirmPage(stagedFrame, confirmText));

            if (isZip) {
                // Descargar ZIP a archivo temporal
                stagedFrame->addStage(new WorkerPage(stagedFrame, "Descargando...", [url]() {
                    fs::createTree("/config/aio-switch-updater-mod/");
                    download::downloadFile(url, APPS_TEMP_PATH, OFF);
                }));

                // Extraer en la raíz de la SD respetando la estructura del ZIP
                stagedFrame->addStage(new WorkerPage(stagedFrame, "Extrayendo...", []() {
                    extract::extract(APPS_TEMP_PATH, "/", false);
                    if (std::filesystem::exists(APPS_TEMP_PATH)) {
                        std::filesystem::remove(APPS_TEMP_PATH);
                    }
                }));
            }
            else {
                // Es un .nro — descarga directa a /switch/
                std::string filename = url.substr(url.find_last_of('/') + 1);
                std::string destPath = std::string("/switch/") + filename;

                stagedFrame->addStage(new WorkerPage(stagedFrame, "Descargando...", [url, destPath]() {
                    fs::createTree("/switch/");
                    download::downloadFile(url, destPath, OFF);
                }));
            }

            stagedFrame->addStage(new ConfirmPage_Done(stagedFrame, "¡Listo! App instalada correctamente."));
            brls::Application::pushView(stagedFrame);
            return true;
        });

        this->addView(item);
    }
}

void AppsTab::displayNotFound()
{
    brls::Label* notFound = new brls::Label(
        brls::LabelStyle::SMALL,
        "No se pudieron cargar las apps.\nVerifica tu conexión o el JSON en GitHub.",
        true);
    notFound->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->addView(notFound);
}
