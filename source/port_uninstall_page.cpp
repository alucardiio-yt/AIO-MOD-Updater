#include "port_uninstall_page.hpp"

#include <borealis.hpp>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "confirm_page.hpp"
#include "file_explorer_ops.hpp"
#include "progress_event.hpp"
#include "worker_page.hpp"

using namespace brls::i18n::literals;
namespace fs = std::filesystem;

namespace {
const fs::path SwitchFolder("/switch");
const fs::path AppFolder("/switch/aio-switch-updater-mod");

struct InstalledPort {
    std::string label;
    fs::path folder;
    fs::path nro;
    bool wholeFolder = false;
    std::vector<unsigned char> icon;
};

constexpr std::uint64_t MaxNroIconSize = 1024 * 1024;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool safeComponent(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." && name.front() != '.' &&
        name.find_first_of("/\\:") == std::string::npos;
}

std::string nroTitle(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto length = file.tellg();
    if (length < static_cast<std::streamoff>(sizeof(NroStart) + sizeof(NroHeader)))
        return {};
    auto readAt = [&file, length](std::uint64_t offset, void* data, size_t size) {
        if (offset > static_cast<std::uint64_t>(length) ||
            size > static_cast<std::uint64_t>(length) - offset) return false;
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        return file.read(static_cast<char*>(data), size).good();
    };
    struct { NroStart start; NroHeader header; } nro{};
    if (!readAt(0, &nro, sizeof(nro)) || nro.header.magic != NROHEADER_MAGIC)
        return {};
    NroAssetHeader asset{};
    if (!readAt(nro.header.size, &asset, sizeof(asset)) ||
        asset.magic != NROASSETHEADER_MAGIC ||
        asset.nacp.offset == 0 ||
        asset.nacp.size != sizeof(NacpStruct)) return {};
    auto nacp = std::make_unique<NacpStruct>();
    const std::uint64_t offset = static_cast<std::uint64_t>(nro.header.size) + asset.nacp.offset;
    if (offset < nro.header.size || !readAt(offset, nacp.get(), sizeof(*nacp)))
        return {};
    for (const auto& language : nacp->lang) {
        const size_t length = strnlen(language.name, sizeof(language.name));
        if (length) return std::string(language.name, length);
    }
    return {};
}

std::vector<unsigned char> nroIcon(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto length = file.tellg();
    if (length < static_cast<std::streamoff>(sizeof(NroStart) + sizeof(NroHeader)))
        return {};
    auto readAt = [&file, length](std::uint64_t offset, void* data, size_t size) {
        if (offset > static_cast<std::uint64_t>(length) ||
            size > static_cast<std::uint64_t>(length) - offset) return false;
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        return file.read(static_cast<char*>(data), size).good();
    };
    struct { NroStart start; NroHeader header; } nro{};
    if (!readAt(0, &nro, sizeof(nro)) || nro.header.magic != NROHEADER_MAGIC)
        return {};
    NroAssetHeader asset{};
    if (!readAt(nro.header.size, &asset, sizeof(asset)) ||
        asset.magic != NROASSETHEADER_MAGIC || asset.icon.offset == 0 ||
        asset.icon.size == 0 || asset.icon.size > MaxNroIconSize)
        return {};
    const std::uint64_t offset = static_cast<std::uint64_t>(nro.header.size) + asset.icon.offset;
    if (offset < nro.header.size) return {};
    std::vector<unsigned char> icon(static_cast<size_t>(asset.icon.size));
    if (!readAt(offset, icon.data(), icon.size())) return {};
    return icon;
}

bool withinSwitch(const fs::path& folder)
{
    return folder.parent_path() == SwitchFolder && folder != AppFolder &&
        safeComponent(folder.filename().string());
}

bool verifiedPortFolder(const InstalledPort& port)
{
    if (!withinSwitch(port.folder) || port.nro.parent_path() != port.folder ||
        lower(port.nro.extension().string()) != ".nro") return false;
    std::error_code ec;
    if (!fs::is_directory(port.folder, ec) || ec || fs::is_symlink(port.folder, ec) || ec)
        return false;
    if (!fs::is_regular_file(port.nro, ec) || ec || fs::is_symlink(port.nro, ec) || ec)
        return false;
    std::ifstream nroFile(port.nro, std::ios::binary);
    struct { NroStart start; NroHeader header; } nroHeader{};
    if (!nroFile.read(reinterpret_cast<char*>(&nroHeader), sizeof(nroHeader)) ||
        nroHeader.header.magic != NROHEADER_MAGIC) return false;
    nroFile.close();
    unsigned nroCount = 0;
    for (fs::recursive_directory_iterator it(port.folder, ec), end;
         it != end; it.increment(ec)) {
        if (ec) return false;
        if (it->is_symlink(ec) || ec) return false;
        if (it->is_regular_file(ec) && !ec &&
            lower(it->path().extension().string()) == ".nro") {
            if (it->path() != port.nro) return false;
            ++nroCount;
        }
        if (ec || nroCount > 1) return false;
    }
    return !ec && nroCount == 1;
}

bool validNroFile(const fs::path& path)
{
    if (lower(path.extension().string()) != ".nro" ||
        path == AppFolder / "aio-switch-updater-mod.nro") return false;
    std::error_code ec;
    for (auto parent = path.parent_path(); parent != SwitchFolder; parent = parent.parent_path()) {
        if (parent.empty() || parent == parent.parent_path() ||
            fs::is_symlink(parent, ec) || ec) return false;
    }
    if (!fs::is_regular_file(path, ec) || ec || fs::is_symlink(path, ec) || ec)
        return false;
    std::ifstream file(path, std::ios::binary);
    struct { NroStart start; NroHeader header; } header{};
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&header), sizeof(header))) &&
        header.header.magic == NROHEADER_MAGIC;
}

std::vector<InstalledPort> findInstalledPorts()
{
    std::vector<InstalledPort> ports;
    std::error_code ec;
    fs::recursive_directory_iterator it(SwitchFolder,
        fs::directory_options::skip_permission_denied, ec), end;
    if (ec) throw std::runtime_error(ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) throw std::runtime_error(ec.message());
        const auto path = it->path();
        if (it->is_symlink(ec)) {
            if (ec) throw std::runtime_error(ec.message());
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            if (ec) throw std::runtime_error(ec.message());
            if (path == AppFolder) it.disable_recursion_pending();
            continue;
        }
        if (ec) throw std::runtime_error(ec.message());
        if (!validNroFile(path)) continue;
        InstalledPort port{nroTitle(path), path.parent_path(), path, false, nroIcon(path)};
        if (port.label.empty()) port.label = path.stem().string();
        port.wholeFolder = verifiedPortFolder(port);
        ports.push_back(std::move(port));
    }
    if (ec) throw std::runtime_error(ec.message());
    std::sort(ports.begin(), ports.end(), [](const auto& a, const auto& b) {
        return lower(a.label) == lower(b.label)
            ? a.nro.string() < b.nro.string() : lower(a.label) < lower(b.label);
    });
    return ports;
}

void removePort(const InstalledPort& port)
{
    if (!validNroFile(port.nro) || port.nro.parent_path() != port.folder ||
        (port.wholeFolder && !verifiedPortFolder(port)))
        throw std::runtime_error("menus/tools/remove_ports_changed"_i18n);

    auto& progress = ProgressEvent::instance();
    std::atomic_bool cancel{false};
    std::atomic<std::uintmax_t> completed{0}, total{0};
    std::vector<fs::path> contents;
    std::error_code ec;
    if (port.wholeFolder) {
        for (fs::directory_iterator it(port.folder, ec), end; it != end; it.increment(ec)) {
            if (ec) throw std::runtime_error(ec.message());
            if (it->path() != port.nro) contents.push_back(it->path());
        }
        if (ec) throw std::runtime_error(ec.message());
    }
    contents.push_back(port.nro);
    const explorer::Job job{explorer::Operation::Delete, std::move(contents), {}};
    progress.setTotalSteps(1000);
    const auto result = explorer::run(job, cancel, completed, total, [&](const fs::path& current) {
        cancel.store(progress.getInterupt());
        progress.setCurrentFile(current.string());
        const auto count = total.load();
        if (count) progress.setStep(static_cast<int>(std::min<std::uintmax_t>(
            999, completed.load() * 1000 / count)));
    });
    if (result.rfind("ERROR:", 0) == 0 && !progress.getInterupt())
        throw std::runtime_error(result);
    if (result.rfind("ERROR:", 0) == 0) return;
    if (port.wholeFolder) {
        const bool removed = fs::remove(port.folder, ec);
        if (ec || !removed)
            throw std::runtime_error(port.folder.string() + ": " +
                (ec ? ec.message() : "menus/tools/remove_ports_changed"_i18n));
    }
}

class PortDeleteDonePage final : public ConfirmPage_Done
{
public:
    PortDeleteDonePage(brls::StagedAppletFrame* frame, const std::string& text,
                       std::function<void()> onReturn)
        : ConfirmPage_Done(frame, text, false)
    {
        button->getClickEvent()->subscribe([this](brls::View*) {
            finish();
        });
        this->onReturn = std::move(onReturn);
        registerAction("menus/common/back"_i18n, brls::Key::B, [this] {
            finish();
            return true;
        });
    }
private:
    void finish() {
        brls::Application::popView(brls::ViewAnimation::FADE, onReturn);
    }
    std::function<void()> onReturn;
};

void showPortList(std::vector<InstalledPort> ports)
{
    auto* frame = new brls::AppletFrame(true, true);
    frame->setTitle("menus/tools/remove_ports"_i18n);
    auto* list = new brls::List();
    if (ports.empty()) {
        auto* empty = new brls::ListItem("menus/tools/remove_ports_none"_i18n);
        empty->setHeight(70);
        list->addView(empty);
    }
    for (auto& port : ports) {
        const auto target = port.wholeFolder ? port.folder : port.nro;
        auto* item = new brls::ListItem(port.label + "  |  " + target.string());
        item->setHeight(70);
        if (!port.icon.empty())
            item->setThumbnail(port.icon.data(), port.icon.size());
        else
            item->setThumbnail("romfs:/forwarder/default_icon.jpg");
        InstalledPort actionPort{port.label, port.folder, port.nro, port.wholeFolder, {}};
        item->getClickEvent()->subscribe([actionPort, item, list](brls::View*) {
            const auto& port = actionPort;
            if (!validNroFile(port.nro) ||
                (port.wholeFolder && !verifiedPortFolder(port))) {
                auto* changed = new brls::Dialog("menus/tools/remove_ports_changed"_i18n);
                changed->addButton("menus/common/ok"_i18n,
                    [changed](brls::View*) { changed->close(); });
                changed->open();
                return;
            }
            auto* dialog = new brls::Dialog(fmt::format(
                port.wholeFolder ? "menus/tools/remove_ports_confirm"_i18n
                                 : "menus/tools/remove_nro_confirm"_i18n,
                port.label, (port.wholeFolder ? port.folder : port.nro).string()));
            dialog->addButton("menus/common/no"_i18n,
                [dialog](brls::View*) { dialog->close(); });
            dialog->addButton("menus/common/yes"_i18n,
                [dialog, port, item, list](brls::View*) {
                    dialog->close([port, item, list]() {
                        auto* stages = new brls::StagedAppletFrame();
                        stages->setTitle("menus/tools/remove_ports"_i18n);
                        stages->addStage(new WorkerPage(stages,
                            port.wholeFolder ? "menus/tools/remove_ports_running"_i18n
                                             : "menus/tools/remove_nro_running"_i18n,
                            [port]() { removePort(port); }));
                        stages->addStage(new PortDeleteDonePage(stages,
                            port.wholeFolder ? "menus/tools/remove_ports_done"_i18n
                                             : "menus/tools/remove_nro_done"_i18n,
                            [list, item]() {
                                size_t removedIndex = list->getViewsCount();
                                for (size_t i = 0; i < list->getViewsCount(); ++i) {
                                    if (list->getChild(i) == item) {
                                        removedIndex = i;
                                        break;
                                    }
                                }
                                if (removedIndex == list->getViewsCount()) return;

                                brls::View* nextFocus = nullptr;
                                for (size_t i = removedIndex + 1;
                                     i < list->getViewsCount() && !nextFocus; ++i)
                                    nextFocus = list->getChild(i)->getDefaultFocus();
                                for (size_t i = removedIndex; i > 0 && !nextFocus; --i)
                                    nextFocus = list->getChild(i - 1)->getDefaultFocus();
                                if (!nextFocus) {
                                    auto* empty = new brls::ListItem("menus/tools/remove_ports_none"_i18n);
                                    empty->setHeight(70);
                                    list->addView(empty);
                                    nextFocus = empty;
                                }
                                brls::Application::giveFocus(nextFocus);
                                // Borealis setThumbnail(nullptr) leaves a dangling pointer.
                                item->collapse(false);
                                list->invalidate(true);
                            }));
                        brls::Application::pushView(stages);
                    });
                });
            dialog->setCancelable(true);
            dialog->open();
        });
        list->addView(item);
    }
    frame->setContentView(list);
    brls::Application::pushView(frame);
}

class PortScanDialog final : public brls::Dialog
{
public:
    PortScanDialog() : brls::Dialog("menus/tools/remove_ports_scanning"_i18n) {
        setCancelable(false);
    }
    ~PortScanDialog() override { if (worker.joinable()) worker.join(); }

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
              brls::Style* style, brls::FrameContext* ctx) override
    {
        brls::Dialog::draw(vg, x, y, width, height, style, ctx);
        if (!started) {
            started = true;
            try {
                worker = std::thread([this]() {
                    try {
                        ports = findInstalledPorts();
                    } catch (const std::exception& e) { error = e.what(); }
                    finished.store(true, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                error = e.what();
                finished.store(true, std::memory_order_release);
            }
        } else if (finished.load(std::memory_order_acquire) && !closing) {
            closing = true;
            if (worker.joinable()) worker.join();
            auto result = std::move(ports);
            auto message = std::move(error);
            close([result = std::move(result), message = std::move(message)]() mutable {
                if (!message.empty()) {
                    auto* failed = new brls::Dialog(message);
                    failed->addButton("menus/common/ok"_i18n,
                        [failed](brls::View*) { failed->close(); });
                    failed->open();
                } else showPortList(std::move(result));
            });
        }
    }
private:
    std::thread worker;
    std::atomic_bool finished{false};
    bool started = false;
    bool closing = false;
    std::vector<InstalledPort> ports;
    std::string error;
};
} // namespace

void openPortUninstaller()
{
    (new PortScanDialog())->open();
}
