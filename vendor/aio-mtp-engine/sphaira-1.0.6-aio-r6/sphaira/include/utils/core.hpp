#pragma once

#include "defines.hpp"

namespace sphaira {

enum class CpuCoreMode : u8 {
    Three = 3,
    Four = 4,
};

}

namespace sphaira::utils {

constexpr u64 SafeCoreMask = 0x7;
constexpr u64 FourCoreMask = 0xF;

inline auto GetSafeCoreMask() -> u64 {
    u64 process_mask{};
    if (R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        return SafeCoreMask;
    }

    const auto safe_mask = process_mask & SafeCoreMask;
    return safe_mask ? safe_mask : process_mask;
}

inline auto SetCurrentThreadSafeAffinity() -> Result {
    const auto mask = GetSafeCoreMask();
    R_UNLESS(mask, Result_CoreUnavailable);
    return svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, mask);
}

}
