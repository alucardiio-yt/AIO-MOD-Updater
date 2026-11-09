#include "protection_page.hpp"

#include <borealis.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "constants.hpp"

using namespace brls::i18n::literals;
namespace path = std::filesystem;

namespace {

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// Only read the effective key in a named INI section. A missing key retains
// Atmosphere's default; template and commented-out values are ignored.
std::optional<std::string> iniValue(const path::path& filename,
                                    const std::string& section, const std::string& key,
                                    bool* readable = nullptr)
{
    if (readable) *readable = false;
    std::error_code error;
    const auto size = path::file_size(filename, error);
    if (error || size > 128 * 1024) return std::nullopt;
    std::ifstream file(filename);
    if (!file) return std::nullopt;
    std::string line;
    std::string currentSection;
    std::optional<std::string> result;
    std::size_t bytes = 0;
    while (std::getline(file, line)) {
        bytes += line.size() + 1;
        if (bytes > 128 * 1024) return std::nullopt;
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        line = trim(line);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            currentSection = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos || currentSection != section) continue;
        if (lower(trim(line.substr(0, equal))) == key)
            result = lower(trim(line.substr(equal + 1)));
    }
    if (file.bad()) return std::nullopt;
    if (readable) *readable = true;
    return result;
}

std::optional<unsigned long> iniNumber(const path::path& filename,
                                        const std::string& section, const std::string& key)
{
    auto value = iniValue(filename, section, key);
    if (!value) return std::nullopt;
    const auto bang = value->find('!');
    if (bang != std::string::npos) *value = trim(value->substr(bang + 1));
    try {
        std::size_t used = 0;
        const auto number = std::stoul(*value, &used, 0);
        if (used == value->size()) return number;
    }
    catch (...) {}
    return std::nullopt;
}

enum class SerialStatus { Zeroed, Visible, Unknown };

SerialStatus shownSerialStatus()
{
    SetSysSerialNumber serial{};
    if (R_FAILED(setsysGetSerialNumber(&serial)))
        return SerialStatus::Unknown;
    const std::string value(serial.number,
        strnlen(serial.number, sizeof(serial.number)));
    if (value.empty()) return SerialStatus::Unknown;
    // The serial is never displayed or written to disk.
    const bool prefixed = value.size() >= 9 &&
        std::all_of(value.begin(), value.begin() + 3, [](unsigned char c) {
            return std::isalpha(c) != 0;
        });
    const auto firstDigit = prefixed ? value.begin() + 3 : value.begin();
    const bool zeros = value.end() - firstDigit >= 6 &&
        std::all_of(firstDigit, value.end(), [](char c) { return c == '0'; });
    return zeros ? SerialStatus::Zeroed : SerialStatus::Visible;
}

enum class Storage { Sys, Emu, Unknown };

Storage currentStorage()
{
    u64 emu = 0;
    if (R_FAILED(splGetConfig(static_cast<SplConfigItem>(65007), &emu)))
        return Storage::Unknown;
    return emu ? Storage::Emu : Storage::Sys;
}

std::string selectedHosts(Storage storage)
{
    const path::path base = "/atmosphere/hosts";
    auto exists = [](const path::path& candidate) {
        std::error_code error;
        return path::is_regular_file(candidate, error) && !error;
    };
    if (storage == Storage::Emu) {
        auto id = iniNumber("/emuMMC/emummc.ini", "emummc", "id");
        if (!id) id = iniNumber("/emuMMC/emummc.ini", "emummc", "emummc_id");
        if (id) {
            char name[32];
            std::snprintf(name, sizeof(name), "emummc_%04lx.txt", *id);
            if (exists(base / name)) return (base / name).string();
        }
        if (exists(base / "emummc.txt"))
            return (base / "emummc.txt").string();
    }
    if (storage == Storage::Sys && exists(base / "sysmmc.txt"))
        return (base / "sysmmc.txt").string();
    if (storage != Storage::Unknown && exists(base / "default.txt"))
        return (base / "default.txt").string();
    return {};
}

bool singularHostsFolderHasFile()
{
    std::error_code error;
    const path::path folder = "/atmosphere/host";
    if (!path::is_directory(folder, error) || error) return false;
    unsigned checked = 0;
    for (path::directory_iterator it(folder, error), end;
         !error && it != end && checked++ < 64; it.increment(error)) {
        if (it->is_regular_file(error) && !error && it->path().extension() == ".txt")
            return true;
    }
    return false;
}

struct HostRules {
    std::vector<std::string> domains;
    bool blocksNintendo = false;
};

std::optional<HostRules> blockedHostRules(const std::string& filename)
{
    std::error_code error;
    const auto size = path::file_size(filename, error);
    if (error || size > 256 * 1024) return std::nullopt;
    std::ifstream file(filename);
    if (!file) return std::nullopt;
    std::string line;
    std::size_t bytes = 0;
    std::map<std::string, std::string> mappings;
    while (std::getline(file, line)) {
        bytes += line.size() + 1;
        if (bytes > 256 * 1024) return std::nullopt;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream parts(lower(line));
        std::string address;
        parts >> address;
        if (address.empty()) continue;
        std::string host;
        while (parts >> host) mappings[host] = address;
    }
    if (file.bad()) return std::nullopt;
    HostRules rules;
    for (const auto& [host, address] : mappings) {
        if (address != "127.0.0.1" && address != "0.0.0.0" && address != "::1")
            continue;
        if (rules.domains.size() >= 512) return std::nullopt;
        rules.domains.push_back(host);
        if (host.find("nintendo") != std::string::npos || host == "*")
            rules.blocksNintendo = true;
    }
    return rules;
}

brls::ListItem* row(brls::List* list, const std::string& name, const std::string& value)
{
    auto* item = new brls::ListItem(name);
    item->setHeight(LISTITEM_HEIGHT);
    item->setValue(value, false, false);
    list->addView(item);
    return item;
}

class DnsRulesPage final : public brls::AppletFrame {
public:
    DnsRulesPage(const std::optional<HostRules>& rules,
                 bool enabled, bool valid)
        : brls::AppletFrame(true, true)
    {
        setTitle("menus/protection/rules"_i18n);
        auto* list = new brls::List();
        if (!valid) {
            row(list, "menus/protection/unknown"_i18n, "");
        }
        else if (!enabled) {
            row(list, "menus/protection/disabled_in_file"_i18n, "");
        }
        else if (!rules) {
            row(list, "menus/protection/unknown"_i18n, "");
        }
        else if (rules->domains.empty()) {
            row(list, "menus/protection/dns_none"_i18n, "");
        }
        else {
            for (const auto& domain : rules->domains)
                row(list, domain, "");
        }
        setContentView(list);
    }
};

class ProtectionPage final : public brls::AppletFrame {
public:
    explicit ProtectionPage(bool details = false) : brls::AppletFrame(true, true)
    {
        setTitle(details ? "menus/protection/details_title"_i18n
                         : "menus/protection/title"_i18n);
        auto* list = new brls::List();
        const Storage storage = currentStorage();
        const SerialStatus serial = shownSerialStatus();
        u64 blank = 0;
        const bool gotBlank = R_SUCCEEDED(splGetConfig(
            static_cast<SplConfigItem>(65005), &blank));
        const auto exoKey = storage == Storage::Emu ? "blank_prodinfo_emummc"
                          : "blank_prodinfo_sysmmc";
        const auto exo = storage == Storage::Unknown ? std::optional<unsigned long>{}
            : iniNumber("/exosphere.ini", "exosphere", exoKey);
        const auto dns = iniNumber("/atmosphere/config/system_settings.ini",
            "atmosphere", "enable_dns_mitm");
        const path::path dnsFile = "/atmosphere/config/system_settings.ini";
        std::error_code dnsError;
        const bool dnsFileExists = path::exists(dnsFile, dnsError);
        bool dnsFileReadable = false;
        const auto dnsRaw = dnsFileExists && !dnsError
            ? iniValue(dnsFile, "atmosphere", "enable_dns_mitm", &dnsFileReadable)
            : std::optional<std::string>{};
        const bool dnsValid = !dnsError &&
            (!dnsFileExists || dnsFileReadable) && (!dnsRaw || dns == 0 || dns == 1);
        const bool dnsEnabled = dnsValid && dns != 0;
        const std::string hosts = selectedHosts(storage);
        const auto rules = hosts.empty() ? std::optional<HostRules>{}
                                         : blockedHostRules(hosts);
        const bool wrongFolder = singularHostsFolderHasFile();
        const bool identityHidden = gotBlank && blank != 0 &&
            serial == SerialStatus::Zeroed && exo == 1;
        const bool networkConfigured = dnsEnabled && !hosts.empty() &&
            rules && rules->blocksNintendo;
        const bool protectedConfig = storage != Storage::Unknown &&
            identityHidden && networkConfigured;
        const bool networkMissing = (dnsValid && !dnsEnabled) ||
            (storage != Storage::Unknown && hosts.empty()) ||
            (rules && !rules->blocksNintendo);
        const bool confirmedUnprotected = (gotBlank && blank == 0) ||
            serial == SerialStatus::Visible || networkMissing;

        row(list, "menus/protection/result"_i18n,
            protectedConfig ? "menus/protection/protected"_i18n
            : confirmedUnprotected ? "menus/protection/unprotected"_i18n
                                   : "menus/protection/unverified"_i18n);
        if (!details) {
            list->addView(new brls::Header("menus/protection/summary"_i18n));
            row(list, "menus/protection/storage"_i18n,
                storage == Storage::Emu ? "menus/protection/emummc"_i18n
                : storage == Storage::Sys ? "menus/protection/sysmmc"_i18n
                                          : "menus/protection/unknown"_i18n);
            row(list, "menus/protection/identity"_i18n,
                identityHidden ? "menus/protection/hidden"_i18n
                : serial == SerialStatus::Visible ? "menus/protection/visible"_i18n
                                                  : "menus/protection/check"_i18n);
            row(list, "menus/protection/network"_i18n,
                networkConfigured ? "menus/protection/configured"_i18n
                : networkMissing ? "menus/protection/network_missing"_i18n
                                 : "menus/protection/check"_i18n);
            auto* detailItem = new brls::ListItem("menus/protection/show_details"_i18n);
            detailItem->setHeight(LISTITEM_HEIGHT);
            detailItem->getClickEvent()->subscribe([](brls::View*) {
                brls::Application::pushView(new ProtectionPage(true));
            });
            list->addView(detailItem);
            setContentView(list);
            return;
        }

        const std::string mode = storage == Storage::Emu
            ? "menus/protection/emummc"_i18n
            : storage == Storage::Sys ? "menus/protection/sysmmc"_i18n
                                      : "menus/protection/unknown"_i18n;
        list->addView(new brls::Header("menus/protection/session"_i18n));
        row(list, "menus/protection/storage"_i18n, mode);

        row(list, "menus/protection/blank_runtime"_i18n,
            !gotBlank ? "menus/protection/unknown"_i18n
            : blank ? "menus/protection/active"_i18n
                    : "menus/protection/inactive"_i18n);
        row(list, "menus/protection/serial"_i18n,
            serial == SerialStatus::Zeroed ? "menus/protection/serial_zeros"_i18n
            : serial == SerialStatus::Visible ? "menus/protection/serial_not_zeros"_i18n
                                              : "menus/protection/unknown"_i18n);

        list->addView(new brls::Header("menus/protection/files"_i18n));
        row(list, "menus/protection/exosphere"_i18n,
            storage == Storage::Unknown ? "menus/protection/unknown"_i18n
            : exo == 1 ? "menus/protection/enabled_in_file"_i18n
                       : "menus/protection/not_enabled_in_file"_i18n);

        row(list, "menus/protection/dns"_i18n,
            !dnsValid ? "menus/protection/unknown"_i18n
            : !dnsEnabled ? "menus/protection/disabled_in_file"_i18n
            : dnsRaw ? "menus/protection/enabled_in_file"_i18n
                     : "menus/protection/dns_default_enabled"_i18n);
        row(list, "menus/protection/hosts"_i18n,
            hosts.empty() ? (wrongFolder
                ? "menus/protection/hosts_wrong_folder"_i18n
                : "menus/protection/hosts_missing"_i18n) : hosts);
        auto* dnsRules = row(list, "menus/protection/rules"_i18n,
            !dnsValid || !rules ? "menus/protection/unknown"_i18n
            : dnsEnabled ? std::to_string(rules->domains.size()) : "0");
        dnsRules->getClickEvent()->subscribe([rules, dnsEnabled, dnsValid](brls::View*) {
            brls::Application::pushView(new DnsRulesPage(rules, dnsEnabled, dnsValid));
        });

        if (!hosts.empty() && wrongFolder)
            list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
                "menus/protection/hosts_wrong_folder"_i18n, true));
        setContentView(list);
    }
};

} // namespace

void openProtectionPage()
{
    brls::Application::pushView(new ProtectionPage());
}
