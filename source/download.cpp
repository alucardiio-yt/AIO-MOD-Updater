#include "download.hpp"

#include <curl/curl.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <switch.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <regex>
#include <string>
#include <thread>

#include "fs.hpp"
#include "progress_event.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

constexpr const char API_AGENT[] = "HamletDuFromage";
constexpr int _1MiB = 0x100000;

using json = nlohmann::ordered_json;

namespace download {

    namespace {

        std::chrono::_V2::steady_clock::time_point time_old;
        std::chrono::_V2::steady_clock::time_point progress_time_old;
        double dlold;

        typedef struct
        {
            char* memory;
            size_t size;
        } MemoryStruct_t;

        typedef struct
        {
            u_int8_t* data;
            size_t data_size;
            u_int64_t offset;
            FILE* out;
            Aes128CtrContext* aes;
            std::vector<std::uint8_t>* memory;
            bool cancellable;
            bool write_failed;
        } ntwrk_struct_t;

        static size_t WriteMemoryCallback(void* contents, size_t size, size_t num_files, void* userp)
        {
            auto* chunk = static_cast<ntwrk_struct_t*>(userp);
            if (chunk->cancellable && ProgressEvent::instance().getInterupt())
                return 0;
            if (size && num_files > std::numeric_limits<size_t>::max() / size)
                return 0;
            const size_t count = size * num_files;
            auto* input = static_cast<unsigned char*>(contents);
            if (chunk->memory) {
                try {
                    const size_t oldSize = chunk->memory->size();
                    if (count > chunk->memory->max_size() - oldSize)
                        return 0;
                    chunk->memory->resize(oldSize + count);
                    if (count) {
                        if (chunk->aes)
                            aes128CtrCrypt(chunk->aes, chunk->memory->data() + oldSize, input, count);
                        else
                            memcpy(chunk->memory->data() + oldSize, input, count);
                    }
                }
                catch (...) { return 0; }
                return count;
            }
            size_t remaining = count;
            while (remaining) {
                const size_t amount = std::min(remaining, chunk->data_size - static_cast<size_t>(chunk->offset));
                if (chunk->aes)
                    aes128CtrCrypt(chunk->aes, chunk->data + chunk->offset, input, amount);
                else
                    memcpy(chunk->data + chunk->offset, input, amount);
                chunk->offset += amount;
                input += amount;
                remaining -= amount;
                if (chunk->offset == chunk->data_size) {
                    if (fwrite(chunk->data, 1, chunk->offset, chunk->out) != chunk->offset) {
                        chunk->write_failed = true;
                        return 0;
                    }
                    chunk->offset = 0;
                }
            }
            return count;
        }

        int download_progress(void* p, double dltotal, double dlnow, double ultotal, double ulnow)
        {
            if (p && !*static_cast<const bool*>(p))
                return 0;
            if (ProgressEvent::instance().getInterupt())
                return 1;

            if (dltotal <= 0.0)
                return 0;

            const auto time_now = std::chrono::steady_clock::now();
            const double progress_elapsed = std::chrono::duration<double>(time_now - progress_time_old).count();

            // The curl callback can run many times per second. Updating the shared
            // UI state at ~10 Hz is visually smooth and avoids needless mutex work.
            if (progress_elapsed >= 0.10 || dlnow >= dltotal) {
                ProgressEvent::instance().setDownloadProgress(dlnow, dltotal);
                progress_time_old = time_now;
            }

            const double elapsed_time = std::chrono::duration<double>(time_now - time_old).count();
            if (elapsed_time > 1.2) {
                ProgressEvent::instance().setSpeed((dlnow - dlold) / elapsed_time);
                dlold = dlnow;
                time_old = time_now;
            }

            return 0;
        }

        std::string getDownloadDisplayName(const std::string& url, const std::string& output, bool isMega)
        {
            if (!isMega) {
                std::string cleanUrl = url;
                const size_t queryPos = cleanUrl.find_first_of("?#");
                if (queryPos != std::string::npos)
                    cleanUrl = cleanUrl.substr(0, queryPos);

                const size_t slashPos = cleanUrl.find_last_of('/');
                if (slashPos != std::string::npos && slashPos + 1 < cleanUrl.size()) {
                    const std::string filename = cleanUrl.substr(slashPos + 1);
                    if (!filename.empty())
                        return filename;
                }
            }

            return output.empty() ? url : output;
        }

        struct MemoryStruct
        {
            char* memory;
            size_t size;
        };

        static size_t WriteMemoryCallback2(void* contents, size_t size, size_t nmemb, void* userp)
        {
            if (size && nmemb > std::numeric_limits<size_t>::max() / size)
                return 0;
            size_t realsize = size * nmemb;
            struct MemoryStruct* mem = (struct MemoryStruct*)userp;
            if (realsize > std::numeric_limits<size_t>::max() - mem->size - 1)
                return 0;

            char* ptr = static_cast<char*>(realloc(mem->memory, mem->size + realsize + 1));
            if (ptr == NULL) {
                /* out of memory! */
                return 0;
            }

            mem->memory = ptr;
            memcpy(&(mem->memory[mem->size]), contents, realsize);
            mem->size += realsize;
            mem->memory[mem->size] = 0;

            return realsize;
        }

        void configureTransfer(CURL* curl, bool* reportProgress)
        {
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
            // An inactivity limit, not a total deadline for large ports.
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, download_progress);
            curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, reportProgress);
        }

        bool checkSize(CURL* curl, const std::string& url)
        {
            curl_off_t dl = -1;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_USERAGENT, API_AGENT);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            const auto result = curl_easy_perform(curl);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
            // Some hosts reject HEAD. The GET remains authoritative.
            if (result == CURLE_OK && curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &dl) == CURLE_OK && dl >= 0) {
                s64 freeStorage = 0;
                if (R_SUCCEEDED(fs::getFreeStorageSD(freeStorage)) && dl * 2.5 > freeStorage)
                    return false;
            }
            return true;
        }

        std::string mega_id(std::string url)
        {
            auto len = url.length();

            if (len < 52)
                throw std::invalid_argument("Invalid URL.");

            bool old_link = url.find("#!") < len;

            /* Start from the last '/' and add 2 characters if old link format starting with '#!' */
            auto init_pos = url.find_last_of('/') + (old_link ? 2 : 0) + 1;

            /* End with the last '#' or "!" if its the old link format */
            auto end_pos = url.find_last_of(old_link ? '!' : '#');

            /* Finally crop the url */
            std::string id = url.substr(init_pos, end_pos - init_pos);

            if (id.length() != 8)
                throw std::invalid_argument("Invalid URL ID.");

            return id;
        }

        std::string mega_url(std::string url, bool reportProgress)
        {
            std::string id = mega_id(url);

            json request = json::array({{
                {"a", "g"},
                {"g", 1},
                {"p", id},
            }});

            std::string body = request.dump();
            std::string output;

            auto curl = curl_easy_init();
            if (!curl)
                throw std::runtime_error("menus/errors/mega_connection"_i18n);
            configureTransfer(curl, &reportProgress);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(curl, CURLOPT_URL, "https://g.api.mega.co.nz/cs");
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, API_AGENT);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(
                curl,
                CURLOPT_WRITEFUNCTION,
                +[](void* buffer, size_t size, size_t nmemb, void* userp) -> size_t {
                    std::string* output = reinterpret_cast<std::string*>(userp);
                    size_t actual_size = size * nmemb;
                    try { output->append(reinterpret_cast<char*>(buffer), actual_size); }
                    catch (...) { return 0; }
                    return actual_size;
                });

            const auto result = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_cleanup(curl);
            if (result != CURLE_OK || status < 200 || status >= 300)
                throw std::runtime_error("menus/errors/mega_request"_i18n);
            json response = json::parse(output);
            if (!response.is_array() || response.empty() || !response[0].is_object() ||
                !response[0].contains("s") || !response[0].contains("g"))
                throw std::runtime_error("menus/errors/mega_response"_i18n);

            s64 freeStorage;
            s64 fileSize = response[0]["s"];
            if (R_SUCCEEDED(fs::getFreeStorageSD(freeStorage)) && fileSize * 2.5 > freeStorage)
                return "";

            return response[0]["g"];
        }

        std::string mega_node_key(std::string url)
        {
            /* Check if old link format */
            auto len = url.length();

            if (len < 52)
                throw std::invalid_argument("Invalid URL.");

            bool old_link = url.find("#!") < len;

            /* End with the last '#' or "!" if its the old link format */
            auto end_pos = url.find_last_of(old_link ? '!' : '#') + 1;

            /* Crop the URL to get the file key */
            std::string key = url.substr(end_pos, len - end_pos);

            /* Replace URL characters with B64 characters */
            std::replace(key.begin(), key.end(), '_', '/');
            std::replace(key.begin(), key.end(), '-', '+');

            /* Add padding */
            auto key_len = key.length();
            unsigned pad = 4 - key_len % 4;
            key.append(pad, '=');

            /* The encoded key should have 44 characters to produce a 32 byte node key */
            if (key.length() != 44)
                throw std::invalid_argument("Invalid URL key.");

            std::string decoded(key.size() * 3 / 4, 0);
            size_t olen = 0;

            mbedtls_base64_decode(
                reinterpret_cast<unsigned char*>(decoded.data()),
                decoded.size(),
                &olen,
                reinterpret_cast<const unsigned char*>(key.c_str()),
                key.size());

            /**
             * The encoded base64 is (usually?) 43 characters long. With padding it goes
             * to 44. When we calculate the decoded size, we need to allocate 33 bytes.
             * But the last encoded character is padding, and combined with the last 
             * valid character, it should produce a 32 byte node key.
             */
            decoded.resize(olen);
            if (decoded.size() != 32)
                throw std::invalid_argument("Invalid node key.");

            return decoded;
        }

        std::string mega_key(const std::string& node_key)
        {
            std::string key(16, 0);

            reinterpret_cast<uint64_t*>(key.data())[0] =
                reinterpret_cast<const uint64_t*>(node_key.data())[0] ^
                reinterpret_cast<const uint64_t*>(node_key.data())[2];
            reinterpret_cast<uint64_t*>(key.data())[1] =
                reinterpret_cast<const uint64_t*>(node_key.data())[1] ^
                reinterpret_cast<const uint64_t*>(node_key.data())[3];

            return key;
        }

        std::string mega_iv(const std::string& node_key)
        {
            std::string iv(16, 0);
            reinterpret_cast<uint64_t*>(iv.data())[0] =
                reinterpret_cast<const uint64_t*>(node_key.data())[2];

            return iv;
        }
    }  // namespace

    long downloadFile(const std::string& url, const std::string& output, int api)
    {
        std::vector<std::uint8_t> dummy;
        return downloadFile(url, dummy, output, api);
    }

    long downloadFile(const std::string& url, std::vector<std::uint8_t>& res, const std::string& output, int api)
    {
        bool reportProgress = api == OFF;
        auto& progress = ProgressEvent::instance();
        long status = 0;
        res.clear();
        time_old = std::chrono::steady_clock::now();
        progress_time_old = time_old;
        dlold = 0.0;
        const bool isMega = url.find("mega.nz") != std::string::npos;
        if (reportProgress) {
            progress.setOperation(ProgressOperation::DOWNLOAD);
            progress.setCurrentFile(getDownloadDisplayName(url, output, isMega));
            progress.setNow(0);
            progress.setTotalCount(0);
            progress.setSpeed(0);
            progress.setFileProgress(0, 0);
            progress.setStatusCode(0);
        }
        try {
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
            if (!curl)
                throw std::runtime_error("menus/errors/download_init"_i18n);
            configureTransfer(curl.get(), &reportProgress);
            const std::string realUrl = isMega ? mega_url(url, reportProgress) : url;
            if (realUrl.empty() || (!output.empty() && !isMega && !checkSize(curl.get(), url)))
                throw std::runtime_error("menus/errors/insufficient_storage"_i18n);
            if (reportProgress && progress.getInterupt())
                return 0;

            Aes128CtrContext aes{};
            ntwrk_struct_t chunk{};
            chunk.cancellable = reportProgress;
            if (isMega) {
                const auto nodeKey = mega_node_key(url);
                const auto key = mega_key(nodeKey);
                const auto iv = mega_iv(nodeKey);
                aes128CtrContextCreate(&aes, key.c_str(), iv.c_str());
                chunk.aes = &aes;
            }
            std::unique_ptr<FILE, decltype(&fclose)> file(nullptr, fclose);
            std::vector<u_int8_t> buffer;
            if (!output.empty()) {
                buffer.resize(_1MiB);
                file.reset(fopen(output.c_str(), "wb"));
                if (!file)
                    throw std::runtime_error(fmt::format("menus/errors/download_open"_i18n, output));
                chunk.data = buffer.data();
                chunk.data_size = buffer.size();
                chunk.out = file.get();
            }
            else {
                chunk.memory = &res;
            }
            curl_easy_setopt(curl.get(), CURLOPT_URL, realUrl.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, API_AGENT);
            curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &chunk);
            const auto result = curl_easy_perform(curl.get());
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
            bool writeFailed = chunk.write_failed;
            if (file) {
                if (result == CURLE_OK && status >= 200 && status < 300 && chunk.offset)
                    writeFailed |= fwrite(chunk.data, 1, chunk.offset, file.get()) != chunk.offset;
                writeFailed |= fclose(file.release()) != 0;
            }
            if (reportProgress && progress.getInterupt()) {
                res.clear();
                return 0;
            }
            if (writeFailed)
                throw std::runtime_error("menus/errors/download_write"_i18n);
            if (result != CURLE_OK)
                throw std::runtime_error(fmt::format("menus/errors/download_transfer"_i18n, curl_easy_strerror(result)));
            if (status < 200 || status >= 300)
                throw std::runtime_error(fmt::format("menus/errors/download_http"_i18n, status));
            if (reportProgress) {
                progress.setStatusCode(status);
                progress.setStep(progress.getMax());
            }
            return status;
        }
        catch (const std::exception& exception) {
            res.clear();
            if (reportProgress && !progress.getInterupt()) {
                progress.setStatusCode(status);
                progress.setErrorMessage(exception.what());
                progress.setStep(progress.getMax());
            }
            return 0;
        }
    }

    std::string fetchTitle(const std::string& url)
    {
        CURL* curl_handle;
        struct MemoryStruct chunk;

        chunk.memory = static_cast<char*>(calloc(1, 1)); /* will be grown as needed by the realloc above */
        chunk.size = 0;                               /* no data at this point */

        curl_global_init(CURL_GLOBAL_ALL);
        curl_handle = curl_easy_init();
        if (!chunk.memory || !curl_handle) {
            free(chunk.memory);
            if (curl_handle) curl_easy_cleanup(curl_handle);
            curl_global_cleanup();
            return {};
        }
        bool reportProgress = false;
        configureTransfer(curl_handle, &reportProgress);
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback2);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, API_AGENT);

        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_perform(curl_handle);

        /* check for errors */
        std::string ver = "-1";
        std::string s(chunk.memory, chunk.size);
        std::regex rgx("<title>.+</title>");
        std::smatch match;
        if (std::regex_search(s, match, rgx)) {
            //ver = std::stoi(match[0]);
            //std::cout << match[0].str().substr(match[0].str().find(" ") + 1, 6) << std::endl;
            ver = match[0].str().substr(match[0].str().find(" ") + 1, 5);
        }
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);

        curl_global_cleanup();

        return ver;
    }

    long downloadPage(const std::string& url, std::string& res, const std::vector<std::string>& headers, const std::string& body)
    {
        res.clear();
        CURL* curl_handle;
        struct MemoryStruct chunk;
        struct curl_slist* list = NULL;
        long status_code = 0;

        chunk.memory = static_cast<char*>(calloc(1, 1)); /* will be grown as needed by the realloc above */
        chunk.size = 0;                               /* no data at this point */

        curl_global_init(CURL_GLOBAL_ALL);
        curl_handle = curl_easy_init();
        if (!chunk.memory || !curl_handle) {
            free(chunk.memory);
            if (curl_handle) curl_easy_cleanup(curl_handle);
            curl_global_cleanup();
            return {};
        }
        bool reportProgress = false;
        configureTransfer(curl_handle, &reportProgress);
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());
        if (!headers.empty()) {
            for (auto& h : headers) {
                list = curl_slist_append(list, h.c_str());
            }
            curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, list);
        }
        if (body != "") {
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, body.c_str());
        }

        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback2);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, API_AGENT);

        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        const auto result = curl_easy_perform(curl_handle);
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &status_code);
        curl_easy_cleanup(curl_handle);
        curl_slist_free_all(list);
        if (result == CURLE_OK && status_code >= 200 && status_code < 300)
            res.assign(chunk.memory, chunk.size);
        else {
            res.clear();
            if (result != CURLE_OK) status_code = 0;
        }
        free(chunk.memory);

        curl_global_cleanup();
        return status_code;
    }

    long getRequest(const std::string& url, nlohmann::ordered_json& res, const std::vector<std::string>& headers, const std::string& body)
    {
        std::string request;
        long status_code = downloadPage(url, request, headers, body);

        if (json::accept(request))
            res = nlohmann::ordered_json::parse(request);
        else
            res = nlohmann::ordered_json::object();

        return status_code;
    }

    std::vector<std::pair<std::string, std::string>> getLinks(const std::string& url)
    {
        nlohmann::ordered_json request;
        getRequest(url, request);

        std::vector<std::pair<std::string, std::string>> res;
        for (auto it = request.begin(); it != request.end(); ++it) {
            res.push_back(std::make_pair(it.key(), it.value()));
        }
        return res;
    }

    std::vector<std::pair<std::string, std::string>> getLinksFromJson(const nlohmann::ordered_json& json_object)
    {
        std::vector<std::pair<std::string, std::string>> res;
        for (auto it = json_object.begin(); it != json_object.end(); ++it) {
            res.push_back(std::make_pair(it.key(), it.value()));
        }
        return res;
    }

}  // namespace download