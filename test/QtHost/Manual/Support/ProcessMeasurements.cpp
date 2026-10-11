#include "ProcessMeasurements.h"
#include <QString>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
namespace Manual {
QJsonObject GetProcessMeasurements()
{
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        return {{"available", false}, {"errorCode", static_cast<double>(GetLastError())}};
    // OS 峰值从进程启动累计，不能作差当作某个业务阶段的峰值。
    return {{"available", true}, {"scope", "current-process-since-start"},
        {"workingSetBytes", QString::number(memory.WorkingSetSize)},
        {"privateBytes", QString::number(memory.PrivateUsage)},
        {"peakWorkingSetBytes", QString::number(memory.PeakWorkingSetSize)},
        {"peakCommitBytes", QString::number(memory.PeakPagefileUsage)}};
}
}
