#pragma once

#include <switch.h>
#include <functional>
#include <string>
#include <vector>

namespace aio::forwarder {

struct Config {
    std::string nro_path;
    std::string args;
    std::string name;
    std::string author;
    NacpStruct nacp{};
    std::vector<u8> icon;
};

using ProgressCallback = std::function<void(int current, int total, const std::string& message)>;

Result install(Config config, NcmStorageId storage_id = NcmStorageId_SdCard, ProgressCallback progress = {});

} 
