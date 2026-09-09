#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceContracts.h"
#include "SurfaceProfileSolver.h"
#include "SurfaceSeedBuilder.h"
#include <vtkSMPThreadLocal.h>
#include <chrono>
#include "Data/DataPayloads.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkSMPTools.h>
#include <vtkType.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Point3 = std::array<double, 3>;

constexpr std::uint32_t algorithmRevision = surfaceAlgorithmRevision;
constexpr std::size_t histogramBinCount = 512;
constexpr double geometryEpsilon = 1.0e-12;
constexpr double qualityRatioThreshold = 0.5;
constexpr std::size_t maxProfileSampleCount = 4097;

using Triangle = SurfaceSeedTriangle;
struct SurfaceCancelled final
{
};
void CheckCancellation(std::size_t &count, const SurfaceCancelCheck &cancelled)
{
    if ((count++ & 1023U) == 0 && cancelled && cancelled())
        throw SurfaceCancelled{};
}

struct ImageGeometry final {
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> spacing{};
    std::array<double, 3> origin{};
    std::array<double, 9> direction{};
    std::array<double, 9> indexToModel{};
    std::array<double, 9> modelToIndex{};
    double voxelVolume = 0.0;
};

template <class T> double ReadScalar(const void *values, const std::size_t index)
{
    return static_cast<double>(static_cast<const T *>(values)[index]);
}

struct ScalarView final {
    const void* values = nullptr;
    std::size_t valueCount = 0;
    int vtkType = VTK_VOID;
    double (*read)(const void *, std::size_t) = nullptr;
    double GetValue(std::size_t index) const noexcept
    {
        return read(values, index);
    }
    void SetReader()
    {
        switch (vtkType)
        {
        case VTK_CHAR:
            read = ReadScalar<char>;
            break;
        case VTK_SIGNED_CHAR:
            read = ReadScalar<signed char>;
            break;
        case VTK_UNSIGNED_CHAR:
            read = ReadScalar<unsigned char>;
            break;
        case VTK_SHORT:
            read = ReadScalar<short>;
            break;
        case VTK_UNSIGNED_SHORT:
            read = ReadScalar<unsigned short>;
            break;
        case VTK_INT:
            read = ReadScalar<int>;
            break;
        case VTK_UNSIGNED_INT:
            read = ReadScalar<unsigned int>;
            break;
        case VTK_LONG:
            read = ReadScalar<long>;
            break;
        case VTK_UNSIGNED_LONG:
            read = ReadScalar<unsigned long>;
            break;
        case VTK_LONG_LONG:
            read = ReadScalar<long long>;
            break;
        case VTK_UNSIGNED_LONG_LONG:
            read = ReadScalar<unsigned long long>;
            break;
        case VTK_FLOAT:
            read = ReadScalar<float>;
            break;
        case VTK_DOUBLE:
            read = ReadScalar<double>;
            break;
        }
    }
};

struct VolumeView final {
    const LabelMap3DPayload *labels = nullptr;
    const SurfaceMeshPayload *initialMesh = nullptr;
    ImageGeometry geometry;
    ScalarView scalars;
    const unsigned char* validity = nullptr;
    std::size_t voxelCount = 0;
};

struct ResolvedParams final : SurfaceLocalParams
{
    SurfaceComponentSelection componentSelection = SurfaceComponentSelection::Largest;
    bool isAutomaticIso = false;
    std::optional<SurfaceIsoEstimate> isoEstimate;
    std::optional<Point3> seedModelPoint;
    RoiReadSnapshot roi;
    std::vector<RoiPlane> clipPlanes;
    std::uint64_t minimumObjectVoxels = 1;
    double sharpCornerAngleDeg = 75.0;
    std::vector<SurfaceRegionOverride> regionOverrides;
};

enum class SampleStatus : std::uint8_t {
    Valid,
    Clipped,
    InvalidSupport
};

struct ScalarSample final {
    SampleStatus status = SampleStatus::Clipped;
    double value = 0.0;
};

struct MeshComponent final {
    std::vector<std::uint32_t> sourcePointIds;
    std::vector<Triangle> triangles;
    std::uint32_t minimumPointId = 0;
    std::uint64_t estimatedVoxelCount = 0;
    bool isClosed = false;
};

struct TopologyMetrics final {
    double area = 0.0;
    double signedVolume = 0.0;
    std::uint64_t boundaryEdgeCount = 0;
    std::uint64_t nonManifoldEdgeCount = 0;
    std::uint64_t degenerateTriangleCount = 0;
    bool isOrientationValid = true;
};

bool GetCancelled(const SurfaceCancelCheck& getCancelled)
{
    return getCancelled && getCancelled();
}

void SendProgress(
    const SurfaceProgressCallback& onProgress,
    const SurfaceDeterminationStage stage,
    const double progress)
{
    if (onProgress) onProgress(stage, std::clamp(progress, 0.0, 1.0));
}

bool GetProduct(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product)
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

bool GetSum(
    const std::size_t left,
    const std::size_t right,
    std::size_t& sum)
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

Point3 Add(const Point3& left, const Point3& right)
{
    return {
        left[0] + right[0],
        left[1] + right[1],
        left[2] + right[2]
    };
}

Point3 Subtract(const Point3& left, const Point3& right)
{
    return {
        left[0] - right[0],
        left[1] - right[1],
        left[2] - right[2]
    };
}

Point3 Scale(const Point3& point, const double scale)
{
    return { point[0] * scale, point[1] * scale, point[2] * scale };
}

double Dot(const Point3& left, const Point3& right)
{
    return left[0] * right[0]
        + left[1] * right[1]
        + left[2] * right[2];
}

Point3 Cross(const Point3& left, const Point3& right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]
    };
}

double GetLength(const Point3& point)
{
    return std::sqrt(std::max(0.0, Dot(point, point)));
}

bool Normalize(Point3& point)
{
    const double length = GetLength(point);
    if (!std::isfinite(length) || length <= geometryEpsilon) return false;
    point = Scale(point, 1.0 / length);
    return true;
}

Point3 Multiply(
    const std::array<double, 9>& matrix,
    const Point3& point)
{
    return {
        matrix[0] * point[0] + matrix[1] * point[1]
            + matrix[2] * point[2],
        matrix[3] * point[0] + matrix[4] * point[1]
            + matrix[5] * point[2],
        matrix[6] * point[0] + matrix[7] * point[1]
            + matrix[8] * point[2]
    };
}

bool GetInverse(
    const std::array<double, 9>& matrix,
    std::array<double, 9>& inverse,
    double& determinant)
{
    determinant = matrix[0] * (matrix[4] * matrix[8]
        - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8]
            - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7]
            - matrix[4] * matrix[6]);
    if (!std::isfinite(determinant)
        || std::abs(determinant) <= geometryEpsilon) {
        return false;
    }
    const double scale = 1.0 / determinant;
    inverse = {
        (matrix[4] * matrix[8] - matrix[5] * matrix[7]) * scale,
        (matrix[2] * matrix[7] - matrix[1] * matrix[8]) * scale,
        (matrix[1] * matrix[5] - matrix[2] * matrix[4]) * scale,
        (matrix[5] * matrix[6] - matrix[3] * matrix[8]) * scale,
        (matrix[0] * matrix[8] - matrix[2] * matrix[6]) * scale,
        (matrix[2] * matrix[3] - matrix[0] * matrix[5]) * scale,
        (matrix[3] * matrix[7] - matrix[4] * matrix[6]) * scale,
        (matrix[1] * matrix[6] - matrix[0] * matrix[7]) * scale,
        (matrix[0] * matrix[4] - matrix[1] * matrix[3]) * scale
    };
    return true;
}

Point3 GetModelPoint(
    const ImageGeometry& geometry,
    const Point3& continuousIndex)
{
    return Add(
        geometry.origin,
        Multiply(geometry.indexToModel, continuousIndex));
}

Point3 GetContinuousIndex(
    const ImageGeometry& geometry,
    const Point3& modelPoint)
{
    return Multiply(
        geometry.modelToIndex,
        Subtract(modelPoint, geometry.origin));
}

bool GetScalarSupported(const int vtkType)
{
    switch (vtkType) {
    case VTK_CHAR:
    case VTK_SIGNED_CHAR:
    case VTK_UNSIGNED_CHAR:
    case VTK_SHORT:
    case VTK_UNSIGNED_SHORT:
    case VTK_INT:
    case VTK_UNSIGNED_INT:
    case VTK_LONG:
    case VTK_UNSIGNED_LONG:
    case VTK_LONG_LONG:
    case VTK_UNSIGNED_LONG_LONG:
    case VTK_FLOAT:
    case VTK_DOUBLE:
        return true;
    default:
        return false;
    }
}

bool GetSameGeometry(
    vtkImageData& image,
    const ImageGeometry& geometry)
{
    int extent[6]{};
    double spacing[3]{};
    double origin[3]{};
    image.GetExtent(extent);
    image.GetSpacing(spacing);
    image.GetOrigin(origin);
    for (std::size_t index = 0; index < geometry.extent.size(); ++index) {
        if (extent[index] != geometry.extent[index]) return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (spacing[axis] != geometry.spacing[axis]
            || origin[axis] != geometry.origin[axis]) {
            return false;
        }
    }
    const auto* direction = image.GetDirectionMatrix();
    if (!direction) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            if (direction->GetElement(
                    static_cast<int>(row),
                    static_cast<int>(column))
                != geometry.direction[row * 3 + column]) {
                return false;
            }
        }
    }
    return true;
}

SurfaceFailureReason BuildVolumeView(
    const VtkImageGridSnapshot& source,
    VolumeView& volume,
    std::string& message)
{
    if (!source || !source->image || !source->data
        || !GetDataRevisionRefValid(source->data->self)) {
        message = "Surface source is unavailable.";
        return SurfaceFailureReason::InvalidSource;
    }
    auto* image = source->image.GetPointer();
    const auto* payload = dynamic_cast<const ImageGrid3DPayload*>(
        source->data->payload.get());
    if (!payload) return SurfaceFailureReason::InvalidSource;
    const auto& sourceGeometry = payload->GetGeometry();
    int dimensions[3]{};
    int extent[6]{};
    double spacing[3]{};
    double origin[3]{};
    image->GetDimensions(dimensions);
    image->GetExtent(extent);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);
    std::size_t voxelCount = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::int64_t extentSize =
            static_cast<std::int64_t>(extent[axis * 2 + 1])
            - static_cast<std::int64_t>(extent[axis * 2]) + 1;
        if (dimensions[axis] < 2 || extentSize != dimensions[axis] ||
            sourceGeometry.dimensions[axis] != dimensions[axis] ||
            sourceGeometry.extent[axis * 2] != extent[axis * 2] ||
            sourceGeometry.extent[axis * 2 + 1] != extent[axis * 2 + 1] || !std::isfinite(spacing[axis]) ||
            spacing[axis] <= 0.0 || !std::isfinite(origin[axis]) ||
            spacing[axis] != sourceGeometry.spacing[axis] || origin[axis] != sourceGeometry.origin[axis] ||
            !GetProduct(voxelCount, static_cast<std::size_t>(dimensions[axis]), voxelCount))
        {
            message = "Surface source geometry is invalid.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        volume.geometry.dimensions[axis] = dimensions[axis];
        volume.geometry.spacing[axis] = spacing[axis];
        volume.geometry.origin[axis] = origin[axis];
        volume.geometry.extent[axis * 2] = extent[axis * 2];
        volume.geometry.extent[axis * 2 + 1] = extent[axis * 2 + 1];
    }

    const auto* direction = image->GetDirectionMatrix();
    if (!direction) {
        message = "Surface source direction is unavailable.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const double value = direction->GetElement(
                static_cast<int>(row), static_cast<int>(column));
            if (!std::isfinite(value) || value != sourceGeometry.direction[row * 3 + column])
            {
                message = "Surface source direction is invalid.";
                return SurfaceFailureReason::InvalidGeometry;
            }
            volume.geometry.direction[row * 3 + column] = value;
            volume.geometry.indexToModel[row * 3 + column] =
                value * spacing[column];
        }
    }
    double determinant = 0.0;
    if (!GetInverse(
            volume.geometry.indexToModel,
            volume.geometry.modelToIndex,
            determinant)) {
        message = "Surface source direction is not invertible.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    volume.geometry.voxelVolume = std::abs(determinant);

    auto* scalars = image->GetPointData()
        ? image->GetPointData()->GetScalars() : nullptr;
    if (!scalars
        || scalars->GetNumberOfComponents() != 1
        || !scalars->HasStandardMemoryLayout()
        || !GetScalarSupported(scalars->GetDataType())) {
        message = "Surface source requires one supported contiguous scalar.";
        return SurfaceFailureReason::UnsupportedScalar;
    }
    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    if (tupleCount <= 0
        || static_cast<unsigned long long>(tupleCount)
            > std::numeric_limits<std::size_t>::max()
        || static_cast<std::size_t>(tupleCount) != voxelCount) {
        message = "Surface source tuple count is inconsistent.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    const void* values = scalars->GetVoidPointer(0);
    if (!values) {
        message = "Surface source scalar buffer is unavailable.";
        return SurfaceFailureReason::UnsupportedScalar;
    }
    volume.scalars = { values, voxelCount, scalars->GetDataType() };
    volume.scalars.SetReader();
    volume.voxelCount = voxelCount;

    if (source->validityMask) {
        auto* mask = source->validityMask.GetPointer();
        auto* maskScalars = mask && mask->GetPointData()
            ? mask->GetPointData()->GetScalars() : nullptr;
        if (!mask || !GetSameGeometry(*mask, volume.geometry)
            || (maskScalars
                && maskScalars->GetNumberOfTuples() != tupleCount)) {
            message = "Surface validity mask is inconsistent.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        if (!maskScalars
            || maskScalars->GetDataType() != VTK_UNSIGNED_CHAR
            || maskScalars->GetNumberOfComponents() != 1
            || !maskScalars->HasStandardMemoryLayout()) {
            message = "Surface validity mask requires one contiguous uint8 scalar.";
            return SurfaceFailureReason::UnsupportedScalar;
        }
        volume.validity = static_cast<const unsigned char*>(
            maskScalars->GetVoidPointer(0));
        if (!volume.validity) {
            message = "Surface validity mask buffer is unavailable.";
            return SurfaceFailureReason::InvalidGeometry;
        }
    }
    return SurfaceFailureReason::None;
}

std::array<double, 6> GetDataBounds(const ImageGeometry& geometry)
{
    std::array<double, 6> bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    for (int corner = 0; corner < 8; ++corner) {
        Point3 index{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            index[axis] = static_cast<double>(
                geometry.extent[axis * 2 + ((corner >> axis) & 1)]);
        }
        const Point3 point = GetModelPoint(geometry, index);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            bounds[axis * 2] = std::min(bounds[axis * 2], point[axis]);
            bounds[axis * 2 + 1] =
                std::max(bounds[axis * 2 + 1], point[axis]);
        }
    }
    return bounds;
}

bool GetBoundsIntersect(
    const std::array<double, 6>& left,
    const std::array<double, 6>& right)
{
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (left[axis * 2 + 1] < right[axis * 2]
            || right[axis * 2 + 1] < left[axis * 2]) {
            return false;
        }
    }
    return true;
}

std::size_t GetTupleIndex(
    const ImageGeometry& geometry,
    const int x,
    const int y,
    const int z)
{
    const std::size_t localX = static_cast<std::size_t>(
        x - geometry.extent[0]);
    const std::size_t localY = static_cast<std::size_t>(
        y - geometry.extent[2]);
    const std::size_t localZ = static_cast<std::size_t>(
        z - geometry.extent[4]);
    return (localZ * static_cast<std::size_t>(geometry.dimensions[1])
        + localY) * static_cast<std::size_t>(geometry.dimensions[0])
        + localX;
}

ScalarSample GetScalarAtIndex(
    const VolumeView& volume,
    const Point3& continuousIndex)
{
    std::array<int, 3> lower{};
    Point3 fraction{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double minimum = static_cast<double>(
            volume.geometry.extent[axis * 2]);
        const double maximum = static_cast<double>(
            volume.geometry.extent[axis * 2 + 1]);
        const double value = continuousIndex[axis];
        const double tolerance = 32.0
            * std::numeric_limits<double>::epsilon()
            * std::max({ 1.0, std::abs(minimum), std::abs(maximum) });
        if (!std::isfinite(value)
            || value < minimum - tolerance
            || value > maximum + tolerance) {
            return { SampleStatus::Clipped, 0.0 };
        }
        const double bounded = std::clamp(value, minimum, maximum);
        int base = static_cast<int>(std::floor(bounded));
        if (base >= volume.geometry.extent[axis * 2 + 1]) {
            base = volume.geometry.extent[axis * 2 + 1] - 1;
        }
        if (base < volume.geometry.extent[axis * 2]) {
            base = volume.geometry.extent[axis * 2];
        }
        lower[axis] = base;
        fraction[axis] = std::clamp(
            bounded - static_cast<double>(base), 0.0, 1.0);
    }

    double value = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
        const int x = lower[0] + (corner & 1);
        const int y = lower[1] + ((corner >> 1) & 1);
        const int z = lower[2] + ((corner >> 2) & 1);
        const std::size_t tupleIndex = GetTupleIndex(
            volume.geometry, x, y, z);
        if (volume.validity && volume.validity[tupleIndex] == 0) {
            return { SampleStatus::InvalidSupport, 0.0 };
        }
        const double xWeight = (corner & 1)
            ? fraction[0] : 1.0 - fraction[0];
        const double yWeight = (corner & 2)
            ? fraction[1] : 1.0 - fraction[1];
        const double zWeight = (corner & 4)
            ? fraction[2] : 1.0 - fraction[2];
        value += xWeight * yWeight * zWeight
            * volume.scalars.GetValue(tupleIndex);
    }
    return { SampleStatus::Valid, value };
}

ScalarSample GetScalarAtModel(
    const VolumeView& volume,
    const Point3& modelPoint)
{
    return GetScalarAtIndex(
        volume,
        GetContinuousIndex(volume.geometry, modelPoint));
}

bool GetGradient(
    const VolumeView& volume,
    const Point3& modelPoint,
    Point3& gradientModel,
    double& magnitude)
{
    const Point3 center = GetContinuousIndex(
        volume.geometry, modelPoint);
    Point3 gradientIndex{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        Point3 negative = center;
        Point3 positive = center;
        negative[axis] -= 0.5;
        positive[axis] += 0.5;
        const ScalarSample before = GetScalarAtIndex(volume, negative);
        const ScalarSample after = GetScalarAtIndex(volume, positive);
        if (before.status != SampleStatus::Valid
            || after.status != SampleStatus::Valid) {
            return false;
        }
        gradientIndex[axis] = after.value - before.value;
    }
    gradientModel = {
        volume.geometry.modelToIndex[0] * gradientIndex[0]
            + volume.geometry.modelToIndex[3] * gradientIndex[1]
            + volume.geometry.modelToIndex[6] * gradientIndex[2],
        volume.geometry.modelToIndex[1] * gradientIndex[0]
            + volume.geometry.modelToIndex[4] * gradientIndex[1]
            + volume.geometry.modelToIndex[7] * gradientIndex[2],
        volume.geometry.modelToIndex[2] * gradientIndex[0]
            + volume.geometry.modelToIndex[5] * gradientIndex[1]
            + volume.geometry.modelToIndex[8] * gradientIndex[2]
    };
    magnitude = GetLength(gradientModel);
    return std::isfinite(magnitude) && magnitude > geometryEpsilon;
}

std::uint64_t GetLabelAtModel(const VolumeView &volume, const Point3 &point)
{
    if (!volume.labels)
        return UINT64_MAX;
    const auto index = GetContinuousIndex(volume.geometry, point);
    std::array<int, 3> nearest{};
    for (unsigned a = 0; a < 3; ++a)
    {
        if (!std::isfinite(index[a]) || index[a] < volume.geometry.extent[a * 2] ||
            index[a] > volume.geometry.extent[a * 2 + 1])
            return UINT64_MAX;
        nearest[a] = static_cast<int>(std::floor(index[a] + 0.5));
    }
    return SurfaceSeedBuilder::GetLabel(*volume.labels,
                                        GetTupleIndex(volume.geometry, nearest[0], nearest[1], nearest[2]));
}

void BuildProfile(const VolumeView &volume, const Point3 &center, const Point3 &normal,
                  const SurfaceLocalParams &params, SurfaceProfileWorkspace &p)
{
    auto intervals =
        std::max<std::size_t>(8, static_cast<std::size_t>(std::ceil(2 * params.profileHalfLengthModel /
                                                                    params.profileSampleStepModel)));
    if (intervals % 2)
        ++intervals;
    p.Reserve(intervals + 1);
    p.offsets.clear();
    p.raw.clear();
    p.support.clear();
    p.labels.clear();
    p.step = 2 * params.profileHalfLengthModel / intervals;
    std::size_t valid = 0;
    for (std::size_t i = 0; i <= intervals; ++i)
    {
        const double offset = -params.profileHalfLengthModel + i * p.step;
        const auto point = Add(center, Scale(normal, offset));
        const auto sample = GetScalarAtModel(volume, point);
        auto support = sample.status == SampleStatus::Valid
                           ? SurfaceSampleStatus::Valid
                           : (sample.status == SampleStatus::Clipped ? SurfaceSampleStatus::Clipped
                                                                     : SurfaceSampleStatus::InvalidSupport);
        if (volume.labels && params.materials)
        {
            const auto label = GetLabelAtModel(volume, point);
            p.labels.push_back(static_cast<std::uint32_t>(label));
            if (support == SurfaceSampleStatus::Valid && label != params.materials->materialA &&
                label != params.materials->materialB)
                support = label == UINT64_MAX ? SurfaceSampleStatus::InvalidSupport
                                              : SurfaceSampleStatus::OtherMaterial;
        }
        p.offsets.push_back(offset);
        p.raw.push_back(sample.value);
        p.support.push_back(support);
        valid += support == SurfaceSampleStatus::Valid;
    }
    p.validRatio = double(valid) / (intervals + 1);
    ++p.sampledProfileCount;
    p.sampledValueCount += intervals + 1;
}

SurfaceLocalParams GetLocalParams(const ResolvedParams &params, const Point3 &point, std::uint32_t &ruleIndex)
{
    SurfaceLocalParams local = params;
    ruleIndex = 0;
    const SurfaceRegionOverride *selected = nullptr;
    for (std::size_t i = 0; i < params.regionOverrides.size(); ++i)
    {
        const auto &rule = params.regionOverrides[i];
        bool inside = true;
        for (unsigned a = 0; a < 3; ++a)
            inside = inside && point[a] >= rule.boundsModel[a * 2] && point[a] < rule.boundsModel[a * 2 + 1];
        if (inside && (!selected || rule.priority > selected->priority))
        {
            selected = &rule;
            ruleIndex = static_cast<std::uint32_t>(i + 1);
        }
    }
    if (selected)
    {
        const auto &r = *selected;
        if (r.method)
            local.method = *r.method;
        if (r.localFraction)
            local.localFraction = *r.localFraction;
        if (r.profileHalfLengthModel)
            local.profileHalfLengthModel = *r.profileHalfLengthModel;
        if (r.profileSampleStepModel)
            local.profileSampleStepModel = *r.profileSampleStepModel;
        if (r.maximumOffsetModel)
            local.maximumOffsetModel = *r.maximumOffsetModel;
        if (r.profileSmoothingSigmaModel)
            local.profileSmoothingSigmaModel = *r.profileSmoothingSigmaModel;
        if (r.minimumContrast)
            local.minimumContrast = *r.minimumContrast;
        if (r.minimumCnr)
            local.minimumCnr = *r.minimumCnr;
    }
    return local;
}

bool GetVoxelUsed(
    const VolumeView& volume,
    const ResolvedParams& params,
    const int x,
    const int y,
    const int z,
    const std::size_t tupleIndex)
{
    if (volume.validity && volume.validity[tupleIndex] == 0) return false;
    if (!params.roi) return true;
    const Point3 point = GetModelPoint(
        volume.geometry,
        Point3{
            static_cast<double>(x),
            static_cast<double>(y),
            static_cast<double>(z)
        });
    return params.roi->GetContains(point);
}

SurfaceFailureReason GetAutomaticIso(
    const VolumeView& volume,
    const ResolvedParams& params,
    const SurfaceCancelCheck& getCancelled,
    double& isoValue,
    std::string& message,
    std::optional<SurfaceIsoEstimate>& estimate)
{
    double minimum = std::numeric_limits<double>::max(), maximum = std::numeric_limits<double>::lowest();
    std::uint64_t validCount = 0, excludedCount = 0;
    const auto &extent = volume.geometry.extent;
    std::array<std::int64_t, 3> sampleSteps{};
    for (unsigned a = 0; a < 3; ++a)
    {
        sampleSteps[a] = (static_cast<std::int64_t>(volume.geometry.dimensions[a]) + 127) / 128;
    }
    // 范围和直方图使用同一采样集合。独立的 16^3 预采样会避开薄壁材料，
    // 随后把真正的材料峰作为越界值丢弃，并把背景附近的次峰误认作材料。
    // 只保留每侧 17 个极值，以固定空间排除最多 16 个孤立极端样本。
    constexpr std::size_t tailCapacity = 17;
    std::array<double, tailCapacity> lowest{}, highest{};
    lowest.fill(std::numeric_limits<double>::max());
    highest.fill(std::numeric_limits<double>::lowest());
    for (std::int64_t z = extent[4] + sampleSteps[2] / 2; z <= extent[5]; z += sampleSteps[2])
        for (std::int64_t y = extent[2] + sampleSteps[1] / 2; y <= extent[3]; y += sampleSteps[1])
        {
            if (GetCancelled(getCancelled))
                return SurfaceFailureReason::Cancelled;
            for (std::int64_t x = extent[0] + sampleSteps[0] / 2; x <= extent[1]; x += sampleSteps[0])
            {
                const auto id = GetTupleIndex(volume.geometry, static_cast<int>(x), static_cast<int>(y),
                                              static_cast<int>(z));
                const auto value = volume.scalars.GetValue(id);
                if (GetVoxelUsed(volume, params, static_cast<int>(x), static_cast<int>(y),
                                 static_cast<int>(z), id) &&
                    std::isfinite(value)) {
                    ++validCount;
                    if (value < lowest.back()) {
                        const auto at = std::lower_bound(lowest.begin(), lowest.end(), value);
                        std::move_backward(at, lowest.end()-1, lowest.end()); *at = value;
                    }
                    if (value > highest.back()) {
                        const auto at = std::lower_bound(highest.begin(), highest.end(), value, std::greater<double>{});
                        std::move_backward(at, highest.end()-1, highest.end()); *at = value;
                    }
                }
            }
        }
    if (validCount >= 64)
    {
        const auto trim = std::min<std::uint64_t>(tailCapacity-1, validCount / 1000);
        minimum = lowest[trim];
        maximum = highest[trim];
    }
    if (validCount < 64 || !std::isfinite(minimum)
        || !std::isfinite(maximum)
        || maximum - minimum <= geometryEpsilon
            * std::max({ 1.0, std::abs(minimum), std::abs(maximum) })) {
        message = "Surface automatic ISO50 requires a non-degenerate bimodal histogram.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    validCount = 0;
    std::array<std::uint64_t, histogramBinCount> histogram{};
    const double scale = static_cast<double>(histogramBinCount - 1)
        / (maximum - minimum);
    for (std::int64_t zValue = extent[4] + sampleSteps[2] / 2;
        zValue <= static_cast<std::int64_t>(extent[5]); zValue += sampleSteps[2]) {
        const int z = static_cast<int>(zValue);
        if (GetCancelled(getCancelled)) {
            message = "Surface threshold estimation was cancelled.";
            return SurfaceFailureReason::Cancelled;
        }
        for (std::int64_t yValue = extent[2] + sampleSteps[1] / 2;
            yValue <= static_cast<std::int64_t>(extent[3]); yValue += sampleSteps[1]) {
            const int y = static_cast<int>(yValue);
            for (std::int64_t xValue = extent[0] + sampleSteps[0] / 2;
                xValue <= static_cast<std::int64_t>(extent[1]); xValue += sampleSteps[0]) {
                const int x = static_cast<int>(xValue);
                const std::size_t tupleIndex = GetTupleIndex(
                    volume.geometry, x, y, z);
                if (!GetVoxelUsed(
                        volume, params, x, y, z, tupleIndex)) {
                    continue;
                }
                const double value = volume.scalars.GetValue(tupleIndex);
                if (!std::isfinite(value) || value < minimum || value > maximum)
                {
                    ++excludedCount;
                    continue;
                }
                ++validCount;
                const auto bin = static_cast<std::size_t>(std::clamp(
                    std::llround((value - minimum) * scale),
                    0LL,
                    static_cast<long long>(histogramBinCount - 1)));
                ++histogram[bin];
            }
        }
    }

    std::array<double, histogramBinCount> smooth{};
    for (std::size_t index = 0; index < histogramBinCount; ++index) {
        double sum = 0.0;
        double weight = 0.0;
        const std::size_t begin = index > 2 ? index - 2 : 0;
        const std::size_t end = std::min(
            histogramBinCount - 1, index + 2);
        for (std::size_t sample = begin; sample <= end; ++sample) {
            const double localWeight = sample == index ? 3.0
                : (sample + 1 == index || sample == index + 1 ? 2.0 : 1.0);
            sum += localWeight * static_cast<double>(histogram[sample]);
            weight += localWeight;
        }
        smooth[index] = sum / weight;
    }

    struct Peak final {
        std::size_t index = 0;
        double height = 0.0;
    };
    std::vector<Peak> peaks;
    peaks.reserve(histogramBinCount);
    for (std::size_t index = 0; index < histogramBinCount; ++index) {
        const double left = index == 0 ? -1.0 : smooth[index - 1];
        const double right = index + 1 == histogramBinCount
            ? -1.0 : smooth[index + 1];
        if (smooth[index] >= left && smooth[index] >= right
            && smooth[index] > 0.0) {
            peaks.push_back({ index, smooth[index] });
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& left, const Peak& right) {
        if (left.height != right.height) return left.height > right.height;
        return left.index < right.index;
    });

    std::optional<std::pair<Peak, Peak>> selected;
    double selectedScore = -1.0;
    const std::size_t minimumSeparation = histogramBinCount / 10;
    const double minimumPeakHeight = std::max(
        2.0, static_cast<double>(validCount) * 1.0e-5);
    if (params.method == SurfaceDeterminationMethod::AutomaticIso50 && !peaks.empty()) {
        // 按谷分群；近背景起伏和同一材料上的小尖峰合并，
        // 非空气材料按原始直方图积分样本数比较，而不是按峰高或离背景的距离比较。
        std::vector<Peak> groups;
        for (const auto& peak : peaks) {
            if (peak.height < minimumPeakHeight) continue;
            const bool separated = std::all_of(groups.begin(), groups.end(), [&](const Peak& group) {
                const auto low = std::min(peak.index, group.index), high = std::max(peak.index, group.index);
                if (high-low < minimumSeparation) return false;
                const auto valley = *std::min_element(smooth.begin()+low, smooth.begin()+high+1);
                return valley <= 0.7 * std::min(peak.height, group.height);
            });
            if (separated) groups.push_back(peak);
        }
        std::sort(groups.begin(), groups.end(), [](const Peak& a, const Peak& b) { return a.index < b.index; });
        std::vector<std::uint64_t> counts;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const auto left = i == 0 ? std::ptrdiff_t{0}
                : std::min_element(smooth.begin()+groups[i-1].index, smooth.begin()+groups[i].index+1)-smooth.begin();
            const auto right = i+1 < groups.size()
                ? std::min_element(smooth.begin()+groups[i].index, smooth.begin()+groups[i+1].index+1)-smooth.begin()
                : static_cast<std::ptrdiff_t>(histogramBinCount);
            counts.push_back(std::accumulate(histogram.begin()+left, histogram.begin()+right, std::uint64_t{0}));
        }
        if (!counts.empty()) {
            // 背景取有显著峰高与样本支持的最低灰度群；最高峰可能属于材料。
            // 低值小伪影不能顶替空气。范围需包含足够背景，才能识别这对灰度群。
            const auto maximumCount = *std::max_element(counts.begin(), counts.end());
            std::size_t background = 0;
            while (background < groups.size()
                && (groups[background].height < 0.01*peaks.front().height || counts[background] < 0.05*maximumCount)) ++background;
            std::uint64_t largestCount = 0;
            for (std::size_t i = background+1; i < groups.size(); ++i) {
                if (counts[i] > largestCount) { largestCount = counts[i]; selected = std::make_pair(groups[background], groups[i]); }
            }
        }
    }
    else {
        const std::size_t candidateCount = std::min<std::size_t>(32, peaks.size());
        for (std::size_t first = 0; first < candidateCount; ++first) {
            for (std::size_t second = first + 1;
                second < candidateCount; ++second) {
                Peak low = peaks[first];
                Peak high = peaks[second];
                if (low.index > high.index) std::swap(low, high);
                if (high.index - low.index < minimumSeparation
                    || low.height < minimumPeakHeight
                    || high.height < minimumPeakHeight) {
                    continue;
                }
                const auto valley = std::min_element(
                    smooth.begin() + static_cast<std::ptrdiff_t>(low.index),
                    smooth.begin() + static_cast<std::ptrdiff_t>(high.index + 1));
                const double valleyHeight = *valley;
                const double smallerPeak = std::min(low.height, high.height);
                if (valleyHeight > 0.85 * smallerPeak) continue;
                const double separation = static_cast<double>(
                    high.index - low.index);
                const double score = separation * smallerPeak
                    * (1.0 - valleyHeight / smallerPeak);
                if (score > selectedScore) {
                    selected = std::make_pair(low, high);
                    selectedScore = score;
                }
            }
        }
    }
    if (!selected) {
        message = "Surface automatic ISO50 could not identify two reliable peaks.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    if (params.method != SurfaceDeterminationMethod::AutomaticIso50) for (const auto &peak : peaks)
    {
        const auto distanceA = peak.index > selected->first.index ? peak.index - selected->first.index
                                                                  : selected->first.index - peak.index;
        const auto distanceB = peak.index > selected->second.index ? peak.index - selected->second.index
                                                                   : selected->second.index - peak.index;
        if (distanceA >= minimumSeparation && distanceB >= minimumSeparation &&
            peak.height >= 0.2 * std::min(selected->first.height, selected->second.height))
        {
            message = "Surface automatic seed is ambiguous between more than two significant peaks.";
            return SurfaceFailureReason::ThresholdUnreliable;
        }
    }
    const double binWidth = (maximum - minimum)
        / static_cast<double>(histogramBinCount - 1);
    const double background = minimum
        + static_cast<double>(selected->first.index) * binWidth;
    const double material = minimum
        + static_cast<double>(selected->second.index) * binWidth;
    isoValue = 0.5 * (background + material);
    if (!std::isfinite(isoValue) || material <= background) {
        message = "Surface automatic ISO50 produced an invalid threshold.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }
    estimate = SurfaceIsoEstimate{
        isoValue,      background,
        material,      validCount,
        excludedCount, double(selected->second.index - selected->first.index) / (histogramBinCount - 1)};
    return SurfaceFailureReason::None;
}

void AddFingerprint(
    std::uint64_t& fingerprint,
    const std::uint64_t value)
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        fingerprint ^= (value >> (byte * 8U)) & 0xffU;
        fingerprint *= prime;
    }
}

SurfaceFailureReason ResolveParams(
    const VolumeView& volume,
    const SurfaceDeterminationStartParams& input,
    const SurfaceCancelCheck& getCancelled,
    ResolvedParams& params,
    std::string& message)
{
    message = SurfaceRecipeCodec::GetError(input);
    if (!message.empty() || input.seedBlockDepth == 0 || input.seedBlockDepth > 4096)
        return SurfaceFailureReason::InvalidGeometry;
    params.localFraction = input.localFraction;
    params.grayPair = input.grayPair;
    params.minimumCnr = input.minimumCnr;
    params.maximumPlateauNoiseRatio = input.maximumPlateauNoiseRatio;
    params.maximumNormalizedResidual = input.maximumNormalizedResidual;
    params.maximumNormalTurnDeg = input.maximumNormalTurnDeg;
    params.sharpCornerAngleDeg = input.sharpCornerAngleDeg;
    params.regionOverrides = input.regionOverrides;
    params.method = input.method;
    params.componentSelection = input.componentSelection;
    params.seedModelPoint = input.seedModelPoint;

    params.minimumObjectVoxels = input.minimumObjectVoxels;
    params.minimumContrast = input.minimumContrast;
    if (params.minimumObjectVoxels == 0
        || !std::isfinite(params.minimumContrast)
        || params.minimumContrast < 0.0) {
        message = "Surface parameters contain an invalid count or contrast.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    if (params.componentSelection == SurfaceComponentSelection::Seeded
        && !params.seedModelPoint) {
        message = "Surface seeded selection requires a model-space seed.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    if (params.seedModelPoint) {
        for (const double value : *params.seedModelPoint) {
            if (!std::isfinite(value)) {
                message = "Surface seed contains a non-finite coordinate.";
                return SurfaceFailureReason::InvalidGeometry;
            }
        }
    }
    if (params.roi) {
        const auto planes=params.roi->GetClipPlanes();
        if (planes.error!=RoiError::None) return SurfaceFailureReason::UnsupportedRoi;
        params.clipPlanes=planes.planes;
        if (!GetBoundsIntersect(params.roi->GetBounds(),GetDataBounds(volume.geometry))) {
            message="Surface ROI does not intersect the source data.";
            return SurfaceFailureReason::InvalidRoi;
        }
    }

    const double minimumSpacing = *std::min_element(
        volume.geometry.spacing.begin(), volume.geometry.spacing.end());
    const double maximumSpacing = *std::max_element(
        volume.geometry.spacing.begin(), volume.geometry.spacing.end());
    params.profileHalfLengthModel = input.profileHalfLengthModel.value_or(
        2.5 * maximumSpacing);
    params.profileSampleStepModel = input.profileSampleStepModel.value_or(
        0.25 * minimumSpacing);
    params.maximumOffsetModel = input.maximumOffsetModel.value_or(
        1.5 * maximumSpacing);
    params.profileSmoothingSigmaModel =
        input.profileSmoothingSigmaModel.value_or(
            params.profileSampleStepModel);
    const std::array<double, 4> profileValues{
        params.profileHalfLengthModel,
        params.profileSampleStepModel,
        params.maximumOffsetModel,
        params.profileSmoothingSigmaModel
    };
    for (const double value : profileValues) {
        if (!std::isfinite(value) || value < 0.0)
        {
            message = "Surface profile parameters must be finite and nonnegative.";
            return SurfaceFailureReason::InvalidGeometry;
        }
    }
    if (params.profileHalfLengthModel <= 0 || params.profileSampleStepModel <= 0 ||
        params.maximumOffsetModel > params.profileHalfLengthModel ||
        2.0 * params.profileHalfLengthModel / params.profileSampleStepModel >
            static_cast<double>(maxProfileSampleCount - 1))
    {
        message = "Surface profile bounds are inconsistent or exceed the sample limit.";
        return SurfaceFailureReason::InvalidGeometry;
    }

    params.minimumEdgeWidthModel = input.minimumEdgeWidthModel.value_or(params.profileSampleStepModel * 0.25);
    params.maximumEdgeWidthModel = input.maximumEdgeWidthModel.value_or(params.profileHalfLengthModel);
    params.minimumEdgeSeparationModel =
        input.minimumEdgeSeparationModel.value_or(params.profileSampleStepModel * 2);
    if (params.minimumEdgeWidthModel > params.maximumEdgeWidthModel)
        return SurfaceFailureReason::InvalidGeometry;
    for (const auto &rule : params.regionOverrides)
    {
        const auto half = rule.profileHalfLengthModel.value_or(params.profileHalfLengthModel);
        const auto step = rule.profileSampleStepModel.value_or(params.profileSampleStepModel);
        const auto offset = rule.maximumOffsetModel.value_or(params.maximumOffsetModel);
        if (offset > half || 2 * half / step > maxProfileSampleCount - 1)
        {
            message = "Surface override exceeds the resolved profile bounds.";
            return SurfaceFailureReason::InvalidGeometry;
        }
    }
    params.isAutomaticIso = !input.initialIsoValue.has_value();
    if (volume.labels || volume.initialMesh)
    {
        params.initialIsoValue = input.initialIsoValue.value_or(0.0);
        return SurfaceFailureReason::None;
    }
    if (input.grayPair && !input.initialIsoValue)
    {
        const double a = 0.5 * input.grayPair->sideA[0] + 0.5 * input.grayPair->sideA[1];
        const double b = 0.5 * input.grayPair->sideB[0] + 0.5 * input.grayPair->sideB[1];
        params.initialIsoValue = (1 - input.seedFraction) * a + input.seedFraction * b;
        return SurfaceFailureReason::None;
    }
    if (input.initialIsoValue) {
        if (!std::isfinite(*input.initialIsoValue)) {
            message = "Surface initial ISO value is not finite.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        params.initialIsoValue = *input.initialIsoValue;
        return SurfaceFailureReason::None;
    }
    const auto status =
        GetAutomaticIso(volume, params, getCancelled, params.initialIsoValue, message, params.isoEstimate);
    if (status == SurfaceFailureReason::None && params.isoEstimate &&
        input.method != SurfaceDeterminationMethod::AutomaticIso50)
        params.initialIsoValue = (1 - input.seedFraction) * params.isoEstimate->backgroundValue +
                                 input.seedFraction * params.isoEstimate->materialValue;
    return status;
}

bool AddWorkingBytes(
    const std::size_t count,
    const std::size_t itemBytes,
    std::size_t& workingBytes)
{
    std::size_t bytes = 0;
    return GetProduct(count, itemBytes, bytes)
        && GetSum(workingBytes, bytes, workingBytes);
}

class DisjointSet final {
public:
    explicit DisjointSet(const std::size_t size)
        : m_parent(size)
        , m_rank(size, 0)
    {
        std::iota(m_parent.begin(), m_parent.end(), 0U);
    }

    std::uint32_t GetRoot(const std::uint32_t value)
    {
        std::uint32_t current = value;
        while (m_parent[current] != current) current = m_parent[current];
        std::uint32_t next = value;
        while (m_parent[next] != next) {
            const std::uint32_t parent = m_parent[next];
            m_parent[next] = current;
            next = parent;
        }
        return current;
    }

    void SetJoined(const std::uint32_t left, const std::uint32_t right)
    {
        std::uint32_t leftRoot = GetRoot(left);
        std::uint32_t rightRoot = GetRoot(right);
        if (leftRoot == rightRoot) return;
        if (m_rank[leftRoot] < m_rank[rightRoot]) {
            std::swap(leftRoot, rightRoot);
        }
        m_parent[rightRoot] = leftRoot;
        if (m_rank[leftRoot] == m_rank[rightRoot]) ++m_rank[leftRoot];
    }

private:
    std::vector<std::uint32_t> m_parent;
    std::vector<std::uint8_t> m_rank;
};

std::uint64_t GetEdgeKey(
    const std::uint32_t left,
    const std::uint32_t right)
{
    const std::uint32_t minimum = std::min(left, right);
    const std::uint32_t maximum = std::max(left, right);
    return (static_cast<std::uint64_t>(minimum) << 32U)
        | static_cast<std::uint64_t>(maximum);
}

TopologyMetrics GetTopologyMetrics(const std::vector<Point3> &points, const std::vector<Triangle> &triangles,
                                   const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    struct EdgeRecord final {
        std::uint32_t count = 0;
        int directionSum = 0;
    };
    std::unordered_map<std::uint64_t, EdgeRecord> edges;
    if (triangles.size()
        <= std::numeric_limits<std::size_t>::max() / 2U) {
        edges.reserve(triangles.size() * 2U);
    }
    TopologyMetrics metrics;
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto& ids = triangle.vertices;
        if (ids[0] >= points.size()
            || ids[1] >= points.size()
            || ids[2] >= points.size()) {
            ++metrics.degenerateTriangleCount;
            metrics.isOrientationValid = false;
            continue;
        }
        const Point3 edge01 = Subtract(points[ids[1]], points[ids[0]]);
        const Point3 edge02 = Subtract(points[ids[2]], points[ids[0]]);
        const Point3 areaVector = Cross(edge01, edge02);
        const double doubleArea = GetLength(areaVector);
        if (!std::isfinite(doubleArea) || doubleArea <= geometryEpsilon) {
            ++metrics.degenerateTriangleCount;
        }
        else {
            metrics.area += 0.5 * doubleArea;
            metrics.signedVolume += Dot(
                points[ids[0]],
                Cross(points[ids[1]], points[ids[2]])) / 6.0;
        }
        for (std::size_t edge = 0; edge < 3; ++edge) {
            CheckCancellation(cancellationBatch, cancelled);
            const std::uint32_t from = ids[edge];
            const std::uint32_t to = ids[(edge + 1) % 3];
            auto& record = edges[GetEdgeKey(from, to)];
            ++record.count;
            record.directionSum += from < to ? 1 : -1;
        }
    }
    for (const auto& item : edges) {
        CheckCancellation(cancellationBatch, cancelled);
        const EdgeRecord& edge = item.second;
        if (edge.count == 1) ++metrics.boundaryEdgeCount;
        else if (edge.count > 2) ++metrics.nonManifoldEdgeCount;
        if (edge.count == 2 && edge.directionSum != 0) {
            metrics.isOrientationValid = false;
        }
        if (edge.count > 2) metrics.isOrientationValid = false;
    }
    if (metrics.degenerateTriangleCount != 0) {
        metrics.isOrientationValid = false;
    }
    return metrics;
}

std::vector<MeshComponent> BuildComponents(const std::vector<Point3> &points,
                                           const std::vector<Triangle> &triangles, const double voxelVolume,
                                           const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    DisjointSet sets(points.size());
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        sets.SetJoined(triangle.vertices[0], triangle.vertices[1]);
        sets.SetJoined(triangle.vertices[1], triangle.vertices[2]);
    }
    std::map<std::uint32_t, std::vector<Triangle>> groupedTriangles;
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        groupedTriangles[sets.GetRoot(triangle.vertices[0])]
            .push_back(triangle);
    }

    std::vector<MeshComponent> components;
    components.reserve(groupedTriangles.size());
    for (auto& item : groupedTriangles) {
        CheckCancellation(cancellationBatch, cancelled);
        MeshComponent component;
        component.triangles = std::move(item.second);
        std::vector<std::uint32_t> pointIds;
        pointIds.reserve(component.triangles.size());
        for (const Triangle& triangle : component.triangles) {
            CheckCancellation(cancellationBatch, cancelled);
            pointIds.insert(
                pointIds.end(),
                triangle.vertices.begin(),
                triangle.vertices.end());
        }
        std::sort(pointIds.begin(), pointIds.end());
        pointIds.erase(
            std::unique(pointIds.begin(), pointIds.end()),
            pointIds.end());
        component.sourcePointIds = std::move(pointIds);
        component.minimumPointId = component.sourcePointIds.front();

        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        remap.reserve(component.sourcePointIds.size());
        std::vector<Point3> localPoints;
        localPoints.reserve(component.sourcePointIds.size());
        for (std::size_t index = 0;
            index < component.sourcePointIds.size(); ++index) {
            CheckCancellation(cancellationBatch, cancelled);
            remap.emplace(
                component.sourcePointIds[index],
                static_cast<std::uint32_t>(index));
            localPoints.push_back(points[component.sourcePointIds[index]]);
        }
        std::vector<Triangle> localTriangles = component.triangles;
        for (Triangle& triangle : localTriangles) {
            CheckCancellation(cancellationBatch, cancelled);
            for (std::uint32_t& pointId : triangle.vertices) {
                CheckCancellation(cancellationBatch, cancelled);
                pointId = remap.at(pointId);
            }
        }
        const TopologyMetrics topology = GetTopologyMetrics(localPoints, localTriangles, cancelled);
        component.isClosed = topology.boundaryEdgeCount == 0
            && topology.nonManifoldEdgeCount == 0;
        if (component.isClosed && voxelVolume > geometryEpsilon) {
            const double voxelCount = std::abs(topology.signedVolume)
                / voxelVolume;
            if (std::isfinite(voxelCount) && voxelCount >= 1.0) {
                component.estimatedVoxelCount = voxelCount
                    >= static_cast<double>(
                        std::numeric_limits<long long>::max())
                    ? std::numeric_limits<std::uint64_t>::max()
                    : static_cast<std::uint64_t>(std::llround(voxelCount));
            }
        }
        components.push_back(std::move(component));
    }
    return components;
}

double GetComponentDistanceSquared(const MeshComponent &component, const std::vector<Point3> &points,
                                   const Point3 &seed, const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    double distance = std::numeric_limits<double>::max();
    for (const std::uint32_t pointId : component.sourcePointIds) {
        CheckCancellation(cancellationBatch, cancelled);
        const Point3 delta = Subtract(points[pointId], seed);
        distance = std::min(distance, Dot(delta, delta));
    }
    return distance;
}

std::vector<std::size_t> GetSelectedComponents(const std::vector<MeshComponent> &components,
                                               const std::vector<Point3> &points,
                                               const ResolvedParams &params,
                                               const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    std::vector<std::size_t> eligible;
    for (std::size_t index = 0; index < components.size(); ++index) {
        CheckCancellation(cancellationBatch, cancelled);
        const MeshComponent& component = components[index];
        // 开放表面没有可信体积，不伪造 voxel count；保留它并由 truncated
        // 与 metric validity 向调用方表达限制。
        if (!component.isClosed
            || component.estimatedVoxelCount >= params.minimumObjectVoxels) {
            eligible.push_back(index);
        }
    }
    const auto score = [&components](const std::size_t index) {
        const MeshComponent& component = components[index];
        return std::make_pair(
            component.estimatedVoxelCount,
            static_cast<std::uint64_t>(component.triangles.size()));
    };
    const auto stableOrder = [&components, &score](
        const std::size_t left,
        const std::size_t right) {
        if (score(left) != score(right)) return score(left) > score(right);
        return components[left].minimumPointId
            < components[right].minimumPointId;
    };
    std::sort(eligible.begin(), eligible.end(), stableOrder);
    if (eligible.empty()) return {};
    if (params.componentSelection == SurfaceComponentSelection::Largest) {
        eligible.resize(1);
        return eligible;
    }
    if (params.componentSelection == SurfaceComponentSelection::Seeded) {
        const Point3& seed = *params.seedModelPoint;
        const auto selected = std::min_element(
            eligible.begin(), eligible.end(),
            [&components, &points, &seed, &cancelled](const std::size_t left, const std::size_t right) {
                const double leftDistance =
                    GetComponentDistanceSquared(components[left], points, seed, cancelled);
                const double rightDistance =
                    GetComponentDistanceSquared(components[right], points, seed, cancelled);
                if (leftDistance != rightDistance)
                {
                    return leftDistance < rightDistance;
                }
                return components[left].minimumPointId
                    < components[right].minimumPointId;
            });
        return { *selected };
    }
    return eligible;
}

void GetLocalMesh(const MeshComponent &component, const std::vector<Point3> &sourcePoints,
                  std::vector<Point3> &points, std::vector<Triangle> &triangles,
                  const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    remap.reserve(component.sourcePointIds.size());
    points.reserve(component.sourcePointIds.size());
    for (std::size_t index = 0;
        index < component.sourcePointIds.size(); ++index) {
        CheckCancellation(cancellationBatch, cancelled);
        const std::uint32_t sourceId = component.sourcePointIds[index];
        remap.emplace(sourceId, static_cast<std::uint32_t>(index));
        points.push_back(sourcePoints[sourceId]);
    }
    triangles = component.triangles;
    for (Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        for (std::uint32_t& pointId : triangle.vertices) {
            CheckCancellation(cancellationBatch, cancelled);
            pointId = remap.at(pointId);
        }
    }
}

std::vector<Point3> GetVertexNormals(const std::vector<Point3> &points,
                                     const std::vector<Triangle> &triangles,
                                     const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    std::vector<Point3> normals(points.size(), Point3{});
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const Point3 normal = Cross(
            Subtract(
                points[triangle.vertices[1]],
                points[triangle.vertices[0]]),
            Subtract(
                points[triangle.vertices[2]],
                points[triangle.vertices[0]]));
        for (const std::uint32_t pointId : triangle.vertices) {
            CheckCancellation(cancellationBatch, cancelled);
            normals[pointId] = Add(normals[pointId], normal);
        }
    }
    for (Point3& normal : normals) {
        CheckCancellation(cancellationBatch, cancelled);
        if (!Normalize(normal)) normal = { 0.0, 0.0, 1.0 };
    }
    return normals;
}

void SetOutwardWinding(
    const VolumeView& volume,
    const std::vector<Point3>& points,
    std::vector<Triangle>& triangles)
{
    double orientationScore = 0.0;
    std::size_t sampleCount = 0;
    const std::size_t stride = std::max<std::size_t>(
        1, triangles.size() / 256U);
    for (std::size_t index = 0; index < triangles.size(); index += stride) {
        const Triangle& triangle = triangles[index];
        const Point3& first = points[triangle.vertices[0]];
        const Point3& second = points[triangle.vertices[1]];
        const Point3& third = points[triangle.vertices[2]];
        Point3 triangleNormal = Cross(
            Subtract(second, first), Subtract(third, first));
        if (!Normalize(triangleNormal)) continue;
        const Point3 center = Scale(Add(Add(first, second), third), 1.0 / 3.0);
        Point3 gradient{};
        double magnitude = 0.0;
        if (!GetGradient(volume, center, gradient, magnitude)
            || !Normalize(gradient)) {
            continue;
        }
        const Point3 outward = Scale(gradient, -1.0);
        orientationScore += Dot(triangleNormal, outward);
        ++sampleCount;
    }
    if (sampleCount != 0 && orientationScore < 0.0) {
        for (Triangle& triangle : triangles) {
            std::swap(triangle.vertices[1], triangle.vertices[2]);
        }
    }
}

float GetFiniteFloat(const double value)
{
    if (!std::isfinite(value)) return 0.0F;
    const double bounded = std::clamp(
        value,
        -static_cast<double>(std::numeric_limits<float>::max()),
        static_cast<double>(std::numeric_limits<float>::max()));
    return static_cast<float>(bounded);
}

bool SetRefinedPoint(const VolumeView &volume, const ResolvedParams &global, const Point3 &initialPoint,
                     const Point3 &initialNormal, SurfacePointRecord &record,
                     SurfaceProfileWorkspace &workspace, SurfaceProfileDiagnostic *diagnostic = nullptr)
{
    record.seedPositionModel = initialPoint;
    record.positionModel = initialPoint;
    Point3 normal = initialNormal, current = initialPoint, gradient{};
    double magnitude = 0;
    auto params = GetLocalParams(global, initialPoint, record.overrideIndex);
    if (global.roi) {
        const auto tolerance = 32 * std::numeric_limits<double>::epsilon()
            * std::max({1.0, std::abs(initialPoint[0]), std::abs(initialPoint[1]), std::abs(initialPoint[2])});
        const auto distance = global.roi->GetBoundaryDistance(initialPoint);
        if (std::isfinite(distance) && distance <= tolerance)
            record.flags |= SurfacePointFlags::RoiBoundary | SurfacePointFlags::SeedRetained;
    }
    if (!params.materials && GetGradient(volume, current, gradient, magnitude) && Normalize(gradient))
    {
        double sign = -1;
        if (params.grayPair)
            sign = params.grayPair->sideB[0] > params.grayPair->sideA[1] ? 1 : -1;
        normal = Scale(gradient, sign);
    }
    if (!Normalize(normal))
    {
        record.flags |= SurfacePointFlags::FitRejected | SurfacePointFlags::SeedRetained;
        return false;
    }
    record.seedNormalModel = normal;
    if (GetGradient(volume, current, gradient, magnitude))
    {
        const auto projected = Dot(gradient, normal);
        params.expectedDerivativeSign = projected > geometryEpsilon    ? 1
                                        : projected < -geometryEpsilon ? -1
                                                                       : 0;
    }
    record.normalModel = {GetFiniteFloat(record.seedNormalModel[0]),
                          GetFiniteFloat(record.seedNormalModel[1]),
                          GetFiniteFloat(record.seedNormalModel[2])};
    if (params.method == SurfaceDeterminationMethod::GlobalIsoPreview)
    {
        record.localThreshold = GetFiniteFloat(params.initialIsoValue);
        record.gradientMagnitude = GetFiniteFloat(magnitude);
        const auto sample = GetScalarAtModel(volume, current);
        record.validSupportRatio = sample.status == SampleStatus::Valid ? 1.0F : 0.0F;
        if (sample.status != SampleStatus::Valid)
            record.flags |= SurfacePointFlags::InvalidSupport;
        if (diagnostic)
        {
            diagnostic->isAvailable = true;
            diagnostic->point = record;
            diagnostic->message = "Preview has no local refinement profile.";
        }
        return true;
    }
    double supportRatio = 1;
    SurfaceProfileFit fit;
    for (unsigned iteration = 0; iteration < 2; ++iteration)
    {
        BuildProfile(volume, current, normal, params, workspace);
        fit = SurfaceProfileSolver::BuildFit(workspace, params);
        supportRatio = std::min(supportRatio, workspace.validRatio);
        record.flags |= fit.flags;
        if (diagnostic)
        {
            diagnostic->profileCenterModel = current;
            diagnostic->directionModel = normal;
            diagnostic->offsetsModel = workspace.offsets;
            diagnostic->rawValues = workspace.raw;
            diagnostic->filteredValues = workspace.values;
            diagnostic->support = workspace.support;
            diagnostic->materialLabels = workspace.labels;
            diagnostic->candidates = workspace.candidates;
            diagnostic->sideA = fit.sideA;
            diagnostic->sideB = fit.sideB;
            diagnostic->noiseSigma = fit.noise;
            diagnostic->normalizedResidual = fit.normalizedResidual;
            diagnostic->minimumOffsetModel = -params.maximumOffsetModel;
            diagnostic->maximumOffsetModel = params.maximumOffsetModel;
        }
        if (record.flags != SurfacePointFlags::None)
            break;
        const auto next = Add(current, Scale(normal, fit.offset));
        if (global.roi && !global.roi->GetContains(next))
        {
            record.flags |= SurfacePointFlags::RoiBoundary;
            break;
        }
        if (GetLength(Subtract(next, initialPoint)) > params.maximumOffsetModel + geometryEpsilon)
        {
            record.flags |= SurfacePointFlags::ExcessiveOffset;
            break;
        }
        current = next;
        if (GetGradient(volume, current, gradient, magnitude) && Normalize(gradient))
        {
            if (Dot(gradient, normal) < 0)
                gradient = Scale(gradient, -1);
            if (Dot(gradient, normal) < std::cos(params.maximumNormalTurnDeg * 3.141592653589793 / 180))
            {
                record.flags |= SurfacePointFlags::SharpCorner;
                break;
            }
            normal = gradient;
        }
    }
    record.localThreshold = GetFiniteFloat(fit.threshold);
    record.contrast = GetFiniteFloat(fit.contrast);
    record.gradientMagnitude = GetFiniteFloat(fit.gradient);
    record.fitResidual = GetFiniteFloat(fit.residual);
    record.validSupportRatio = GetFiniteFloat(supportRatio);
    record.crossingCount = fit.crossingCount;
    record.transitionWidthModel = GetFiniteFloat(fit.width);
    record.pairedSeparationModel = GetFiniteFloat(fit.separation);
    record.estimatedLocalizationSigma = GetFiniteFloat(
        std::hypot(std::hypot(fit.noise, fit.residual) / std::max(fit.gradient, geometryEpsilon),
                   workspace.step / std::sqrt(12.0)));
    const bool accepted = record.flags == SurfacePointFlags::None;
    if (!accepted)
        record.flags |= SurfacePointFlags::SeedRetained;
    record.positionModel = accepted ? current : initialPoint;
    record.offsetFromSeed = GetFiniteFloat(GetLength(Subtract(record.positionModel, initialPoint)));
    record.normalModel = accepted ? std::array<float, 3>{GetFiniteFloat(normal[0]), GetFiniteFloat(normal[1]),
                                                         GetFiniteFloat(normal[2])}
                                  : std::array<float, 3>{GetFiniteFloat(record.seedNormalModel[0]),
                                                         GetFiniteFloat(record.seedNormalModel[1]),
                                                         GetFiniteFloat(record.seedNormalModel[2])};
    if (diagnostic)
    {
        diagnostic->isAvailable = true;
        diagnostic->point = record;
    }
    return accepted;
}

bool GetPointAtDataBoundary(
    const ImageGeometry& geometry,
    const Point3& modelPoint,
    const double toleranceModel)
{
    const Point3 index = GetContinuousIndex(geometry, modelPoint);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double toleranceIndex = toleranceModel
            / geometry.spacing[axis];
        if (std::abs(index[axis]
                - static_cast<double>(geometry.extent[axis * 2]))
                <= toleranceIndex
            || std::abs(index[axis]
                - static_cast<double>(geometry.extent[axis * 2 + 1]))
                <= toleranceIndex) {
            return true;
        }
    }
    return false;
}

bool GetPointAtRoiBoundary(const RoiReadSnapshot& roi, const Point3& point, double tolerance)
{
    return roi && roi->GetBoundaryDistance(point)<=tolerance;
}

void SetTriangleFlipFlags(const std::vector<Point3> &originalPoints, const std::vector<Point3> &refinedPoints,
                          const std::vector<Triangle> &triangles, std::vector<SurfacePointRecord> &records,
                          const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto& ids = triangle.vertices;
        const Point3 originalNormal = Cross(
            Subtract(originalPoints[ids[1]], originalPoints[ids[0]]),
            Subtract(originalPoints[ids[2]], originalPoints[ids[0]]));
        const Point3 refinedNormal = Cross(
            Subtract(refinedPoints[ids[1]], refinedPoints[ids[0]]),
            Subtract(refinedPoints[ids[2]], refinedPoints[ids[0]]));
        const double originalLength = GetLength(originalNormal);
        const double refinedLength = GetLength(refinedNormal);
        if (originalLength <= geometryEpsilon
            || refinedLength <= geometryEpsilon
            || Dot(originalNormal, refinedNormal) <= 0.0) {
            for (const std::uint32_t pointId : ids) {
                CheckCancellation(cancellationBatch, cancelled);
                records[pointId].flags |=
                    SurfacePointFlags::TriangleFlipRisk | SurfacePointFlags::SeedRetained;
                records[pointId].positionModel = originalPoints[pointId];
                records[pointId].offsetFromSeed = 0;
                records[pointId].normalModel = {GetFiniteFloat(records[pointId].seedNormalModel[0]),
                                                GetFiniteFloat(records[pointId].seedNormalModel[1]),
                                                GetFiniteFloat(records[pointId].seedNormalModel[2])};
            }
        }
    }
}

SurfaceMetricValidity GetVolumeValidity(
    const TopologyMetrics& topology,
    const bool isTruncated,
    const bool hasQuality)
{
    if (topology.nonManifoldEdgeCount != 0) {
        return SurfaceMetricValidity::NonManifold;
    }
    if (isTruncated) return SurfaceMetricValidity::Truncated;
    if (topology.boundaryEdgeCount != 0) {
        return SurfaceMetricValidity::OpenSurface;
    }
    if (!topology.isOrientationValid
        || topology.degenerateTriangleCount != 0) {
        return SurfaceMetricValidity::NonManifold;
    }
    if (!hasQuality || !std::isfinite(topology.signedVolume)) return SurfaceMetricValidity::InsufficientQuality;
    return SurfaceMetricValidity::Valid;
}

SurfaceMetricValidity GetAreaValidity(
    const TopologyMetrics& topology,
    const bool isTruncated,
    const bool hasQuality)
{
    if (!hasQuality) return SurfaceMetricValidity::InsufficientQuality;
    if (topology.nonManifoldEdgeCount != 0
        || topology.degenerateTriangleCount != 0
        || !topology.isOrientationValid) {
        return SurfaceMetricValidity::NonManifold;
    }
    if (isTruncated) return SurfaceMetricValidity::Truncated;
    if (topology.boundaryEdgeCount != 0) {
        return SurfaceMetricValidity::OpenSurface;
    }
    return SurfaceMetricValidity::Valid;
}

void AddObjectResult(const VolumeView &volume, const ResolvedParams &params, const std::uint32_t objectIndex,
                     const std::vector<Point3> &originalPoints, const std::vector<Triangle> &triangles,
                     std::vector<SurfacePointRecord> records, SurfaceAlgorithmResult &result,
                     const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    std::vector<Point3> refinedPoints;
    refinedPoints.reserve(records.size());
    for (const SurfacePointRecord& record : records) {
        CheckCancellation(cancellationBatch, cancelled);
        refinedPoints.push_back(record.positionModel);
    }
    SetTriangleFlipFlags(originalPoints, refinedPoints, triangles, records, cancelled);
    refinedPoints.clear();
    refinedPoints.reserve(records.size());
    for (const SurfacePointRecord& record : records) {
        CheckCancellation(cancellationBatch, cancelled);
        refinedPoints.push_back(record.positionModel);
    }
    const TopologyMetrics topology = GetTopologyMetrics(refinedPoints, triangles, cancelled);

    std::uint64_t acceptedCount = 0;
    std::uint64_t lowContrastCount = 0;
    bool isTruncated = false;
    const double boundaryTolerance = std::max(
        params.profileSampleStepModel, 1.0e-8);
    std::array<double, 6> bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    for (SurfacePointRecord& record : records) {
        CheckCancellation(cancellationBatch, cancelled);
        record.objectIndex = objectIndex;
        if (SurfaceContract::GetPointValid(record, params.method)) ++acceptedCount;
        if (GetSurfaceFlag(
                record.flags, SurfacePointFlags::LowContrast)) {
            ++lowContrastCount;
        }
        if (GetSurfaceFlag(record.flags, SurfacePointFlags::ProfileClipped) ||
            GetSurfaceFlag(record.flags, SurfacePointFlags::RoiBoundary) ||
            GetPointAtDataBoundary(volume.geometry, record.positionModel, boundaryTolerance) ||
            GetPointAtRoiBoundary(params.roi, record.positionModel, boundaryTolerance))
        {
            isTruncated = true;
            ++result.truncatedPointCount;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            CheckCancellation(cancellationBatch, cancelled);
            bounds[axis * 2] = std::min(
                bounds[axis * 2], record.positionModel[axis]);
            bounds[axis * 2 + 1] = std::max(
                bounds[axis * 2 + 1], record.positionModel[axis]);
        }
    }
    double validArea = 0.0;
    double totalArea = 0.0;
    bool hasCompleteSupport = !triangles.empty();
    for (const auto& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto& ids = triangle.vertices;
        const auto edgeA = Subtract(records[ids[1]].positionModel, records[ids[0]].positionModel);
        const auto edgeB = Subtract(records[ids[2]].positionModel, records[ids[0]].positionModel);
        const double area = 0.5 * GetLength(Cross(edgeA, edgeB));
        bool isValid = std::isfinite(area) && area > geometryEpsilon;
        for (const auto id : ids) {
            CheckCancellation(cancellationBatch, cancelled);
            isValid = isValid && SurfaceContract::GetPointValid(records[id], params.method)
                && !GetPointAtDataBoundary(volume.geometry, records[id].positionModel, boundaryTolerance)
                && !GetPointAtRoiBoundary(params.roi, records[id].positionModel, boundaryTolerance);
        }
        if (std::isfinite(area)) { totalArea += area; if (isValid) validArea += area; }
        hasCompleteSupport = hasCompleteSupport && isValid;
        result.triangleValidity.push_back(isValid ? 1 : 0);
    }
    const bool hasFiniteArea = std::isfinite(totalArea) && std::isfinite(validArea);
    const double validAreaRatio = hasFiniteArea && totalArea > 0.0 ? validArea / totalArea : 0.0;
    hasCompleteSupport = hasCompleteSupport && hasFiniteArea;
    const bool hasQuality = hasFiniteArea && validAreaRatio >= qualityRatioThreshold;

    SurfaceObjectRecord object;
    object.objectIndex = objectIndex;
    object.firstPoint = result.points.size();
    object.pointCount = records.size();
    object.firstTriangle = result.triangleIndices.size() / 3U;
    object.triangleCount = triangles.size();
    object.boundsModel = bounds;
    object.validAreaRatio = validAreaRatio;
    object.isClosed = topology.boundaryEdgeCount == 0;
    object.isManifold = topology.nonManifoldEdgeCount == 0
        && topology.degenerateTriangleCount == 0;
    object.isTruncated = isTruncated;
    object.isOrientationValid = topology.isOrientationValid;
    object.areaValidity = GetAreaValidity(
        topology, isTruncated, hasQuality);
    if (object.areaValidity != SurfaceMetricValidity::InsufficientQuality
        && object.areaValidity != SurfaceMetricValidity::NonManifold) {
        object.areaModelUnit2 = validArea;
    }
    object.volumeValidity = GetVolumeValidity(
        topology, isTruncated, hasCompleteSupport);
    if (object.volumeValidity == SurfaceMetricValidity::Valid) {
        object.volumeModelUnit3 = std::abs(topology.signedVolume);
    }
    if (!object.isManifold) ++result.nonManifoldObjectCount;

    result.acceptedPointCount += acceptedCount;
    result.lowContrastPointCount += lowContrastCount;
    result.rejectedPointCount += records.size() - acceptedCount;
    const std::uint32_t pointOffset = static_cast<std::uint32_t>(
        result.points.size());
    result.points.insert(
        result.points.end(),
        std::make_move_iterator(records.begin()),
        std::make_move_iterator(records.end()));
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        for (const std::uint32_t pointId : triangle.vertices) {
            CheckCancellation(cancellationBatch, cancelled);
            result.triangleIndices.push_back(pointOffset + pointId);
        }
    }
    result.objects.push_back(std::move(object));
}

SurfaceFailureReason SetAdditionalInputs(const VtkImageGridSnapshot &source,
                                         const SurfaceDeterminationStartParams &params,
                                         const SurfaceAlgorithmInputs &inputs, VolumeView &volume,
                                         std::string &message)
{
    const auto *image = dynamic_cast<const ImageGrid3DPayload *>(source->data->payload.get());
    const auto &geometry = image->GetGeometry();
    if (params.analysisRoi.has_value() != static_cast<bool>(inputs.roi)
        || (inputs.roi && (inputs.roi->GetRevision() != *params.analysisRoi
                           || inputs.roi->GetSource() != source->data->self))) {
        message = "Surface ROI snapshot does not match the request/source.";
        return SurfaceFailureReason::InvalidRoi;
    }
    if (inputs.roi && inputs.roi->GetClipPlanes().error != RoiError::None) {
        message = "Surface requires a convex ROI with exact clipping planes.";
        return SurfaceFailureReason::UnsupportedRoi;
    }
    const auto matches = [&](const DataSnapshot &snapshot, const std::optional<DataRevisionRef> &expected) {
        return snapshot && expected && snapshot->self == *expected &&
               std::any_of(snapshot->inputs.begin(), snapshot->inputs.end(), [&](const auto &input) {
                   return input.role == "source-volume" && input.source == source->data->self;
               });
    };
    if (bool(params.materialLabels) != bool(inputs.materialLabels) ||
        bool(params.initialSurface) != bool(inputs.initialSurface) ||
        bool(params.materialLabels) != !params.materialPairs.empty())
    {
        message = "Surface optional input snapshots do not match the request.";
        return SurfaceFailureReason::InvalidSource;
    }
    if (inputs.materialLabels)
    {
        const auto *labels = dynamic_cast<const LabelMap3DPayload *>(inputs.materialLabels->payload.get());
        if (!matches(inputs.materialLabels, params.materialLabels) || !labels || !labels->GetValid())
        {
            message = "Surface material labels require the exact source-volume revision.";
            return SurfaceFailureReason::InvalidSource;
        }
        const auto &other = labels->GetGeometry();
        if (other.extent != geometry.extent || other.dimensions != geometry.dimensions ||
            other.spacing != geometry.spacing || other.origin != geometry.origin ||
            other.direction != geometry.direction || other.coordinateFrame != geometry.coordinateFrame)
        {
            message = "Surface material labels must have the source geometry and frame.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        if (labels->GetScalarRange()[0] < 0 || labels->GetScalarRange()[1] > UINT32_MAX)
        {
            message = "Surface material identities must fit uint32.";
            return SurfaceFailureReason::InvalidSource;
        }
        volume.labels = labels;
    }
    if (inputs.initialSurface)
    {
        const auto *mesh = dynamic_cast<const SurfaceMeshPayload *>(inputs.initialSurface->payload.get());
        if (!matches(inputs.initialSurface, params.initialSurface) || !mesh || !mesh->GetValid() ||
            mesh->GetCoordinateFrame() != geometry.coordinateFrame)
        {
            message = "Surface initial mesh requires the exact source-volume revision and frame.";
            return SurfaceFailureReason::InvalidSource;
        }
        volume.initialMesh = mesh;
    }
    return SurfaceFailureReason::None;
}

void SetInterfaceWinding(const VolumeView &volume, const ResolvedParams &params,
                         const std::vector<Point3> &points, std::vector<Triangle> &triangles)
{
    if (!params.materials)
    {
        SetOutwardWinding(volume, points, triangles);
        if (params.grayPair && params.grayPair->sideB[0] > params.grayPair->sideA[1])
            for (auto &t : triangles)
                std::swap(t.vertices[1], t.vertices[2]);
        return;
    }
    double score = 0;
    const double probe = *std::max_element(volume.geometry.spacing.begin(), volume.geometry.spacing.end());
    const auto stride = std::max<std::size_t>(1, triangles.size() / 256);
    for (std::size_t i = 0; i < triangles.size(); i += stride)
    {
        const auto &ids = triangles[i].vertices;
        const auto center = Scale(Add(Add(points[ids[0]], points[ids[1]]), points[ids[2]]), 1.0 / 3);
        auto normal =
            Cross(Subtract(points[ids[1]], points[ids[0]]), Subtract(points[ids[2]], points[ids[0]]));
        if (!Normalize(normal))
            continue;
        const auto a = GetLabelAtModel(volume, Add(center, Scale(normal, -probe)));
        const auto b = GetLabelAtModel(volume, Add(center, Scale(normal, probe)));
        if (a == params.materials->materialA && b == params.materials->materialB)
            score += 1;
        if (a == params.materials->materialB && b == params.materials->materialA)
            score -= 1;
    }
    if (score < 0)
        for (auto &t : triangles)
            std::swap(t.vertices[1], t.vertices[2]);
}

SurfaceAlgorithmResult BuildSurfaceImpl(const VtkImageGridSnapshot &source,
                                        const SurfaceDeterminationStartParams &inputParams,
                                        const std::size_t maxWorkingBytes,
                                        const SurfaceCancelCheck &getCancelled,
                                        const SurfaceProgressCallback &onProgress,
                                        const SurfaceAlgorithmInputs &inputs)
{
    SurfaceAlgorithmResult result;
    result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
    result.method = inputParams.method;
    result.algorithmRevision = algorithmRevision;
    const auto fail = [&](SurfaceFailureReason reason, const std::string &message) {
        result.failureReason = reason;
        result.message = message;
        result.status = reason == SurfaceFailureReason::Cancelled ? SurfaceResultStatus::Cancelled
                                                                  : SurfaceResultStatus::Failed;
        result.points.clear();
        result.triangleIndices.clear();
        result.triangleValidity.clear();
        result.objects.clear();
        result.interfaces.clear();
        result.acceptedPointCount = 0;
        result.rejectedPointCount = 0;
        result.lowContrastPointCount = 0;
        result.truncatedPointCount = 0;
        result.nonManifoldObjectCount = 0;
        return result;
    };
    SendProgress(onProgress, SurfaceDeterminationStage::Preparing, 0.01);
    VolumeView volume;
    auto reason = BuildVolumeView(source, volume, result.message);
    if (reason != SurfaceFailureReason::None)
        return fail(reason, result.message);
    reason = SetAdditionalInputs(source, inputParams, inputs, volume, result.message);
    if (reason != SurfaceFailureReason::None)
        return fail(reason, result.message);
    result.requiredBytes = 64 * 1024;
    if (maxWorkingBytes < result.requiredBytes)
        return fail(SurfaceFailureReason::BudgetExceeded, "Surface fixed workspace exceeds the budget.");
    SendProgress(onProgress, SurfaceDeterminationStage::ThresholdEstimation, 0.05);
    ResolvedParams params;
    params.roi = inputs.roi;
    reason = ResolveParams(volume, inputParams, getCancelled, params, result.message);
    if (reason != SurfaceFailureReason::None)
        return fail(reason, result.message);
    result.resolvedParams = inputParams;
    result.resolvedParams.targetViews = {};
    auto &resolved = result.resolvedParams;
    resolved.purpose = SurfaceContract::GetPurpose(inputParams);
    resolved.sourceVolume = result.sourceRevision;
    resolved.initialIsoValue = params.initialIsoValue;
    resolved.profileHalfLengthModel = params.profileHalfLengthModel;
    resolved.profileSampleStepModel = params.profileSampleStepModel;
    resolved.maximumOffsetModel = params.maximumOffsetModel;
    resolved.profileSmoothingSigmaModel = params.profileSmoothingSigmaModel;
    resolved.minimumEdgeWidthModel = params.minimumEdgeWidthModel;
    resolved.maximumEdgeWidthModel = params.maximumEdgeWidthModel;
    resolved.minimumEdgeSeparationModel = params.minimumEdgeSeparationModel;
    result.initialIsoValue = params.initialIsoValue;
    result.isoEstimate = params.isoEstimate;
    result.parameterFingerprint = 1469598103934665603ULL;
    AddFingerprint(result.parameterFingerprint, algorithmRevision);
    for (const unsigned char c : SurfaceRecipeCodec::BuildText(resolved))
        AddFingerprint(result.parameterFingerprint, c);
    AddFingerprint(result.parameterFingerprint, params.roi ? 1U : 0U);
    if (params.roi) {
        AddFingerprint(result.parameterFingerprint, roiSchemaVersion);
        for (const auto byte : params.roi->GetRevision().entityId.bytes)
            AddFingerprint(result.parameterFingerprint, byte);
        AddFingerprint(result.parameterFingerprint, params.roi->GetRevision().generation);
    }
    if (inputParams.method == SurfaceDeterminationMethod::AutomaticIso50)
    {
        if (!params.isoEstimate)
            return fail(SurfaceFailureReason::ThresholdUnreliable,
                        "Automatic ISO50 requires an estimated bimodal threshold.");
        result.status = SurfaceResultStatus::Succeeded;
        result.failureReason = SurfaceFailureReason::None;
        result.execution.estimatedWorkingBytes = result.requiredBytes;
        result.message = "Automatic ISO50 succeeded.";
        SendProgress(onProgress, SurfaceDeterminationStage::ThresholdEstimation, 1);
        return result;
    }
    double halo = params.profileHalfLengthModel + params.maximumOffsetModel;
    std::size_t sampleCount = static_cast<std::size_t>(std::ceil(2 * params.profileHalfLengthModel /
                                                                 params.profileSampleStepModel)) +
                              2;
    for (const auto &rule : params.regionOverrides)
    {
        const double half = rule.profileHalfLengthModel.value_or(params.profileHalfLengthModel);
        halo = std::max(halo, half + rule.maximumOffsetModel.value_or(params.maximumOffsetModel));
        sampleCount =
            std::max(sampleCount,
                     static_cast<std::size_t>(std::ceil(
                         2 * half / rule.profileSampleStepModel.value_or(params.profileSampleStepModel))) +
                         2);
    }
    // VTK SMP 的上界只用于 workspace 预算，不修改宿主的并发配置。
    std::size_t profileBytes = 0;
    if (!GetProduct(sampleCount,
                    2 * (8 * sizeof(double) + sizeof(SurfaceSampleStatus) + sizeof(std::uint32_t) +
                         sizeof(SurfaceEdgeCandidate)),
                    profileBytes) ||
        !GetProduct(profileBytes,
                    static_cast<std::size_t>(std::max(1, vtkSMPTools::GetEstimatedNumberOfThreads())),
                    profileBytes) ||
        profileBytes > maxWorkingBytes - result.requiredBytes)
        return fail(SurfaceFailureReason::BudgetExceeded,
                    "Surface parallel profile workspace exceeds the budget.");
    SurfaceSeedGrid grid;
    grid.extent = volume.geometry.extent;
    grid.dimensions = volume.geometry.dimensions;
    grid.origin = volume.geometry.origin;
    grid.indexToModel = volume.geometry.indexToModel;
    grid.modelToIndex = volume.geometry.modelToIndex;
    grid.values = volume.scalars.values;
    grid.readScalar = volume.scalars.read;
    grid.validity = volume.validity;
    grid.labels = volume.labels;
    grid.initialMesh = volume.initialMesh;
    const auto pairCount = std::max<std::size_t>(1, inputParams.materialPairs.size());
    for (std::size_t pairIndex = 0; pairIndex < pairCount; ++pairIndex)
    {
        params.materials = inputParams.materialPairs.empty() ? std::optional<SurfaceMaterialPair>{}
                                                             : inputParams.materialPairs[pairIndex];
        SurfaceInterfaceRecord interfaceRecord;
        if (params.materials)
        {
            interfaceRecord.materials = *params.materials;
            interfaceRecord.canonicalId =
                std::to_string(std::min(params.materials->materialA, params.materials->materialB)) + ":" +
                std::to_string(std::max(params.materials->materialA, params.materials->materialB));
        }
        else
            interfaceRecord.canonicalId = "gray";
        interfaceRecord.firstPoint = result.points.size();
        interfaceRecord.firstTriangle = result.triangleIndices.size() / 3;
        std::size_t retained = 0;
        if (!AddWorkingBytes(result.points.capacity(), sizeof(SurfacePointRecord), retained) ||
            !AddWorkingBytes(result.triangleIndices.capacity(), sizeof(std::uint32_t), retained) ||
            !AddWorkingBytes(result.triangleValidity.capacity(), sizeof(std::uint8_t), retained) ||
            !AddWorkingBytes(result.objects.capacity(), sizeof(SurfaceObjectRecord), retained) ||
            retained > maxWorkingBytes - profileBytes)
            return fail(SurfaceFailureReason::BudgetExceeded,
                        "Surface retained interfaces exceed the budget.");
        std::vector<Point3> meshPoints;
        std::vector<Triangle> meshTriangles;
        SurfaceExecutionStats statistics;
        SendProgress(onProgress, SurfaceDeterminationStage::SeedExtraction,
                     0.1 + 0.8 * pairIndex / pairCount);
        const auto seedStatus = SurfaceSeedBuilder::BuildMesh(
            grid, params.initialIsoValue, params.materials, params.roi, halo,
            inputParams.seedBlockDepth, maxWorkingBytes - retained - profileBytes, getCancelled, meshPoints,
            meshTriangles, statistics);
        result.requiredBytes =
            std::max(result.requiredBytes, statistics.estimatedWorkingBytes + retained + profileBytes);
        result.execution.scannedCellCount += statistics.scannedCellCount;
        result.execution.skippedCellCount += statistics.skippedCellCount;
        result.execution.blockCount += statistics.blockCount;
        result.execution.processedExtent = statistics.processedExtent;
        result.execution.seedMs += statistics.seedMs;
        if (seedStatus != SurfaceSeedStatus::Succeeded)
        {
            if (seedStatus == SurfaceSeedStatus::NoSurface && params.materials)
            {
                result.interfaces.push_back(interfaceRecord);
                continue;
            }
            return fail(seedStatus == SurfaceSeedStatus::Cancelled ? SurfaceFailureReason::Cancelled
                        : seedStatus == SurfaceSeedStatus::BudgetExceeded
                            ? SurfaceFailureReason::BudgetExceeded
                        : seedStatus == SurfaceSeedStatus::NoSurface ? SurfaceFailureReason::NoSurface
                                                                     : SurfaceFailureReason::InvalidGeometry,
                        "Surface seed extraction failed (status=" +
                            std::to_string(static_cast<unsigned>(seedStatus)) + ").");
        }
        if (meshPoints.size() > UINT32_MAX - result.points.size())
            return fail(SurfaceFailureReason::BudgetExceeded, "Surface output exceeds uint32 topology.");
        const auto started = std::chrono::steady_clock::now();
        SendProgress(onProgress, SurfaceDeterminationStage::SubvoxelRefinement,
                     .1 + .8 * (pairIndex + .25) / pairCount);
        auto components =
            BuildComponents(meshPoints, meshTriangles, volume.geometry.voxelVolume, getCancelled);
        const auto selected = GetSelectedComponents(components, meshPoints, params, getCancelled);
        for (const auto selectedIndex : selected)
        {
            if (GetCancelled(getCancelled))
                return fail(SurfaceFailureReason::Cancelled, "Surface refinement was cancelled.");
            std::vector<Point3> points;
            std::vector<Triangle> triangles;
            GetLocalMesh(components[selectedIndex], meshPoints, points, triangles, getCancelled);
            SetInterfaceWinding(volume, params, points, triangles);
            const auto normals = GetVertexNormals(points, triangles, getCancelled);
            std::vector<SurfacePointRecord> records(points.size());
            const double cornerCosine = std::cos(params.sharpCornerAngleDeg * 0.5 * 3.141592653589793 / 180);
            for (const auto &triangle : triangles)
            {
                const auto &ids = triangle.vertices;
                auto face =
                    Cross(Subtract(points[ids[1]], points[ids[0]]), Subtract(points[ids[2]], points[ids[0]]));
                if (!Normalize(face))
                    continue;
                for (auto id : ids)
                    if (Dot(face, normals[id]) < cornerCosine)
                        records[id].flags |= SurfacePointFlags::SharpCorner;
            }
            vtkSMPThreadLocal<SurfaceProfileWorkspace> workspaces;
            std::atomic<bool> cancelled{false};
            vtkSMPTools::For(
                0, static_cast<vtkIdType>(points.size()), 128, [&](vtkIdType begin, vtkIdType end) {
                    auto &workspace = workspaces.Local();
                    workspace.Reserve(sampleCount);
                    for (auto i = begin; i < end; ++i)
                    {
                        if (cancelled.load(std::memory_order_acquire))
                            return;
                        if (((i - begin) & 31) == 0 && GetCancelled(getCancelled))
                        {
                            cancelled.store(true, std::memory_order_release);
                            return;
                        }
                        const auto id = static_cast<std::size_t>(i);
                        SetRefinedPoint(volume, params, points[id], normals[id], records[id], workspace);
                        records[id].interfaceIndex = static_cast<std::uint32_t>(pairIndex);
                    }
                });
            if (cancelled.load())
                return fail(SurfaceFailureReason::Cancelled, "Surface refinement was cancelled.");
            for (const auto &t : triangles)
            {
                const auto &ids = t.vertices;
                if (records[ids[0]].overrideIndex != records[ids[1]].overrideIndex ||
                    records[ids[0]].overrideIndex != records[ids[2]].overrideIndex)
                    for (auto id : ids)
                        records[id].flags |= SurfacePointFlags::OverrideBoundary;
            }
            for (auto &workspace : workspaces)
            {
                result.execution.sampledProfileCount += workspace.sampledProfileCount;
                result.execution.sampledValueCount += workspace.sampledValueCount;
            }
            SendProgress(onProgress, SurfaceDeterminationStage::TopologyValidation,
                         .1 + .8 * (pairIndex + .85) / pairCount);
            AddObjectResult(volume, params, static_cast<std::uint32_t>(result.objects.size()), points,
                            triangles, std::move(records), result, getCancelled);
            result.objects.back().interfaceIndex = static_cast<std::uint32_t>(pairIndex);
        }
        result.execution.refinementMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        interfaceRecord.pointCount = result.points.size() - interfaceRecord.firstPoint;
        interfaceRecord.triangleCount = result.triangleIndices.size() / 3 - interfaceRecord.firstTriangle;
        result.interfaces.push_back(std::move(interfaceRecord));
    }
    if (GetCancelled(getCancelled))
        return fail(SurfaceFailureReason::Cancelled, "Surface validation was cancelled.");
    if (result.points.empty() || result.triangleIndices.empty())
        return fail(SurfaceFailureReason::NoSurface, "Surface output is empty.");
    result.execution.estimatedWorkingBytes = result.requiredBytes;
    result.execution.retainedBytes = result.points.capacity() * sizeof(SurfacePointRecord) +
                                     result.triangleIndices.capacity() * sizeof(std::uint32_t) +
                                     result.triangleValidity.capacity() +
                                     result.objects.capacity() * sizeof(SurfaceObjectRecord);
    result.status = SurfaceResultStatus::Succeeded;
    result.failureReason = SurfaceFailureReason::None;
    result.message = "Surface determination succeeded: points=" + std::to_string(result.points.size()) +
                     ", accepted=" + std::to_string(result.acceptedPointCount) + ".";
    SendProgress(onProgress, SurfaceDeterminationStage::TopologyValidation, 1);
    return result;
}

} // namespace

SurfaceAlgorithmResult SurfaceDeterminationAlgorithm::BuildSurface(
    const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &params,
    const std::size_t maxWorkingBytes, const SurfaceCancelCheck &getCancelled,
    const SurfaceProgressCallback &onProgress, const SurfaceAlgorithmInputs &inputs)
{
    try {
        return BuildSurfaceImpl(source, params, maxWorkingBytes, getCancelled, onProgress, inputs);
    }
    catch (const SurfaceCancelled &)
    {
        SurfaceAlgorithmResult result;
        result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
        result.status = SurfaceResultStatus::Cancelled;
        result.failureReason = SurfaceFailureReason::Cancelled;
        result.message = "Surface topology operation was cancelled.";
        return result;
    }
    catch (const std::bad_alloc&) {
        SurfaceAlgorithmResult result;
        result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
        result.method = params.method;
        result.failureReason = SurfaceFailureReason::BudgetExceeded;
        result.message = "Surface allocation failed within the configured budget.";
        return result;
    }
    catch (...) {
        SurfaceAlgorithmResult result;
        result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
        result.method = params.method;
        result.failureReason = SurfaceFailureReason::InternalError;
        result.message = "Surface determination raised an internal error.";
        return result;
    }
}

SurfaceProfileDiagnostic SurfaceDeterminationAlgorithm::GetProfileDiagnostic(
    const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &resolved,
    const SurfacePointRecord &point, const SurfaceAlgorithmInputs &inputs)
{
    SurfaceProfileDiagnostic diagnostic;
    try
    {
        VolumeView volume;
        if (BuildVolumeView(source, volume, diagnostic.message) != SurfaceFailureReason::None)
            return diagnostic;
        if (SetAdditionalInputs(source, resolved, inputs, volume, diagnostic.message) !=
            SurfaceFailureReason::None)
            return diagnostic;
        if (!resolved.initialIsoValue)
        {
            diagnostic.message = "Diagnostic replay requires a resolved recipe.";
            return diagnostic;
        }
        ResolvedParams params;
        params.roi = inputs.roi;
        if (ResolveParams(volume, resolved, {}, params, diagnostic.message) != SurfaceFailureReason::None)
            return diagnostic;
        if (!resolved.materialPairs.empty())
        {
            if (point.interfaceIndex >= resolved.materialPairs.size())
            {
                diagnostic.message = "Invalid interface index.";
                return diagnostic;
            }
            params.materials = resolved.materialPairs[point.interfaceIndex];
        }
        SurfacePointRecord replay;
        SurfaceProfileWorkspace workspace;
        const Point3 normal{point.seedNormalModel[0], point.seedNormalModel[1], point.seedNormalModel[2]};
        SetRefinedPoint(volume, params, point.seedPositionModel, normal, replay, workspace, &diagnostic);
        // 拓扑/覆盖区边界是网格级判定；保留发布记录，使调用方能区分局部拟合与最终质量。
        diagnostic.point = point;
        diagnostic.message = "Profile replay uses the frozen source, resolved recipe and original seed; "
                             "point includes final mesh quality flags.";
    }
    catch (const std::bad_alloc &)
    {
        diagnostic = {};
        diagnostic.message = "Diagnostic workspace allocation failed.";
    }
    catch (...)
    {
        diagnostic = {};
        diagnostic.message = "Diagnostic replay failed.";
    }
    return diagnostic;
}

SurfaceFailureReason SurfaceDeterminationAlgorithm::GetInputFailure(
    const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &params,
    const SurfaceAlgorithmInputs &inputs)
{
    try
    {
        VolumeView volume;
        std::string message;
        const auto reason = BuildVolumeView(source, volume, message);
        return reason == SurfaceFailureReason::None
                   ? SetAdditionalInputs(source, params, inputs, volume, message)
                   : reason;
    }
    catch (...)
    {
        return SurfaceFailureReason::InvalidSource;
    }
}
