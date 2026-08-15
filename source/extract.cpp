#include "extract.hpp"

#include <dirent.h>
#include <minizip/unzip.h>

#include <algorithm>
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

        s64 getUncompressedSize(const std::string& archivePath)
        {
            s64 size = 0;
            unzFile zfile = unzOpen(archivePath.c_str());
            unz_global_info gi;
            unzGetGlobalInfo(zfile, &gi);
            for (uLong i = 0; i < gi.number_entry; ++i) {
                unz_file_info fi;
                unzOpenCurrentFile(zfile);
                unzGetCurrentFileInfo(zfile, &fi, NULL, 0, NULL, 0, NULL, 0);
                size += fi.uncompressed_size;
                unzCloseCurrentFile(zfile);
                unzGoToNextFile(zfile);
            }
            unzClose(zfile);
            return size;  
        }

        s64 ensureAvailableStorage(const std::string& archivePath)
        {
            s64 uncompressedSize = getUncompressedSize(archivePath);
            s64 freeStorage;

            if (R_SUCCEEDED(fs::getFreeStorageSD(freeStorage))) {
                brls::Logger::info("Uncompressed size of archive {}: {}. Available: {}", archivePath, uncompressedSize, freeStorage);
                if (uncompressedSize * 1.1 > freeStorage) {
                    auto& progress = ProgressEvent::instance();
                    progress.setErrorMessage("menus/errors/insufficient_storage"_i18n);
                    progress.setStep(progress.getMax());
                    return -1;
                }
            }

            return uncompressedSize;
        }

        void publishExtractionProgress(s64 overallNow, s64 overallTotal, s64 fileNow, s64 fileTotal)
        {
            ProgressEvent::instance().setExtractionProgress(
                static_cast<double>(overallNow),
                static_cast<double>(overallTotal),
                static_cast<double>(fileNow),
                static_cast<double>(fileTotal));
        }

        void extractEntry(std::string filename, unzFile& zfile, s64& overallNow, s64 overallTotal, bool forceCreateTree = false)
        {
            unz_file_info fi{};
            unzGetCurrentFileInfo(zfile, &fi, NULL, 0, NULL, 0, NULL, 0);

            const s64 fileTotal = static_cast<s64>(fi.uncompressed_size);
            s64 fileNow = 0;

            auto& progress = ProgressEvent::instance();
            progress.setCurrentFile(filename);
            publishExtractionProgress(overallNow, overallTotal, fileNow, fileTotal);

            if (filename.empty()) {
                overallNow += fileTotal;
                publishExtractionProgress(overallNow, overallTotal, fileTotal, fileTotal);
                return;
            }

            if (filename.back() == '/') {
                fs::createTree(filename);
                overallNow += fileTotal;
                publishExtractionProgress(overallNow, overallTotal, fileTotal, fileTotal);
                return;
            }
            if (forceCreateTree) {
                fs::createTree(filename);
            }

            void* buf = malloc(WRITE_BUFFER_SIZE);
            FILE* outfile = fopen(filename.c_str(), "wb");
            if (!buf || !outfile) {
                if (buf) free(buf);
                if (outfile) fclose(outfile);

                
                overallNow += fileTotal;
                publishExtractionProgress(overallNow, overallTotal, fileTotal, fileTotal);
                return;
            }

            auto lastUiUpdate = std::chrono::steady_clock::now();

            for (int bytesRead = unzReadCurrentFile(zfile, buf, WRITE_BUFFER_SIZE);
                 bytesRead > 0;
                 bytesRead = unzReadCurrentFile(zfile, buf, WRITE_BUFFER_SIZE)) {
                if (progress.getInterupt())
                    break;

                const size_t bytesWritten = fwrite(buf, 1, static_cast<size_t>(bytesRead), outfile);
                fileNow += static_cast<s64>(bytesWritten);
                overallNow += static_cast<s64>(bytesWritten);

                const auto now = std::chrono::steady_clock::now();
                const bool uiIntervalReached = std::chrono::duration<double>(now - lastUiUpdate).count() >= 0.10;
                const bool fileFinished = fileTotal > 0 && fileNow >= fileTotal;

                
                if (uiIntervalReached || fileFinished) {
                    publishExtractionProgress(overallNow, overallTotal, fileNow, fileTotal);
                    lastUiUpdate = now;
                }

                if (bytesWritten != static_cast<size_t>(bytesRead))
                    break;
            }

            
            publishExtractionProgress(overallNow, overallTotal, fileNow, fileTotal);

            free(buf);
            fclose(outfile);
        }
    }  

    void extract(const std::string& archivePath, const std::string& workingPath, bool preserveInis, std::function<void()> func)
    {
        ProgressEvent::instance().setOperation(ProgressOperation::EXTRACT);
        ProgressEvent::instance().setCurrentFile("");
        ProgressEvent::instance().setFileProgress(0, 0);

        const s64 totalUncompressed = ensureAvailableStorage(archivePath);
        if (totalUncompressed < 0)
            return;

        unzFile zfile = unzOpen(archivePath.c_str());
        unz_global_info gi;
        unzGetGlobalInfo(zfile, &gi);

        ProgressEvent::instance().setTotalSteps(1000);
        ProgressEvent::instance().setStep(0);
        ProgressEvent::instance().setNow(0);
        ProgressEvent::instance().setTotalCount(static_cast<double>(totalUncompressed));
        ProgressEvent::instance().setSpeed(0);

        s64 overallNow = 0;

        std::set<std::string> ignoreList = fs::readLineByLine(FILES_IGNORE);
        std::string appPath = util::getAppPath();

        for (uLong i = 0; i < gi.number_entry; ++i) {
            char szFilename[0x301] = "";
            unz_file_info fi{};
            unzOpenCurrentFile(zfile);
            unzGetCurrentFileInfo(zfile, &fi, szFilename, sizeof(szFilename), NULL, 0, NULL, 0);
            std::string filename = workingPath + szFilename;
            bool extracted = false;

            if (ProgressEvent::instance().getInterupt()) {
                unzCloseCurrentFile(zfile);
                break;
            }
            if (appPath != filename) {
                if ((preserveInis == true && filename.substr(filename.length() - 4) == ".ini") || std::find_if(ignoreList.begin(), ignoreList.end(), [&filename](std::string ignored) {
                                                                                                                    u8 res = filename.find(ignored);
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
                        if (filename.substr(0, 14) == "/hekate_ctcaer") {
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

            unzCloseCurrentFile(zfile);
            unzGoToNextFile(zfile);
        }
        unzClose(zfile);
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
        ProgressEvent::instance().setCurrentFile("");
        ProgressEvent::instance().setFileProgress(0, 0);

        const s64 totalUncompressed = ensureAvailableStorage(archivePath);
        if (totalUncompressed < 0)
            return;

        unzFile zfile = unzOpen(archivePath.c_str());
        unz_global_info gi;
        unzGetGlobalInfo(zfile, &gi);

        ProgressEvent::instance().setTotalSteps(1000);
        ProgressEvent::instance().setStep(0);
        ProgressEvent::instance().setNow(0);
        ProgressEvent::instance().setTotalCount(static_cast<double>(totalUncompressed));
        ProgressEvent::instance().setSpeed(0);

        int offset = computeOffset(cfw);
        s64 overallNow = 0;

        for (uLong i = 0; i < gi.number_entry; ++i) {
            char szFilename[0x301] = "";
            unz_file_info fi{};
            unzOpenCurrentFile(zfile);
            unzGetCurrentFileInfo(zfile, &fi, szFilename, sizeof(szFilename), NULL, 0, NULL, 0);
            std::string filename = szFilename;
            bool extracted = false;

            if (ProgressEvent::instance().getInterupt()) {
                unzCloseCurrentFile(zfile);
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

            unzCloseCurrentFile(zfile);
            unzGoToNextFile(zfile);
        }
        unzClose(zfile);
        if (!ProgressEvent::instance().getInterupt())
            ProgressEvent::instance().setNow(ProgressEvent::instance().getTotal());
        if (version != "offline" && version != "") {
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

}  
