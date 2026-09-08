#pragma once

#include "Host/ArtifactReductionHostTypes.h"
#include "Data/DataPayloads.h"
#include "Host/RoiReadTypes.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace ArtifactReduction {

struct TaskControl final {
    std::atomic<bool> isCancelled{ false };
    std::atomic<unsigned int> progress{ 0 };
    std::chrono::steady_clock::time_point deadline;
    ArtifactError GetError() const noexcept;
};

struct AlgorithmInput final {
    std::shared_ptr<const ImageGrid3DPayload> image;
    RoiReadSnapshot processing;
    RoiReadSnapshot protection;
    RoiReadSnapshot material;
    std::size_t roiBytes = 0;
};

struct AlgorithmResult final {
    ArtifactError error = ArtifactError::None;
    std::shared_ptr<const ImageGrid3DPayload> image;
    std::shared_ptr<const RecordTablePayload> report;
    ArtifactQuality quality;
    std::size_t requiredBytes = 0;
    std::size_t publishBytes = 0;
    std::string parameters;
};

// 所有输入 bytes 都由已冻结 payload 持有；临时阶段 bytes 的 owner 是 worker。
class VolumeView final {
public:
    explicit VolumeView(const AlgorithmInput& input);
    double GetValue(std::size_t index) const noexcept;
    bool GetValid(std::size_t index) const noexcept;
    bool GetWritable(std::size_t index) const noexcept;
    bool GetMaterial(std::size_t index) const noexcept;
    bool GetProtected(std::size_t index) const noexcept;
    void SetValues(const std::vector<float>& values) noexcept;
    std::size_t GetIndex(int x, int y, int z) const noexcept;
    const GridGeometry3D& GetGeometry() const noexcept;
    std::size_t GetCount() const noexcept;
private:
    std::array<double, 3> GetPoint(std::size_t index) const noexcept;
    const AlgorithmInput& m_input;
    const std::vector<float>* m_values = nullptr;
};

ArtifactError GetInputError(const AlgorithmInput& input,
    const ArtifactRequest& request, const ArtifactConfig& config,
    std::size_t& requiredBytes, int* diffusionWorkerCount = nullptr) noexcept;
AlgorithmResult BuildArtifactCandidate(const AlgorithmInput& input,
    const ArtifactRequest& request, const ArtifactConfig& config,
    TaskControl& control) noexcept;

} // namespace ArtifactReduction
