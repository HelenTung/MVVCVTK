// 测试用途：共享真实 RAW 输入、几何和可追溯元信息的校验；不进入 SDK。
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
struct HostLoadRequest;
struct ImageDescriptor;
struct FeatureTestOptions final {
    // 0 由应用按当前可用内存决定；显式命令行预算始终优先。
    std::size_t budgetBytes = 0;
    std::uint32_t timeoutMs = 300000;
    std::string inputPath = "F:\\data\\ct\\1536x1536x1536_1440.raw";
    std::array<int, 3> dimensions{1536, 1536, 1536};
    bool hasDimensions = false;
    std::optional<std::array<float, 3>> spacing;
    std::optional<std::array<float, 3>> origin;
    std::optional<std::array<double, 9>> direction;
    std::string inputFrame;
    std::string inputUnit;
    std::string inputFormat;
    std::string datasetId;
    std::string inputDigest;
    double editRadius = 0.0; // 0 selects 1.5 times the largest voxel spacing.
    std::uint64_t islandVoxels = 2;
    int ringAxis = 2;
    int ringWidth = 1;
    double ringStrength = 0.5;
    int diffusionIterations = 1;
    std::optional<std::array<double, 2>> ringCenter;
};

FeatureTestOptions GetFeatureTestOptions(int argc, char* argv[]);
HostLoadRequest GetFeatureLoadRequest(const FeatureTestOptions& options, bool isAudit);
bool GetFeatureInputValid(const HostLoadRequest& request, const ImageDescriptor& descriptor);
