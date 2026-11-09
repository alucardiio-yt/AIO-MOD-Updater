#include "fs.hpp"

#include <borealis.hpp>
#include <filesystem>
#include <fstream>

#include "constants.hpp"
#include "storage_error.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

namespace fs {
    std::vector<std::string> splitString(const std::string& s, char delimiter)
    {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    bool removeDir(const std::string& path)
    {
        Result ret = 0;
        FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
        if (R_FAILED(ret = fsFsDeleteDirectoryRecursively(fs, path.c_str())))
            return false;
        return true;
    }

    nlohmann::ordered_json parseJsonFile(const std::string& path)
    {
        std::ifstream file(path);

        std::string fileContent((std::istreambuf_iterator<char>(file)),
                                (std::istreambuf_iterator<char>()));

        if (nlohmann::ordered_json::accept(fileContent))
            return nlohmann::ordered_json::parse(fileContent);
        else
            return nlohmann::ordered_json::object();
    }

    void writeJsonToFile(nlohmann::ordered_json& data, const std::string& path)
    {
        std::ofstream out(path);
        out << data.dump(4);
    }

    bool copyFile(const std::string& from, const std::string& to)
    {
        std::ifstream src(from, std::ios::binary);
        if (src.good()) {
            std::ofstream dst(to, std::ios::binary);
            if (!dst.good()) return false;
            dst << src.rdbuf();
            dst.flush();
            const bool written = !src.bad() && dst.good();
            dst.close();
            if (!written || dst.fail()) return false;
            std::error_code error;
            const auto sourceSize = std::filesystem::file_size(from, error);
            if (error) return false;
            const auto destinationSize = std::filesystem::file_size(to, error);
            return !error && sourceSize == destinationSize;
        }
        return false;
    }

    void createTree(std::string path)
    {
        std::string delimiter = "/";
        size_t pos = 0;
        std::string token;
        std::string directories("");
        while ((pos = path.find(delimiter)) != std::string::npos) {
            token = path.substr(0, pos);
            directories += token + "/";
            std::filesystem::create_directory(directories);
            path.erase(0, pos + delimiter.length());
        }
    }

    std::string copyFiles(const std::string& path)
    {
        std::string error = "";
        if (std::filesystem::exists(path)) {
            std::string str;
            std::ifstream in(path);
            if (in) {
                while (std::getline(in, str)) {
                    if (str.size() > 0) {
                        auto toMove = splitString(str, '|');
                        if (std::filesystem::exists(toMove[0]) && toMove.size() > 1) {
                            if (!copyFile(toMove[0], toMove[1]))
                                error += toMove[0] + " -> " + toMove[1] + "\n";
                        }
                        else {
                            error += toMove[0] + "\n";
                        }
                    }
                }
            }
        }
        if (error == "") {
            error = "menus/common/all_done"_i18n;
        }
        else {
            error = "menus/tools/batch_copy_not_found"_i18n + error;
        }
        return error;
    }

    std::set<std::string> readLineByLine(const std::string& path)
    {
        std::set<std::string> res;
        std::ifstream lines(path);
        std::string line;
        if (lines) {
            while (std::getline(lines, line)) {
                if (line.size() > 0) {
                    if (line.back() == '\r')
                        line.pop_back();
                    res.insert(line);
                }
            }
        }
        return res;
    }

    Result getFreeStorageSD(s64& free)
    {
        return nsGetFreeSpaceSize(NcmStorageId_SdCard, &free);
    }

}  // namespace fs

namespace storage {
    std::string describeError(const std::string& path, const std::error_code& error)
    {
        if (error == std::errc::no_such_file_or_directory)
            return fmt::format("menus/errors/storage_missing"_i18n, path);
        if (error == std::errc::no_space_on_device)
            return fmt::format("menus/errors/storage_no_space"_i18n, path);
        if (error == std::errc::permission_denied || error == std::errc::read_only_file_system)
            return fmt::format("menus/errors/storage_denied"_i18n, path);
        if (error == std::errc::io_error)
            return fmt::format("menus/errors/storage_io"_i18n, path);
        return fmt::format("menus/errors/storage_other"_i18n, path);
    }

}  // namespace storage
