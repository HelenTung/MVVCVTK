// 测试用途：调度表面确定的算法、几何、显示与生命周期回归。
#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceDeterminationTestSupport.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
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
    // 对外部原始体按生产步长提取的 128^3 float32 样本运行同一算法，
    // 不在测试仓库保存 CT，也不为阈值审计分配整卷图像或构造测量网格。
    if (argc == 5 && std::string_view(argv[1]) == "--iso-samples") {
        try {
            std::vector<float> values(128U*128U*128U);
            std::ifstream input(argv[2], std::ios::binary);
            if (!input.read(reinterpret_cast<char*>(values.data()), values.size()*sizeof(float))
                || input.peek() != std::char_traits<char>::eof()) return 2;
            const auto source = SurfaceTest::BuildSnapshot({128,128,128}, {1,1,1}, {0,0,0},
                {1,0,0,0,1,0,0,0,1}, VTK_FLOAT, [&values](const SurfaceTest::Point3& p) {
                    return values[static_cast<std::size_t>(p[0])+128U*(static_cast<std::size_t>(p[1])+128U*static_cast<std::size_t>(p[2]))];
                });
            auto params = SurfaceTest::GetParams(SurfaceDeterminationMethod::AutomaticIso50); params.initialIsoValue.reset();
            const auto result = SurfaceDeterminationAlgorithm::BuildSurface(source, params, 64U*1024U, [] { return false; }, {});
            if (result.status != SurfaceResultStatus::Succeeded || !result.isoEstimate) {
                std::cerr << result.message << '\n'; return 1;
            }
            const auto& estimate = *result.isoEstimate;
            std::cout << std::setprecision(17) << "iso=" << estimate.isoValue << " background=" << estimate.backgroundValue
                << " material=" << estimate.materialValue << " samples=" << estimate.sampleCount
                << " excluded=" << estimate.excludedSampleCount << " workingBytes=" << result.requiredBytes << '\n';
            return estimate.isoValue >= std::stod(argv[3]) && estimate.isoValue <= std::stod(argv[4]) ? 0 : 1;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
    }
    const std::string_view suite = argc > 1 && argv[1]
        ? std::string_view{ argv[1] }
        : std::string_view{ "all" };
    const int failureCount = GetSuiteFailCount(suite);
    std::cout << "SurfaceDetermination suite=" << suite
        << " failures=" << failureCount << '\n';
    return failureCount == 0 ? 0 : 1;
}
