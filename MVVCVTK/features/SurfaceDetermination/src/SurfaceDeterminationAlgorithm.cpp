#include "SurfaceDeterminationAlgorithm.h"
#include "Data/DataPayloads.h"

#include <vtkBox.h>
#include <vtkCellArray.h>
#include <vtkClipPolyData.h>
#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkIdList.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSMPTools.h>
#include <vtkTriangleFilter.h>
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

constexpr std::uint32_t algorithmRevision = 1;
constexpr std::size_t histogramBinCount = 512;
constexpr double geometryEpsilon = 1.0e-12;
constexpr double qualityRatioThreshold = 0.5;
constexpr std::size_t maxProfileSampleCount = 4097;

struct Triangle final {
    std::array<std::uint32_t, 3> vertices{};
};

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

struct ScalarView final {
    const void* values = nullptr;
    std::size_t valueCount = 0;
    int vtkType = VTK_VOID;

    double GetValue(const std::size_t index) const noexcept
    {
        if (!values || index >= valueCount) return 0.0;
        switch (vtkType) {
        case VTK_CHAR:
            return static_cast<double>(
                static_cast<const char*>(values)[index]);
        case VTK_SIGNED_CHAR:
            return static_cast<double>(
                static_cast<const signed char*>(values)[index]);
        case VTK_UNSIGNED_CHAR:
            return static_cast<double>(
                static_cast<const unsigned char*>(values)[index]);
        case VTK_SHORT:
            return static_cast<double>(
                static_cast<const short*>(values)[index]);
        case VTK_UNSIGNED_SHORT:
            return static_cast<double>(
                static_cast<const unsigned short*>(values)[index]);
        case VTK_INT:
            return static_cast<double>(
                static_cast<const int*>(values)[index]);
        case VTK_UNSIGNED_INT:
            return static_cast<double>(
                static_cast<const unsigned int*>(values)[index]);
        case VTK_LONG:
            return static_cast<double>(
                static_cast<const long*>(values)[index]);
        case VTK_UNSIGNED_LONG:
            return static_cast<double>(
                static_cast<const unsigned long*>(values)[index]);
        case VTK_LONG_LONG:
            return static_cast<double>(
                static_cast<const long long*>(values)[index]);
        case VTK_UNSIGNED_LONG_LONG:
            return static_cast<double>(
                static_cast<const unsigned long long*>(values)[index]);
        case VTK_FLOAT:
            return static_cast<double>(
                static_cast<const float*>(values)[index]);
        case VTK_DOUBLE:
            return static_cast<const double*>(values)[index];
        default:
            return 0.0;
        }
    }
};

struct VolumeView final {
    ImageGeometry geometry;
    ScalarView scalars;
    const unsigned char* validity = nullptr;
    std::size_t voxelCount = 0;
};

struct ResolvedParams final {
    SurfaceDeterminationMethod method =
        SurfaceDeterminationMethod::LocalAdaptiveIso50;
    SurfaceComponentSelection componentSelection =
        SurfaceComponentSelection::Largest;
    double initialIsoValue = 0.0;
    bool isAutomaticIso = false;
    std::optional<Point3> seedModelPoint;
    std::optional<std::array<double, 6>> roiModelBounds;
    double profileHalfLengthModel = 0.0;
    double profileSampleStepModel = 0.0;
    double maximumOffsetModel = 0.0;
    double profileSmoothingSigmaModel = 0.0;
    std::uint64_t minimumObjectVoxels = 1;
    double minimumContrast = 0.0;
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

struct Profile final {
    std::vector<double> offsets;
    std::vector<double> values;
    double step = 0.0;
    double validRatio = 0.0;
    bool isClipped = false;
    bool hasInvalidSupport = false;
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
        if (dimensions[axis] < 2
            || extentSize != dimensions[axis]
            || sourceGeometry.dimensions[axis] != dimensions[axis]
            || !std::isfinite(spacing[axis])
            || spacing[axis] <= 0.0
            || !std::isfinite(origin[axis])
            || spacing[axis] != sourceGeometry.spacing[axis]
            || origin[axis] != sourceGeometry.origin[axis]
            || !GetProduct(
                voxelCount,
                static_cast<std::size_t>(dimensions[axis]),
                voxelCount)) {
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
            if (!std::isfinite(value)) {
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

bool GetBoundsValid(const std::array<double, 6>& bounds)
{
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double minimum = bounds[axis * 2];
        const double maximum = bounds[axis * 2 + 1];
        if (!std::isfinite(minimum)
            || !std::isfinite(maximum)
            || minimum >= maximum) {
            return false;
        }
    }
    return true;
}

bool GetPointInBounds(
    const Point3& point,
    const std::array<double, 6>& bounds,
    const double tolerance = 0.0)
{
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (point[axis] < bounds[axis * 2] - tolerance
            || point[axis] > bounds[axis * 2 + 1] + tolerance) {
            return false;
        }
    }
    return true;
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

double GetMedian(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    const std::size_t middle = values.size() / 2;
    std::nth_element(
        values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if ((values.size() & 1U) != 0U) return upper;
    const double lower = *std::max_element(
        values.begin(), values.begin() + middle);
    return 0.5 * (lower + upper);
}

double GetMad(
    const std::vector<double>& values,
    const double median)
{
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values) {
        deviations.push_back(std::abs(value - median));
    }
    return 1.4826 * GetMedian(std::move(deviations));
}

std::vector<double> GetSmoothedValues(
    const std::vector<double>& values,
    const double sigma,
    const double step)
{
    if (values.size() < 3 || sigma <= geometryEpsilon
        || step <= geometryEpsilon) {
        return values;
    }
    const std::size_t radius = std::min<std::size_t>(
        8,
        std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::ceil(3.0 * sigma / step))));
    std::vector<double> weights(radius + 1, 0.0);
    for (std::size_t offset = 0; offset <= radius; ++offset) {
        const double distance = static_cast<double>(offset) * step;
        weights[offset] = std::exp(
            -0.5 * distance * distance / (sigma * sigma));
    }
    std::vector<double> smoothed(values.size(), 0.0);
    for (std::size_t index = 0; index < values.size(); ++index) {
        double weighted = 0.0;
        double totalWeight = 0.0;
        const std::size_t begin = index > radius ? index - radius : 0;
        const std::size_t end = std::min(
            values.size() - 1, index + radius);
        for (std::size_t sample = begin; sample <= end; ++sample) {
            const std::size_t offset = sample > index
                ? sample - index : index - sample;
            weighted += values[sample] * weights[offset];
            totalWeight += weights[offset];
        }
        smoothed[index] = weighted / totalWeight;
    }
    return smoothed;
}

Profile BuildProfile(
    const VolumeView& volume,
    const Point3& center,
    const Point3& normal,
    const ResolvedParams& params)
{
    Profile profile;
    std::size_t intervalCount = static_cast<std::size_t>(std::ceil(
        2.0 * params.profileHalfLengthModel
        / params.profileSampleStepModel));
    intervalCount = std::max<std::size_t>(8, intervalCount);
    if ((intervalCount & 1U) != 0U) ++intervalCount;
    if (intervalCount + 1 > maxProfileSampleCount) {
        profile.isClipped = true;
        return profile;
    }
    profile.step = 2.0 * params.profileHalfLengthModel
        / static_cast<double>(intervalCount);
    profile.offsets.reserve(intervalCount + 1);
    profile.values.reserve(intervalCount + 1);
    std::size_t validCount = 0;
    for (std::size_t index = 0; index <= intervalCount; ++index) {
        const double offset = -params.profileHalfLengthModel
            + static_cast<double>(index) * profile.step;
        const ScalarSample sample = GetScalarAtModel(
            volume, Add(center, Scale(normal, offset)));
        profile.offsets.push_back(offset);
        profile.values.push_back(sample.value);
        if (sample.status == SampleStatus::Valid) ++validCount;
        else if (sample.status == SampleStatus::Clipped) {
            profile.isClipped = true;
        }
        else {
            profile.hasInvalidSupport = true;
        }
    }
    profile.validRatio = static_cast<double>(validCount)
        / static_cast<double>(intervalCount + 1);
    if (!profile.isClipped && !profile.hasInvalidSupport) {
        profile.values = GetSmoothedValues(
            profile.values,
            params.profileSmoothingSigmaModel,
            profile.step);
    }
    return profile;
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
    if (!params.roiModelBounds) return true;
    const Point3 point = GetModelPoint(
        volume.geometry,
        Point3{
            static_cast<double>(x),
            static_cast<double>(y),
            static_cast<double>(z)
        });
    return GetPointInBounds(point, *params.roiModelBounds);
}

SurfaceFailureReason GetAutomaticIso(
    const VolumeView& volume,
    const ResolvedParams& params,
    const SurfaceCancelCheck& getCancelled,
    double& isoValue,
    std::string& message)
{
    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    std::uint64_t validCount = 0;
    const auto& extent = volume.geometry.extent;
    for (std::int64_t zValue = extent[4];
        zValue <= static_cast<std::int64_t>(extent[5]); ++zValue) {
        const int z = static_cast<int>(zValue);
        if (GetCancelled(getCancelled)) {
            message = "Surface threshold estimation was cancelled.";
            return SurfaceFailureReason::Cancelled;
        }
        for (std::int64_t yValue = extent[2];
            yValue <= static_cast<std::int64_t>(extent[3]); ++yValue) {
            const int y = static_cast<int>(yValue);
            for (std::int64_t xValue = extent[0];
                xValue <= static_cast<std::int64_t>(extent[1]); ++xValue) {
                const int x = static_cast<int>(xValue);
                const std::size_t tupleIndex = GetTupleIndex(
                    volume.geometry, x, y, z);
                if (!GetVoxelUsed(
                        volume, params, x, y, z, tupleIndex)) {
                    continue;
                }
                const double value = volume.scalars.GetValue(tupleIndex);
                if (!std::isfinite(value)) continue;
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
                ++validCount;
            }
        }
    }
    if (validCount < 64 || !std::isfinite(minimum)
        || !std::isfinite(maximum)
        || maximum - minimum <= geometryEpsilon
            * std::max({ 1.0, std::abs(minimum), std::abs(maximum) })) {
        message = "Surface automatic ISO50 requires a non-degenerate bimodal histogram.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    std::array<std::uint64_t, histogramBinCount> histogram{};
    const double scale = static_cast<double>(histogramBinCount - 1)
        / (maximum - minimum);
    for (std::int64_t zValue = extent[4];
        zValue <= static_cast<std::int64_t>(extent[5]); ++zValue) {
        const int z = static_cast<int>(zValue);
        if (GetCancelled(getCancelled)) {
            message = "Surface threshold estimation was cancelled.";
            return SurfaceFailureReason::Cancelled;
        }
        for (std::int64_t yValue = extent[2];
            yValue <= static_cast<std::int64_t>(extent[3]); ++yValue) {
            const int y = static_cast<int>(yValue);
            for (std::int64_t xValue = extent[0];
                xValue <= static_cast<std::int64_t>(extent[1]); ++xValue) {
                const int x = static_cast<int>(xValue);
                const std::size_t tupleIndex = GetTupleIndex(
                    volume.geometry, x, y, z);
                if (!GetVoxelUsed(
                        volume, params, x, y, z, tupleIndex)) {
                    continue;
                }
                const double value = volume.scalars.GetValue(tupleIndex);
                if (!std::isfinite(value)) continue;
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
    if (!selected) {
        message = "Surface automatic ISO50 could not identify two reliable peaks.";
        return SurfaceFailureReason::ThresholdUnreliable;
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
    return SurfaceFailureReason::None;
}

std::uint64_t GetDoubleBits(const double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
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

std::uint64_t GetFingerprint(const ResolvedParams& params)
{
    std::uint64_t fingerprint = 1469598103934665603ULL;
    AddFingerprint(fingerprint, static_cast<std::uint64_t>(params.method));
    AddFingerprint(
        fingerprint,
        static_cast<std::uint64_t>(params.componentSelection));
    AddFingerprint(fingerprint, GetDoubleBits(params.initialIsoValue));
    AddFingerprint(fingerprint, params.isAutomaticIso ? 1U : 0U);
    AddFingerprint(fingerprint, GetDoubleBits(params.profileHalfLengthModel));
    AddFingerprint(fingerprint, GetDoubleBits(params.profileSampleStepModel));
    AddFingerprint(fingerprint, GetDoubleBits(params.maximumOffsetModel));
    AddFingerprint(
        fingerprint,
        GetDoubleBits(params.profileSmoothingSigmaModel));
    AddFingerprint(fingerprint, params.minimumObjectVoxels);
    AddFingerprint(fingerprint, GetDoubleBits(params.minimumContrast));
    AddFingerprint(fingerprint, params.seedModelPoint ? 1U : 0U);
    if (params.seedModelPoint) {
        for (const double value : *params.seedModelPoint) {
            AddFingerprint(fingerprint, GetDoubleBits(value));
        }
    }
    AddFingerprint(fingerprint, params.roiModelBounds ? 1U : 0U);
    if (params.roiModelBounds) {
        for (const double value : *params.roiModelBounds) {
            AddFingerprint(fingerprint, GetDoubleBits(value));
        }
    }
    return fingerprint;
}

SurfaceFailureReason ResolveParams(
    const VolumeView& volume,
    const SurfaceDeterminationStartParams& input,
    const SurfaceCancelCheck& getCancelled,
    ResolvedParams& params,
    std::string& message)
{
    params.method = input.method;
    params.componentSelection = input.componentSelection;
    params.seedModelPoint = input.seedModelPoint;
    params.roiModelBounds = input.roiModelBounds;
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
    if (params.roiModelBounds) {
        if (!GetBoundsValid(*params.roiModelBounds)
            || !GetBoundsIntersect(
                *params.roiModelBounds,
                GetDataBounds(volume.geometry))) {
            message = "Surface ROI does not intersect the source data.";
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
        if (!std::isfinite(value) || value <= 0.0) {
            message = "Surface profile parameters must be finite and positive.";
            return SurfaceFailureReason::InvalidGeometry;
        }
    }
    if (params.maximumOffsetModel > params.profileHalfLengthModel
        || 2.0 * params.profileHalfLengthModel
            / params.profileSampleStepModel
            > static_cast<double>(maxProfileSampleCount - 1)) {
        message = "Surface profile bounds are inconsistent or exceed the sample limit.";
        return SurfaceFailureReason::InvalidGeometry;
    }

    params.isAutomaticIso = !input.initialIsoValue.has_value();
    if (input.initialIsoValue) {
        if (!std::isfinite(*input.initialIsoValue)) {
            message = "Surface initial ISO value is not finite.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        params.initialIsoValue = *input.initialIsoValue;
        return SurfaceFailureReason::None;
    }
    return GetAutomaticIso(
        volume, params, getCancelled, params.initialIsoValue, message);
}

bool GetBudgetEstimate(
    const VolumeView& volume,
    std::size_t& requiredBytes)
{
    // FlyingEdges 最坏输出依赖数据；以每体素 64 字节作为启动前保守门禁，
    // 后续在取得真实 point/cell 数后再次核对预算。
    return GetProduct(volume.voxelCount, 64U, requiredBytes)
        && GetSum(requiredBytes, histogramBinCount * sizeof(std::uint64_t),
            requiredBytes);
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

SurfaceFailureReason BuildInitialMesh(
    vtkImageData& image,
    const ResolvedParams& params,
    const SurfaceCancelCheck& getCancelled,
    std::vector<Point3>& points,
    std::vector<Triangle>& triangles,
    std::string& message)
{
    if (GetCancelled(getCancelled)) {
        message = "Surface seed extraction was cancelled.";
        return SurfaceFailureReason::Cancelled;
    }

    vtkNew<vtkFlyingEdges3D> surface;
    surface->SetInputData(&image);
    surface->SetValue(0, params.initialIsoValue);
    surface->ComputeNormalsOff();
    surface->ComputeGradientsOff();

    vtkNew<vtkTriangleFilter> triangleFilter;
    vtkNew<vtkBox> roiBox;
    vtkNew<vtkClipPolyData> roiClip;
    if (params.roiModelBounds) {
        roiBox->SetBounds(params.roiModelBounds->data());
        roiClip->SetInputConnection(surface->GetOutputPort());
        roiClip->SetClipFunction(roiBox);
        roiClip->InsideOutOn();
        roiClip->GenerateClippedOutputOff();
        triangleFilter->SetInputConnection(roiClip->GetOutputPort());
    }
    else {
        triangleFilter->SetInputConnection(surface->GetOutputPort());
    }
    triangleFilter->PassLinesOff();
    triangleFilter->PassVertsOff();
    triangleFilter->Update();

    if (GetCancelled(getCancelled)) {
        message = "Surface seed extraction was cancelled.";
        return SurfaceFailureReason::Cancelled;
    }
    auto* output = triangleFilter->GetOutput();
    if (!output || !output->GetPoints() || !output->GetPolys()
        || output->GetNumberOfPoints() <= 0
        || output->GetNumberOfPolys() <= 0) {
        message = "Surface initial ISO did not produce a surface.";
        return SurfaceFailureReason::NoSurface;
    }
    if (static_cast<unsigned long long>(output->GetNumberOfPoints())
        > std::numeric_limits<std::uint32_t>::max()) {
        message = "Surface point count exceeds the uint32 topology limit.";
        return SurfaceFailureReason::BudgetExceeded;
    }

    points.resize(static_cast<std::size_t>(output->GetNumberOfPoints()));
    for (std::size_t index = 0; index < points.size(); ++index) {
        output->GetPoint(static_cast<vtkIdType>(index), points[index].data());
        if (!std::all_of(
                points[index].begin(), points[index].end(),
                [](const double value) { return std::isfinite(value); })) {
            message = "Surface seed mesh contains a non-finite point.";
            return SurfaceFailureReason::InvalidGeometry;
        }
    }

    vtkNew<vtkIdList> pointIds;
    auto* cells = output->GetPolys();
    cells->InitTraversal();
    while (cells->GetNextCell(pointIds)) {
        if (pointIds->GetNumberOfIds() != 3) continue;
        Triangle triangle;
        bool isValid = true;
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            const vtkIdType pointId = pointIds->GetId(
                static_cast<vtkIdType>(vertex));
            if (pointId < 0
                || static_cast<unsigned long long>(pointId)
                    >= points.size()) {
                isValid = false;
                break;
            }
            triangle.vertices[vertex] = static_cast<std::uint32_t>(pointId);
        }
        if (isValid
            && triangle.vertices[0] != triangle.vertices[1]
            && triangle.vertices[1] != triangle.vertices[2]
            && triangle.vertices[2] != triangle.vertices[0]) {
            triangles.push_back(triangle);
        }
    }
    if (triangles.empty()) {
        message = "Surface seed mesh does not contain valid triangles.";
        return SurfaceFailureReason::NoSurface;
    }
    return SurfaceFailureReason::None;
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

TopologyMetrics GetTopologyMetrics(
    const std::vector<Point3>& points,
    const std::vector<Triangle>& triangles)
{
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
            const std::uint32_t from = ids[edge];
            const std::uint32_t to = ids[(edge + 1) % 3];
            auto& record = edges[GetEdgeKey(from, to)];
            ++record.count;
            record.directionSum += from < to ? 1 : -1;
        }
    }
    for (const auto& item : edges) {
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

std::vector<MeshComponent> BuildComponents(
    const std::vector<Point3>& points,
    const std::vector<Triangle>& triangles,
    const double voxelVolume)
{
    DisjointSet sets(points.size());
    for (const Triangle& triangle : triangles) {
        sets.SetJoined(triangle.vertices[0], triangle.vertices[1]);
        sets.SetJoined(triangle.vertices[1], triangle.vertices[2]);
    }
    std::map<std::uint32_t, std::vector<Triangle>> groupedTriangles;
    for (const Triangle& triangle : triangles) {
        groupedTriangles[sets.GetRoot(triangle.vertices[0])]
            .push_back(triangle);
    }

    std::vector<MeshComponent> components;
    components.reserve(groupedTriangles.size());
    for (auto& item : groupedTriangles) {
        MeshComponent component;
        component.triangles = std::move(item.second);
        std::vector<std::uint32_t> pointIds;
        pointIds.reserve(component.triangles.size());
        for (const Triangle& triangle : component.triangles) {
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
            remap.emplace(
                component.sourcePointIds[index],
                static_cast<std::uint32_t>(index));
            localPoints.push_back(points[component.sourcePointIds[index]]);
        }
        std::vector<Triangle> localTriangles = component.triangles;
        for (Triangle& triangle : localTriangles) {
            for (std::uint32_t& pointId : triangle.vertices) {
                pointId = remap.at(pointId);
            }
        }
        const TopologyMetrics topology = GetTopologyMetrics(
            localPoints, localTriangles);
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

double GetComponentDistanceSquared(
    const MeshComponent& component,
    const std::vector<Point3>& points,
    const Point3& seed)
{
    double distance = std::numeric_limits<double>::max();
    for (const std::uint32_t pointId : component.sourcePointIds) {
        const Point3 delta = Subtract(points[pointId], seed);
        distance = std::min(distance, Dot(delta, delta));
    }
    return distance;
}

std::vector<std::size_t> GetSelectedComponents(
    const std::vector<MeshComponent>& components,
    const std::vector<Point3>& points,
    const ResolvedParams& params)
{
    std::vector<std::size_t> eligible;
    for (std::size_t index = 0; index < components.size(); ++index) {
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
            [&components, &points, &seed](
                const std::size_t left,
                const std::size_t right) {
                const double leftDistance = GetComponentDistanceSquared(
                    components[left], points, seed);
                const double rightDistance = GetComponentDistanceSquared(
                    components[right], points, seed);
                if (leftDistance != rightDistance) {
                    return leftDistance < rightDistance;
                }
                return components[left].minimumPointId
                    < components[right].minimumPointId;
            });
        return { *selected };
    }
    return eligible;
}

void GetLocalMesh(
    const MeshComponent& component,
    const std::vector<Point3>& sourcePoints,
    std::vector<Point3>& points,
    std::vector<Triangle>& triangles)
{
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    remap.reserve(component.sourcePointIds.size());
    points.reserve(component.sourcePointIds.size());
    for (std::size_t index = 0;
        index < component.sourcePointIds.size(); ++index) {
        const std::uint32_t sourceId = component.sourcePointIds[index];
        remap.emplace(sourceId, static_cast<std::uint32_t>(index));
        points.push_back(sourcePoints[sourceId]);
    }
    triangles = component.triangles;
    for (Triangle& triangle : triangles) {
        for (std::uint32_t& pointId : triangle.vertices) {
            pointId = remap.at(pointId);
        }
    }
}

std::vector<Point3> GetVertexNormals(
    const std::vector<Point3>& points,
    const std::vector<Triangle>& triangles)
{
    std::vector<Point3> normals(points.size(), Point3{});
    for (const Triangle& triangle : triangles) {
        const Point3 normal = Cross(
            Subtract(
                points[triangle.vertices[1]],
                points[triangle.vertices[0]]),
            Subtract(
                points[triangle.vertices[2]],
                points[triangle.vertices[0]]));
        for (const std::uint32_t pointId : triangle.vertices) {
            normals[pointId] = Add(normals[pointId], normal);
        }
    }
    for (Point3& normal : normals) {
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

bool GetFatalPointFlags(const SurfacePointFlags flags)
{
    constexpr SurfacePointFlags fatal =
        SurfacePointFlags::LowContrast
        | SurfacePointFlags::InvalidSupport
        | SurfacePointFlags::ProfileClipped
        | SurfacePointFlags::ExcessiveOffset
        | SurfacePointFlags::FitRejected
        | SurfacePointFlags::TriangleFlipRisk;
    return (flags & fatal) != SurfacePointFlags::None;
}

double GetLineResidual(
    const Profile& profile,
    const std::size_t leftIndex)
{
    if (profile.values.size() < 4
        || leftIndex == 0
        || leftIndex + 2 >= profile.values.size()) {
        return 0.0;
    }
    const double slope = (profile.values[leftIndex + 1]
        - profile.values[leftIndex]) / profile.step;
    const double beforePrediction = profile.values[leftIndex]
        - slope * profile.step;
    const double afterPrediction = profile.values[leftIndex + 1]
        + slope * profile.step;
    const double beforeResidual = profile.values[leftIndex - 1]
        - beforePrediction;
    const double afterResidual = profile.values[leftIndex + 2]
        - afterPrediction;
    return std::sqrt(0.5 * (
        beforeResidual * beforeResidual
        + afterResidual * afterResidual));
}

bool GetPlateauValues(
    const Profile& profile,
    std::vector<double>& insideValues,
    std::vector<double>& outsideValues)
{
    if (profile.values.size() < 9) return false;
    const std::size_t plateauCount = std::max<std::size_t>(
        2, profile.values.size() / 5U);
    insideValues.assign(
        profile.values.begin(),
        profile.values.begin() + static_cast<std::ptrdiff_t>(plateauCount));
    outsideValues.assign(
        profile.values.end() - static_cast<std::ptrdiff_t>(plateauCount),
        profile.values.end());
    return true;
}

bool GetLocalAdaptiveOffset(
    const Profile& profile,
    const double threshold,
    double& offset,
    std::uint32_t& crossingCount,
    double& gradientMagnitude,
    double& fitResidual)
{
    std::optional<std::size_t> selected;
    double selectedDistance = std::numeric_limits<double>::max();
    crossingCount = 0;
    for (std::size_t index = 0;
        index + 1 < profile.values.size(); ++index) {
        const double left = profile.values[index];
        const double right = profile.values[index + 1];
        const bool hasCrossing =
            ((left >= threshold && right < threshold)
                || (left <= threshold && right > threshold))
            && std::abs(left - right) > geometryEpsilon;
        if (hasCrossing) ++crossingCount;
        if (left >= threshold && right < threshold
            && std::abs(left - right) > geometryEpsilon) {
            const double fraction = (threshold - left) / (right - left);
            const double candidate = profile.offsets[index]
                + fraction * profile.step;
            const double distance = std::abs(candidate);
            if (distance < selectedDistance) {
                selected = index;
                selectedDistance = distance;
                offset = candidate;
            }
        }
    }
    if (!selected) return false;
    gradientMagnitude = std::abs(
        profile.values[*selected + 1] - profile.values[*selected])
        / profile.step;
    fitResidual = GetLineResidual(profile, *selected);
    return std::isfinite(offset) && std::isfinite(gradientMagnitude);
}

bool GetGradientPeakOffset(
    const Profile& profile,
    const double maximumOffset,
    double& offset,
    std::uint32_t& peakCount,
    double& gradientMagnitude,
    double& fitResidual,
    double& localThreshold)
{
    if (profile.values.size() < 7) return false;
    std::vector<double> derivatives(profile.values.size(), 0.0);
    for (std::size_t index = 1;
        index + 1 < profile.values.size(); ++index) {
        derivatives[index] = std::abs(
            profile.values[index + 1] - profile.values[index - 1])
            / (2.0 * profile.step);
    }
    std::optional<std::size_t> selected;
    double maximum = 0.0;
    for (std::size_t index = 2;
        index + 2 < derivatives.size(); ++index) {
        if (std::abs(profile.offsets[index]) > maximumOffset
            || derivatives[index] < derivatives[index - 1]
            || derivatives[index] < derivatives[index + 1]) {
            continue;
        }
        if (derivatives[index] > maximum) {
            maximum = derivatives[index];
            selected = index;
        }
    }
    if (!selected || maximum <= geometryEpsilon) return false;

    peakCount = 0;
    for (std::size_t index = 2;
        index + 2 < derivatives.size(); ++index) {
        if (std::abs(profile.offsets[index]) <= maximumOffset
            && derivatives[index] >= 0.5 * maximum
            && derivatives[index] >= derivatives[index - 1]
            && derivatives[index] >= derivatives[index + 1]) {
            ++peakCount;
        }
    }
    const std::size_t index = *selected;
    const double before = derivatives[index - 1];
    const double center = derivatives[index];
    const double after = derivatives[index + 1];
    const double denominator = before - 2.0 * center + after;
    double sampleOffset = 0.0;
    if (std::abs(denominator) > geometryEpsilon) {
        sampleOffset = 0.5 * (before - after) / denominator;
        sampleOffset = std::clamp(sampleOffset, -1.0, 1.0);
    }
    offset = profile.offsets[index] + sampleOffset * profile.step;
    gradientMagnitude = center
        - 0.25 * (before - after) * sampleOffset;
    fitResidual = std::abs(before - after)
        / std::max(center, geometryEpsilon);
    const double valueSlope = 0.5
        * (profile.values[index + 1] - profile.values[index - 1]);
    localThreshold = profile.values[index] + sampleOffset * valueSlope;
    return std::isfinite(offset)
        && std::isfinite(gradientMagnitude)
        && gradientMagnitude > geometryEpsilon;
}

bool SetRefinedPoint(
    const VolumeView& volume,
    const ResolvedParams& params,
    const Point3& initialPoint,
    const Point3& initialNormal,
    SurfacePointRecord& record)
{
    Point3 current = initialPoint;
    Point3 normal = initialNormal;
    Point3 gradient{};
    double gradientMagnitude = 0.0;
    if (GetGradient(volume, current, gradient, gradientMagnitude)
        && Normalize(gradient)) {
        normal = Scale(gradient, -1.0);
    }
    if (!Normalize(normal)) {
        record.flags |= SurfacePointFlags::FitRejected;
        return false;
    }

    if (params.method == SurfaceDeterminationMethod::GlobalIsoPreview) {
        record.positionModel = current;
        record.normalModel = {
            GetFiniteFloat(normal[0]),
            GetFiniteFloat(normal[1]),
            GetFiniteFloat(normal[2])
        };
        record.localThreshold = GetFiniteFloat(params.initialIsoValue);
        record.gradientMagnitude = GetFiniteFloat(gradientMagnitude);
        const ScalarSample support = GetScalarAtModel(volume, current);
        if (support.status == SampleStatus::Clipped) {
            record.flags |= SurfacePointFlags::ProfileClipped;
        }
        else if (support.status == SampleStatus::InvalidSupport) {
            record.flags |= SurfacePointFlags::InvalidSupport;
        }
        record.validSupportRatio = support.status == SampleStatus::Valid
            ? 1.0F : 0.0F;
        return !GetFatalPointFlags(record.flags);
    }

    double lastNoise = 0.0;
    double lastResidual = 0.0;
    double lastGradient = 0.0;
    double lastThreshold = params.initialIsoValue;
    double minimumValidRatio = 1.0;
    std::uint32_t lastCrossingCount = 0;
    for (int iteration = 0; iteration < 2; ++iteration) {
        Profile profile = BuildProfile(volume, current, normal, params);
        minimumValidRatio = std::min(
            minimumValidRatio, profile.validRatio);
        if (profile.isClipped) {
            record.flags |= SurfacePointFlags::ProfileClipped;
        }
        if (profile.hasInvalidSupport) {
            record.flags |= SurfacePointFlags::InvalidSupport;
        }
        if (profile.isClipped || profile.hasInvalidSupport
            || profile.values.empty()) {
            break;
        }

        std::vector<double> insideValues;
        std::vector<double> outsideValues;
        if (!GetPlateauValues(
                profile, insideValues, outsideValues)) {
            record.flags |= SurfacePointFlags::FitRejected;
            break;
        }
        double inside = GetMedian(insideValues);
        double outside = GetMedian(outsideValues);
        if (inside < outside) {
            normal = Scale(normal, -1.0);
            profile = BuildProfile(volume, current, normal, params);
            minimumValidRatio = std::min(
                minimumValidRatio, profile.validRatio);
            if (profile.isClipped) {
                record.flags |= SurfacePointFlags::ProfileClipped;
            }
            if (profile.hasInvalidSupport) {
                record.flags |= SurfacePointFlags::InvalidSupport;
            }
            if (profile.isClipped || profile.hasInvalidSupport
                || !GetPlateauValues(
                    profile, insideValues, outsideValues)) {
                break;
            }
            inside = GetMedian(insideValues);
            outside = GetMedian(outsideValues);
        }
        const double contrast = inside - outside;
        record.contrast = GetFiniteFloat(std::max(0.0, contrast));
        const double contrastThreshold = std::max(
            params.minimumContrast,
            geometryEpsilon * std::max({
                1.0, std::abs(inside), std::abs(outside) }));
        if (!std::isfinite(contrast) || contrast < contrastThreshold) {
            record.flags |= SurfacePointFlags::LowContrast;
            break;
        }

        const double insideNoise = GetMad(insideValues, inside);
        const double outsideNoise = GetMad(outsideValues, outside);
        lastNoise = std::sqrt(0.5 * (
            insideNoise * insideNoise
            + outsideNoise * outsideNoise));
        double localOffset = 0.0;
        bool hasFit = false;
        if (params.method
            == SurfaceDeterminationMethod::LocalAdaptiveIso50) {
            lastThreshold = 0.5 * (inside + outside);
            hasFit = GetLocalAdaptiveOffset(
                profile,
                lastThreshold,
                localOffset,
                lastCrossingCount,
                lastGradient,
                lastResidual);
        }
        else {
            hasFit = GetGradientPeakOffset(
                profile,
                params.maximumOffsetModel,
                localOffset,
                lastCrossingCount,
                lastGradient,
                lastResidual,
                lastThreshold);
        }
        if (!hasFit) {
            record.flags |= SurfacePointFlags::FitRejected;
            break;
        }
        if (lastCrossingCount > 1) {
            record.flags |= SurfacePointFlags::MultipleCrossings;
        }
        const Point3 next = Add(current, Scale(normal, localOffset));
        const double totalOffset = GetLength(Subtract(next, initialPoint));
        if (!std::isfinite(totalOffset)
            || totalOffset > params.maximumOffsetModel) {
            record.flags |= SurfacePointFlags::ExcessiveOffset;
            break;
        }
        current = next;
        if (GetGradient(volume, current, gradient, gradientMagnitude)
            && Normalize(gradient)) {
            normal = Scale(gradient, -1.0);
        }
    }

    record.localThreshold = GetFiniteFloat(lastThreshold);
    record.gradientMagnitude = GetFiniteFloat(lastGradient);
    record.fitResidual = GetFiniteFloat(lastResidual);
    record.crossingCount = lastCrossingCount;
    record.validSupportRatio = GetFiniteFloat(minimumValidRatio);
    record.offsetFromSeed = GetFiniteFloat(
        GetLength(Subtract(current, initialPoint)));
    const double safeGradient = std::max(lastGradient, geometryEpsilon);
    const double noiseSigma = lastNoise / safeGradient;
    const double residualSigma = lastResidual / safeGradient;
    const double quantizationSigma = params.profileSampleStepModel
        / std::sqrt(12.0);
    record.estimatedLocalizationSigma = GetFiniteFloat(std::sqrt(
        noiseSigma * noiseSigma
        + residualSigma * residualSigma
        + quantizationSigma * quantizationSigma));

    const bool isAccepted = !GetFatalPointFlags(record.flags);
    record.positionModel = isAccepted ? current : initialPoint;
    record.normalModel = {
        GetFiniteFloat(normal[0]),
        GetFiniteFloat(normal[1]),
        GetFiniteFloat(normal[2])
    };
    return isAccepted;
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

bool GetPointAtRoiBoundary(
    const std::optional<std::array<double, 6>>& bounds,
    const Point3& point,
    const double tolerance)
{
    if (!bounds) return false;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(point[axis] - (*bounds)[axis * 2]) <= tolerance
            || std::abs(point[axis] - (*bounds)[axis * 2 + 1])
                <= tolerance) {
            return true;
        }
    }
    return false;
}

void SetTriangleFlipFlags(
    const std::vector<Point3>& originalPoints,
    const std::vector<Point3>& refinedPoints,
    const std::vector<Triangle>& triangles,
    std::vector<SurfacePointRecord>& records)
{
    for (const Triangle& triangle : triangles) {
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
                records[pointId].flags |=
                    SurfacePointFlags::TriangleFlipRisk;
                records[pointId].positionModel = originalPoints[pointId];
            }
        }
    }
}

SurfaceMetricValidity GetVolumeValidity(
    const TopologyMetrics& topology,
    const bool isTruncated,
    const bool hasQuality)
{
    if (!hasQuality) return SurfaceMetricValidity::InsufficientQuality;
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

void AddObjectResult(
    const VolumeView& volume,
    const ResolvedParams& params,
    const std::uint32_t objectIndex,
    const std::vector<Point3>& originalPoints,
    const std::vector<Triangle>& triangles,
    std::vector<SurfacePointRecord> records,
    SurfaceAlgorithmResult& result)
{
    std::vector<Point3> refinedPoints;
    refinedPoints.reserve(records.size());
    for (const SurfacePointRecord& record : records) {
        refinedPoints.push_back(record.positionModel);
    }
    SetTriangleFlipFlags(
        originalPoints, refinedPoints, triangles, records);
    refinedPoints.clear();
    refinedPoints.reserve(records.size());
    for (const SurfacePointRecord& record : records) {
        refinedPoints.push_back(record.positionModel);
    }
    const TopologyMetrics topology = GetTopologyMetrics(
        refinedPoints, triangles);

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
        record.objectIndex = objectIndex;
        if (!GetFatalPointFlags(record.flags)) ++acceptedCount;
        if (GetSurfaceFlag(
                record.flags, SurfacePointFlags::LowContrast)) {
            ++lowContrastCount;
        }
        if (GetSurfaceFlag(
                record.flags, SurfacePointFlags::ProfileClipped)
            || GetPointAtDataBoundary(
                volume.geometry,
                record.positionModel,
                boundaryTolerance)
            || GetPointAtRoiBoundary(
                params.roiModelBounds,
                record.positionModel,
                boundaryTolerance)) {
            isTruncated = true;
            ++result.truncatedPointCount;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            bounds[axis * 2] = std::min(
                bounds[axis * 2], record.positionModel[axis]);
            bounds[axis * 2 + 1] = std::max(
                bounds[axis * 2 + 1], record.positionModel[axis]);
        }
    }
    const bool hasQuality = !records.empty()
        && static_cast<double>(acceptedCount)
            / static_cast<double>(records.size())
            >= qualityRatioThreshold;

    SurfaceObjectRecord object;
    object.objectIndex = objectIndex;
    object.firstPoint = result.points.size();
    object.pointCount = records.size();
    object.firstTriangle = result.triangleIndices.size() / 3U;
    object.triangleCount = triangles.size();
    object.boundsModel = bounds;
    object.isClosed = topology.boundaryEdgeCount == 0;
    object.isManifold = topology.nonManifoldEdgeCount == 0
        && topology.degenerateTriangleCount == 0;
    object.isTruncated = isTruncated;
    object.isOrientationValid = topology.isOrientationValid;
    object.areaValidity = GetAreaValidity(
        topology, isTruncated, hasQuality);
    if (object.areaValidity != SurfaceMetricValidity::InsufficientQuality
        && object.areaValidity != SurfaceMetricValidity::NonManifold) {
        object.areaModelUnit2 = topology.area;
    }
    object.volumeValidity = GetVolumeValidity(
        topology, isTruncated, hasQuality);
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
        for (const std::uint32_t pointId : triangle.vertices) {
            result.triangleIndices.push_back(pointOffset + pointId);
        }
    }
    result.objects.push_back(std::move(object));
}

SurfaceAlgorithmResult BuildSurfaceImpl(
    const VtkImageGridSnapshot& source,
    const SurfaceDeterminationStartParams& inputParams,
    const std::size_t maxWorkingBytes,
    const SurfaceCancelCheck& getCancelled,
    const SurfaceProgressCallback& onProgress)
{
    SurfaceAlgorithmResult result;
    result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
    result.method = inputParams.method;
    SendProgress(
        onProgress, SurfaceDeterminationStage::Preparing, 0.01);

    VolumeView volume;
    result.failureReason = BuildVolumeView(
        source, volume, result.message);
    if (result.failureReason != SurfaceFailureReason::None) return result;

    if (maxWorkingBytes == 0
        || !GetBudgetEstimate(volume, result.requiredBytes)) {
        result.failureReason = SurfaceFailureReason::BudgetExceeded;
        result.message = "Surface working-set estimate overflows.";
        return result;
    }
    if (result.requiredBytes > maxWorkingBytes) {
        result.failureReason = SurfaceFailureReason::BudgetExceeded;
        result.message = "Surface working-set budget is exceeded: requiredBytes="
            + std::to_string(result.requiredBytes)
            + ", maxWorkingBytes=" + std::to_string(maxWorkingBytes) + ".";
        return result;
    }

    SendProgress(
        onProgress, SurfaceDeterminationStage::ThresholdEstimation, 0.05);
    ResolvedParams params;
    result.failureReason = ResolveParams(
        volume, inputParams, getCancelled, params, result.message);
    if (result.failureReason != SurfaceFailureReason::None) {
        result.status = result.failureReason == SurfaceFailureReason::Cancelled
            ? SurfaceResultStatus::Cancelled
            : SurfaceResultStatus::Failed;
        return result;
    }
    result.initialIsoValue = params.initialIsoValue;
    result.parameterFingerprint = GetFingerprint(params);

    SendProgress(
        onProgress, SurfaceDeterminationStage::SeedExtraction, 0.20);
    std::vector<Point3> meshPoints;
    std::vector<Triangle> meshTriangles;
    result.failureReason = BuildInitialMesh(
        *source->image,
        params,
        getCancelled,
        meshPoints,
        meshTriangles,
        result.message);
    if (result.failureReason != SurfaceFailureReason::None) {
        result.status = result.failureReason == SurfaceFailureReason::Cancelled
            ? SurfaceResultStatus::Cancelled
            : SurfaceResultStatus::Failed;
        return result;
    }
    std::size_t actualBytes = 0;
    std::size_t pointBytes = 0;
    std::size_t triangleBytes = 0;
    if (!GetProduct(meshPoints.size(), sizeof(Point3), pointBytes)
        || !GetProduct(
            meshTriangles.size(), sizeof(Triangle), triangleBytes)
        || !GetSum(pointBytes, triangleBytes, actualBytes)
        || actualBytes > maxWorkingBytes) {
        result.failureReason = SurfaceFailureReason::BudgetExceeded;
        result.message = "Surface seed mesh exceeds the working-set budget.";
        return result;
    }

    auto components = BuildComponents(
        meshPoints, meshTriangles, volume.geometry.voxelVolume);
    const auto selected = GetSelectedComponents(
        components, meshPoints, params);
    if (selected.empty()) {
        result.failureReason = SurfaceFailureReason::NoSurface;
        result.message = "Surface component selection produced no object.";
        return result;
    }

    // 取得真实 topology 后，对同时存活的原始 mesh、局部 workspace 和
    // immutable generation 再做一次 checked 预算；拒绝发生在 point refine
    // 和结果 vector 扩容之前。
    std::size_t refinedWorkingBytes = actualBytes;
    for (const std::size_t componentIndex : selected) {
        const auto& component = components[componentIndex];
        constexpr std::size_t pointWorkspaceBytes =
            sizeof(Point3) * 4U
            + sizeof(SurfacePointRecord)
            + sizeof(std::uint32_t);
        constexpr std::size_t triangleWorkspaceBytes =
            sizeof(Triangle) * 2U
            + sizeof(std::uint32_t) * 3U;
        if (!AddWorkingBytes(
                component.sourcePointIds.size(),
                pointWorkspaceBytes,
                refinedWorkingBytes)
            || !AddWorkingBytes(
                component.triangles.size(),
                triangleWorkspaceBytes,
                refinedWorkingBytes)
            || !AddWorkingBytes(
                1U,
                sizeof(SurfaceObjectRecord),
                refinedWorkingBytes)) {
            result.failureReason = SurfaceFailureReason::BudgetExceeded;
            result.message = "Surface refined working-set estimate overflows.";
            return result;
        }
    }
    result.requiredBytes = std::max(
        result.requiredBytes, refinedWorkingBytes);
    if (result.requiredBytes > maxWorkingBytes) {
        result.failureReason = SurfaceFailureReason::BudgetExceeded;
        result.message = "Surface refined working-set budget is exceeded: requiredBytes="
            + std::to_string(result.requiredBytes)
            + ", maxWorkingBytes=" + std::to_string(maxWorkingBytes) + ".";
        return result;
    }

    SendProgress(
        onProgress, SurfaceDeterminationStage::SubvoxelRefinement, 0.35);
    for (std::size_t selectedIndex = 0;
        selectedIndex < selected.size(); ++selectedIndex) {
        if (GetCancelled(getCancelled)) {
            result.status = SurfaceResultStatus::Cancelled;
            result.failureReason = SurfaceFailureReason::Cancelled;
            result.message = "Surface subvoxel refinement was cancelled.";
            result.points.clear();
            result.triangleIndices.clear();
            result.objects.clear();
            return result;
        }
        std::vector<Point3> points;
        std::vector<Triangle> triangles;
        GetLocalMesh(
            components[selected[selectedIndex]],
            meshPoints,
            points,
            triangles);
        SetOutwardWinding(volume, points, triangles);
        const auto normals = GetVertexNormals(points, triangles);
        std::vector<SurfacePointRecord> records(points.size());
        std::atomic<bool> isRefinementCancelled{ false };
        const auto setPointBlock = [
            &volume,
            &params,
            &points,
            &normals,
            &records,
            &getCancelled,
            &isRefinementCancelled](
                const vtkIdType begin,
                const vtkIdType end) {
            if (isRefinementCancelled.load(std::memory_order_acquire)) return;
            for (vtkIdType pointId = begin; pointId < end; ++pointId) {
                if (((pointId - begin) & 31) == 0
                    && GetCancelled(getCancelled)) {
                    isRefinementCancelled.store(
                        true, std::memory_order_release);
                    return;
                }
                const auto pointIndex = static_cast<std::size_t>(pointId);
                SetRefinedPoint(
                    volume,
                    params,
                    points[pointIndex],
                    normals[pointIndex],
                    records[pointIndex]);
            }
        };
        vtkSMPTools::For(
            0,
            static_cast<vtkIdType>(points.size()),
            128,
            setPointBlock);
        if (isRefinementCancelled.load(std::memory_order_acquire)) {
            result.status = SurfaceResultStatus::Cancelled;
            result.failureReason = SurfaceFailureReason::Cancelled;
            result.message = "Surface subvoxel refinement was cancelled.";
            result.points.clear();
            result.triangleIndices.clear();
            result.objects.clear();
            return result;
        }
        AddObjectResult(
            volume,
            params,
            static_cast<std::uint32_t>(selectedIndex),
            points,
            triangles,
            std::move(records),
            result);
        const double objectProgress = static_cast<double>(selectedIndex + 1)
            / static_cast<double>(selected.size());
        SendProgress(
            onProgress,
            SurfaceDeterminationStage::SubvoxelRefinement,
            0.35 + 0.50 * objectProgress);
    }

    SendProgress(
        onProgress, SurfaceDeterminationStage::TopologyValidation, 0.90);
    if (result.points.empty() || result.triangleIndices.empty()
        || result.objects.empty()) {
        result.failureReason = SurfaceFailureReason::NoSurface;
        result.message = "Surface output is empty after topology validation.";
        return result;
    }
    if (GetCancelled(getCancelled)) {
        result.status = SurfaceResultStatus::Cancelled;
        result.failureReason = SurfaceFailureReason::Cancelled;
        result.message = "Surface topology validation was cancelled.";
        result.points.clear();
        result.triangleIndices.clear();
        result.objects.clear();
        return result;
    }
    result.status = SurfaceResultStatus::Succeeded;
    result.failureReason = SurfaceFailureReason::None;
    result.algorithmRevision = algorithmRevision;
    result.message = "Surface determination succeeded: points="
        + std::to_string(result.points.size())
        + ", objects=" + std::to_string(result.objects.size())
        + ", accepted=" + std::to_string(result.acceptedPointCount)
        + ".";
    SendProgress(
        onProgress, SurfaceDeterminationStage::TopologyValidation, 1.0);
    return result;
}

} // namespace

SurfaceAlgorithmResult SurfaceDeterminationAlgorithm::BuildSurface(
    const VtkImageGridSnapshot& source,
    const SurfaceDeterminationStartParams& params,
    const std::size_t maxWorkingBytes,
    const SurfaceCancelCheck& getCancelled,
    const SurfaceProgressCallback& onProgress)
{
    try {
        return BuildSurfaceImpl(
            source,
            params,
            maxWorkingBytes,
            getCancelled,
            onProgress);
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
