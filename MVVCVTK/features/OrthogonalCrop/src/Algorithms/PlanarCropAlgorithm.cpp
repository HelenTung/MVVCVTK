// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Algorithms/PlanarCropAlgorithm.cpp
// 分类: Math / Core Algorithm Implementation
// 说明: 负责平面 request 归一化、半空间 preview 元数据和 image submit 产物构建。
// =====================================================================

#include "Algorithms/PlanarCropAlgorithm.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// Plane 后端的无状态实现集合：统一将 request 法线归一化，在 active input model/VTK physical
// 坐标中计算无限平面的正负半空间，并为 image submit 构造同结构主图与单字节 mask。
class PlanarCropAlgorithm::Impl final {
public:
    struct VoxelStep {
        // 平面中心经 image physical-to-continuous-index 变换后的 [i, j, k] 坐标。
        std::array<double, 3> planeIndex = { 0.0, 0.0, 0.0 };
        // 单位 i/j/k index 增量在平面单位法线上的有符号 physical 投影。
        double iStep = 0.0;
        double jStep = 0.0;
        double kStep = 0.0;
        // side = (i-planeIndex[0])*iStep + (j-planeIndex[1])*jStep + (k-planeIndex[2])*kStep；
        // epsilon 按三轴完整遍历幅度缩放，|side| 不超过它时回退到精确 physical-point 判断。
        double boundaryEpsilon = 1e-8;
    };

    struct SubmitImages {
        // 与输入结构、标量类型和分量数一致；保留 voxel 复制原值，移除 voxel 写入标量范围最小值。
        vtkSmartPointer<vtkImageData> submitImage;
        // 与 submitImage 对齐的单分量 VTK_UNSIGNED_CHAR mask：255 为保留，0 为移除。
        vtkSmartPointer<vtkImageData> maskImage;
    };

    enum class RowSide {
        NormalSide,
        OppositeSide,
        Mixed
    };

    static constexpr double PlaneEpsilon = 1e-8;

    static OrthogonalCropResult GetResolved(const OrthogonalCropRequest& request);
    static OrthogonalCropResult GetFailure(
        const OrthogonalCropRequest& request,
        CropFailure failureReason,
        const std::string& message,
        const CropDataModel& cropData = CropDataModel());

    static CropBoundsDouble6Array GetImageModelBounds(vtkImageData* image);
    static CropBoundsDouble6Array GetPolyBounds(vtkPolyData* polyData);
    static bool GetBoundsValid(const CropBoundsDouble6Array& bounds);
    static std::size_t GetVoxelBytes(vtkImageData* image);
    static std::size_t GetImageVoxelCount(vtkImageData* image);
    static bool GetNormalizedPlane(
        const OrthogonalCropRequest& request,
        CropVectorDouble3Array& planeNormalInInputModel,
        CropVectorDouble3Array& planeCenterInInputModel,
        CropFailure& failureReason,
        std::string& message);
    static bool GetPlaneData(
        const OrthogonalCropRequest& request,
        const CropBoundsDouble6Array& inputModelBounds,
        CropDataModel& cropData,
        CropFailure& failureReason,
        std::string& message);
    static bool GetPointIsOnNormalSide(
        const double inputModelPoint[3],
        const CropVectorDouble3Array& planeCenterInInputModel,
        const CropVectorDouble3Array& planeNormalInInputModel);
    static VoxelStep GetSideStep(
        vtkImageData* image,
        const CropDataModel& cropData,
        const int dims[3]);
    static double GetSideAtIndex(
        const VoxelStep& sideStep,
        int i,
        int j,
        int k);
    static bool GetVoxelOnSide(
        vtkImageData* image,
        const VoxelStep& sideStep,
        const CropDataModel& cropData,
        const int index[3],
        double planeSide);
    static RowSide GetPlanarRowSide(
        double rowStartSide,
        double rowEndSide,
        double boundaryEpsilon);
    static std::vector<unsigned char> GetBgBytes(
        vtkDataArray* sourceScalars,
        vtkDataArray* submitScalars,
        unsigned char* submitBytes,
        int componentCount,
        std::size_t bytesPerVoxel);
    static void SetRowBg(
        unsigned char* submitRowPtr,
        const std::vector<unsigned char>& backgroundVoxelBytes,
        int voxelCount);
    static void SetRowBytes(
        unsigned char* maskRowPtr,
        unsigned char* submitRowPtr,
        const unsigned char* sourceRowPtr,
        const std::vector<unsigned char>& backgroundVoxelBytes,
        std::size_t rowBytes,
        int voxelCount,
        bool isRowKept);
    static void SetVoxelBytes(
        unsigned char* maskPtr,
        unsigned char* submitPtr,
        const unsigned char* sourcePtr,
        const std::vector<unsigned char>& backgroundVoxelBytes,
        std::size_t bytesPerVoxel,
        bool isVoxelKept);
    static SubmitImages GetSubmitImages(
        vtkImageData* image,
        const CropDataModel& cropData,
        CropRemovalMode removalMode);
    static OrthogonalCropResult GetPreviewResult(
        const CropDataModel& cropData,
        const OrthogonalCropRequest& request);
    static OrthogonalCropResult GetSubmitResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const OrthogonalCropRequest& request,
        std::size_t availableRamBytes);
};

OrthogonalCropResult PlanarCropAlgorithm::Impl::GetResolved(const OrthogonalCropRequest& request)
{
    OrthogonalCropResult result;
    result.resolvedDataSource = request.dataSource;
    result.resolvedOperation = request.operation;
    result.resolvedGeometryType = request.geometryType;
    result.resolvedRemovalMode = request.removalMode;
    return result;
}

OrthogonalCropResult PlanarCropAlgorithm::Impl::GetFailure(
    const OrthogonalCropRequest& request,
    CropFailure failureReason,
    const std::string& message,
    const CropDataModel& cropData)
{
    auto result = GetResolved(request);
    result.failureReason = failureReason;
    result.message = message;
    result.cropDataModel = cropData;
    return result;
}

CropBoundsDouble6Array PlanarCropAlgorithm::Impl::GetImageModelBounds(vtkImageData* image)
{
    CropBoundsDouble6Array bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!image) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    image->GetBounds(rawBounds);
    return {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
}

CropBoundsDouble6Array PlanarCropAlgorithm::Impl::GetPolyBounds(vtkPolyData* polyData)
{
    CropBoundsDouble6Array bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!polyData) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    polyData->GetBounds(rawBounds);
    return {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
}

bool PlanarCropAlgorithm::Impl::GetBoundsValid(const CropBoundsDouble6Array& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

std::size_t PlanarCropAlgorithm::Impl::GetVoxelBytes(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    return static_cast<std::size_t>(image->GetScalarSize())
        * static_cast<std::size_t>(image->GetNumberOfScalarComponents());
}

std::size_t PlanarCropAlgorithm::Impl::GetImageVoxelCount(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    return static_cast<std::size_t>(std::max(dims[0], 0))
        * static_cast<std::size_t>(std::max(dims[1], 0))
        * static_cast<std::size_t>(std::max(dims[2], 0));
}

bool PlanarCropAlgorithm::Impl::GetNormalizedPlane(
    const OrthogonalCropRequest& request,
    CropVectorDouble3Array& planeNormalInInputModel,
    CropVectorDouble3Array& planeCenterInInputModel,
    CropFailure& failureReason,
    std::string& message)
{
    // center 保持 request 中的 active input model 坐标；normal 归一化后才进入所有 side 公式，
    // 因此 dot 值同时具有有符号 physical distance 语义。零法线沿用现有 BadBounds 失败码。
    planeNormalInInputModel = request.planeNormalInInputModel;
    planeCenterInInputModel = request.planeCenterInInputModel;

    const double length = vtkMath::Norm(planeNormalInInputModel.data());
    if (length <= PlaneEpsilon) {
        failureReason = CropFailure::BadBounds;
        message = "Plane normal length is too small.";
        return false;
    }

    for (double& component : planeNormalInInputModel) {
        component /= length;
    }

    failureReason = CropFailure::None;
    message.clear();
    return true;
}

bool PlanarCropAlgorithm::Impl::GetPlaneData(
    const OrthogonalCropRequest& request,
    const CropBoundsDouble6Array& inputModelBounds,
    CropDataModel& cropData,
    CropFailure& failureReason,
    std::string& message)
{
    // 构造顺序：先验证输入 AABB，再验证/归一化几何，最后一次性写入 cropData。
    // planeHalf 只保留 widget 可视尺度；裁切方程不使用它，实际几何始终是无限半空间。
    if (!GetBoundsValid(inputModelBounds)) {
        failureReason = CropFailure::BadBounds;
        message = "Input model bounds are invalid.";
        return false;
    }

    CropVectorDouble3Array planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    CropVectorDouble3Array planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    if (!GetNormalizedPlane(
            request,
            planeNormalInInputModel,
            planeCenterInInputModel,
            failureReason,
            message)) {
        return false;
    }

    cropData.planeNormalInInputModel = planeNormalInInputModel;
    cropData.planeCenterInInputModel = planeCenterInInputModel;
    cropData.planeHalf = request.planeHalf;
    cropData.inputModelBounds = inputModelBounds;
    return true;
}

bool PlanarCropAlgorithm::Impl::GetPointIsOnNormalSide(
    const double inputModelPoint[3],
    const CropVectorDouble3Array& planeCenterInInputModel,
    const CropVectorDouble3Array& planeNormalInInputModel)
{
    const double offset[3] = {
        inputModelPoint[0] - planeCenterInInputModel[0],
        inputModelPoint[1] - planeCenterInInputModel[1],
        inputModelPoint[2] - planeCenterInInputModel[2]
    };

    // Inside 定义为严格正半空间；平面上的点（dot == 0）归入 OppositeSide。
    return vtkMath::Dot(offset, planeNormalInInputModel.data()) > 0.0;
}

PlanarCropAlgorithm::Impl::VoxelStep PlanarCropAlgorithm::Impl::GetSideStep(
    vtkImageData* image,
    const CropDataModel& cropData,
    const int dims[3])
{
    // 将 physical 平面方程预展开到连续 index 空间：矩阵三列分别表示 i/j/k 单位步长，
    // 与单位法线点积后即可用线性增量计算整行 side，避免每个体素都执行矩阵变换。
    VoxelStep sideStep;
    if (!image) {
        return sideStep;
    }

    const auto planeCenterInInputModel = cropData.planeCenterInInputModel;
    image->TransformPhysicalPointToContinuousIndex(
        planeCenterInInputModel.data(),
        sideStep.planeIndex.data());

    const auto planeNormalInInputModel = cropData.planeNormalInInputModel;
    auto indexToPhysicalMatrix = image->GetIndexToPhysicalMatrix();
    sideStep.iStep = planeNormalInInputModel[0] * indexToPhysicalMatrix->GetElement(0, 0)
        + planeNormalInInputModel[1] * indexToPhysicalMatrix->GetElement(1, 0)
        + planeNormalInInputModel[2] * indexToPhysicalMatrix->GetElement(2, 0);
    sideStep.jStep = planeNormalInInputModel[0] * indexToPhysicalMatrix->GetElement(0, 1)
        + planeNormalInInputModel[1] * indexToPhysicalMatrix->GetElement(1, 1)
        + planeNormalInInputModel[2] * indexToPhysicalMatrix->GetElement(2, 1);
    sideStep.kStep = planeNormalInInputModel[0] * indexToPhysicalMatrix->GetElement(0, 2)
        + planeNormalInInputModel[1] * indexToPhysicalMatrix->GetElement(1, 2)
        + planeNormalInInputModel[2] * indexToPhysicalMatrix->GetElement(2, 2);

    const double traversalMagnitude =
        std::abs(sideStep.iStep) * static_cast<double>(std::max(dims[0] - 1, 0))
        + std::abs(sideStep.jStep) * static_cast<double>(std::max(dims[1] - 1, 0))
        + std::abs(sideStep.kStep) * static_cast<double>(std::max(dims[2] - 1, 0));
    sideStep.boundaryEpsilon = PlaneEpsilon * (1.0 + traversalMagnitude);
    return sideStep;
}

double PlanarCropAlgorithm::Impl::GetSideAtIndex(
    const VoxelStep& sideStep,
    int i,
    int j,
    int k)
{
    return (static_cast<double>(i) - sideStep.planeIndex[0]) * sideStep.iStep
        + (static_cast<double>(j) - sideStep.planeIndex[1]) * sideStep.jStep
        + (static_cast<double>(k) - sideStep.planeIndex[2]) * sideStep.kStep;
}

bool PlanarCropAlgorithm::Impl::GetVoxelOnSide(
    vtkImageData* image,
    const VoxelStep& sideStep,
    const CropDataModel& cropData,
    const int index[3],
    double planeSide)
{
    // 远离边界时直接信任 index 空间线性式；接近零面时回到 VTK physical point 精确重算，
    // 避免大尺寸遍历累积误差改变边界 voxel 的归属。
    if (std::abs(planeSide) > sideStep.boundaryEpsilon) {
        return planeSide > 0.0;
    }

    double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(index, inputModelPoint);
    return GetPointIsOnNormalSide(
        inputModelPoint,
        cropData.planeCenterInInputModel,
        cropData.planeNormalInInputModel);
}

PlanarCropAlgorithm::Impl::RowSide PlanarCropAlgorithm::Impl::GetPlanarRowSide(
    double rowStartSide,
    double rowEndSide,
    double boundaryEpsilon)
{
    const double minSide = std::min(rowStartSide, rowEndSide);
    const double maxSide = std::max(rowStartSide, rowEndSide);
    if (minSide > boundaryEpsilon) {
        return RowSide::NormalSide;
    }
    if (maxSide < -boundaryEpsilon) {
        return RowSide::OppositeSide;
    }
    return RowSide::Mixed;
}

std::vector<unsigned char> PlanarCropAlgorithm::Impl::GetBgBytes(
    vtkDataArray* sourceScalars,
    vtkDataArray* submitScalars,
    unsigned char* submitBytes,
    int componentCount,
    std::size_t bytesPerVoxel)
{
    std::vector<unsigned char> backgroundBytes(bytesPerVoxel, 0);
    if (!sourceScalars || !submitScalars || !submitBytes || componentCount <= 0 || bytesPerVoxel == 0) {
        return backgroundBytes;
    }

    double scalarRange[2] = { 0.0, 0.0 };
    sourceScalars->GetRange(scalarRange);
    const std::vector<double> backgroundTuple(
        static_cast<std::size_t>(componentCount),
        scalarRange[0]);

    submitScalars->SetTuple(0, backgroundTuple.data());
    std::memcpy(backgroundBytes.data(), submitBytes, bytesPerVoxel);
    return backgroundBytes;
}

void PlanarCropAlgorithm::Impl::SetRowBg(
    unsigned char* submitRowPtr,
    const std::vector<unsigned char>& backgroundVoxelBytes,
    int voxelCount)
{
    if (!submitRowPtr || backgroundVoxelBytes.empty() || voxelCount <= 0) {
        return;
    }

    const auto bytesPerVoxel = backgroundVoxelBytes.size();
    for (int voxelIndex = 0; voxelIndex < voxelCount; ++voxelIndex) {
        std::memcpy(
            submitRowPtr + static_cast<std::size_t>(voxelIndex) * bytesPerVoxel,
            backgroundVoxelBytes.data(),
            bytesPerVoxel);
    }
}

void PlanarCropAlgorithm::Impl::SetRowBytes(
    unsigned char* maskRowPtr,
    unsigned char* submitRowPtr,
    const unsigned char* sourceRowPtr,
    const std::vector<unsigned char>& backgroundVoxelBytes,
    std::size_t rowBytes,
    int voxelCount,
    bool isRowKept)
{
    const auto maskBytes = static_cast<std::size_t>(voxelCount);
    if (isRowKept) {
        std::memset(maskRowPtr, 255, maskBytes);
        std::memcpy(submitRowPtr, sourceRowPtr, rowBytes);
        return;
    }

    std::memset(maskRowPtr, 0, maskBytes);
    SetRowBg(
        submitRowPtr,
        backgroundVoxelBytes,
        voxelCount);
}

void PlanarCropAlgorithm::Impl::SetVoxelBytes(
    unsigned char* maskPtr,
    unsigned char* submitPtr,
    const unsigned char* sourcePtr,
    const std::vector<unsigned char>& backgroundVoxelBytes,
    std::size_t bytesPerVoxel,
    bool isVoxelKept)
{
    if (isVoxelKept) {
        *maskPtr = 255;
        std::memcpy(submitPtr, sourcePtr, bytesPerVoxel);
        return;
    }

    *maskPtr = 0;
    if (!backgroundVoxelBytes.empty()) {
        std::memcpy(submitPtr, backgroundVoxelBytes.data(), bytesPerVoxel);
    }
}

PlanarCropAlgorithm::Impl::SubmitImages PlanarCropAlgorithm::Impl::GetSubmitImages(
    vtkImageData* image,
    const CropDataModel& cropData,
    CropRemovalMode removalMode)
{
    // 1. 输出主图复制输入结构、标量类型和分量数；mask 复制结构但固定为单分量 UCHAR。
    // 2. 以 x-fast 的连续内存行作为分类单位：整行同侧走 memcpy/背景快路径，跨面行才逐 voxel 判断。
    // 3. KeepInside 保留 normal 正侧，RemoveInside 反转该布尔；mask 始终 255=保留、0=移除。
    SubmitImages images;
    if (!image) {
        return images;
    }

    auto sourceScalars = image->GetPointData()
        ? image->GetPointData()->GetScalars()
        : nullptr;
    if (!sourceScalars) {
        return images;
    }

    const int componentCount = sourceScalars->GetNumberOfComponents();
    if (componentCount <= 0) {
        return images;
    }

    int extent[6] = { 0, -1, 0, -1, 0, -1 };
    image->GetExtent(extent);
    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return images;
    }

    const std::size_t bytesPerVoxel = static_cast<std::size_t>(sourceScalars->GetDataTypeSize())
        * static_cast<std::size_t>(componentCount);
    if (bytesPerVoxel == 0) {
        return images;
    }

    images.submitImage = vtkSmartPointer<vtkImageData>::New();
    images.submitImage->CopyStructure(image);
    images.submitImage->AllocateScalars(sourceScalars->GetDataType(), componentCount);

    auto submitScalars = images.submitImage->GetPointData()
        ? images.submitImage->GetPointData()->GetScalars()
        : nullptr;
    if (!submitScalars) {
        images.submitImage = nullptr;
        return images;
    }
    if (sourceScalars->GetName()) {
        submitScalars->SetName(sourceScalars->GetName());
    }

    images.maskImage = vtkSmartPointer<vtkImageData>::New();
    images.maskImage->CopyStructure(image);
    images.maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    const auto* sourceBytes = static_cast<const unsigned char*>(
        image->GetScalarPointer(extent[0], extent[2], extent[4]));
    auto* submitBytes = static_cast<unsigned char*>(
        images.submitImage->GetScalarPointer(extent[0], extent[2], extent[4]));
    auto* maskBytes = static_cast<unsigned char*>(
        images.maskImage->GetScalarPointer(extent[0], extent[2], extent[4]));
    if (!sourceBytes || !submitBytes || !maskBytes) {
        images.submitImage = nullptr;
        images.maskImage = nullptr;
        return images;
    }

    const auto backgroundVoxelBytes = GetBgBytes(
        sourceScalars,
        submitScalars,
        submitBytes,
        componentCount,
        bytesPerVoxel);
    const bool isKeepInside = removalMode == CropRemovalMode::KeepInside;
    const auto sideStep = GetSideStep(image, cropData, dims);
    const vtkIdType rowStride = dims[0];
    const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];
    const std::size_t rowBytes = static_cast<std::size_t>(dims[0]) * bytesPerVoxel;

    for (int kOffset = 0; kOffset < dims[2]; ++kOffset) {
        const int k = extent[4] + kOffset;
        const vtkIdType sliceStart = static_cast<vtkIdType>(kOffset) * sliceStride;
        for (int jOffset = 0; jOffset < dims[1]; ++jOffset) {
            const int j = extent[2] + jOffset;
            const vtkIdType rowStart = sliceStart + static_cast<vtkIdType>(jOffset) * rowStride;
            const auto rowByteOffset = static_cast<std::size_t>(rowStart) * bytesPerVoxel;
            auto* maskRowPtr = maskBytes + rowStart;
            auto* submitRowPtr = submitBytes + rowByteOffset;
            const auto* sourceRowPtr = sourceBytes + rowByteOffset;

            const double rowStartSide = GetSideAtIndex(sideStep, extent[0], j, k);
            const double rowEndSide = rowStartSide
                + static_cast<double>(dims[0] - 1) * sideStep.iStep;
            const auto rowSide = GetPlanarRowSide(
                rowStartSide,
                rowEndSide,
                sideStep.boundaryEpsilon);
            if (rowSide != RowSide::Mixed) {
                // 两端 side 均越过同一 epsilon 且 side 沿 i 为线性函数，因此整行必在同一半空间。
                const bool isRowInside = rowSide == RowSide::NormalSide;
                const bool isRowKept = isKeepInside ? isRowInside : !isRowInside;
                SetRowBytes(
                    maskRowPtr,
                    submitRowPtr,
                    sourceRowPtr,
                    backgroundVoxelBytes,
                    rowBytes,
                    dims[0],
                    isRowKept);
                continue;
            }

            double planeSide = rowStartSide;
            for (int iOffset = 0; iOffset < dims[0]; ++iOffset) {
                // Mixed 行逐点消费 epsilon 回退；extent 可非零，内存 offset 仍按局部 iOffset 递增。
                const int index[3] = { extent[0] + iOffset, j, k };
                const bool isInside = GetVoxelOnSide(
                    image,
                    sideStep,
                    cropData,
                    index,
                    planeSide);
                const bool isVoxelKept = isKeepInside ? isInside : !isInside;
                const auto voxelByteOffset = static_cast<std::size_t>(iOffset) * bytesPerVoxel;
                SetVoxelBytes(
                    maskRowPtr + iOffset,
                    submitRowPtr + voxelByteOffset,
                    sourceRowPtr + voxelByteOffset,
                    backgroundVoxelBytes,
                    bytesPerVoxel,
                    isVoxelKept);

                planeSide += sideStep.iStep;
            }
        }
    }

    images.submitImage->Modified();
    images.maskImage->Modified();
    return images;
}

OrthogonalCropResult PlanarCropAlgorithm::Impl::GetPreviewResult(
    const CropDataModel& cropData,
    const OrthogonalCropRequest& request)
{
    auto result = PlanarCropAlgorithm::Impl::GetResolved(request);

    // 平面 preview 的真实语义是无限半空间：dot(point - center, normal) 决定保留/移除侧。
    // 之前生成有限矩形 outline 只是显示参照，会误导用户以为裁切只发生在框内，因此这里不再返回 outline。
    result.cropDataModel = cropData;
    result.isSucceeded = true;
    return result;
}

OrthogonalCropResult PlanarCropAlgorithm::Impl::GetSubmitResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const OrthogonalCropRequest& request,
    std::size_t availableRamBytes)
{
    const std::size_t voxelCount = GetImageVoxelCount(image);
    const std::size_t estimatedRamUsageBytes =
        voxelCount * (GetVoxelBytes(image) + sizeof(unsigned char));
    if (availableRamBytes != 0 && estimatedRamUsageBytes > availableRamBytes) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::LowRam,
            "Estimated planar image submit memory usage exceeds currently available RAM.",
            cropData);
    }

    auto submitImages = GetSubmitImages(
        image,
        cropData,
        request.removalMode);
    if (!submitImages.submitImage) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::ImageFailed,
            "Failed to build planar image submit image.",
            cropData);
    }

    if (!submitImages.maskImage) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::MaskFailed,
            "Failed to build planar image submit mask.",
            cropData);
    }

    auto result = PlanarCropAlgorithm::Impl::GetResolved(request);

    result.cropDataModel = cropData;
    result.submitImage = submitImages.submitImage;
    result.maskImage = submitImages.maskImage;
    result.isSucceeded = true;
    return result;
}

OrthogonalCropResult PlanarCropAlgorithm::GetResult(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    // image 入口只接受 Plane 的 Volume preview 或 Image submit；其它 operation/dataSource 组合
    // 在读取输入或计算平面之前返回 NoBackend，避免“路由错误但产生了部分 artifact”。
    const bool isPreviewRoute =
        request.geometryType == CropShape::Plane
        && request.operation == OrthogonalCropOperation::Preview
        && request.dataSource == OrthogonalCropDataSource::VolumeData;
    const bool isSubmitRoute =
        request.geometryType == CropShape::Plane
        && request.operation == OrthogonalCropOperation::Submit
        && request.dataSource == OrthogonalCropDataSource::ImageData;
    if (!isPreviewRoute && !isSubmitRoute) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::NoBackend,
            "Image algorithm received an unsupported planar crop route.");
    }

    if (!image) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::NoImage,
            "Input image is null.");
    }

    const auto inputModelBounds = PlanarCropAlgorithm::Impl::GetImageModelBounds(image);
    CropDataModel cropData;
    CropFailure failureReason = CropFailure::None;
    std::string message;
    if (!PlanarCropAlgorithm::Impl::GetPlaneData(
            request,
            inputModelBounds,
            cropData,
            failureReason,
            message)) {
        return PlanarCropAlgorithm::Impl::GetFailure(request, failureReason, message);
    }

    if (isPreviewRoute) {
        return PlanarCropAlgorithm::Impl::GetPreviewResult(
            cropData,
            request);
    }

    const std::size_t availableRamBytes = request.availableRamBytes != 0
        ? request.availableRamBytes
        : fallbackAvailableRamBytes;
    return PlanarCropAlgorithm::Impl::GetSubmitResult(
        image,
        cropData,
        request,
        availableRamBytes);
}

OrthogonalCropResult PlanarCropAlgorithm::GetResult(
    vtkPolyData* polyData,
    const OrthogonalCropRequest& request)
{
    const bool isPreviewRoute =
        request.geometryType == CropShape::Plane
        && request.operation == OrthogonalCropOperation::Preview
        && request.dataSource == OrthogonalCropDataSource::PolyData;
    if (!isPreviewRoute) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::NoBackend,
            "PolyData algorithm received an unsupported planar crop route.");
    }

    if (!polyData) {
        return PlanarCropAlgorithm::Impl::GetFailure(
            request,
            CropFailure::NoPolyData,
            "Input polydata is null.");
    }

    const auto inputModelBounds = PlanarCropAlgorithm::Impl::GetPolyBounds(polyData);
    CropDataModel cropData;
    CropFailure failureReason = CropFailure::None;
    std::string message;
    if (!PlanarCropAlgorithm::Impl::GetPlaneData(
            request,
            inputModelBounds,
            cropData,
            failureReason,
            message)) {
        return PlanarCropAlgorithm::Impl::GetFailure(request, failureReason, message);
    }

    return PlanarCropAlgorithm::Impl::GetPreviewResult(
        cropData,
        request);
}
