#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace sd_card {
using Cid = std::array<std::uint8_t, 16>;

struct Identity {
    unsigned mid, oem, revision, month, year;
    std::uint32_t serial;
    std::string product;
};

inline std::optional<Identity> decode(const Cid& cid)
{
    // Horizon returns the 120 CID data bits least-significant byte first;
    // byte 15 is padding. The controller has stripped the CRC/end byte.
    if (std::all_of(cid.begin(), cid.begin() + 15, [](auto b) { return b == 0; }) ||
        std::all_of(cid.begin(), cid.begin() + 15, [](auto b) { return b == 0xff; }))
        return std::nullopt;
    Identity id{};
    id.mid = cid[14];
    id.oem = (cid[13] << 8) | cid[12];
    id.revision = cid[6];
    id.month = cid[0] & 0x0f;
    id.year = 2000 + ((cid[1] & 0x0f) << 4) + (cid[0] >> 4);
    for (int i = 5; i >= 2; --i)
        id.serial = (id.serial << 8) | cid[i];
    for (int i = 11; i >= 7; --i) {
        if (cid[i] < 32 || cid[i] > 126) break;
        id.product += static_cast<char>(cid[i]);
    }
    return id;
}

inline const char* manufacturer(const Identity& id)
{
    switch (id.mid) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba";
        case 0x03: return id.oem == 0x5744 ? "Western Digital" : "SanDisk";
        case 0x06: return "Ritek";
        case 0x09: return "ATP";
        case 0x13: return "Kingmax";
        case 0x1b: return "Samsung";
        case 0x1d: return "ADATA";
        case 0x27: return "Phison";
        case 0x31: return "Silicon Power";
        default: return nullptr;
    }
}
}
