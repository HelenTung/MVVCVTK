// 测试用途：读取 Windows 内存快照并保留余量；不改变 Feature SDK 的默认预算或公开契约。
#include "TestResources.h"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace Manual {
QJsonObject TestResources::GetJson() const
{
    return {{"workingBytes", QString::number(workingBytes)}, {"publishBytes", QString::number(publishBytes)},
        {"physicalBytes", QString::number(physicalBytes)}, {"availableBytes", QString::number(availableBytes)}, {"commitAvailableBytes", QString::number(commitAvailableBytes)}};
}
TestResources GetTestResources(std::uint64_t budgetMiB)
{
    constexpr std::uint64_t mib = 1024 * 1024;
    TestResources result;
#ifdef _WIN32
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.physicalBytes = memory.ullTotalPhys; result.availableBytes = memory.ullAvailPhys; result.commitAvailableBytes = memory.ullAvailPageFile;
        result.workingBytes = std::min({result.physicalBytes/2, result.availableBytes/4*3, result.commitAvailableBytes/4*3}) / mib * mib;
    }
#endif
    if (budgetMiB) {
        if (budgetMiB > result.workingBytes/mib) throw std::invalid_argument("指定预算超过当前可用内存的保守上限，请降低 --memory-budget-mib 或释放其它任务资源");
        result.workingBytes = budgetMiB*mib;
    }
    if (!result.workingBytes) throw std::runtime_error("当前没有可供算法使用的内存预算");
    result.publishBytes = result.workingBytes/2;
    return result;
}
}
