#include "ArtifactReductionAlgorithm.h"
#include "ArtifactQualityEvaluator.h"
#include "TomoPyRingAdapter.h"
#include "VtkDiffusionAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <new>
#include <sstream>

namespace ArtifactReduction {
namespace {
template<class T>
double ReadScalar(const std::vector<std::uint8_t>& bytes, std::size_t index) noexcept
{
    T value{};
    std::memcpy(&value, bytes.data() + index * sizeof(T), sizeof(T));
    return static_cast<double>(value);
}

bool GetSameGrid(const GridGeometry3D& a, const GridGeometry3D& b) noexcept
{
    return a.extent == b.extent && a.dimensions == b.dimensions && a.spacing == b.spacing
        && a.origin == b.origin && a.direction == b.direction && a.coordinateFrame == b.coordinateFrame;
}

bool AddBytes(std::size_t& total, std::size_t count, std::size_t size) noexcept
{
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max() - total) / size) return false;
    total += count * size;
    return true;
}

std::string CreateParameters(const ArtifactRequest& request)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "{\"domain\":\"stored\",\"outputType\":\"float32\",\"order\":\"ring,diffusion\","
        "\"tomopyCommit\":\"3377fed68103959e37a849309a096a9919b0bc97\",\"patch\":\"mvvcvtk-1\","
        "\"vtkVersion\":\"9.4.2\",\"inputMode\":" << static_cast<unsigned int>(request.inputMode) << ",\"ring\":";
    if (request.ring) {
        const auto& r = *request.ring;
        stream << "{\"axis\":" << r.axis << ",\"centerIndex\":[" << r.centerIndex[0] << ',' << r.centerIndex[1]
            << "],\"threshMin\":" << r.threshMin << ",\"threshMax\":" << r.threshMax << ",\"threshold\":" << r.threshold
            << ",\"angularMin\":" << r.angularMin << ",\"ringWidth\":" << r.ringWidth << ",\"mode\":" << static_cast<int>(r.mode)
            << ",\"strength\":" << r.strength << ",\"maxCorrection\":" << r.maxCorrection << '}';
    }
    else stream << "null";
    stream << ",\"diffusion\":";
    if (request.diffusion) {
        const auto& d = *request.diffusion;
        stream << "{\"iterations\":" << d.iterations << ",\"threshold\":" << d.threshold << ",\"factor\":" << d.factor
            << ",\"slabDepth\":" << d.slabDepth << ",\"neighbors\":26,\"gradientMagnitude\":false}";
    }
    else stream << "null";
    stream << ",\"timeoutMs\":" << request.timeoutMs << '}';
    return stream.str();
}
} // namespace

ArtifactError TaskControl::GetError() const noexcept
{
    if (isCancelled.load(std::memory_order_relaxed)) return ArtifactError::Cancelled;
    return std::chrono::steady_clock::now() >= deadline ? ArtifactError::TimedOut : ArtifactError::None;
}

VolumeView::VolumeView(const AlgorithmInput& input) : m_input(input) {}

double VolumeView::GetValue(std::size_t index) const noexcept
{
    if (m_values) return (*m_values)[index];
    const auto& bytes = *m_input.image->GetValues();
    switch (m_input.image->GetValueType()) {
    case ImageValueType::UInt8: return ReadScalar<std::uint8_t>(bytes, index);
    case ImageValueType::UInt16: return ReadScalar<std::uint16_t>(bytes, index);
    case ImageValueType::Int16: return ReadScalar<std::int16_t>(bytes, index);
    case ImageValueType::Float32: return ReadScalar<float>(bytes, index);
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

bool VolumeView::GetValid(std::size_t index) const noexcept
{
    const auto& mask = m_input.image->GetValidityMask();
    if (mask && (*mask)[index] == 0) return false;
    const double value = GetValue(index);
    const auto& noData = m_input.image->GetMetadata().scalar.noData;
    return std::isfinite(value) && (!noData || value != *noData);
}

bool VolumeView::GetProtected(std::size_t index) const noexcept
{
    return m_input.protection && (*m_input.protection->GetValues())[index] != 0;
}

bool VolumeView::GetWritable(std::size_t index) const noexcept
{
    return (!m_input.processing || (*m_input.processing->GetValues())[index] != 0)
        && !GetProtected(index) && GetValid(index);
}

bool VolumeView::GetMaterial(std::size_t index) const noexcept
{
    return m_input.material && (*m_input.material->GetValues())[index] != 0;
}

void VolumeView::SetValues(const std::vector<float>& values) noexcept { m_values = &values; }

std::size_t VolumeView::GetIndex(int x, int y, int z) const noexcept
{
    const auto& dims = GetGeometry().dimensions;
    return (static_cast<std::size_t>(z) * dims[1] + y) * dims[0] + x;
}
const GridGeometry3D& VolumeView::GetGeometry() const noexcept { return m_input.image->GetGeometry(); }
std::size_t VolumeView::GetCount() const noexcept { return *GetGridVoxelCount(GetGeometry()); }

ArtifactError GetInputError(const AlgorithmInput& input, const ArtifactRequest& request,
    const ArtifactConfig& config, std::size_t& requiredBytes) noexcept
{
    requiredBytes = 0;
    if (!input.image || !input.image->GetValid() || input.image->GetComponentCount() != 1) return ArtifactError::InvalidData;
    const auto type = input.image->GetValueType();
    if (type != ImageValueType::UInt8 && type != ImageValueType::UInt16
        && type != ImageValueType::Int16 && type != ImageValueType::Float32) return ArtifactError::UnsupportedType;
    const auto& grid = input.image->GetGeometry();
    // 旋转与反射的正交网格可直接按spacing解释；不支持剪切或退化方向。
    for (int i = 0; i < 3; ++i) {
        if (grid.dimensions[i] > 16384 || grid.spacing[i] < 1e-12 || grid.spacing[i] > 1e12)
            return ArtifactError::UnsupportedGeometry;
        for (int j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k) dot += grid.direction[k * 3 + i] * grid.direction[k * 3 + j];
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.0 : 0.0)) > 1e-6) return ArtifactError::UnsupportedGeometry;
        }
    }
    for (const auto& mask : { input.processing, input.protection, input.material }) {
        if (mask && (!mask->GetValid() || !GetSameGrid(grid, mask->GetGeometry()))) return ArtifactError::InvalidData;
    }
    if (request.timeoutMs == 0 || request.timeoutMs > 3600000 || config.stopTimeoutMs > 60000
        || config.memoryBudgetBytes == 0 || config.publishBudgetBytes == 0
        || (request.inputMode != ArtifactInputMode::CurrentPrimary && request.inputMode != ArtifactInputMode::ExplicitRevision))
        return ArtifactError::InvalidRequest;
    std::size_t ringBytes = 0;
    if (request.ring) {
        RingLayout layout;
        const auto error = GetRingLayout(grid, *request.ring, layout);
        if (error != ArtifactError::None) return error;
        if (request.ring->strength > 0.0) ringBytes = layout.native.workspace_bytes;
    }
    std::size_t diffusionCount = 0;
    if (request.diffusion) {
        const auto& d = *request.diffusion;
        if (d.iterations < 1 || d.iterations > 16 || d.slabDepth < 1 || d.slabDepth > 1024
            || !std::isfinite(d.threshold) || d.threshold <= 0.0 || d.threshold > 1e30
            || !std::isfinite(d.factor) || d.factor < 0.0 || d.factor > 1.0) return ArtifactError::InvalidRequest;
        if (d.factor > 0.0) diffusionCount = static_cast<std::size_t>(grid.dimensions[0]) * grid.dimensions[1]
            * std::min(grid.dimensions[2], d.slabDepth + 2 * d.iterations);
    }
    const auto count = *GetGridVoxelCount(grid);
    const auto validityBytes = input.image->GetValidityMask() ? input.image->GetValidityMask()->size() : 0;
    if (!AddBytes(requiredBytes, input.image->GetValues()->size(), 1)
        || !AddBytes(requiredBytes, count, 12) || !AddBytes(requiredBytes, validityBytes, 3)
        || !AddBytes(requiredBytes, ringBytes, 1) || !AddBytes(requiredBytes, diffusionCount, 24)
        || !AddBytes(requiredBytes, 1, 1024 * 1024)) return ArtifactError::TooLarge;
    for (const auto& mask : { input.processing, input.protection, input.material }) {
        if (mask && !AddBytes(requiredBytes, mask->GetValues()->size(), 1)) return ArtifactError::TooLarge;
    }
    return requiredBytes > config.memoryBudgetBytes ? ArtifactError::TooLarge : ArtifactError::None;
}

AlgorithmResult BuildArtifactCandidate(const AlgorithmInput& input,
    const ArtifactRequest& request, const ArtifactConfig& config, TaskControl& control) noexcept
{
    AlgorithmResult result;
    try {
        result.error = GetInputError(input, request, config, result.requiredBytes);
        if (result.error != ArtifactError::None) return result;
        const VolumeView source(input);
        const bool hasProcessing = (request.ring && request.ring->strength > 0.0)
            || (request.diffusion && request.diffusion->factor > 0.0);
        std::vector<float> values(source.GetCount());
        // 1. 转换到独立float32缓冲；任何缺失估计上下文均拒绝，不做填零恢复。
        for (std::size_t i = 0; i < values.size(); ++i) {
            if ((i & 4095) == 0) {
                result.error = control.GetError();
                if (result.error != ArtifactError::None) return result;
            }
            if (hasProcessing && !source.GetValid(i)) { result.error = ArtifactError::UnsupportedValidity; return result; }
            if (input.image->GetValueType() == ImageValueType::Float32)
                std::memcpy(&values[i], input.image->GetValues()->data() + i * sizeof(float), sizeof(float));
            else values[i] = static_cast<float>(source.GetValue(i));
        }
        if (request.ring && request.ring->strength > 0.0) {
            result.error = BuildRingCorrection(source.GetGeometry(), *request.ring, values, control, result.quality);
            if (result.error != ArtifactError::None) return result;
        }
        if (request.diffusion && request.diffusion->factor > 0.0) {
            result.error = BuildDiffusion(source.GetGeometry(), *request.diffusion, values, control);
            if (result.error != ArtifactError::None) return result;
        }
        // 2. 两种算法均使用完整阶段上下文，最后才限制写回；新无效值回退并报告。
        const auto& noData = input.image->GetMetadata().scalar.noData;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if ((i & 4095) == 0) {
                result.error = control.GetError();
                if (result.error != ArtifactError::None) return result;
            }
            const bool isCollision = source.GetValid(i)
                && (!std::isfinite(values[i]) || (noData && values[i] == *noData));
            if (!source.GetWritable(i) || isCollision) {
                if (isCollision) ++result.quality.guardedCount;
                if (input.image->GetValueType() == ImageValueType::Float32)
                    std::memcpy(&values[i], input.image->GetValues()->data() + i * sizeof(float), sizeof(float));
                else values[i] = static_cast<float>(source.GetValue(i));
            }
        }
        std::array<double, 2> range{};
        result.error = BuildQuality(input, values, control, result.quality, range);
        if (result.error != ArtifactError::None) return result;
        result.report = CreateQualityReport(result.quality);
        result.parameters = CreateParameters(request);
        auto metadata = input.image->GetMetadata();
        metadata.source = { ImageSourceKind::Memory, {}, values.size() * sizeof(float), {} };
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(values.size() * sizeof(float));
        std::memcpy(bytes->data(), values.data(), bytes->size());
        result.publishBytes = bytes->size();
        if (!AddBytes(result.publishBytes, input.image->GetValidityMask() ? input.image->GetValidityMask()->size() : 0, 1)
            || !AddBytes(result.publishBytes, 1, 64 * 1024)) { result.error = ArtifactError::TooLarge; return result; }
        result.error = control.GetError();
        if (result.error != ArtifactError::None) return result;
        // 3. payload首次冻结复制仅在worker；之后Store共享该不可变缓冲。
        result.image = std::make_shared<const ImageGrid3DPayload>(source.GetGeometry(), ImageValueType::Float32,
            1, bytes, input.image->GetValidityMask(), range, std::move(metadata));
        result.error = control.GetError();
        if (result.error != ArtifactError::None) { result.image.reset(); result.report.reset(); }
        else control.progress.store(100, std::memory_order_relaxed);
    }
    catch (const std::bad_alloc&) { result.error = ArtifactError::TooLarge; }
    catch (...) { result.error = ArtifactError::KernelFailed; }
    return result;
}
} // namespace ArtifactReduction
