// 测试用途：为伪影校正回归提供共享断言、数据准备和测试套件声明。
#pragma once
#include "ArtifactReductionAlgorithm.h"
#include <cstring>
#include <stdexcept>
#include <string>

inline void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

inline GridGeometry3D CreateGrid(int x, int y, int z)
{
    GridGeometry3D grid;
    grid.dimensions = { x, y, z };
    grid.extent = { 0, x - 1, 0, y - 1, 0, z - 1 };
    return grid;
}

template<class T>
std::shared_ptr<const ImageGrid3DPayload> CreateImage(GridGeometry3D grid,
    ImageValueType type, const std::vector<T>& values, DataBytes mask = {}, ImageMetadata metadata = {})
{
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(values.size() * sizeof(T));
    std::memcpy(bytes->data(), values.data(), bytes->size());
    return std::make_shared<const ImageGrid3DPayload>(std::move(grid), type, 1, bytes, mask,
        std::array<double, 2>{ -1000, 1000 }, std::move(metadata));
}

inline std::shared_ptr<const BinaryMask3DPayload> CreateMask(const GridGeometry3D& grid, std::vector<std::uint8_t> values)
{
    return std::make_shared<const BinaryMask3DPayload>(grid,
        std::make_shared<const std::vector<std::uint8_t>>(std::move(values)));
}

inline ArtifactReduction::AlgorithmResult BuildCandidate(const ArtifactReduction::AlgorithmInput& input,
    const ArtifactRequest& request = {}, const ArtifactConfig& config = {})
{
    ArtifactReduction::TaskControl control;
    control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    return ArtifactReduction::BuildArtifactCandidate(input, request, config, control);
}

inline std::vector<float> GetValues(const ImageGrid3DPayload& image)
{
    Require(image.GetValueType() == ImageValueType::Float32, "output float32");
    std::vector<float> values(image.GetValues()->size() / sizeof(float));
    std::memcpy(values.data(), image.GetValues()->data(), image.GetValues()->size());
    return values;
}

void TestNative();
void TestAlgorithm();
void TestLifecycle();
