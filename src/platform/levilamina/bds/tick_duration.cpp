#include "platform/levilamina/bds/tick_duration.h"

#include <cmath>

#include "mc/profile/ProfilerLite.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace spark::levilamina::bds {

std::optional<double> readServerTickMilliseconds() noexcept
{
    try {
        const auto nanoseconds = ProfilerLite::gProfilerLiteInstance().mDebugServerTickTime.get().count();
        const double milliseconds = static_cast<double>(nanoseconds) / 1'000'000.0;
        if (nanoseconds < 0 || !std::isfinite(milliseconds)) {
            return std::nullopt;
        }
        return milliseconds;
    }
    catch (...) {
        return std::nullopt;
    }
}

std::uint64_t currentThreadId() noexcept
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return 0;
#endif
}

}  // namespace spark::levilamina::bds
