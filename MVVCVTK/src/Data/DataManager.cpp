#include "DataManager.h"
#include "Platform/Path.h"
#include <vtkDataArray.h>
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkUnsignedCharArray.h>
#include <vtkClipPolyData.h>
#include <vtkColorTransferFunction.h>
#include <vtkErrorCode.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImplicitVolume.h>
#include <vtkOBJWriter.h>
#include <vtkPLYWriter.h>
#include <vtkSTLWriter.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <fstream>
#include <filesystem>
#include <vtkTIFFReader.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vtkTransform.h>
#include <vtkImageReslice.h>
#include <vtkImageChangeInformation.h>
#include <vtkPNGWriter.h>
#include <vtkImageImport.h>
#include <vtkImageAppend.h>
#include <vtkMatrix3x3.h>
#include <vtkMatrix4x4.h>
#include <cstring>
#include "MemMappedFile.h"

class BaseDataManager::Impl final {
public:
    Impl()
        : m_current(std::make_shared<TrustedImageState>(TrustedImageState{
            vtkSmartPointer<vtkImageData>::New(),
            {},
            { 0, 0, 0 },
            { 1.0, 1.0, 1.0 },
            { 0.0, 0.0, 0.0 },
            { 0.0, 0.0 },
            0 }))
    {
    }

    static bool ExportRaw(
        const TrustedImageSnapshot& imageSnapshot,
        const std::string& outputDir,
        const std::array<double, 16>& modelToWorldMatrix,
        const TaskStopToken& stopToken);
    static bool ExportMesh(
        const TrustedImageSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params,
        const TaskStopToken& stopToken);
    static bool BuildMeshColors(
        vtkPolyData* mesh,
        const DataExportParams& params,
        const TaskStopToken& stopToken);
    static bool GetIsAffine(
        const std::array<double, 16>& modelToWorld);
    static std::filesystem::path BuildExportPath(
        const std::string& outputDir,
        const int dimensions[3],
        const std::string& extension);

    static std::array<double, 3> GetRasOrigin(
        const std::array<double, 3>& lpsOrigin,
        const int dims[3],
        const std::array<double, 3>& spacing)
    {
        std::array<double, 3> rasOrigin = {
            -lpsOrigin[0], -lpsOrigin[1], lpsOrigin[2]
        };
        if (dims[0] > 0) {
            rasOrigin[0] -= static_cast<double>(dims[0] - 1) * spacing[0];
        }
        if (dims[1] > 0) {
            rasOrigin[1] -= static_cast<double>(dims[1] - 1) * spacing[1];
        }
        return rasOrigin;
    }

    static std::optional<size_t> GetVoxelCount(const int dims[3])
    {
        size_t voxelCount = 1;
        for (int axis = 0; axis < 3; ++axis) {
            if (dims[axis] <= 0
                || voxelCount > std::numeric_limits<size_t>::max()
                    / static_cast<size_t>(dims[axis])) {
                return std::nullopt;
            }
            voxelCount *= static_cast<size_t>(dims[axis]);
        }
        if (voxelCount > std::numeric_limits<size_t>::max() / sizeof(float)
            || voxelCount > static_cast<size_t>(
                std::numeric_limits<std::ptrdiff_t>::max())) {
            return std::nullopt;
        }
        return voxelCount;
    }

    static bool GetMaskValid(
        vtkImageData* image,
        vtkImageData* validityMask)
    {
        if (!validityMask) {
            return true;
        }
        if (!image
            || validityMask->GetScalarType() != VTK_UNSIGNED_CHAR
            || validityMask->GetNumberOfScalarComponents() != 1
            || !validityMask->GetScalarPointer()) {
            return false;
        }
        vtkPointData* const maskData = validityMask->GetPointData();
        vtkDataArray* const maskScalars = maskData
            ? maskData->GetScalars() : nullptr;
        if (!maskScalars
            || maskScalars->GetNumberOfTuples()
                != image->GetNumberOfPoints()) {
            return false;
        }

        int imageExtent[6] = {};
        int maskExtent[6] = {};
        double imageOrigin[3] = {};
        double maskOrigin[3] = {};
        double imageSpacing[3] = {};
        double maskSpacing[3] = {};
        image->GetExtent(imageExtent);
        validityMask->GetExtent(maskExtent);
        image->GetOrigin(imageOrigin);
        validityMask->GetOrigin(maskOrigin);
        image->GetSpacing(imageSpacing);
        validityMask->GetSpacing(maskSpacing);
        for (int index = 0; index < 6; ++index) {
            if (imageExtent[index] != maskExtent[index]) {
                return false;
            }
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (imageOrigin[axis] != maskOrigin[axis]
                || imageSpacing[axis] != maskSpacing[axis]) {
                return false;
            }
        }

        const auto* imageDirection = image->GetDirectionMatrix();
        const auto* maskDirection = validityMask->GetDirectionMatrix();
        if (!imageDirection || !maskDirection) {
            return imageDirection == maskDirection;
        }
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (imageDirection->GetElement(row, column)
                    != maskDirection->GetElement(row, column)) {
                    return false;
                }
            }
        }
        return true;
    }

    static ImageValueType GetValueType(const int vtkType) noexcept
    {
        switch (vtkType) {
        case VTK_CHAR:
            return std::numeric_limits<char>::is_signed
                ? ImageValueType::Int8 : ImageValueType::UInt8;
        case VTK_SIGNED_CHAR:
            return ImageValueType::Int8;
        case VTK_UNSIGNED_CHAR:
            return ImageValueType::UInt8;
        case VTK_SHORT:
            return ImageValueType::Int16;
        case VTK_UNSIGNED_SHORT:
            return ImageValueType::UInt16;
        case VTK_INT:
            return ImageValueType::Int32;
        case VTK_UNSIGNED_INT:
            return ImageValueType::UInt32;
        case VTK_LONG:
            return sizeof(long) == sizeof(std::int64_t)
                ? ImageValueType::Int64 : ImageValueType::Int32;
        case VTK_UNSIGNED_LONG:
            return sizeof(unsigned long) == sizeof(std::uint64_t)
                ? ImageValueType::UInt64 : ImageValueType::UInt32;
        case VTK_LONG_LONG:
            return ImageValueType::Int64;
        case VTK_UNSIGNED_LONG_LONG:
            return ImageValueType::UInt64;
        case VTK_FLOAT:
            return ImageValueType::Float32;
        case VTK_DOUBLE:
            return ImageValueType::Float64;
        default:
            return ImageValueType::Unknown;
        }
    }

    static std::optional<std::size_t>
        GetArrayByteCount(vtkDataArray* values)
    {
        if (!values) return std::nullopt;
        const vtkIdType valueCount = values->GetDataSize();
        const int valueBytes = values->GetDataTypeSize();
        if (valueCount < 0 || valueBytes <= 0
            || static_cast<std::uint64_t>(valueCount)
                > std::numeric_limits<std::size_t>::max()
                    / static_cast<std::size_t>(valueBytes)) {
            return std::nullopt;
        }

        return static_cast<std::size_t>(valueCount)
            * static_cast<std::size_t>(valueBytes);
    }

    static std::shared_ptr<const std::vector<std::uint8_t>>
        GetArrayBytes(
            vtkDataArray* values,
            const std::size_t byteCount)
    {
        if (!values) return {};
        try {
            auto bytes =
                std::make_shared<std::vector<std::uint8_t>>(byteCount);
            if (byteCount != 0) {
                const void* const source = values->GetVoidPointer(0);
                if (!source) return {};
                std::memcpy(bytes->data(), source, byteCount);
            }
            return bytes;
        }
        catch (const std::bad_alloc&) {
            return {};
        }
        catch (const std::length_error&) {
            return {};
        }
    }

    struct ReadPlan final {
        TrustedImageSnapshot snapshot;
        vtkDataArray* values = nullptr;
        vtkDataArray* mask = nullptr;
        std::array<std::size_t, 3> sourceDims = { 0, 0, 0 };
        std::array<int, 6> sourceExtent = { 0, -1, 0, -1, 0, -1 };
        ImageReadRegion region;
        ImageValueType valueType = ImageValueType::Unknown;
        std::size_t sourceVoxels = 0;
        std::size_t regionVoxels = 0;
        std::size_t tupleBytes = 0;
        std::size_t requiredBytes = 0;
    };

    struct ReadPlanResult final {
        ImageReadError error = ImageReadError::InvalidData;
        std::size_t requiredBytes = 0;
        std::optional<ReadPlan> plan;
    };

    static bool GetCheckedProduct(
        const std::size_t left,
        const std::size_t right,
        std::size_t& product) noexcept
    {
        product = 0;
        if (left != 0
            && right > std::numeric_limits<std::size_t>::max() / left) {
            return false;
        }
        product = left * right;
        return true;
    }

    static ReadPlanResult GetReadPlan(
        TrustedImageSnapshot snapshot,
        const ImageReadRequest& request)
    {
        ReadPlanResult result;
        if (!snapshot || !snapshot->image) {
            result.error = ImageReadError::NoImage;
            return result;
        }

        ReadPlan plan;
        plan.snapshot = std::move(snapshot);
        int dims[3] = {};
        int extent[6] = {};
        plan.snapshot->image->GetDimensions(dims);
        plan.snapshot->image->GetExtent(extent);
        std::size_t sourceVoxels = 1;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto minIndex = static_cast<std::int64_t>(
                extent[axis * 2]);
            const auto maxIndex = static_cast<std::int64_t>(
                extent[axis * 2 + 1]);
            const auto extentSize = maxIndex - minIndex + 1;
            if (dims[axis] <= 0
                || extentSize != static_cast<std::int64_t>(dims[axis])
                || plan.snapshot->dims[axis] != dims[axis]) {
                result.error = ImageReadError::InvalidData;
                return result;
            }
            plan.sourceDims[axis] = static_cast<std::size_t>(dims[axis]);
            if (!GetCheckedProduct(
                    sourceVoxels,
                    plan.sourceDims[axis],
                    sourceVoxels)) {
                result.error = ImageReadError::TooLarge;
                result.requiredBytes =
                    std::numeric_limits<std::size_t>::max();
                return result;
            }
        }
        plan.sourceVoxels = sourceVoxels;
        std::copy_n(extent, plan.sourceExtent.size(),
            plan.sourceExtent.begin());

        vtkPointData* const pointData =
            plan.snapshot->image->GetPointData();
        plan.values = pointData ? pointData->GetScalars() : nullptr;
        if (!plan.values
            || plan.values->GetDataTypeSize() <= 0
            || plan.values->GetNumberOfComponents() <= 0
            || sourceVoxels > static_cast<std::size_t>(
                std::numeric_limits<vtkIdType>::max())
            || plan.values->GetNumberOfTuples()
                != static_cast<vtkIdType>(sourceVoxels)) {
            result.error = ImageReadError::InvalidData;
            return result;
        }
        plan.valueType = GetValueType(plan.values->GetDataType());
        if (plan.valueType == ImageValueType::Unknown) {
            result.error = ImageReadError::UnsupportedType;
            return result;
        }

        const auto componentBytes = static_cast<std::size_t>(
            plan.values->GetDataTypeSize());
        const auto componentCount = static_cast<std::size_t>(
            plan.values->GetNumberOfComponents());
        if (!GetCheckedProduct(
                componentBytes, componentCount, plan.tupleBytes)) {
            result.error = ImageReadError::TooLarge;
            result.requiredBytes =
                std::numeric_limits<std::size_t>::max();
            return result;
        }
        std::size_t expectedValueBytes = 0;
        const auto actualValueBytes = GetArrayByteCount(plan.values);
        if (!GetCheckedProduct(
                sourceVoxels, plan.tupleBytes, expectedValueBytes)
            || !actualValueBytes
            || *actualValueBytes != expectedValueBytes) {
            result.error = ImageReadError::InvalidData;
            return result;
        }

        if (plan.snapshot->validityMask) {
            if (!GetMaskValid(
                    plan.snapshot->image,
                    plan.snapshot->validityMask)) {
                result.error = ImageReadError::InvalidData;
                return result;
            }
            vtkPointData* const maskData =
                plan.snapshot->validityMask->GetPointData();
            plan.mask = maskData ? maskData->GetScalars() : nullptr;
            const auto maskBytes = GetArrayByteCount(plan.mask);
            if (!plan.mask
                || plan.mask->GetDataType() != VTK_UNSIGNED_CHAR
                || plan.mask->GetNumberOfComponents() != 1
                || plan.mask->GetNumberOfTuples()
                    != static_cast<vtkIdType>(sourceVoxels)
                || !maskBytes
                || *maskBytes != sourceVoxels) {
                result.error = ImageReadError::InvalidData;
                return result;
            }
        }

        plan.region = request.region.value_or(ImageReadRegion{
            { 0, 0, 0 }, plan.sourceDims });
        std::size_t regionVoxels = 1;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto offset = plan.region.offset[axis];
            const auto regionSize = plan.region.size[axis];
            if (regionSize == 0
                || offset > plan.sourceDims[axis]
                || regionSize > plan.sourceDims[axis] - offset) {
                result.error = ImageReadError::InvalidRegion;
                return result;
            }
            if (!GetCheckedProduct(
                    regionVoxels, regionSize, regionVoxels)) {
                result.error = ImageReadError::TooLarge;
                result.requiredBytes =
                    std::numeric_limits<std::size_t>::max();
                return result;
            }
        }
        plan.regionVoxels = regionVoxels;

        std::size_t voxelBytes = plan.tupleBytes;
        if (plan.mask) {
            if (voxelBytes == std::numeric_limits<std::size_t>::max()) {
                result.error = ImageReadError::TooLarge;
                result.requiredBytes = voxelBytes;
                return result;
            }
            ++voxelBytes;
        }
        if (!GetCheckedProduct(
                regionVoxels, voxelBytes, plan.requiredBytes)) {
            result.error = ImageReadError::TooLarge;
            result.requiredBytes =
                std::numeric_limits<std::size_t>::max();
            return result;
        }

        result.error = ImageReadError::None;
        result.requiredBytes = plan.requiredBytes;
        result.plan = std::move(plan);
        return result;
    }

    static ImageReadState GetReadState(
        const ReadPlan& plan,
        const std::size_t voxelOffset,
        const std::size_t voxelCount)
    {
        ImageReadState state;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            state.dims[axis] = static_cast<int>(plan.region.size[axis]);
            state.extent[axis * 2] = 0;
            state.extent[axis * 2 + 1] = state.dims[axis] - 1;
        }

        double sourceOrigin[3] = {};
        double sourceSpacing[3] = {};
        plan.snapshot->image->GetOrigin(sourceOrigin);
        plan.snapshot->image->GetSpacing(sourceSpacing);
        const auto* direction =
            plan.snapshot->image->GetDirectionMatrix();
        for (std::size_t row = 0; row < 3; ++row) {
            state.spacing[row] = sourceSpacing[row];
            state.origin[row] = sourceOrigin[row];
            for (std::size_t column = 0; column < 3; ++column) {
                const double value = direction
                    ? direction->GetElement(
                        static_cast<int>(row),
                        static_cast<int>(column))
                    : (row == column ? 1.0 : 0.0);
                state.direction[row * 3 + column] = value;
                const auto sourceIndex = static_cast<double>(
                    static_cast<std::int64_t>(
                        plan.sourceExtent[column * 2])
                    + static_cast<std::int64_t>(
                        plan.region.offset[column]));
                state.origin[row] += value
                    * sourceIndex * sourceSpacing[column];
            }
        }
        state.scalarRange = plan.snapshot->scalarRange;
        state.valueType = plan.valueType;
        state.componentBytes = static_cast<std::size_t>(
            plan.values->GetDataTypeSize());
        state.componentCount = static_cast<std::size_t>(
            plan.values->GetNumberOfComponents());
        state.region = plan.region;
        state.voxelOffset = voxelOffset;
        state.voxelCount = voxelCount;
        state.version = plan.snapshot->version;
        return state;
    }

    static ImageReadBytes GetReadBytes(
        const ReadPlan& plan,
        vtkDataArray* values,
        const std::size_t voxelBytes,
        const std::size_t voxelOffset,
        const std::size_t voxelCount,
        const TaskStopToken& stopToken)
    {
        if (!values || stopToken.GetIsStopped()) return {};
        std::size_t byteCount = 0;
        if (!GetCheckedProduct(
                voxelCount, voxelBytes, byteCount)) {
            return {};
        }
        try {
            auto bytes =
                std::make_shared<std::vector<std::uint8_t>>(byteCount);
            if (byteCount == 0) return bytes;
            const auto* source = static_cast<const std::uint8_t*>(
                values->GetVoidPointer(0));
            if (!source) return {};

            const std::size_t regionX = plan.region.size[0];
            const std::size_t regionY = plan.region.size[1];
            const std::size_t regionSlice = regionX * regionY;
            std::size_t copied = 0;
            while (copied < voxelCount) {
                if (stopToken.GetIsStopped()) return {};
                const std::size_t regionIndex = voxelOffset + copied;
                const std::size_t localZ = regionIndex / regionSlice;
                const std::size_t sliceIndex = regionIndex % regionSlice;
                const std::size_t localY = sliceIndex / regionX;
                const std::size_t localX = sliceIndex % regionX;
                const std::size_t rowVoxels = std::min(
                    voxelCount - copied, regionX - localX);

                const std::size_t sourceX =
                    plan.region.offset[0] + localX;
                const std::size_t sourceY =
                    plan.region.offset[1] + localY;
                const std::size_t sourceZ =
                    plan.region.offset[2] + localZ;
                const std::size_t sourceIndex =
                    (sourceZ * plan.sourceDims[1] + sourceY)
                    * plan.sourceDims[0] + sourceX;
                const std::size_t sourceByte = sourceIndex * voxelBytes;
                const std::size_t targetByte = copied * voxelBytes;
                const std::size_t rowBytes = rowVoxels * voxelBytes;
                std::memcpy(
                    bytes->data() + targetByte,
                    source + sourceByte,
                    rowBytes);
                copied += rowVoxels;
            }
            return stopToken.GetIsStopped() ? ImageReadBytes{} : bytes;
        }
        catch (...) {
            return {};
        }
    }

    bool SetCurrent(TrustedImageState state)
    {
        if (!state.image
            || !GetMaskValid(state.image, state.validityMask)) {
            return false;
        }
        auto nextState = std::make_shared<TrustedImageState>(std::move(state));
        std::shared_ptr<const TrustedImageState> retiredState;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            if (!m_current
                || m_current->version == std::numeric_limits<DataVersion>::max()) {
                return false;
            }
            nextState->version = m_current->version + 1;
            retiredState = std::move(m_current);
            m_current = std::move(nextState);
        }
        return true;
    }

    // current TrustedImageState 与 scalar range 共用此锁；snapshot 是跨字段一致性的读取入口。
    mutable std::mutex m_dataMutex;
    // current 只向受控内部消费链发布 const owner；写入只能通过 DataManager 提交新批次。
    TrustedImageSnapshot m_current;
    mutable std::mutex m_pendingMutex;
    TrustedImageSnapshot m_pending;
    // 与 current image 同批提交的 RAS 物理轴间距 [x,y,z]，单位沿用输入。

    bool SetRasScalars(
        const float* src,
        float* dst,
        const int dims[3],
        size_t availableCount,
        const TaskStopToken& stopToken) const
    {
        const auto voxelCount = GetVoxelCount(dims);
        if (!voxelCount) {
            return false;
        }
        const size_t nx = static_cast<size_t>(dims[0]);
        const size_t ny = static_cast<size_t>(dims[1]);
        const size_t nz = static_cast<size_t>(dims[2]);
        const size_t sliceSize = nx * ny;
        const size_t totalCount = *voxelCount;

        if (!dst || totalCount == 0 || availableCount > totalCount
            || stopToken.GetIsStopped()) {
            return false;
        }

        if (!src || availableCount == 0) {
            std::fill(dst, dst + totalCount, 0.0f);
            return !stopToken.GetIsStopped();
        }

        if (availableCount == totalCount) {
            for (size_t z = 0; z < nz; ++z) {
                if (stopToken.GetIsStopped()) return false;
                const size_t srcSliceOffset = z * sliceSize;
                const size_t dstSliceOffset = z * sliceSize;
                for (size_t y = 0; y < ny; ++y) {
                    const float* srcRow = src + srcSliceOffset + y * nx;
                    float* dstRow = dst + dstSliceOffset + (ny - 1 - y) * nx;
                    for (size_t x = 0; x < nx; ++x) {
                        dstRow[nx - 1 - x] = srcRow[x];
                    }
                }
            }
            return true;
        }

        std::fill(dst, dst + totalCount, 0.0f);
        for (size_t srcIndex = 0; srcIndex < availableCount; ++srcIndex) {
            if ((srcIndex % sliceSize) == 0
                && stopToken.GetIsStopped()) {
                return false;
            }
            const size_t z = srcIndex / sliceSize;
            const size_t rem = srcIndex % sliceSize;
            const size_t y = rem / nx;
            const size_t x = rem % nx;
            const size_t dstIndex = z * sliceSize + (ny - 1 - y) * nx + (nx - 1 - x);
            dst[dstIndex] = src[srcIndex];
        }

        return !stopToken.GetIsStopped();
    }

    std::string GetOrientName(Orientation value) const
    {
        switch (value) {
        case Orientation::Front_back:
            return "Front_back";
        case Orientation::Left_right:
            return "Left_right";
        case Orientation::Top_down:
        default:
            return "Top_down";
        }
    }

    std::array<int, 2> GetSliceSize(
        const int sliceDims[3],
        Orientation value) const
    {
        switch (value) {
        case Orientation::Front_back:
            return { sliceDims[0], sliceDims[2] };
        case Orientation::Left_right:
            return { sliceDims[1], sliceDims[2] };
        case Orientation::Top_down:
        default:
            return { sliceDims[0], sliceDims[1] };
        }
    }

    unsigned char GetWindowGray(
        double value,
        const WindowLevelParams& params) const
    {
        const double safeWindowWidth = std::max(params.windowWidth, 1e-6); // 当前导出使用的窗宽，避免除零
        const double windowMin = params.windowCenter - safeWindowWidth * 0.5; // 当前灰度映射下限
        const double normalized = (value - windowMin) / safeWindowWidth;
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        return static_cast<unsigned char>(clamped * 255.0 + 0.5);
    }
};

class TiffVolumeDataManager::Impl final {
public:
    vtkSmartPointer<vtkImageData> LoadImage(
        const std::string& inputPath,
        const VolumeLayout& layout,
        const TaskStopToken& stopToken);

private:
    bool SetLpsRasImage(
        vtkImageData* source,
        vtkImageData* target,
        const TaskStopToken& stopToken) const;
};

BaseDataManager::BaseDataManager()
    : m_impl(std::make_unique<BaseDataManager::Impl>())
{
}

BaseDataManager::~BaseDataManager() = default;

vtkSmartPointer<vtkImageData> BaseDataManager::GetVtkImage() const
{
    return GetImageState().image;
}

std::array<double, 2> BaseDataManager::GetScalarRange() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current->scalarRange;
}

std::array<double, 3> BaseDataManager::GetSpacing() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current->spacing;
}

bool BaseDataManager::SetSpacing(const std::array<double, 3>& spacing)
{
    // 1. spacing 是物理尺度，三个分量都必须有限且严格为正。
    if (!std::all_of(spacing.begin(), spacing.end(), [](double value) {
        return std::isfinite(value) && value > 0.0;
        })) {
        return false;
    }

    constexpr int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        // 2. 锁内取得 immutable current 身份，锁外构造候选批次，避免复制/VTK 调用阻塞读线程。
        std::shared_ptr<const TrustedImageState> baseState;
        {
            std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
            if (!m_impl->m_current || !m_impl->m_current->image) {
                return false;
            }
            if (m_impl->m_current->spacing == spacing) {
                return true;
            }
            if (m_impl->m_current->version == std::numeric_limits<DataVersion>::max()) {
                return false;
            }
            baseState = m_impl->m_current;
        }

        // spacing 只修改 VTK 外壳；scalar 在版本间保持只读共享，避免复制整卷 voxel。
        auto candidate = vtkSmartPointer<vtkImageData>::New();
        candidate->ShallowCopy(baseState->image);
        candidate->SetSpacing(spacing.data());

        auto nextState = std::make_shared<TrustedImageState>(*baseState);
        nextState->image = std::move(candidate);
        if (baseState->validityMask) {
            auto maskCandidate =
                vtkSmartPointer<vtkImageData>::New();
            maskCandidate->ShallowCopy(
                baseState->validityMask);
            maskCandidate->SetSpacing(spacing.data());
            nextState->validityMask =
                std::move(maskCandidate);
        }
        nextState->spacing = spacing;
        nextState->version = baseState->version + 1;
        std::shared_ptr<const TrustedImageState> retiredState;
        {
            std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
            // 3. 仅当 current 仍是基准对象才提交；并发发布抢先时重试，三次冲突后返回失败。
            if (m_impl->m_current != baseState) {
                continue;
            }
            retiredState = std::move(m_impl->m_current);
            m_impl->m_current = std::move(nextState);
        }
        return true;
    }
    return false;
}

DataVersion BaseDataManager::GetDataVersion() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current->version;
}

TrustedImageState BaseDataManager::GetImageState() const
{
    const auto currentState = GetImageSnapshot();
    TrustedImageState publicState = *currentState;
    if (publicState.image) {
        auto imageCopy = vtkSmartPointer<vtkImageData>::New();
        imageCopy->DeepCopy(publicState.image);
        publicState.image = std::move(imageCopy);
    }
    if (publicState.validityMask) {
        auto maskCopy = vtkSmartPointer<vtkImageData>::New();
        maskCopy->DeepCopy(publicState.validityMask);
        publicState.validityMask = std::move(maskCopy);
    }
    return publicState;
}

std::optional<ImageReadState>
BaseDataManager::GetImageReadState() const
{
    auto result = GetImageReadResult(imageReadLimit);
    return std::move(result.state);
}

ImageReadResult BaseDataManager::GetImageReadResult(
    const std::size_t maxReadBytes) const
{
    ImageReadRequest request;
    request.maxBytes = maxReadBytes;
    return GetImageReadResult(request, TaskStopToken{});
}

ImageReadResult BaseDataManager::GetImageReadResult(
    const ImageReadRequest& request,
    const TaskStopToken& stopToken) const
{
    ImageReadResult result;
    if (stopToken.GetIsStopped()) {
        result.error = ImageReadError::Cancelled;
        return result;
    }
    auto planResult = Impl::GetReadPlan(
        GetImageSnapshot(), request);
    result.error = planResult.error;
    result.requiredBytes = planResult.requiredBytes;
    if (!planResult.plan
        || planResult.error != ImageReadError::None) {
        return result;
    }
    const auto& plan = *planResult.plan;
    if (plan.requiredBytes > request.maxBytes) {
        result.error = ImageReadError::TooLarge;
        return result;
    }

    auto state = Impl::GetReadState(
        plan, 0, plan.regionVoxels);
    state.values = Impl::GetReadBytes(
        plan,
        plan.values,
        plan.tupleBytes,
        0,
        plan.regionVoxels,
        stopToken);
    if (!state.values) {
        result.error = stopToken.GetIsStopped()
            ? ImageReadError::Cancelled
            : ImageReadError::CopyFailed;
        return result;
    }
    if (plan.mask) {
        state.validityMask = Impl::GetReadBytes(
            plan,
            plan.mask,
            1,
            0,
            plan.regionVoxels,
            stopToken);
        if (!state.validityMask) {
            result.error = stopToken.GetIsStopped()
                ? ImageReadError::Cancelled
                : ImageReadError::CopyFailed;
            return result;
        }
    }
    result.error = ImageReadError::None;
    result.state = std::move(state);
    return result;
}

ImageReadChunkResult BaseDataManager::GetImageReadChunk(
    const ImageReadRequest& request,
    const std::size_t voxelOffset,
    const TaskStopToken& stopToken) const
{
    ImageReadChunkResult result;
    if (stopToken.GetIsStopped()) {
        result.error = ImageReadError::Cancelled;
        return result;
    }
    auto planResult = Impl::GetReadPlan(
        GetImageSnapshot(), request);
    result.error = planResult.error;
    result.requiredBytes = planResult.requiredBytes;
    if (!planResult.plan
        || planResult.error != ImageReadError::None) {
        return result;
    }
    const auto& plan = *planResult.plan;
    if (voxelOffset > plan.regionVoxels) {
        result.error = ImageReadError::InvalidRegion;
        return result;
    }
    if (voxelOffset == plan.regionVoxels) {
        result.error = ImageReadError::None;
        result.nextVoxelOffset = voxelOffset;
        result.isDone = true;
        return result;
    }

    const std::size_t voxelBytes = plan.tupleBytes
        + (plan.mask ? 1U : 0U);
    const std::size_t chunkBytes = std::min(
        request.maxBytes, imageChunkLimit);
    if (chunkBytes < voxelBytes) {
        result.error = ImageReadError::TooLarge;
        return result;
    }
    const std::size_t voxelCount = std::min(
        plan.regionVoxels - voxelOffset,
        chunkBytes / voxelBytes);
    auto state = Impl::GetReadState(
        plan, voxelOffset, voxelCount);
    state.values = Impl::GetReadBytes(
        plan,
        plan.values,
        plan.tupleBytes,
        voxelOffset,
        voxelCount,
        stopToken);
    if (!state.values) {
        result.error = stopToken.GetIsStopped()
            ? ImageReadError::Cancelled
            : ImageReadError::CopyFailed;
        return result;
    }
    if (plan.mask) {
        state.validityMask = Impl::GetReadBytes(
            plan,
            plan.mask,
            1,
            voxelOffset,
            voxelCount,
            stopToken);
        if (!state.validityMask) {
            result.error = stopToken.GetIsStopped()
                ? ImageReadError::Cancelled
                : ImageReadError::CopyFailed;
            return result;
        }
    }
    result.nextVoxelOffset = voxelOffset + voxelCount;
    result.isDone = result.nextVoxelOffset == plan.regionVoxels;
    result.error = ImageReadError::None;
    result.state = std::move(state);
    return result;
}

TrustedImageSnapshot BaseDataManager::GetImageSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current;
}

bool BaseDataManager::SetCurrentData(
    TrustedImageState state,
    const TrustedImageSnapshot& expectedSnapshot,
    TrustedImageSnapshot& publishedSnapshot)
{
    publishedSnapshot.reset();
    if (!expectedSnapshot
        || !state.image
        || !Impl::GetMaskValid(
            state.image, state.validityMask)
        || expectedSnapshot->version
            == std::numeric_limits<DataVersion>::max()) {
        return false;
    }

    auto nextState =
        std::make_shared<TrustedImageState>(std::move(state));
    std::shared_ptr<const TrustedImageState> retiredState;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        // 同时比较 owner 身份与 version，避免 ABA 或并发 load 覆盖更晚 current。
        if (m_impl->m_current != expectedSnapshot
            || m_impl->m_current->version
                != expectedSnapshot->version) {
            return false;
        }
        nextState->version = expectedSnapshot->version + 1;
        retiredState = std::move(m_impl->m_current);
        m_impl->m_current = std::move(nextState);
        publishedSnapshot = m_impl->m_current;
    }
    return true;
}

bool BaseDataManager::SetFromBuffer(
    const VolumeBuffer&)
{
    return false;
}

bool BaseDataManager::SetCurrentFromPending(bool& hasPending)
{
    const auto pending = GetPendingSnapshot();
    hasPending = pending != nullptr;
    if (!pending) return true;
    TrustedImageSnapshot published;
    return SetCurrentFromPending(pending, published);
}

TrustedImageSnapshot BaseDataManager::GetPendingSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_pendingMutex);
    return m_impl->m_pending;
}

bool BaseDataManager::SetCurrentFromPending(
    const TrustedImageSnapshot& expectedPending,
    TrustedImageSnapshot& publishedSnapshot)
{
    publishedSnapshot.reset();
    if (!expectedPending || !expectedPending->image) return false;

    TrustedImageSnapshot retiredState;
    {
        // pending 与 current 在同一临界区完成 CAS 和 owner 转移：失败绝不清除
        // pending，成功也不复制或修改 VTK payload，保证读者只看到单调版本。
        std::scoped_lock lock(
            m_impl->m_dataMutex, m_impl->m_pendingMutex);
        if (m_impl->m_pending != expectedPending
            || !m_impl->m_current
            || m_impl->m_current->version
                == std::numeric_limits<DataVersion>::max()
            || expectedPending->version
                != m_impl->m_current->version + 1) {
            return false;
        }
        retiredState = std::move(m_impl->m_current);
        m_impl->m_current = std::move(m_impl->m_pending);
        publishedSnapshot = m_impl->m_current;
    }
    return true;
}

bool BaseDataManager::ClearPending()
{
    TrustedImageSnapshot retiredPending;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_pendingMutex);
        // 锁内只断开 pending 引用；旧候选在离开锁后析构，避免释放大体数据拉长临界区。
        retiredPending = std::move(m_impl->m_pending);
        m_impl->m_pending = {};
    }
    return true;
}

bool BaseDataManager::SetPendingImage(TrustedImageState image)
{
    if (!image.image) return false;
    const auto current = GetImageSnapshot();
    if (!current
        || current->version == std::numeric_limits<DataVersion>::max()) {
        return false;
    }
    image.version = current->version + 1;
    auto pending = std::make_shared<TrustedImageState>(std::move(image));
    TrustedImageSnapshot retiredPending;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_pendingMutex);
        // 单槽只保留最新候选；被覆盖批次在离开锁后随 retiredPending 析构。
        retiredPending = std::move(m_impl->m_pending);
        m_impl->m_pending = std::move(pending);
    }
    return true;
}

bool BaseDataManager::SetOwnedImage(vtkSmartPointer<vtkImageData> image)
{
    if (!image) {
        return false;
    }

    double range[2] = { 0.0, 0.0 };
    double imageSpacing[3] = { 1.0, 1.0, 1.0 };
    double imageOrigin[3] = { 0.0, 0.0, 0.0 };
    image->GetScalarRange(range);
    image->GetSpacing(imageSpacing);
    image->GetOrigin(imageOrigin);

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    return m_impl->SetCurrent({
        std::move(image),
        {},
        { dims[0], dims[1], dims[2] },
        { imageSpacing[0], imageSpacing[1], imageSpacing[2] },
        { imageOrigin[0], imageOrigin[1], imageOrigin[2] },
        { range[0], range[1] },
        0
    });
}

bool BaseDataManager::ExportSlices(
    const std::string& dirPath,
    Orientation orientation,
    const WindowLevelParams& windowLevel,
    const std::array<double, 16>& modelToWorldMatrix)
{
    return ExportSlices(
        dirPath,
        orientation,
        windowLevel,
        modelToWorldMatrix,
        TaskStopToken{});
}

bool BaseDataManager::ExportSlices(
    const std::string& dirPath,
    Orientation orientation,
    const WindowLevelParams& windowLevel,
    const std::array<double, 16>& modelToWorldMatrix,
    const TaskStopToken& stopToken)
{

    // 导出路径：1. 固定 current 批次并把 modelToWorld 取逆；2. 重采样到轴对齐体数据；
    // 3. 按 Orientation 将二维像素映射回 X/Y/Z；4. 应用窗宽窗位并逐层写 PNG。

    if (dirPath.empty() || stopToken.GetIsStopped()) {
        std::cerr << "[Export] Slice image export failed: output directory is empty." << std::endl;
        return false;
    }

    auto imageCopy = vtkSmartPointer<vtkImageData>::New();
    vtkSmartPointer<vtkImageData> maskCopy;
    std::shared_ptr<const TrustedImageState> currentState;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        currentState = m_impl->m_current;
    }
    if (!currentState->image) return false;
    imageCopy->ShallowCopy(currentState->image);
    if (currentState->validityMask) {
        maskCopy = vtkSmartPointer<vtkImageData>::New();
        maskCopy->ShallowCopy(
            currentState->validityMask);
    }

    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(modelToWorldMatrix.data());
    worldToModelMatrix->Invert();

    auto worldToModelTransform = vtkSmartPointer<vtkTransform>::New();
    worldToModelTransform->SetMatrix(worldToModelMatrix);

    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(imageCopy);
    reslice->SetResliceTransform(worldToModelTransform);
    reslice->SetInterpolationModeToLinear();
    reslice->SetOutputDimensionality(3);
    reslice->SetAutoCropOutput(true);

    double range[2] = { 0.0, 0.0 };
    imageCopy->GetScalarRange(range);
    reslice->SetBackgroundLevel(range[0]);

    try {
        reslice->Update();
    }
    catch (...) {
        return false;
    }
    if (stopToken.GetIsStopped()) return false;

    auto outputImage = reslice->GetOutput();
    if (!outputImage || outputImage->GetNumberOfPoints() == 0) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    outputImage->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }
    int outputExtent[6] = { 0, -1, 0, -1, 0, -1 };
    outputImage->GetExtent(outputExtent);

    vtkSmartPointer<vtkImageData> outputMask;
    if (maskCopy) {
        auto maskReslice =
            vtkSmartPointer<vtkImageReslice>::New();
        maskReslice->SetInputData(maskCopy);
        maskReslice->SetResliceTransform(
            worldToModelTransform);
        maskReslice->SetInterpolationModeToNearestNeighbor();
        maskReslice->SetOutputDimensionality(3);
        maskReslice->SetBackgroundLevel(0.0);
        maskReslice->SetOutputOrigin(
            outputImage->GetOrigin());
        maskReslice->SetOutputSpacing(
            outputImage->GetSpacing());
        maskReslice->SetOutputExtent(
            outputImage->GetExtent());
        try {
            maskReslice->Update();
        }
        catch (...) {
            return false;
        }
        if (stopToken.GetIsStopped()) return false;
        outputMask = maskReslice->GetOutput();
        if (!outputMask
            || outputMask->GetScalarType()
                != VTK_UNSIGNED_CHAR
            || outputMask->GetNumberOfScalarComponents()
                != 1) {
            return false;
        }
    }

    std::filesystem::path outputDir = PlatformPath::GetNativePath(dirPath);
    if (outputDir.has_extension()) {
        outputDir = outputDir.parent_path() / outputDir.stem();
    }
    if (outputDir.empty()) {
        return false;
    }

    if (stopToken.GetIsStopped()) return false;
    try {
        std::filesystem::create_directories(outputDir);
    }
    catch (...) {
        return false;
    }

    const int sliceAxis = static_cast<int>(orientation); // 与 Orientation 枚举值保持一致
    const int sliceCount = dims[sliceAxis];
    const std::array<int, 2> sliceSize = m_impl->GetSliceSize(dims, orientation); // 当前导出图片宽高
    const int width = sliceSize[0];
    const int height = sliceSize[1];
    const int digits = std::max(4, static_cast<int>(std::to_string(std::max(sliceCount - 1, 0)).size()));
    const std::string orientationName = m_impl->GetOrientName(orientation);

    for (int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        if (stopToken.GetIsStopped()) return false;
        auto sliceImage = vtkSmartPointer<vtkImageData>::New();
        sliceImage->SetDimensions(width, height, 1);
        sliceImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

        auto* dst = static_cast<unsigned char*>(sliceImage->GetScalarPointer());
        if (!dst) {
            return false;
        }
        for (int py = 0; py < height; ++py) {
            if (stopToken.GetIsStopped()) return false;
            for (int px = 0; px < width; ++px) {
                int x = 0;
                int y = 0;
                int z = 0;

                // A. TopDown 固定 Z，图片横纵轴映射为 X/Y。
                if (orientation == Orientation::Top_down) {
                    x = outputExtent[0] + px;
                    y = outputExtent[2] + py;
                    z = outputExtent[4] + sliceIndex;
                }
                // B. FrontBack 固定 Y，图片横纵轴映射为 X/Z。
                else if (orientation == Orientation::Front_back) {
                    x = outputExtent[0] + px;
                    y = outputExtent[2] + sliceIndex;
                    z = outputExtent[4] + py;
                }
                // C. LeftRight 固定 X，图片横纵轴映射为 Y/Z。
                else {
                    x = outputExtent[0] + sliceIndex;
                    y = outputExtent[2] + px;
                    z = outputExtent[4] + py;
                }
                const bool isValid = !outputMask
                    || outputMask->GetScalarComponentAsDouble(
                        x, y, z, 0) != 0.0;
                const double value =
                    outputImage->GetScalarComponentAsDouble(
                        x, y, z, 0);
                dst[py * width + px] = isValid
                    ? m_impl->GetWindowGray(
                        value, windowLevel)
                    : 0;
            }
        }

        std::ostringstream fileName;
        fileName << orientationName << "_"
            << std::setw(digits) << std::setfill('0') << sliceIndex
            << ".png";

        auto writer = vtkSmartPointer<vtkPNGWriter>::New();
        const std::filesystem::path outputFilePath = outputDir / fileName.str();
        const std::string vtkFileName = PlatformPath::GetUtf8Path(outputFilePath);
        writer->SetFileName(vtkFileName.c_str());
        writer->SetInputData(sliceImage);
        if (stopToken.GetIsStopped()) return false;
        writer->Write();
        if (writer->GetErrorCode() != 0
            || stopToken.GetIsStopped()) {
            return false;
        }
    }

    return true;
}

bool BaseDataManager::ExportData(
    const TrustedImageSnapshot& imageSnapshot,
    const std::string& outputDir,
    const DataExportParams& params)
{
    return ExportData(
        imageSnapshot,
        outputDir,
        params,
        TaskStopToken{});
}

bool BaseDataManager::ExportData(
    const TrustedImageSnapshot& imageSnapshot,
    const std::string& outputDir,
    const DataExportParams& params,
    const TaskStopToken& stopToken)
{
    if (!imageSnapshot || !imageSnapshot->image
        || imageSnapshot->image->GetNumberOfPoints() == 0
        || outputDir.empty()
        || stopToken.GetIsStopped()
        || !Impl::GetMaskValid(
            imageSnapshot->image,
            imageSnapshot->validityMask)) {
        return false;
    }

    // RAW 与网格必须解释同一个数值可逆 affine model-to-world；非法矩阵不得先创建输出目录。
    if (!Impl::GetIsAffine(params.modelToWorld)) {
        return false;
    }

    std::string normalizedExtension = params.extension;
    std::transform(
        normalizedExtension.begin(),
        normalizedExtension.end(),
        normalizedExtension.begin(),
        [](unsigned char value) {
            return static_cast<char>(
                std::tolower(value));
        });
    if (normalizedExtension != ".raw"
        && normalizedExtension != ".ply"
        && normalizedExtension != ".stl"
        && normalizedExtension != ".obj") {
        return false;
    }

    const auto nativeOutputDir =
        PlatformPath::GetNativePath(outputDir);
    try {
        std::filesystem::create_directories(
            nativeOutputDir);
        if (!std::filesystem::is_directory(
                nativeOutputDir)) {
            return false;
        }
    }
    catch (...) {
        return false;
    }
    if (stopToken.GetIsStopped()) return false;

    if (normalizedExtension == ".raw") {
        return Impl::ExportRaw(
            imageSnapshot, outputDir,
            params.modelToWorld,
            stopToken);
    }
    DataExportParams normalizedParams = params;
    normalizedParams.extension = std::move(
        normalizedExtension);
    return Impl::ExportMesh(
        imageSnapshot, outputDir,
        normalizedParams,
        stopToken);
}

bool BaseDataManager::Impl::GetIsAffine(
    const std::array<double, 16>& modelToWorld)
{
    constexpr double affineTolerance = 1e-12;
    if (std::any_of(
            modelToWorld.begin(),
            modelToWorld.end(),
            [](double value) {
                return !std::isfinite(value);
            })
        || std::abs(modelToWorld[12]) > affineTolerance
        || std::abs(modelToWorld[13]) > affineTolerance
        || std::abs(modelToWorld[14]) > affineTolerance
        || std::abs(modelToWorld[15] - 1.0)
            > affineTolerance) {
        return false;
    }

    // 行范数乘积给出 determinant 的自然尺度；相对判定不会误拒绝合法的小尺度 affine。
    std::array<double, 3> rowNorms = {};
    for (int row = 0; row < 3; ++row) {
        const int offset = row * 4;
        rowNorms[static_cast<std::size_t>(row)] =
            std::hypot(
                std::hypot(
                    modelToWorld[offset],
                    modelToWorld[offset + 1]),
                modelToWorld[offset + 2]);
    }
    const double determinantScale =
        rowNorms[0] * rowNorms[1] * rowNorms[2];
    if (!std::isfinite(determinantScale)
        || determinantScale == 0.0) {
        return false;
    }

    auto modelMatrix =
        vtkSmartPointer<vtkMatrix4x4>::New();
    modelMatrix->DeepCopy(modelToWorld.data());
    const double determinant =
        std::abs(modelMatrix->Determinant());
    constexpr double determinantFactor = 8.0;
    return std::isfinite(determinant)
        && determinant > determinantFactor
            * std::numeric_limits<double>::epsilon()
            * determinantScale;
}

std::filesystem::path
BaseDataManager::Impl::BuildExportPath(
    const std::string& outputDir,
    const int dimensions[3],
    const std::string& extension)
{
    const std::string fileName =
        std::to_string(dimensions[0])
        + "x" + std::to_string(dimensions[1])
        + "x" + std::to_string(dimensions[2])
        + "_transform" + extension;
    return PlatformPath::GetNativePath(outputDir)
        / std::filesystem::path(fileName);
}

bool BaseDataManager::Impl::ExportRaw(
    const TrustedImageSnapshot& imageSnapshot,
    const std::string& outputDir,
    const std::array<double, 16>& modelToWorldMatrix,
    const TaskStopToken& stopToken)
{
    // RAW 导出路径：固定接纳时的 immutable image/mask -> 逆变换到同一输出网格 ->
    // 把无效体素物化为背景值 -> 按 VTK increments 写出无头、X-fast 的 float32 数据。
    if (!imageSnapshot || !imageSnapshot->image
        || outputDir.empty() || stopToken.GetIsStopped()) {
        return false;
    }
    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    // snapshot 批次不可变，因此可建立只读浅拷贝供导出管线使用。
    imageCopy->ShallowCopy(imageSnapshot->image);

    //  VTK 逆变换矩阵
    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(modelToWorldMatrix.data());
    worldToModelMatrix->Invert();

    auto worldToModelTransform = vtkSmartPointer<vtkTransform>::New();
    worldToModelTransform->SetMatrix(worldToModelMatrix);

    // 利用 vtkImageReslice 进行核心插值运算
    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(imageCopy);
    reslice->SetResliceTransform(worldToModelTransform);
    reslice->SetInterpolationModeToLinear();

    // VTK 会自动计算旋转后新的 Bounding Box，避免模型的边角被切割。
    // 这会导致输出的数据维度（Dimensions）发生变化。
    reslice->SetOutputDimensionality(3);

    reslice->SetAutoCropOutput(true);
    double range[2] = {};
    imageCopy->GetScalarRange(range);
    reslice->SetBackgroundLevel(range[0]); // 取真实的最小标量值

    try {
        // 更新管线，触发计算
        reslice->Update();
    }
    catch (...) {
        std::cerr << "[Error] Exception during image reslicing/changing info." << std::endl;
        return false;
    }
    if (stopToken.GetIsStopped()) return false;

    auto outputImage = reslice->GetOutput();
    if (!outputImage || outputImage->GetNumberOfPoints() == 0) {
        std::cerr << "[Error] Reslice produced an empty image!" << std::endl;
        return false;
    }

    // RAW 契约固定为单分量 float32；其它标量布局必须显式拒绝，不能按 float* 误解释。
    if (outputImage->GetScalarType() != VTK_FLOAT
        || outputImage->GetNumberOfScalarComponents() != 1) {
        return false;
    }
    int newDims[3] = {};
    outputImage->GetDimensions(newDims);
    if (newDims[0] <= 0 || newDims[1] <= 0 || newDims[2] <= 0) {
        return false;
    }
    int outputExtent[6] = {};
    outputImage->GetExtent(outputExtent);
    auto* outDataPtr = static_cast<float*>(
        outputImage->GetScalarPointer(
            outputExtent[0], outputExtent[2], outputExtent[4]));

    if (!outDataPtr) {
        return false;
    }

    vtkIdType incs[3] = {};
    outputImage->GetIncrements(incs);

    const bool hasMask = imageSnapshot->validityMask != nullptr;
    const unsigned char* maskDataPtr = nullptr;
    int maskExtent[6] = {};
    vtkIdType maskIncs[3] = {};
    std::array<double, 3> maskStart = {};
    std::array<double, 3> maskStepX = {};
    std::array<double, 3> maskStepY = {};
    std::array<double, 3> maskStepZ = {};
    constexpr double maskRoundTolerance =
        7.62939453125e-06; // 与 VTK 9.4 nearest interpolator 的 2^-17 容差一致。
    const float backgroundValue = static_cast<float>(range[0]);
    if (hasMask) {
        const auto* outputIndexToPhysical =
            outputImage->GetIndexToPhysicalMatrix();
        const auto* maskPhysicalToIndex =
            imageSnapshot->validityMask->GetPhysicalToIndexMatrix();
        if (!outputIndexToPhysical || !maskPhysicalToIndex) {
            return false;
        }

        auto outputIndexToModel =
            vtkSmartPointer<vtkMatrix4x4>::New();
        auto outputIndexToMask =
            vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMatrix4x4::Multiply4x4(
            worldToModelMatrix,
            outputIndexToPhysical,
            outputIndexToModel);
        vtkMatrix4x4::Multiply4x4(
            maskPhysicalToIndex,
            outputIndexToModel,
            outputIndexToMask);
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!std::isfinite(
                        outputIndexToMask->GetElement(
                            row, column))) {
                    return false;
                }
            }
        }

        imageSnapshot->validityMask->GetExtent(maskExtent);
        imageSnapshot->validityMask->GetIncrements(maskIncs);
        maskDataPtr = static_cast<const unsigned char*>(
            imageSnapshot->validityMask->GetScalarPointer(
                maskExtent[0], maskExtent[2], maskExtent[4]));
        if (!maskDataPtr) {
            return false;
        }
        for (int axis = 0; axis < 3; ++axis) {
            maskStart[static_cast<std::size_t>(axis)] =
                outputIndexToMask->GetElement(axis, 0)
                    * static_cast<double>(outputExtent[0])
                + outputIndexToMask->GetElement(axis, 1)
                    * static_cast<double>(outputExtent[2])
                + outputIndexToMask->GetElement(axis, 2)
                    * static_cast<double>(outputExtent[4])
                + outputIndexToMask->GetElement(axis, 3);
            maskStepX[static_cast<std::size_t>(axis)] =
                outputIndexToMask->GetElement(axis, 0);
            maskStepY[static_cast<std::size_t>(axis)] =
                outputIndexToMask->GetElement(axis, 1);
            maskStepZ[static_cast<std::size_t>(axis)] =
                outputIndexToMask->GetElement(axis, 2);
        }
    }

    const auto finalPath = BuildExportPath(
        outputDir, newDims, ".raw");

    // 按 x-fast、逐行无 padding 的 float32 裸数据写出，不附带维度、spacing、origin 等元数据。
    std::ofstream rawFile(finalPath, std::ios::binary);
    if (!rawFile.is_open()) {
        std::cerr
            << "[Error] Failed to open RAW file for writing: "
            << PlatformPath::GetUtf8Path(finalPath)
            << std::endl;
        return false;
    }

    // 由于旋转后行尾可能有 Padding（根据 VTK 内部内存布局），不能直接写入整块内存。
    size_t rowBytes = static_cast<size_t>(newDims[0]) * sizeof(float);
    for (int z = 0; z < newDims[2]; ++z) {
        if (stopToken.GetIsStopped()) return false;
        for (int y = 0; y < newDims[1]; ++y) {
            if (stopToken.GetIsStopped()) return false;
            // 利用真实步长计算出当前行准确的内存起始地址
            float* rowPtr = outDataPtr + z * incs[2] + y * incs[1];
            if (hasMask) {
                std::array<double, 3> maskIndex = {};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    maskIndex[axis] = maskStart[axis]
                        + static_cast<double>(y) * maskStepY[axis]
                        + static_cast<double>(z) * maskStepZ[axis];
                }
                for (int x = 0; x < newDims[0]; ++x) {
                    bool isValid = true;
                    int sourceIndex[3] = {};
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const int extentOffset =
                            static_cast<int>(axis) * 2;
                        const double value = maskIndex[axis];
                        const double minValue = static_cast<double>(
                            maskExtent[extentOffset]) - 0.5;
                        const double maxValue = static_cast<double>(
                            maskExtent[extentOffset + 1]) + 0.5;
                        if (!std::isfinite(value)
                            || value < minValue
                            || value > maxValue) {
                            isValid = false;
                            break;
                        }
                        const double roundedValue = std::floor(
                            value + 0.5 + maskRoundTolerance);
                        sourceIndex[axis] = static_cast<int>(
                            std::clamp(
                                roundedValue,
                                static_cast<double>(
                                    maskExtent[extentOffset]),
                                static_cast<double>(
                                    maskExtent[extentOffset + 1])));
                    }
                    if (isValid) {
                        const vtkIdType maskOffset =
                            static_cast<vtkIdType>(
                                sourceIndex[0] - maskExtent[0])
                                * maskIncs[0]
                            + static_cast<vtkIdType>(
                                sourceIndex[1] - maskExtent[2])
                                * maskIncs[1]
                            + static_cast<vtkIdType>(
                                sourceIndex[2] - maskExtent[4])
                                * maskIncs[2];
                        isValid = maskDataPtr[maskOffset] != 0;
                    }
                    if (!isValid) {
                        rowPtr[x * incs[0]] = backgroundValue;
                    }
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        maskIndex[axis] += maskStepX[axis];
                    }
                }
            }
            // 每次只写入当前行真正有效的数据宽度（摒弃行尾的 Padding）
            rawFile.write(reinterpret_cast<const char*>(rowPtr), rowBytes);
            if (!rawFile) {
                return false;
            }
        }
    }

    rawFile.close();
    if (!rawFile || stopToken.GetIsStopped()) {
        return false;
    }
    std::cout << "[Export] Successfully saved transformed RAW to: "
        << PlatformPath::GetUtf8Path(finalPath) << "\n"
        << "[Export] IMPORTANT: New Dimensions are "
        << newDims[0] << " x " << newDims[1] << " x " << newDims[2] << std::endl;

    return true;
}

bool BaseDataManager::Impl::ExportMesh(
    const TrustedImageSnapshot& imageSnapshot,
    const std::string& outputDir,
    const DataExportParams& params,
    const TaskStopToken& stopToken)
{
    if (!imageSnapshot || !imageSnapshot->image
        || imageSnapshot->image->GetNumberOfPoints() == 0
        || outputDir.empty()
        || stopToken.GetIsStopped()
        || !std::isfinite(params.isoValue)) {
        return false;
    }

    // 1. 全分辨率 image 与 mask 来自同一个 TrustedImageState，不读取显示 mapper。
    auto imageCopy =
        vtkSmartPointer<vtkImageData>::New();
    imageCopy->ShallowCopy(imageSnapshot->image);
    auto isoFilter =
        vtkSmartPointer<vtkFlyingEdges3D>::New();
    isoFilter->SetInputData(imageCopy);
    isoFilter->SetValue(0, params.isoValue);
    isoFilter->ComputeNormalsOn();
    isoFilter->ComputeGradientsOff();

    vtkAlgorithmOutput* surfacePort =
        isoFilter->GetOutputPort();
    vtkSmartPointer<vtkImageData> maskCopy;
    vtkSmartPointer<vtkImplicitVolume> maskFunction;
    vtkSmartPointer<vtkClipPolyData> maskClip;
    if (imageSnapshot->validityMask) {
        maskCopy = vtkSmartPointer<vtkImageData>::New();
        maskCopy->ShallowCopy(
            imageSnapshot->validityMask);
        maskFunction =
            vtkSmartPointer<vtkImplicitVolume>::New();
        maskFunction->SetVolume(maskCopy);
        maskFunction->SetOutValue(-1.0);
        maskClip =
            vtkSmartPointer<vtkClipPolyData>::New();
        maskClip->SetInputConnection(surfacePort);
        maskClip->SetClipFunction(maskFunction);
        // 二值 mask 为 0/255；127.5 令边界落在相邻体素中心之间。
        maskClip->SetValue(127.5);
        maskClip->InsideOutOff();
        maskClip->GenerateClippedOutputOff();
        surfacePort = maskClip->GetOutputPort();
    }

    // 2. 文件没有 actor，在写出前把 model-to-world 烘焙到点和法向量。
    auto modelMatrix =
        vtkSmartPointer<vtkMatrix4x4>::New();
    modelMatrix->DeepCopy(
        params.modelToWorld.data());
    auto modelToWorld =
        vtkSmartPointer<vtkTransform>::New();
    modelToWorld->SetMatrix(modelMatrix);
    auto transformFilter =
        vtkSmartPointer<
            vtkTransformPolyDataFilter>::New();
    transformFilter->SetInputConnection(
        surfacePort);
    transformFilter->SetTransform(modelToWorld);

    // 3. 三种 writer 共用同一份三角网格；仅在 Data 底层按后缀选择序列化器。
    auto triangleFilter =
        vtkSmartPointer<vtkTriangleFilter>::New();
    triangleFilter->SetInputConnection(
        transformFilter->GetOutputPort());
    try {
        triangleFilter->Update();
    }
    catch (...) {
        std::cerr
            << "[Export] PolyData generation failed.\n";
        return false;
    }
    if (stopToken.GetIsStopped()) return false;
    vtkPolyData* outputMesh =
        triangleFilter->GetOutput();
    if (!outputMesh
        || outputMesh->GetNumberOfPoints() == 0
        || outputMesh->GetNumberOfCells() == 0) {
        return false;
    }

    int sourceDims[3] = {};
    imageCopy->GetDimensions(sourceDims);
    const auto outputPath = BuildExportPath(
        outputDir, sourceDims, params.extension);
    const std::string vtkFileName =
        PlatformPath::GetUtf8Path(outputPath);
    int errorCode = vtkErrorCode::NoError;
    if (params.extension == ".ply") {
        if (!BuildMeshColors(
                outputMesh, params, stopToken)) {
            return false;
        }
        auto writer =
            vtkSmartPointer<vtkPLYWriter>::New();
        writer->SetFileName(vtkFileName.c_str());
        writer->SetFileTypeToBinary();
        writer->SetArrayName("RGB");
        writer->SetColorModeToDefault();
        writer->SetInputData(outputMesh);
        if (stopToken.GetIsStopped()) return false;
        writer->Write();
        errorCode = writer->GetErrorCode();
    }
    else if (params.extension == ".stl") {
        auto writer =
            vtkSmartPointer<vtkSTLWriter>::New();
        writer->SetFileName(vtkFileName.c_str());
        writer->SetFileTypeToBinary();
        writer->SetInputData(outputMesh);
        if (stopToken.GetIsStopped()) return false;
        writer->Write();
        errorCode = writer->GetErrorCode();
    }
    else if (params.extension == ".obj") {
        auto writer =
            vtkSmartPointer<vtkOBJWriter>::New();
        writer->SetFileName(vtkFileName.c_str());
        writer->SetInputData(outputMesh);
        if (stopToken.GetIsStopped()) return false;
        writer->Write();
        errorCode = writer->GetErrorCode();
    }
    else {
        return false;
    }

    std::error_code fileError;
    const auto fileSize =
        std::filesystem::file_size(
            outputPath, fileError);
    return errorCode == vtkErrorCode::NoError
        && !fileError && fileSize > 0
        && !stopToken.GetIsStopped();
}

bool BaseDataManager::Impl::BuildMeshColors(
    vtkPolyData* mesh,
    const DataExportParams& params,
    const TaskStopToken& stopToken)
{
    if (!mesh || !mesh->GetPointData()
        || mesh->GetNumberOfPoints() == 0
        || !std::isfinite(params.scalarRange[0])
        || !std::isfinite(params.scalarRange[1])
        || params.scalarRange[1] < params.scalarRange[0]
        || stopToken.GetIsStopped()) {
        return false;
    }

    // 1. 真实 scalar 节点与数据快照在同一次任务接纳时冻结；
    // PLY 颜色不读取 renderer 或 framebuffer。
    auto colorMap =
        vtkSmartPointer<vtkColorTransferFunction>::New();
    double previousScalar = 0.0;
    const auto& function = params.volumeTransferFunction;
    if (!function.colorNodes.empty()
        && function.colorNodes.size() < 2) {
        return false;
    }
    for (std::size_t index = 0;
        index < function.colorNodes.size(); ++index) {
        const auto& node = function.colorNodes[index];
        if (!std::isfinite(node.scalar)
            || !std::isfinite(node.r)
            || !std::isfinite(node.g)
            || !std::isfinite(node.b)
            || node.r < 0.0 || node.r > 1.0
            || node.g < 0.0 || node.g > 1.0
            || node.b < 0.0 || node.b > 1.0
            || (index > 0
                && node.scalar <= previousScalar)) {
            return false;
        }
        colorMap->AddRGBPoint(
            node.scalar, node.r, node.g, node.b);
        previousScalar = node.scalar;
    }
    if (function.colorNodes.empty()) {
        // 没有显式 TF 时使用数据域灰阶；常量域使用白色，避免生成未定义颜色。
        if (params.scalarRange[1] > params.scalarRange[0]) {
            colorMap->AddRGBPoint(
                params.scalarRange[0], 0.0, 0.0, 0.0);
            colorMap->AddRGBPoint(
                params.scalarRange[1], 1.0, 1.0, 1.0);
        }
        else {
            colorMap->AddRGBPoint(
                params.scalarRange[0], 1.0, 1.0, 1.0);
        }
    }

    // 2. 颜色必须逐点来自最终网格标量；缺失 point scalar 时拒绝，避免静默退化为统一 iso 颜色。
    vtkDataArray* pointScalars =
        mesh->GetPointData()->GetScalars();
    if (!pointScalars
        || pointScalars->GetNumberOfComponents() < 1
        || pointScalars->GetNumberOfTuples()
            != mesh->GetNumberOfPoints()) {
        return false;
    }
    auto colors =
        vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("RGB");
    colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(
        mesh->GetNumberOfPoints());
    for (vtkIdType pointId = 0;
        pointId < mesh->GetNumberOfPoints(); ++pointId) {
        if ((pointId % 4096) == 0
            && stopToken.GetIsStopped()) {
            return false;
        }
        const double scalar =
            pointScalars->GetComponent(pointId, 0);
        if (!std::isfinite(scalar)) {
            return false;
        }
        double mapped[3] = {};
        colorMap->GetColor(scalar, mapped);
        unsigned char rgb[3] = {};
        for (int component = 0; component < 3;
            ++component) {
            const double normalized = std::clamp(
                mapped[component], 0.0, 1.0);
            rgb[component] = static_cast<unsigned char>(
                std::lround(normalized * 255.0));
        }
        colors->SetTypedTuple(pointId, rgb);
    }

    mesh->GetPointData()->AddArray(colors);
    return mesh->GetPointData()->SetActiveScalars("RGB") >= 0
        && !stopToken.GetIsStopped();
}

RawVolumeDataManager::RawVolumeDataManager()
{
}

RawVolumeDataManager::~RawVolumeDataManager() = default;

bool RawVolumeDataManager::SetDataLoaded(
    const std::string& filePath,
    const VolumeLayout& layout)
{
    return SetDataLoaded(
        filePath, layout, TaskStopToken{});
}

bool RawVolumeDataManager::SetDataLoaded(
    const std::string& filePath,
    const VolumeLayout& layout,
    const TaskStopToken& stopToken)
{
    if (stopToken.GetIsStopped()) return false;
    const auto& dimensions = layout.GetDimensions();
    const int rasDims[3] = {
        dimensions[0], dimensions[1], dimensions[2]
    };
    std::error_code fileError;
    const auto fileBytes = std::filesystem::file_size(
        PlatformPath::GetNativePath(filePath), fileError);
    if (fileError || fileBytes != layout.GetByteCount()
        || stopToken.GetIsStopped()) {
        return false;
    }

    // 输入 layout 明确描述 LPS 物理空间；加载链只负责转换为内部 RAS 空间。
    const auto& spacing = layout.GetSpacing();
    const auto& origin = layout.GetOrigin();
    const std::array<double, 3> lpsSpacing = {
        static_cast<double>(spacing[0]),
        static_cast<double>(spacing[1]),
        static_cast<double>(spacing[2])
    };
    const std::array<double, 3> lpsOrigin = {
        static_cast<double>(origin[0]),
        static_cast<double>(origin[1]),
        static_cast<double>(origin[2])
    };
    const std::array<double, 3> rasSpacing = lpsSpacing;
    const std::array<double, 3> rasOrigin = BaseDataManager::Impl::GetRasOrigin(
        lpsOrigin, rasDims, rasSpacing);

    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(rasDims[0], rasDims[1], rasDims[2]);
    newImage->SetSpacing(rasSpacing[0], rasSpacing[1], rasSpacing[2]);
    newImage->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);

    float* dst = static_cast<float*>(newImage->GetScalarPointer());
    MemMappedFile mmf;
    if (!mmf.Load(filePath) || stopToken.GetIsStopped()
        || mmf.GetSize() != layout.GetByteCount()
        || !m_impl->SetRasScalars(
            static_cast<const float*>(mmf.GetData()), dst,
            rasDims, layout.GetVoxelCount(), stopToken)) {
        return false;
    }

    newImage->Modified();
    double range[2] = { 0.0, 0.0 };
    newImage->GetScalarRange(range);

    if (stopToken.GetIsStopped()) return false;
    return SetPendingImage({
        std::move(newImage),
        {},
        dimensions,
        rasSpacing,
        rasOrigin,
        { range[0], range[1] },
        0
    });
}

bool RawVolumeDataManager::SetFromBuffer(
    const VolumeBuffer& buffer)
{
    return SetFromBuffer(buffer, TaskStopToken{});
}

bool RawVolumeDataManager::SetFromBuffer(
    const VolumeBuffer& buffer,
    const TaskStopToken& stopToken)
{
    if (stopToken.GetIsStopped()) return false;
    const auto& layout = buffer.GetLayout();
    const auto& dims = layout.GetDimensions();
    const auto& spacing = layout.GetSpacing();
    const auto& origin = layout.GetOrigin();
    const int rasDims[3] = { dims[0], dims[1], dims[2] };
    const std::array<double, 3> rasSpacing = {
        static_cast<double>(spacing[0]),
        static_cast<double>(spacing[1]),
        static_cast<double>(spacing[2])
    };
    const std::array<double, 3> lpsOrigin = {
        static_cast<double>(origin[0]),
        static_cast<double>(origin[1]),
        static_cast<double>(origin[2])
    };
    const std::array<double, 3> rasOrigin = BaseDataManager::Impl::GetRasOrigin(
        lpsOrigin, rasDims, rasSpacing);
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(dims[0], dims[1], dims[2]);
    newImage->SetSpacing(rasSpacing[0], rasSpacing[1], rasSpacing[2]);
    newImage->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());
    if (!m_impl->SetRasScalars(
        buffer.GetVoxels().data(),
        dst,
        rasDims,
        layout.GetVoxelCount(),
        stopToken)) {
        return false;
    }
    double range[2] = { 0.0, 0.0 };
    newImage->GetScalarRange(range);

    if (stopToken.GetIsStopped()) return false;
    return SetPendingImage({
        std::move(newImage), {}, dims, rasSpacing, rasOrigin,
        { range[0], range[1] }, 0 });
}

bool RawVolumeDataManager::SetImageSnapshot(vtkSmartPointer<vtkImageData> image)
{
    // 该入口接收已构造的 VTK image，只校验并发布 pending 快照，不直接替换 current 真源。
    if (!image) {
        return false;
    }

    auto imageCopy = vtkSmartPointer<vtkImageData>::New();
    imageCopy->DeepCopy(image);

    int dims[3] = { 0, 0, 0 };
    imageCopy->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 || !imageCopy->GetScalarPointer()) {
        return false;
    }

    double range[2] = { 0.0, 0.0 };
    imageCopy->GetScalarRange(range);
    const std::array<double, 3> imageSpacing = {
        imageCopy->GetSpacing()[0],
        imageCopy->GetSpacing()[1],
        imageCopy->GetSpacing()[2]
    };
    const std::array<double, 3> imageOrigin = {
        imageCopy->GetOrigin()[0],
        imageCopy->GetOrigin()[1],
        imageCopy->GetOrigin()[2]
    };

    return SetPendingImage({
        std::move(imageCopy),
        {},
        { dims[0], dims[1], dims[2] },
        imageSpacing,
        imageOrigin,
        { range[0], range[1] },
        0 });
}

bool RawVolumeDataManager::SetCurrentFromPending(bool& hasPending)
{
    return BaseDataManager::SetCurrentFromPending(hasPending);
}

bool RawVolumeDataManager::ClearPending()
{
    return BaseDataManager::ClearPending();
}

TiffVolumeDataManager::TiffVolumeDataManager()
    : m_impl(std::make_unique<TiffVolumeDataManager::Impl>())
{
}

TiffVolumeDataManager::~TiffVolumeDataManager() = default;

bool TiffVolumeDataManager::SetDataLoaded(
    const std::string& inputPath,
    const VolumeLayout& layout)
{
    return SetDataLoaded(
        inputPath, layout, TaskStopToken{});
}

bool TiffVolumeDataManager::SetDataLoaded(
    const std::string& inputPath,
    const VolumeLayout& layout,
    const TaskStopToken& stopToken)
{
    if (!m_impl || stopToken.GetIsStopped()) {
        return false;
    }

    auto image = m_impl->LoadImage(
        inputPath, layout, stopToken);
    if (!image) return false;
    double range[2] = { 0.0, 0.0 };
    image->GetScalarRange(range);
    const auto& dimensions = layout.GetDimensions();
    double spacing[3] = { 1.0, 1.0, 1.0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetSpacing(spacing);
    image->GetOrigin(origin);
    if (stopToken.GetIsStopped()) return false;
    return SetPendingImage({
        std::move(image), {}, dimensions,
        { spacing[0], spacing[1], spacing[2] },
        { origin[0], origin[1], origin[2] },
        { range[0], range[1] }, 0 });
}

vtkSmartPointer<vtkImageData> TiffVolumeDataManager::Impl::LoadImage(
    const std::string& inputPath,
    const VolumeLayout& layout,
    const TaskStopToken& stopToken) {
    // 路径检查
    const std::filesystem::path pathObj = PlatformPath::GetNativePath(inputPath);
    if (stopToken.GetIsStopped()
        || !std::filesystem::exists(pathObj)) {
        std::cerr << "[Error] Path does not exist: " << inputPath << std::endl;
        return nullptr;
    }

    vtkSmartPointer<vtkImageData> decodedImage;
    if (std::filesystem::is_directory(pathObj)) {
        //
        std::cout << "[Info] Loading TIFF series from folder: " << inputPath << std::endl;

        std::vector<std::string> fileList;

        try {
            for (const auto& entry : std::filesystem::directory_iterator(pathObj)) {
                if (stopToken.GetIsStopped()) return nullptr;
                if (entry.is_regular_file()) {
                    std::string ext = PlatformPath::GetUtf8Path(entry.path().extension());
                    // 转小写比较，兼容 .TIF 和 .tif
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

                    if (ext == ".tif" || ext == ".tiff") {
                        fileList.push_back(PlatformPath::GetUtf8Path(entry.path()));
                    }
                }
            }
        }
        catch (...) {
            std::cerr << "[Error] Failed to enumerate TIFF directory." << std::endl;
            return nullptr;
        }

        if (fileList.empty()) {
            std::cerr << "[Error] No .tif files found in folder." << std::endl;
            return nullptr;
        }

        auto naturalSort = [](const std::string& s1, const std::string& s2) {
            size_t i = 0, j = 0;
            const auto isDigit = [](char value) {
                return std::isdigit(static_cast<unsigned char>(value)) != 0;
            };
            while (i < s1.size() && j < s2.size()) {
                if (isDigit(s1[i]) && isDigit(s2[j])) {
                    const size_t digitStart1 = i;
                    const size_t digitStart2 = j;
                    while (i < s1.size() && isDigit(s1[i])) {
                        ++i;
                    }
                    while (j < s2.size() && isDigit(s2[j])) {
                        ++j;
                    }
                    size_t significant1 = digitStart1;
                    size_t significant2 = digitStart2;
                    while (significant1 < i && s1[significant1] == '0') {
                        ++significant1;
                    }
                    while (significant2 < j && s2[significant2] == '0') {
                        ++significant2;
                    }
                    const size_t length1 = i - significant1;
                    const size_t length2 = j - significant2;
                    if (length1 != length2) {
                        return length1 < length2;
                    }
                    const int digitCompare = s1.compare(
                        significant1, length1, s2, significant2, length2);
                    if (digitCompare != 0) {
                        return digitCompare < 0;
                    }
                }
                else {
                    // 非数字字符，按标准 ASCII 比较
                    // (如果需要忽略大小写，可在此处转 tolower)
                    if (s1[i] != s2[j]) {
                        return s1[i] < s2[j];
                    }
                    i++;
                    j++;
                }
            }
            // 如果一个字符串是另一个的前缀，短的在前
            return s1.size() < s2.size();
        };


        // 排序
        std::sort(fileList.begin(), fileList.end(), naturalSort);

        // 每张 TIFF 独立解码后沿 Z 轴组装，避开批量 SetFileNames 在部分 Windows
        // libtiff 构建中的跨文件状态损坏；fileList 的自然排序仍是切片顺序真源。
        auto append = vtkSmartPointer<vtkImageAppend>::New();
        append->SetAppendAxis(2);
        std::vector<vtkSmartPointer<vtkImageData>> slices;
        slices.reserve(fileList.size());
        int expectedExtent[6] = { 0, -1, 0, -1, 0, -1 };
        int expectedScalarType = VTK_VOID;
        int expectedComponents = 0;
        try {
            for (const auto& file : fileList) {
                if (stopToken.GetIsStopped()) return nullptr;
                auto sliceReader = vtkSmartPointer<vtkTIFFReader>::New();
                sliceReader->SetFileName(file.c_str());
                if (!sliceReader->CanReadFile(file.c_str())) {
                    return nullptr;
                }
                sliceReader->Update();
                if (stopToken.GetIsStopped()
                    || sliceReader->GetErrorCode() != 0
                    || !sliceReader->GetOutput()
                    || sliceReader->GetOutput()->GetNumberOfPoints() == 0) {
                    return nullptr;
                }
                auto slice = vtkSmartPointer<vtkImageData>::New();
                slice->DeepCopy(sliceReader->GetOutput());
                if (stopToken.GetIsStopped()) return nullptr;
                int sliceExtent[6] = { 0, -1, 0, -1, 0, -1 };
                slice->GetExtent(sliceExtent);
                if (slice->GetDimensions()[0] <= 0
                    || slice->GetDimensions()[1] <= 0
                    || slice->GetDimensions()[2] != 1) {
                    return nullptr;
                }
                if (slices.empty()) {
                    std::copy(std::begin(sliceExtent), std::end(sliceExtent), expectedExtent);
                    expectedScalarType = slice->GetScalarType();
                    expectedComponents = slice->GetNumberOfScalarComponents();
                }
                else if (sliceExtent[0] != expectedExtent[0]
                    || sliceExtent[1] != expectedExtent[1]
                    || sliceExtent[2] != expectedExtent[2]
                    || sliceExtent[3] != expectedExtent[3]
                    || slice->GetScalarType() != expectedScalarType
                    || slice->GetNumberOfScalarComponents() != expectedComponents) {
                    return nullptr;
                }
                slices.push_back(slice);
                append->AddInputData(slice);
            }
            if (stopToken.GetIsStopped()) return nullptr;
            append->Update();
        }
        catch (...) {
            std::cerr << "[Error] Exception during TIFF series reading." << std::endl;
            return nullptr;
        }
        if (stopToken.GetIsStopped()
            || append->GetErrorCode() != 0
            || !append->GetOutput()
            || append->GetOutput()->GetNumberOfPoints() == 0) {
            return nullptr;
        }
        decodedImage = vtkSmartPointer<vtkImageData>::New();
        decodedImage->DeepCopy(append->GetOutput());
        if (stopToken.GetIsStopped()) return nullptr;
    }
    else {
        // 单文件
        std::cout << "[Info] Loading single TIFF file: " << inputPath << std::endl;
        auto reader = vtkSmartPointer<vtkTIFFReader>::New();
        const std::string vtkInputPath = PlatformPath::GetUtf8Path(pathObj);
        reader->SetFileName(vtkInputPath.c_str());

        if (!reader->CanReadFile(vtkInputPath.c_str())) {
            std::cerr << "[Error] VTK cannot read this TIFF file." << std::endl;
            return nullptr;
        }
        try {
            reader->Update();
            if (stopToken.GetIsStopped()
                || reader->GetErrorCode() != 0
                || !reader->GetOutput()
                || reader->GetOutput()->GetNumberOfPoints() == 0) {
                return nullptr;
            }
        }
        catch (...) {
            std::cerr << "[Error] Exception during TIFF reading." << std::endl;
            return nullptr;
        }
        decodedImage = vtkSmartPointer<vtkImageData>::New();
        decodedImage->DeepCopy(reader->GetOutput());
        if (stopToken.GetIsStopped()) return nullptr;
    }

    auto output = decodedImage;
    if (!output || output->GetDimensions()[0] == 0) {
        return nullptr;
    }

    const auto& expectedDims = layout.GetDimensions();
    const int* decodedDims = output->GetDimensions();
    if (decodedDims[0] != expectedDims[0]
        || decodedDims[1] != expectedDims[1]
        || decodedDims[2] != expectedDims[2]) {
        return nullptr;
    }

    // --- 数据提交 (Back Buffer 策略) ---
    auto lpsImage = vtkSmartPointer<vtkImageData>::New();
    lpsImage->ShallowCopy(output);
    const auto& spacing = layout.GetSpacing();
    const auto& origin = layout.GetOrigin();
    lpsImage->SetSpacing(spacing[0], spacing[1], spacing[2]);
    lpsImage->SetOrigin(origin[0], origin[1], origin[2]);

    auto newImage = vtkSmartPointer<vtkImageData>::New();
    if (!SetLpsRasImage(
            lpsImage, newImage, stopToken)) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    newImage->GetDimensions(dims);

    std::cout << "[Success] Loaded Volume: " << dims[0] << "x" << dims[1] << "x" << dims[2] << std::endl;
    return newImage;
}

bool TiffVolumeDataManager::Impl::SetLpsRasImage(
    vtkImageData* source,
    vtkImageData* target,
    const TaskStopToken& stopToken) const
{
    if (!source || !target || stopToken.GetIsStopped()) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    source->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }

    double spacingRaw[3] = { 1.0, 1.0, 1.0 };
    double originRaw[3] = { 0.0, 0.0, 0.0 };
    source->GetSpacing(spacingRaw);
    source->GetOrigin(originRaw);

    const std::array<double, 3> imageSpacing = {
        spacingRaw[0], spacingRaw[1], spacingRaw[2]
    };
    const std::array<double, 3> lpsOrigin = {
        originRaw[0], originRaw[1], originRaw[2]
    };
    const std::array<double, 3> rasOrigin = BaseDataManager::Impl::GetRasOrigin(lpsOrigin, dims, imageSpacing);

    target->SetDimensions(dims[0], dims[1], dims[2]);
    target->SetSpacing(imageSpacing[0], imageSpacing[1], imageSpacing[2]);
    target->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    target->AllocateScalars(source->GetScalarType(), source->GetNumberOfScalarComponents());

    const int componentCount = source->GetNumberOfScalarComponents();
    const int scalarSize = source->GetScalarSize();
    const size_t pixelBytes = static_cast<size_t>(componentCount) * static_cast<size_t>(scalarSize);
    const size_t nx = static_cast<size_t>(dims[0]);
    const size_t ny = static_cast<size_t>(dims[1]);
    const size_t rowBytes = nx * pixelBytes;
    const size_t sliceBytes = ny * rowBytes;

    const char* srcBase = static_cast<const char*>(source->GetScalarPointer());
    char* dstBase = static_cast<char*>(target->GetScalarPointer());
    if (!srcBase || !dstBase) {
        return false;
    }

    for (size_t z = 0; z < static_cast<size_t>(dims[2]); ++z) {
        if (stopToken.GetIsStopped()) return false;
        const size_t srcSliceOffset = z * sliceBytes;
        const size_t dstSliceOffset = z * sliceBytes;
        for (size_t y = 0; y < ny; ++y) {
            const char* srcRow = srcBase + srcSliceOffset + y * rowBytes;
            char* dstRow = dstBase + dstSliceOffset + (ny - 1 - y) * rowBytes;
            for (size_t x = 0; x < nx; ++x) {
                std::memcpy(
                    dstRow + (nx - 1 - x) * pixelBytes,
                    srcRow + x * pixelBytes,
                    pixelBytes);
            }
        }
    }

    target->Modified();
    return !stopToken.GetIsStopped();
}
