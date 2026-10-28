#pragma once

constexpr int ON = 1;
constexpr int OFF = 0;

#include <json.hpp>
#include <cstdint>

namespace download {

    // Returns false when the server does not publish a usable size.
    bool getRemoteSize(const std::string& url, std::uint64_t& bytes);
    std::string spaceSummaryFromSize(std::uint64_t bytes, bool sizeKnown, bool archive);
    std::string spaceSummary(const std::string& url, bool archive);

    long downloadFile(const std::string& url, std::vector<std::uint8_t>& res, const std::string& output = "", int api = OFF);
    long downloadFile(const std::string& url, const std::string& output = "", int api = OFF);
    std::vector<std::pair<std::string, std::string>> getLinks(const std::string& url);
    std::vector<std::pair<std::string, std::string>> getLinksFromJson(const nlohmann::ordered_json& json_object);
    std::string fetchTitle(const std::string& url);
    long downloadPage(const std::string& url, std::string& res, const std::vector<std::string>& headers = {}, const std::string& body = "");
    long getRequest(const std::string& url, nlohmann::ordered_json& res, const std::vector<std::string>& headers = {}, const std::string& body = "");

}  // namespace download
