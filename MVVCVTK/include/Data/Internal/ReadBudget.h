#pragma once

#include <cstddef>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Only full value copies use the automatic budget. Chunks retain their 8 MiB cap.
inline std::size_t GetReadBudget(std::size_t requestedBytes) noexcept
{
    if (requestedBytes) return requestedBytes;
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status)
        ? static_cast<std::size_t>(status.ullAvailPhys) : 0;
#else
    return std::numeric_limits<std::size_t>::max();
#endif
}
