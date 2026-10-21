#include "extract.hpp"

#include <dirent.h>
#include <minizip/unzip.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "current_cfw.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "main_frame.hpp"
#include "progress_event.hpp"
#include "utils.hpp"

namespace i18n = brls::i18n;
using namespace i18n::literals;

constexpr size_t WRITE_BUFFER_SIZE = 0x10000;

namespace extract {

    namespace {
        bool caselessCompare(const std::string& a, const std::string& b)
        {
            return strcasecmp(a.c_str(), b.c_str()) == 0;
        }

        class Archive
        {
        public:
            unzFile file;
            explicit Archive(const std::string& path) : file(unzOpen64(path.c_str()))
            {
                if (!file)
                    throw std::runtime_error("menus/errors/archive_open"_i18n);
            }
            ~Archive() { unzClose(file); }
            Archive(const Archive&) = delete;
            Archive& operator=(const Archive&) = delete;
        };

        void checkZip(int result, const std::string& operation)
        {
            if (result != UNZ_OK)
                throw std::runtime_error(std::string(operation) + " (ZIP " + std::to_string(result) + ").");
        }

        unz_global_info64 archiveInfo(unzFile file)
        {
            unz_global_info64 info{};
            checkZip(unzGetGlobalInfo64(file, &info), "menus/errors/archive_info"_i18n);
            return info;
        }

        unz_file_info64 entryInfo(unzFile file, std::string* name = nullptr)
        {
            unz_file_info64 info{};
            checkZip(unzGetCurrentFileInfo64(file, &info, nullptr, 0, nullptr, 0, nullptr, 0),
                     "menus/errors/archive_info"_i18n);
            if (info.uncompressed_size > static_cast<ZPOS64_T>(std::numeric_limits<s64>::max()))
                throw std::runtime_error("menus/errors/archive_too_large"_i18n);
            if (name) {
                if (info.size_filename == 0 || info.size_filename > 0x300)
                    throw std::runtime_error("menus/errors/archive_name"_i18n);
                std::vector<char> buffer(static_cast<size_t>(info.size_filename) + 1, '\0');
                checkZip(unzGetCurrentFileInfo64(file, &info, buffer.data(), buffer.size(), nullptr, 0, nullptr, 0),
                         "menus/errors/archive_info"_i18n);
                name->assign(buffer.data(), info.size_filename);
                if (name->find('\0') != std::string::npos)
                    throw std::runtime_error("Nombre de archivo no valido en el ZIP.");
            }
            return info;
        }

        s64 getUncompressedSize(const std::string& archivePath)
        {
            Archive archive(archivePath);
            const auto gi = archiveInfo(archive.file);
            s64 size = 0;
            for (ZPOS64_T i = 0; i < gi.number_entry; ++i) {
                if (ProgressEvent::instance().getInterupt())
                    return -1;
                const auto fi = entryInfo(archive.file);
                if (fi.uncompressed_size > static_cast<ZPOS64_T>(std::numeric_limits<s64>::max() - size))
                    throw std::runtime_error("menus/errors/archive_too_large"_i18n);
                size += static_cast<s64>(fi.uncompressed_size);
                if (i + 1 < gi.number_entry)
                    checkZip(unzGoToNextFile(archive.file), "menus/errors/archive_walk"_i18n);
            }
            return size;
        }

        s64 ensureAvailableStorage(const std::string& archivePath)
        {
            const s64 uncompressedSize = getUncompressedSize(archivePath);
            if (uncompressedSize < 0)
                return -1;
            s64 freeStorage = 0;
            if (R_SUCCEEDED(fs::getFreeStorageSD(freeStorage))) {
                brls::Logger::info("Uncompressed size of archive {}: {}. Available: {}", archivePath, uncompressedSize, freeStorage);
                if (uncompressedSize * 1.1 > freeStorage)
                    throw std::runtime_error("menus/errors/insufficient_storage"_i18n);
            }
            return uncompressedSize;
        }

        void publishExtractionProgress(s64 overallNow, s64 overallTotal, s64 fileNow, s64 fileTotal)
        {
            ProgressEvent::instance().setExtractionProgress(
                static_cast<double>(overallNow), static_cast<double>(overallTotal),
                static_cast<double>(fileNow), static_cast<double>(fileTotal));
        }

        void extractEntry(const std::string& filename, unzFile& zfile, s64& overallNow, s64 overallTotal)
        {
            const auto fi = entryInfo(zfile);
            const s64 fileTotal = static_cast<s64>(fi.uncompressed_size);
            s64 fileNow = 0;
            auto& progress = ProgressEvent::instance();
            progress.setCurrentFile(filename);
            publishExtractionProgress(overallNow, overallTotal, 0, fileTotal);
            if (progress.getInterupt())
                return;
            if (filename.empty())
                throw std::runtime_error("menus/errors/archive_name"_i18n);
            if (filename.back() == '/') {
                fs::createTree(filename);
                overallNow += fileTotal;
                publishExtractionProgress(overallNow, overallTotal, fileTotal, fileTotal);
                return;
            }

            // ZIP files need not contain separate directory entries.
            fs::createTree(filename);
            checkZip(unzOpenCurrentFile(zfile), "menus/errors/archive_entry_open"_i18n);
            struct CurrentEntry {
                unzFile file;
                bool open = true;
                ~CurrentEntry() { if (open) unzCloseCurrentFile(file); }
            } current{zfile};
            std::vector<unsigned char> buffer(WRITE_BUFFER_SIZE);
            std::unique_ptr<FILE, decltype(&fclose)> outfile(fopen(filename.c_str(), "wb"), fclose);
            if (!outfile)
                throw std::runtime_error(fmt::format("menus/errors/extract_create"_i18n, filename));
            auto lastUiUpdate = std::chrono::steady_clock::now();
            while (!progress.getInterupt()) {
                const int bytesRead = unzReadCurrentFile(zfile, buffer.data(), buffer.size());
                if (bytesRead < 0)
                    throw std::runtime_error(fmt::format("menus/errors/extract_read"_i18n, filename, bytesRead));
                if (bytesRead == 0)
                    break;
                const size_t bytesWritten = fwrite(buffer.data(), 1, static_cast<size_t>(bytesRead), outfile.get());
                if (bytesWritten != static_cast<size_t>(bytesRead))
                    throw std::runtime_error(fmt::format("menus/errors/extract_write"_i18n, filename));
                fileNow += static_cast<s64>(bytesWritten);
                overallNow += static_cast<s64>(bytesWritten);
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - lastUiUpdate).count() >= 0.10 || fileNow >= fileTotal) {
                    publishExtractionProgress(overallNow, overallTotal, fileNow, fileTotal);
                    lastUiUpdate = now;
                }
            }
            const int closeResult = fclose(outfile.release());
            const int zipResult = unzCloseCurrentFile(zfile);
            current.open = false;
            if (closeResult != 0)
                throw std::runtime_error(fmt::format("menus/errors/extract_close"_i18n, filename));
            if (progress.getInterupt())
                return;
            checkZip(zipResult, "menus/errors/extract_crc"_i18n);
            if (fileNow != fileTotal)
                throw std::runtime_error(fmt::format("menus/errors/extract_incomplete"_i18n, filename));
            publishExtractionProgress(overallNow, overallTotal, fileNow, fileTotal);
        }
    }  // namespace

    void extract(const std::string& archivePath, const std::string& workingPath, bool preserveInis, std::function<void()> func)
    {
        ProgressEvent::instance().setOperation(ProgressOperation::EXTRACT);
        ProgressEvent::instance().setCurrentFile("Preparando extraccion...");
        ProgressEvent::instance().setFileProgress(0, 0);

        const s64 totalUncompressed = ensureAvailableStorage(archivePath);
        if (totalUncompressed < 0)
            return;

        Archive archive(archivePath);
        unzFile zfile = archive.file;
        const auto gi = archiveInfo(zfile);

        ProgressEvent::instance().setTotalSteps(1000);
        ProgressEvent::instance().setStep(0);
        ProgressEvent::instance().setNow(0);
        ProgressEvent::instance().setTotalCount(static_cast<double>(totalUncompressed));
        ProgressEvent::instance().setSpeed(0);

        s64 overallNow = 0;

        std::set<std::string> ignoreList = fs::readLineByLine(FILES_IGNORE);
        std::string appPath = util::getAppPath();

        for (ZPOS64_T i = 0; i < gi.number_entry; ++i) {
            std::string entryName;
            const auto fi = entryInfo(zfile, &entryName);
            std::string filename = workingPath + entryName;
            bool extracted = false;

            if (ProgressEvent::instance().getInterupt()) {
                break;
            }
            if (appPath != filename) {
                if ((preserveInis == true && filename.size() >= 4 && filename.substr(filename.length() - 4) == ".ini") || std::find_if(ignoreList.begin(), ignoreList.end(), [&filename](std::string ignored) {
                                                                                                                    size_t res = filename.find(ignored);
                                                                                                                    return (res == 0 || res == 1); }) != ignoreList.end()) {
                    if (!std::filesystem::exists(filename)) {
                        extractEntry(filename, zfile, overallNow, totalUncompressed);
                        extracted = true;
                    }
                }
                else {
                    if ((filename == "/atmosphere/package3") || (filename == "/atmosphere/stratosphere.romfs")) {
                        extractEntry(filename + ".aio", zfile, overallNow, totalUncompressed);
                        extracted = true;
                    }
                    else {
                        extractEntry(filename, zfile, overallNow, totalUncompressed);
                        extracted = true;
                        if (!ProgressEvent::instance().getInterupt() && filename.substr(0, 14) == "/hekate_ctcaer") {
                            fs::copyFile(filename, UPDATE_BIN_PATH);
                            if (CurrentCfw::running_cfw == CFW::ams && util::showDialogBoxBlocking(fmt::format("menus/utils/set_hekate_reboot_payload"_i18n, UPDATE_BIN_PATH, REBOOT_PAYLOAD_PATH), "menus/common/yes"_i18n, "menus/common/no"_i18n) == 0) {
                                fs::copyFile(UPDATE_BIN_PATH, REBOOT_PAYLOAD_PATH);
                            }
                        }
                    }
                }
            }
            if (!extracted) {
                overallNow += static_cast<s64>(fi.uncompressed_size);
                ProgressEvent::instance().setCurrentFile(filename);
                publishExtractionProgress(
                    overallNow,
                    totalUncompressed,
                    static_cast<s64>(fi.uncompressed_size),
                    static_cast<s64>(fi.uncompressed_size));
            }

            if (ProgressEvent::instance().getInterupt())
                break;
            if (i + 1 < gi.number_entry)
                checkZip(unzGoToNextFile(zfile), "menus/errors/archive_walk"_i18n);
        }
        if (!ProgressEvent::instance().getInterupt()) {
            publishExtractionProgress(totalUncompressed, totalUncompressed, 0, 0);
        }
        ProgressEvent::instance().setStep(ProgressEvent::instance().getMax());
    }

    std::vector<std::string> getInstalledTitlesNs()
    {
        std::vector<std::string> titles;

        NsApplicationRecord* records = new NsApplicationRecord[MaxTitleCount]();
        NsApplicationControlData* controlData = NULL;

        s32 recordCount = 0;
        u64 controlSize = 0;

        if (R_SUCCEEDED(nsListApplicationRecord(records, MaxTitleCount, 0, &recordCount))) {
            for (s32 i = 0; i < recordCount; i++) {
                controlSize = 0;
                free(controlData);
                controlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
                if (controlData == NULL) {
                    break;
                }
                else {
                    memset(controlData, 0, sizeof(NsApplicationControlData));
                }

                if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, records[i].application_id, controlData, sizeof(NsApplicationControlData), &controlSize))) continue;

                if (controlSize < sizeof(controlData->nacp)) {
                    continue;
                }

                titles.push_back(util::formatApplicationId(records[i].application_id));
            }
            free(controlData);
        }
        delete[] records;
        std::sort(titles.begin(), titles.end());
        return titles;
    }

    std::vector<std::string> excludeTitles(const std::string& path, const std::vector<std::string>& listedTitles)
    {
        std::vector<std::string> titles;
        std::ifstream file(path);
        std::string name;

        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                std::transform(line.begin(), line.end(), line.begin(), ::toupper);
                for (size_t i = 0; i < listedTitles.size(); i++) {
                    if (line == listedTitles[i]) {
                        titles.push_back(line);
                        break;
                    }
                }
            }
        }

        std::sort(titles.begin(), titles.end());

        std::vector<std::string> diff;
        std::set_difference(listedTitles.begin(), listedTitles.end(), titles.begin(), titles.end(),
                            std::inserter(diff, diff.begin()));
        return diff;
    }

    int computeOffset(CFW cfw)
    {
        switch (cfw) {
            case CFW::ams:
                std::filesystem::create_directory(AMS_PATH);
                std::filesystem::create_directory(AMS_CONTENTS);
                chdir(AMS_PATH);
                return std::string(CONTENTS_PATH).length();
                break;
            case CFW::rnx:
                std::filesystem::create_directory(REINX_PATH);
                std::filesystem::create_directory(REINX_CONTENTS);
                chdir(REINX_PATH);
                return std::string(CONTENTS_PATH).length();
                break;
            case CFW::sxos:
                std::filesystem::create_directory(SXOS_PATH);
                std::filesystem::create_directory(SXOS_TITLES);
                chdir(SXOS_PATH);
                return std::string(TITLES_PATH).length();
                break;
        }
        return 0;
    }

    void extractCheats(const std::string& archivePath, const std::vector<std::string>& titles, CFW cfw, const std::string& version, bool extractAll)
    {
        ProgressEvent::instance().setOperation(ProgressOperation::EXTRACT);
        ProgressEvent::instance().setCurrentFile("Preparando extraccion...");
        ProgressEvent::instance().setFileProgress(0, 0);

        const s64 totalUncompressed = ensureAvailableStorage(archivePath);
        if (totalUncompressed < 0)
            return;

        Archive archive(archivePath);
        unzFile zfile = archive.file;
        const auto gi = archiveInfo(zfile);

        ProgressEvent::instance().setTotalSteps(1000);
        ProgressEvent::instance().setStep(0);
        ProgressEvent::instance().setNow(0);
        ProgressEvent::instance().setTotalCount(static_cast<double>(totalUncompressed));
        ProgressEvent::instance().setSpeed(0);

        int offset = computeOffset(cfw);
        s64 overallNow = 0;

        for (ZPOS64_T i = 0; i < gi.number_entry; ++i) {
            std::string filename;
            const auto fi = entryInfo(zfile, &filename);
            bool extracted = false;

            if (ProgressEvent::instance().getInterupt()) {
                break;
            }

            if ((int)filename.size() > offset + 16 + 7 && caselessCompare(filename.substr(offset + 16, 7), "/cheats")) {
                if (extractAll) {
                    extractEntry(filename, zfile, overallNow, totalUncompressed);
                    extracted = true;
                }
                else {
                    if (std::find_if(titles.begin(), titles.end(), [&filename, offset](std::string title) {
                            return caselessCompare((title.substr(0, 13)), filename.substr(offset, 13));
                        }) != titles.end()) {
                        extractEntry(filename, zfile, overallNow, totalUncompressed);
                        extracted = true;
                    }
                }
            }

            if (!extracted) {
                overallNow += static_cast<s64>(fi.uncompressed_size);
                ProgressEvent::instance().setCurrentFile(filename);
                publishExtractionProgress(
                    overallNow,
                    totalUncompressed,
                    static_cast<s64>(fi.uncompressed_size),
                    static_cast<s64>(fi.uncompressed_size));
            }

            if (ProgressEvent::instance().getInterupt())
                break;
            if (i + 1 < gi.number_entry)
                checkZip(unzGoToNextFile(zfile), "menus/errors/archive_walk"_i18n);
        }
        if (!ProgressEvent::instance().getInterupt())
            ProgressEvent::instance().setNow(ProgressEvent::instance().getTotal());
        if (!ProgressEvent::instance().getInterupt() && version != "offline" && version != "") {
            util::saveToFile(version, CHEATS_VERSION);
        }
        ProgressEvent::instance().setStep(ProgressEvent::instance().getMax());
    }

    void extractAllCheats(const std::string& archivePath, CFW cfw, const std::string& version)
    {
        extractCheats(archivePath, {}, cfw, version, true);
    }

    bool isBID(const std::string& bid)
    {
        for (char const& c : bid) {
            if (!isxdigit(c)) return false;
        }
        return true;
    }

    void writeTitlesToFile(const std::set<std::string>& titles, const std::string& path)
    {
        std::ofstream updatedTitlesFile;
        std::set<std::string>::iterator it = titles.begin();
        updatedTitlesFile.open(path, std::ofstream::out | std::ofstream::trunc);
        if (updatedTitlesFile.is_open()) {
            while (it != titles.end()) {
                updatedTitlesFile << (*it) << std::endl;
                it++;
            }
            updatedTitlesFile.close();
        }
    }

    void removeCheats()
    {
        std::string path = util::getContentsPath();
        ProgressEvent::instance().setTotalSteps(std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator()) + 1);
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (ProgressEvent::instance().getInterupt()) {
                break;
            }
            removeCheatsDirectory(entry.path().string());
            ProgressEvent::instance().incrementStep(1);
        }
        std::filesystem::remove(CHEATS_VERSION);
        ProgressEvent::instance().setStep(ProgressEvent::instance().getMax());
    }

    void removeOrphanedCheats()
    {
        auto path = util::getContentsPath();
        std::vector<std::string> titles = getInstalledTitlesNs();
        ProgressEvent::instance().setTotalSteps(std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator()) + 1);
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (ProgressEvent::instance().getInterupt()) {
                break;
            }
            if (std::find_if(titles.begin(), titles.end(), [&entry](std::string title) {
                    return caselessCompare(entry.path().filename(), title);
                }) == titles.end()) {
                removeCheatsDirectory(entry.path().string());
            }
            ProgressEvent::instance().incrementStep(1);
        }
        std::filesystem::remove(CHEATS_VERSION);
        ProgressEvent::instance().setStep(ProgressEvent::instance().getMax());
    }

    bool removeCheatsDirectory(const std::string& entry)
    {
        bool res = true;
        std::string cheatsPath = fmt::format("{}/cheats", entry);
        if (std::filesystem::exists(cheatsPath)) res &= fs::removeDir(cheatsPath);
        if (std::filesystem::is_empty(entry)) res &= fs::removeDir(entry);
        return res;
    }

}  // namespace extract
