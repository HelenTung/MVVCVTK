// 测试用途：调度表面确定的算法、几何、显示与生命周期回归。
#include "SurfaceDeterminationTestCases.h"

#include <iostream>
#include <string_view>

namespace {

int GetSuiteFailCount(const std::string_view suite)
{
    if (suite == "algorithm")
        return GetSurfaceBusinessFailCount() + GetSurfaceAlgorithmFailCount() + GetSurfaceContractFailCount();
    if (suite == "geometry") return GetSurfaceGeometryFailCount();
    if (suite == "lifecycle") return GetSurfaceLifecycleFailCount();
    if (suite == "display") return GetSurfaceDisplayFailCount();
    if (suite == "all") {
        return GetSurfaceBusinessFailCount() + GetSurfaceContractFailCount() +
               GetSurfaceAlgorithmFailCount() + GetSurfaceGeometryFailCount() +
               GetSurfaceLifecycleFailCount() + GetSurfaceDisplayFailCount();
    }
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string_view suite = argc > 1 && argv[1]
        ? std::string_view{ argv[1] }
        : std::string_view{ "all" };
    const int failureCount = GetSuiteFailCount(suite);
    std::cout << "SurfaceDetermination suite=" << suite
        << " failures=" << failureCount << '\n';
    return failureCount == 0 ? 0 : 1;
}
