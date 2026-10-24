#pragma once

#include "ui/types.hpp"

#include <cstddef>
#include <string>

namespace sphaira::ui::activity_log {

enum class Tone {
    Normal,
    Muted,
    Success,
    Warning,
    Error,
};

struct DrawInfo {
    std::size_t first{};
    std::size_t last{};
    std::size_t total{};
    bool following_tail{true};
};

struct CopyStatus {
    bool active{};
    std::string path;
    u64 id{};
    u64 copied{};
    u64 total{};
    u64 bytes_per_second{};
};

void Add(const std::string& tag, const std::string& text, Tone tone = Tone::Normal);
// Adds the original hexadecimal result plus a short, translated explanation.
// The module/description pair is retained so uncommon failures remain useful
// when the user reports them.
void AddResult(Result result, Tone tone = Tone::Error);
void ScrollOlder(std::size_t lines = 1);
void ScrollNewer(std::size_t lines = 1);

// Tracks only writes made through the normal microSD MTP mount. Install
// transfers use their existing YATI progress path and never enter this state.
auto BeginCopy(const std::string& path, u64 total) -> u64;
void UpdateCopy(u64 id, u64 copied, u64 total);
void FinishCopy(u64 id, bool complete, Result result = 0);
auto GetCopyStatus() -> CopyStatus;

// Draws a clipped, session-long console view. The log is intentionally kept
// in memory: it survives every installation while this helper is open, but it
// does not write extra files to the user's microSD.
auto Draw(NVGcontext* vg, Theme* theme, float x, float y, float w, float h) -> DrawInfo;

} // namespace sphaira::ui::activity_log
