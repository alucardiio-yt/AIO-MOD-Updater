#include "cfw_pack_date.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <cstddef>
#include <string>

namespace cfw_pack_date {
namespace {

constexpr const char* ReleaseApi = "https://api.github.com/repos/alucardiio-yt/archives-nx/releases/latest";
constexpr std::size_t MaxResponse = 512 * 1024;

std::size_t receive(char* data, std::size_t size, std::size_t count, void* user)
{
    if (size && count > static_cast<std::size_t>(-1) / size) return 0;
    const std::size_t bytes = size * count;
    auto& body = *static_cast<std::string*>(user);
    if (body.size() > MaxResponse || bytes > MaxResponse - body.size()) return 0;
    try { body.append(data, bytes); }
    catch (...) { return 0; }
    return bytes;
}

std::string dateUtc(const std::string& value)
{
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value.back() != 'Z') return {};
    return value.substr(8, 2) + "/" + value.substr(5, 2) + "/" +
        value.substr(0, 4) + " UTC";
}

} // namespace

std::string latestZipDateUtc()
{
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, ReleaseApi);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AIO-MOD");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status != 200) return {};

    try {
        const auto release = nlohmann::ordered_json::parse(body);
        if (!release.is_object() || !release.contains("assets") ||
            !release["assets"].is_array()) return {};
        for (const auto& asset : release["assets"]) {
            if (!asset.is_object() || !asset.contains("name") ||
                !asset["name"].is_string() || asset["name"] != "NX.latest.zip" ||
                !asset.contains("updated_at") || !asset["updated_at"].is_string())
                continue;
            return dateUtc(asset["updated_at"].get<std::string>());
        }
    }
    catch (...) {
    }
    return {};
}

} // namespace cfw_pack_date
