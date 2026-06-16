// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/OrthogonalCropAlgorithm.cpp
// 分类: Math / Core Algorithm Implementation
// 说明: 负责 bounds 校验、request 归一化、image 2D mask preview、box 3D outline preview 与 image submit 统计/执行。
// =====================================================================

#include "OrthogonalCropAlgorithm.h"

#include <vtkBoundingBox.h>
#include <vtkCubeSource.h>
#include <vtkDataArray.h>
#include <vtkExtractVOI.h>
#include <vtkImageChangeInformation.h>
#include <vtkMatrix4x4.h>
#include <vtkOutlineSource.h>
#include <vtkPointData.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

// 统一的 bounds 比较容差，避免浮点误差导致边界判断抖动。
static constexpr double BoundsEpsilon = 1e-6;

static std::array<double, 6> GetImageBounds(vtkImageData* image)
{
    // vtkImageData::GetBounds 返回 image model 范围；底层仍通过 VTK physical-point API 表达。
    // 这里不再额外做 world/model 折叠，算法层后续所有 image 校验都直接用 model 语义。
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!image) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    image->GetBounds(rawBounds);
    return { rawBounds[0], rawBounds[1], rawBounds[2], rawBounds[3], rawBounds[4], rawBounds[5] };
}

static bool GetBoundsHavePositiveVolume(const std::array<double, 6>& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

static std::string GetBoundsMessage(const char* prefix, const std::array<double, 6>& bounds)
{
    std::ostringstream stream;
    stream << prefix << " ["
        << bounds[0] << ", " << bounds[1] << "; "
        << bounds[2] << ", " << bounds[3] << "; "
        << bounds[4] << ", " << bounds[5] << "]";
    return stream.str();
}

static bool GetBoundsOverlap(
    const std::array<double, 6>& dataModelBounds,
    const std::array<double, 6>& cropModelBounds)
{
    // preview 允许 partial overlap，因此这里只判断两个轴对齐盒是否真正有体积交集。
    return cropModelBounds[1] > dataModelBounds[0] + BoundsEpsilon
        && cropModelBounds[0] < dataModelBounds[1] - BoundsEpsilon
        && cropModelBounds[3] > dataModelBounds[2] + BoundsEpsilon
        && cropModelBounds[2] < dataModelBounds[3] - BoundsEpsilon
        && cropModelBounds[5] > dataModelBounds[4] + BoundsEpsilon
        && cropModelBounds[4] < dataModelBounds[5] - BoundsEpsilon;
}

static bool GetBoundsContained(
    const std::array<double, 6>& dataModelBounds,
    const std::array<double, 6>& cropModelBounds)
{
    // 严格校验路径要求裁切盒完整包含在数据 model bounds 内。
    // Image KeepInside submit 会走 partial overlap：后续 snapped index 会 clamp 到有效体素范围。
    return cropModelBounds[0] >= dataModelBounds[0] - BoundsEpsilon
        && cropModelBounds[1] <= dataModelBounds[1] + BoundsEpsilon
        && cropModelBounds[2] >= dataModelBounds[2] - BoundsEpsilon
        && cropModelBounds[3] <= dataModelBounds[3] + BoundsEpsilon
        && cropModelBounds[4] >= dataModelBounds[4] - BoundsEpsilon
        && cropModelBounds[5] <= dataModelBounds[5] + BoundsEpsilon;
}

static std::array<double, 6> GetModelBoundsFromBoxToModelMatrix(const std::array<double, 16>& boxToModelMatrixData)
{
    // boxToModelMatrix 是标准盒 [-1,1]^3 到 model 的唯一几何真源；
    // AABB 只通过 8 个标准角点派生，用于统一校验、index 吸附和统计范围。
    auto boxToModelTransform = vtkSmartPointer<vtkTransform>::New();
    boxToModelTransform->SetMatrix(boxToModelMatrixData.data());

    vtkBoundingBox modelBounds;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const double boxPoint[3] = { static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz) };
                double modelPoint[3] = { 0.0, 0.0, 0.0 };
                boxToModelTransform->TransformPoint(boxPoint, modelPoint);
                modelBounds.AddPoint(modelPoint);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    modelBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

static std::array<double, 16> GetSourceToTargetMatrixArray(vtkMatrix4x4* sourceToTargetMatrix)
{
    if (!sourceToTargetMatrix) {
        return GetIdentityMatrixArray();
    }

    std::array<double, 16> sourceToTargetMatrixData = { 0.0 };
    vtkMatrix4x4::DeepCopy(sourceToTargetMatrixData.data(), sourceToTargetMatrix);
    return sourceToTargetMatrixData;
}

static vtkSmartPointer<vtkTransform> GetBoxToModelTransform(const std::array<double, 16>& boxToModelMatrixData)
{
    auto boxToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToModelMatrix->DeepCopy(boxToModelMatrixData.data());

    // 输出 outline 时从标准盒 [-1,1]^3 出发，直接用 box -> model 生成显示几何。
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(boxToModelMatrix);
    return transform;
}

static vtkSmartPointer<vtkMatrix4x4> GetModelToBoxMatrix(const CropDataModel& cropData)
{
    auto boxToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToModelMatrix->DeepCopy(cropData.GetBoxToModelMatrix().data());

    // inside/outside 判定统一在标准盒空间完成：
    // model 点乘以 modelToBox 后，只需检查三个坐标是否落在 [-1,1]。
    auto modelToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToModelMatrix, modelToBoxMatrix);
    return modelToBoxMatrix;
}

static bool GetBoxToModelIsAxisAligned(const std::array<double, 16>& boxToModelMatrixData)
{
    // 只识别无旋转/无剪切的对角矩阵快路径；任意旋转或剪切仍走标准 modelToBox 判断。
    return std::abs(boxToModelMatrixData[1]) <= BoundsEpsilon
        && std::abs(boxToModelMatrixData[2]) <= BoundsEpsilon
        && std::abs(boxToModelMatrixData[4]) <= BoundsEpsilon
        && std::abs(boxToModelMatrixData[6]) <= BoundsEpsilon
        && std::abs(boxToModelMatrixData[8]) <= BoundsEpsilon
        && std::abs(boxToModelMatrixData[9]) <= BoundsEpsilon;
}

static bool GetCanonicalBoxContainsPoint(const double boxPoint[4])
{
    const double invW = std::abs(boxPoint[3]) > 1e-12 ? 1.0 / boxPoint[3] : 1.0;
    return std::abs(boxPoint[0] * invW) <= 1.0 + BoundsEpsilon
        && std::abs(boxPoint[1] * invW) <= 1.0 + BoundsEpsilon
        && std::abs(boxPoint[2] * invW) <= 1.0 + BoundsEpsilon;
}

static std::array<double, 16> GetUpdatedOffsetMatrix(
    const std::array<double, 16>& sourceToTargetMatrixData,
    const std::array<double, 3>& translation)
{
    // image submit 会改变输出 image 的 origin，
    // 这里把这次位移继续累加进 globalOffsetMatrix，保证上层共享坐标语义保持连续。
    auto sourceToTargetMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    sourceToTargetMatrix->DeepCopy(sourceToTargetMatrixData.data());

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->PostMultiply();
    transform->SetMatrix(sourceToTargetMatrix);
    transform->Translate(translation[0], translation[1], translation[2]);

    return GetSourceToTargetMatrixArray(transform->GetMatrix());
}

static std::size_t GetImageBytesPerVoxel(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    return static_cast<std::size_t>(image->GetScalarSize())
        * static_cast<std::size_t>(image->GetNumberOfScalarComponents());
}

static std::size_t GetVoxelCountFromIndexBounds(const std::array<int, 6>& indexBounds)
{
    const std::size_t sizeI = static_cast<std::size_t>(indexBounds[1] - indexBounds[0] + 1);
    const std::size_t sizeJ = static_cast<std::size_t>(indexBounds[3] - indexBounds[2] + 1);
    const std::size_t sizeK = static_cast<std::size_t>(indexBounds[5] - indexBounds[4] + 1);
    return sizeI * sizeJ * sizeK;
}

static std::size_t GetEffectiveAvailableRamBytes(
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    return request.GetAvailableRamBytes() != 0
        ? request.GetAvailableRamBytes()
        : fallbackAvailableRamBytes;
}

static std::array<int, 6> GetSnappedIndexBounds(vtkImageData* image, const CropDataModel& cropData);

struct SubmitPlan {
    bool canExecute = false;
    CropIndexBoundsInt6Array snappedIndexBounds = { 0, 0, 0, 0, 0, 0 };
    OrthogonalCropStatistics diagnostics;
};

static SubmitPlan BuildSubmitPlan(
    vtkImageData* image,
    const CropDataModel& cropData,
    CropRemovalMode removalMode,
    std::size_t availableRamBytes)
{
    SubmitPlan plan;
    plan.diagnostics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    plan.diagnostics.SetResolvedBackend(OrthogonalCropResolvedBackend::SubmitExtractVOI);

    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string validationMessage;
    const bool allowPartialOverlap = removalMode == CropRemovalMode::KeepInside;
    if (!image) {
        plan.diagnostics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        plan.diagnostics.SetValidationMessage("Input image is null.");
        return plan;
    }

    if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
        GetImageBounds(image),
        cropData.GetModelBounds(),
        failureReason,
        validationMessage,
        allowPartialOverlap)) {
        plan.diagnostics.SetFailureReason(failureReason);
        plan.diagnostics.SetValidationMessage(validationMessage);
        return plan;
    }

    if (removalMode == CropRemovalMode::RemoveInside) {
        plan.diagnostics.SetFailureReason(OrthogonalCropFailureReason::SubmitRemoveInsideUnsupported);
        plan.diagnostics.SetValidationMessage(
            "Image submit with RemoveInside is intentionally blocked because it cannot preserve a contiguous derived volume without resampling.");
        return plan;
    }

    plan.snappedIndexBounds = GetSnappedIndexBounds(image, cropData);
    const std::size_t outputVoxelCount = GetVoxelCountFromIndexBounds(plan.snappedIndexBounds);
    const std::size_t estimatedRamUsageBytes = outputVoxelCount * GetImageBytesPerVoxel(image);
    if (availableRamBytes != 0 && estimatedRamUsageBytes > availableRamBytes) {
        plan.diagnostics.SetFailureReason(OrthogonalCropFailureReason::InsufficientRam);
        plan.diagnostics.SetValidationMessage("Estimated image submit memory usage exceeds currently available RAM.");
        return plan;
    }

    plan.canExecute = true;
    plan.diagnostics.SetFailureReason(OrthogonalCropFailureReason::None);
    return plan;
}

static vtkSmartPointer<vtkImageData> GetMaskImage(
    vtkImageData* image,
    const CropDataModel& cropData,
    const std::array<int, 6>& indexBounds,
    CropRemovalMode removalMode)
{
    // 2D mask preview 的结果不是导出新体数据，而是构造一张用于预览的 inside/outside 掩码。
    // 整体流程分两段：
    // 1. 先用 snapped index bounds 把执行域收缩到最小候选 AABB
    // 2. 再用 modelToBox 把体素中心归一化到标准盒 [-1,1]^3 做 inside 判定
    if (!image) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);

    const int minI = std::max(0, indexBounds[0]);
    const int maxI = std::min(dims[0] - 1, indexBounds[1]);
    const int minJ = std::max(0, indexBounds[2]);
    const int maxJ = std::min(dims[1] - 1, indexBounds[3]);
    const int minK = std::max(0, indexBounds[4]);
    const int maxK = std::min(dims[2] - 1, indexBounds[5]);
    const bool useCompactMask = removalMode == CropRemovalMode::KeepInside;

    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    if (minI > maxI || minJ > maxJ || minK > maxK) {
        // 无有效交叠时仍返回一张与输入结构兼容的空 mask，避免上层处理 nullptr 分支。
        maskImage->CopyStructure(image);
        maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);  // 每个体素 1 个分量 类型是VTK_UNSIGNED_CHAR 就是说，这是一张单通道的 mask 图
        auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
        if (!maskPtr) {
            return nullptr;
        }
        const vtkIdType totalVoxelCount = static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
        std::memset(maskPtr, 0, static_cast<std::size_t>(totalVoxelCount)); // 通常语义是：0 表示不在区域内 1 或 255 表示在区域内
        maskImage->Modified();
        return maskImage;
    }

    const int width = maxI - minI + 1;
    const int height = maxJ - minJ + 1;
    const int depth = maxK - minK + 1;
    if (useCompactMask) {
        const int outputStartIndex[3] = { minI, minJ, minK };
        double outputOrigin[3] = { 0.0, 0.0, 0.0 };
        image->TransformIndexToPhysicalPoint(outputStartIndex, outputOrigin);

        // KeepInside 只需要 snapped ROI 大小的 mask；
        // 输出图像改成 ROI 结构即可，避免每次 preview 都整图分配和清零。
        maskImage->CopyStructure(image);
        maskImage->SetExtent(0, width - 1, 0, height - 1, 0, depth - 1);
        maskImage->SetOrigin(outputOrigin);
    }
    else {
        // RemoveInside 需要表达盒外补集，因此仍保留 full-volume mask。
        maskImage->CopyStructure(image);
    }
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
    if (!maskPtr) {
        return nullptr;
    }

    const bool keepInside = removalMode == CropRemovalMode::KeepInside;
    // 255保留 0去除
    const unsigned char keptValue = 255;
    const unsigned char removedValue = 0;
    const unsigned char insideValue = keepInside ? keptValue : removedValue;
    const unsigned char outsideValue = keepInside ? removedValue : keptValue;

    const vtkIdType totalVoxelCount = static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
    const vtkIdType allocatedVoxelCount = useCompactMask
        ? static_cast<vtkIdType>(width) * height * depth
        : totalVoxelCount;
    std::memset(maskPtr, outsideValue, static_cast<std::size_t>(allocatedVoxelCount));

    if (GetBoxToModelIsAxisAligned(cropData.GetBoxToModelMatrix())) {
        // 标准盒矩阵退化为 model 轴对齐盒时，snapped AABB 内部整块都属于 inside。
        if (useCompactMask) {
            std::memset(maskPtr, insideValue, static_cast<std::size_t>(allocatedVoxelCount));
        }
        else {
            const vtkIdType rowStride = dims[0];
            const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];
            for (int k = minK; k <= maxK; ++k) {
                for (int j = minJ; j <= maxJ; ++j) {
					// z = index_z * y *x + index_y * x + index_x 刚好是mini+width 批量填充的线性地址计算方式，直接 memset 整行。
                    const vtkIdType rowStart = static_cast<vtkIdType>(k) * sliceStride
                        + static_cast<vtkIdType>(j) * rowStride
                        + minI;
                    std::memset(maskPtr + rowStart, insideValue, static_cast<std::size_t>(width));
                }
            }
        }

        maskImage->Modified();
        return maskImage;
    }

    auto modelToBoxMatrix = GetModelToBoxMatrix(cropData);

    // 旋转/缩放盒的真实 inside 由“index 体素中心 -> model -> 标准盒空间”决定。
    // 这里仍旧只在 snapped AABB 范围内遍历，避免为了旋转盒额外扩张执行域。
    const vtkIdType rowStride = useCompactMask ? width : dims[0];
    const vtkIdType sliceStride = useCompactMask
        ? static_cast<vtkIdType>(width) * height
        : static_cast<vtkIdType>(dims[0]) * dims[1];
    auto indexToPhysicalMatrix = image->GetIndexToPhysicalMatrix();
    const double modelToBoxStepVector[4] = {
        indexToPhysicalMatrix->GetElement(0, 0),
        indexToPhysicalMatrix->GetElement(1, 0),
        indexToPhysicalMatrix->GetElement(2, 0),
        0.0
    };
    double modelToBoxStepResult[4] = { 0.0, 0.0, 0.0, 0.0 };
    modelToBoxMatrix->MultiplyPoint(modelToBoxStepVector, modelToBoxStepResult);

    for (int k = minK; k <= maxK; ++k) {
        for (int j = minJ; j <= maxJ; ++j) {
            const int rowStartIndex[3] = { minI, j, k };
            double modelPoint[3] = { 0.0, 0.0, 0.0 };
            // 每一行先把 row 起点 index 转到 model，
            // 再整体乘 modelToBox；后续沿 i 方向只做增量推进，避免每个点都重乘矩阵。
            // image model 底层通过 VTK physical-point API 表达。
            image->TransformIndexToPhysicalPoint(rowStartIndex, modelPoint);
            const double modelPoint4[4] = { modelPoint[0], modelPoint[1], modelPoint[2], 1.0 };
            double boxPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
            modelToBoxMatrix->MultiplyPoint(modelPoint4, boxPoint);

            for (int i = minI; i <= maxI; ++i) {
                const bool isInside = GetCanonicalBoxContainsPoint(boxPoint);

                if (isInside) {
                    const vtkIdType linearIndex = useCompactMask
                        ? static_cast<vtkIdType>(k - minK) * sliceStride
                            + static_cast<vtkIdType>(j - minJ) * rowStride
                            + (i - minI)
                        : static_cast<vtkIdType>(k) * sliceStride
                            + static_cast<vtkIdType>(j) * rowStride
                            + i;
                    maskPtr[linearIndex] = insideValue; // 初始化
                }

                boxPoint[0] += modelToBoxStepResult[0];
                boxPoint[1] += modelToBoxStepResult[1];
                boxPoint[2] += modelToBoxStepResult[2];
                boxPoint[3] += modelToBoxStepResult[3];
            }
        }
    }

    maskImage->Modified();
    return maskImage;
}

static vtkSmartPointer<vtkImageData> GetExtractedImage(
    vtkImageData* image,
    const std::array<int, 6>& indexBounds)
{
    // image submit 的目标是导出真正独立的输出 image。
    // 这里先按 snapped index 做 vtkExtractVOI，再把 extent 起点归零，
    // 同时把新的 model origin 设置成输出块起点对应的 model 坐标。
    if (!image) {
        return nullptr;
    }

    const int width = indexBounds[1] - indexBounds[0] + 1;
    const int height = indexBounds[3] - indexBounds[2] + 1;
    const int depth = indexBounds[5] - indexBounds[4] + 1;
    if (width <= 0 || height <= 0 || depth <= 0) {
        return nullptr;
    }

    const int outputStartIndex[3] = { indexBounds[0], indexBounds[2], indexBounds[4] };
    double outputOrigin[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(outputStartIndex, outputOrigin);

    // 这里让 vtkExtractVOI 只负责提取体素子块，
    // vtkImageChangeInformation 再只修正输出 extent/origin，不改真实体素内容。
    auto extract = vtkSmartPointer<vtkExtractVOI>::New();
    extract->SetInputData(image);
    extract->SetVOI(
        indexBounds[0], indexBounds[1],
        indexBounds[2], indexBounds[3],
        indexBounds[4], indexBounds[5]);

    auto normalizeInformation = vtkSmartPointer<vtkImageChangeInformation>::New();
    normalizeInformation->SetInputConnection(extract->GetOutputPort());
    normalizeInformation->SetOutputExtentStart(0, 0, 0);
    normalizeInformation->SetOutputOrigin(outputOrigin);
    normalizeInformation->Update();

    auto output = vtkSmartPointer<vtkImageData>::New();
    output->ShallowCopy(normalizeInformation->GetOutput());
    return output;
}

static void SetSubmitOutsideBoxToBackground(
    vtkImageData* submitImage,
    const CropDataModel& cropData)
{
    if (!submitImage
        || GetBoxToModelIsAxisAligned(cropData.GetBoxToModelMatrix())) {
        return;
    }

    auto scalars = submitImage->GetPointData()
        ? submitImage->GetPointData()->GetScalars()
        : nullptr;
    if (!scalars) {
        return;
    }

    int dims[3] = { 0, 0, 0 };
    submitImage->GetDimensions(dims);
    const int componentCount = scalars->GetNumberOfComponents();
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 || componentCount <= 0) {
        return;
    }

    double scalarRange[2] = { 0.0, 0.0 };
    submitImage->GetScalarRange(scalarRange);
    const std::vector<double> backgroundTuple(
        static_cast<std::size_t>(componentCount),
        scalarRange[0]);

    auto modelToBoxMatrix = GetModelToBoxMatrix(cropData);
    auto indexToPhysicalMatrix = submitImage->GetIndexToPhysicalMatrix();
    const double modelToBoxStepVector[4] = {
        indexToPhysicalMatrix->GetElement(0, 0),
        indexToPhysicalMatrix->GetElement(1, 0),
        indexToPhysicalMatrix->GetElement(2, 0),
        0.0
    };
    double modelToBoxStepResult[4] = { 0.0, 0.0, 0.0, 0.0 };
    modelToBoxMatrix->MultiplyPoint(modelToBoxStepVector, modelToBoxStepResult);

    for (int k = 0; k < dims[2]; ++k) {
        for (int j = 0; j < dims[1]; ++j) {
            const int rowStartIndex[3] = { 0, j, k };
            double modelPoint[3] = { 0.0, 0.0, 0.0 };
            submitImage->TransformIndexToPhysicalPoint(rowStartIndex, modelPoint);

            const double modelPoint4[4] = { modelPoint[0], modelPoint[1], modelPoint[2], 1.0 };
            double boxPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
            modelToBoxMatrix->MultiplyPoint(modelPoint4, boxPoint);

            for (int i = 0; i < dims[0]; ++i) {
                if (!GetCanonicalBoxContainsPoint(boxPoint)) {
                    int index[3] = { i, j, k };
                    scalars->SetTuple(
                        submitImage->ComputePointId(index),
                        backgroundTuple.data());
                }

                boxPoint[0] += modelToBoxStepResult[0];
                boxPoint[1] += modelToBoxStepResult[1];
                boxPoint[2] += modelToBoxStepResult[2];
                boxPoint[3] += modelToBoxStepResult[3];
            }
        }
    }

    submitImage->Modified();
}

static vtkSmartPointer<vtkPolyData> GetOutlinePolyDataInternal(const CropDataModel& cropData)
{
    // box 3D outline preview 的几何真源同样是标准盒 [-1,1]^3 + boxToModelMatrix。
    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    const auto canonicalBounds = GetCanonicalCropBoxBounds();
    cube->SetBounds(
        canonicalBounds[0], canonicalBounds[1],
        canonicalBounds[2], canonicalBounds[3],
        canonicalBounds[4], canonicalBounds[5]);
    cube->Update();

    auto boxToModelTransform = GetBoxToModelTransform(cropData.GetBoxToModelMatrix());
    auto transformFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    transformFilter->SetInputConnection(cube->GetOutputPort());
    transformFilter->SetTransform(boxToModelTransform);
    transformFilter->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(transformFilter->GetOutput());
    return output;
}
bool OrthogonalCropAlgorithm::GetBoundsAreValid(
    const std::array<double, 6>& dataModelBounds,
    const std::array<double, 6>& cropModelBounds,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // allowPartialOverlap 表示裁切盒只要和输入有真实体积交集即可。
    // preview artifact 和 image KeepInside submit 都允许该语义；后者会提取交集范围。
    // 这里默认调用方已经把 dataModelBounds 和 cropModelBounds 放到了同一 model 坐标系里，不再做二次折叠。
    if (!GetBoundsHavePositiveVolume(dataModelBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = GetBoundsMessage("Data model bounds are invalid:", dataModelBounds);
        return false;
    }

    if (!GetBoundsHavePositiveVolume(cropModelBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = GetBoundsMessage("Crop model bounds are invalid:", cropModelBounds);
        return false;
    }

    const bool inRange = allowPartialOverlap
        ? GetBoundsOverlap(dataModelBounds, cropModelBounds)
        : GetBoundsContained(dataModelBounds, cropModelBounds);
    if (!inRange) {
        failureReason = OrthogonalCropFailureReason::BoundsOutOfRange;
        message = allowPartialOverlap
            ? "Crop model bounds do not overlap the active model bounds."
            : "Crop model bounds exceed the active model bounds.";
        return false;
    }

    failureReason = OrthogonalCropFailureReason::None;
    message.clear();
    return true;
}

bool OrthogonalCropAlgorithm::GetCropDataModel(
    const std::array<double, 6>& dataModelBounds,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // request 的几何真源只有 boxToModelMatrix；cropData 只额外缓存由它派生出的 model AABB。
    cropData = CropDataModel();
    cropData.SetGlobalOffsetMatrix(request.GetGlobalOffsetMatrix());
    cropData.SetBoxToModelMatrix(request.GetBoxToModelMatrix());
    cropData.SetModelBounds(GetModelBoundsFromBoxToModelMatrix(request.GetBoxToModelMatrix()));

    return GetBoundsAreValid(dataModelBounds, cropData.GetModelBounds(), failureReason, message, allowPartialOverlap);
}

static bool BuildCropDataModel(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // image 重载只负责把 vtkImageData 的 model bounds 作为 dataModelBounds 传下去；
    // 真正的 request -> cropData 归一化仍由通用重载完成。
    if (!image) {
        failureReason = OrthogonalCropFailureReason::InputImageMissing;
        message = "Input image is null.";
        return false;
    }

    return OrthogonalCropAlgorithm::GetCropDataModel(
        GetImageBounds(image),
        request,
        cropData,
        failureReason,
        message,
        allowPartialOverlap);
}

static std::array<int, 6> GetSnappedIndexBounds(vtkImageData* image, const CropDataModel& cropData)
{
    // image 路径最终都要落到体素级执行，因此先把已经折叠回 image model 的 bounds
    // 通过 vtkImageData 原生的 model->continuous-index 变换吸附到 index 整数区间。
    // 这里显式变换 8 个角点，避免 direction 非单位矩阵时只按各轴独立换算导致包围盒失真。
    // 最终结果语义是一个“完整覆盖 crop bounds 的整数 index 包围盒”，供统计和执行路径共用。
    std::array<int, 6> indexBounds = { 0, 0, 0, 0, 0, 0 };
    if (!image) {
        return indexBounds;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);

    const auto modelBounds = cropData.GetModelBounds();
    std::array<double, 3> minContinuousIndex = {
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max()
    };
    std::array<double, 3> maxContinuousIndex = {
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest()
    };

    for (int sx = 0; sx < 2; ++sx) {
        for (int sy = 0; sy < 2; ++sy) {
            for (int sz = 0; sz < 2; ++sz) {
                const double modelPoint[3] = {
                    modelBounds[sx == 0 ? 0 : 1],
                    modelBounds[sy == 0 ? 2 : 3],
                    modelBounds[sz == 0 ? 4 : 5]
                };
                // model 点落在 index 的哪个连续子区间；VTK API 名里的 PhysicalPoint 对应 image model。
                double continuousIndex[3] = { 0.0, 0.0, 0.0 };
                image->TransformPhysicalPointToContinuousIndex(modelPoint, continuousIndex);
                for (int axis = 0; axis < 3; ++axis) {
                    minContinuousIndex[axis] = std::min(minContinuousIndex[axis], continuousIndex[axis]);
                    maxContinuousIndex[axis] = std::max(maxContinuousIndex[axis], continuousIndex[axis]);
                }
            }
        }
    }

    for (int axis = 0; axis < 3; ++axis) {
        const int minIndex = static_cast<int>(std::floor(minContinuousIndex[axis] + BoundsEpsilon));
        const int maxIndex = static_cast<int>(std::ceil(maxContinuousIndex[axis] - BoundsEpsilon));
        // axis = 0 表示 X 轴  axis = 1 表示 Y 轴  axis = 2 表示 Z 轴
        indexBounds[axis * 2 + 0] = std::clamp(std::min(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0)); // min
        indexBounds[axis * 2 + 1] = std::clamp(std::max(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0)); // max
    }

    return indexBounds;
}

vtkSmartPointer<vtkPolyData> OrthogonalCropAlgorithm::GetOutlinePolyData(const CropDataModel& cropData)
{
    return GetOutlinePolyDataInternal(cropData);
}

static OrthogonalCropStatistics GetImageDiagnostics(
    vtkImageData* image,
    const CropDataModel& cropData,
    CropRemovalMode removalMode,
    CropExecutionMode executionMode,
    std::size_t availableRamBytes)
{
    // Statistics 只对外报告数据源、后端和失败诊断。
    // image submit 的 snapped index / RAM gate 属于内部执行计划，不再挂在 statistics 上。
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);

    if (executionMode == CropExecutionMode::PreviewArtifact) {
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::MaskPreview);
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string validationMessage;
        if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
            GetImageBounds(image),
            cropData.GetModelBounds(),
            failureReason,
            validationMessage,
            true)) {
            statistics.SetFailureReason(failureReason);
            statistics.SetValidationMessage(validationMessage);
            return statistics;
        }
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        return statistics;
    }

    return BuildSubmitPlan(image, cropData, removalMode, availableRamBytes).diagnostics;
}

OrthogonalCropStatistics OrthogonalCropAlgorithm::GetStatistics(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    // request 入口的职责非常窄：先做一次 request -> cropData 归一化，
    // 再把 execution/removal/RAM 约束带进 cropData 版本的统计核心。
    OrthogonalCropStatistics statistics;
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const bool allowPartialOverlap =
        request.GetExecutionMode() == CropExecutionMode::PreviewArtifact
        || (request.GetExecutionMode() == CropExecutionMode::Submit
            && request.GetRemovalMode() == CropRemovalMode::KeepInside);
    if (!BuildCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    // 对外暴露的 request 入口最终先归一化，再转入 cropData 版本的统计核心。
    return GetImageDiagnostics(
        image,
        cropData,
        request.GetRemovalMode(),
        request.GetExecutionMode(),
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}

static OrthogonalCropResult GetPreviewResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const CropStateModel& cropState,
    CropRemovalMode removalMode)
{
    // ── 步骤 1：构造基本结果骨架 ──
    // 预先填充数据源/后端标记、几何快照与交互状态
    // 后续 steps 逐步回填 mask / diagnostics / outline
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::MaskPreview);
    result.SetCropDataModel(cropData);
    result.SetCropStateModel(cropState);

    // ── 步骤 2：校验 bounds 合法性 ──
    // allowPartialOverlap=true，预览允许盒子部分超出输入边界
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string validationMessage;
    if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
        GetImageBounds(image),
        cropData.GetModelBounds(),
        failureReason,
        validationMessage,
        true)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(validationMessage);
        return result;
    }
    // 8 个 model 角点转换成覆盖它们的 index bounds。
    const auto snappedIndexBounds = GetSnappedIndexBounds(image, cropData);

    // ── 步骤 3a：生成 mask ──
    // KeepInside  → ROI 大小 compact mask（memset 快路径）
    // RemoveInside → full-volume mask（标记盒外区域）
    result.SetMaskImage(::GetMaskImage(
        image,
        cropData,
        snappedIndexBounds,
        removalMode));

    // ── 步骤 3b：回填 diagnostics ──
    // Statistics 只携带数据源、后端和失败诊断；mask 产物本身由 result 持有。
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::MaskPreview);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetStatistics(statistics);
    result.SetFailureReason(statistics.GetFailureReason());

    // ── 步骤 3c：回填 outline 与最终状态 ──
    // outline 落在 model 下，供 overlay 直接消费
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(cropData));
    result.SetSucceeded(result.GetMaskImage() != nullptr);
    if (!result.GetSucceeded()) {
        result.SetFailureReason(OrthogonalCropFailureReason::MaskPreviewCreationFailed);
        result.SetMessage("Failed to build image 2D mask preview.");
    }
    return result;
}

static OrthogonalCropResult GetSubmitResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const CropStateModel& cropState,
    CropRemovalMode removalMode,
    std::size_t availableRamBytes)
{
    // image submit 返回的是新的主数据 image，
    // 因此除了提取体素数据，还必须同步修正 bounds 和 globalOffsetMatrix。
    // 结果说明：输出 image 是新的主数据快照，derivedCropData 记录它自己的 model bounds 与位移补偿。
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    result.SetCropDataModel(cropData);
    result.SetCropStateModel(cropState);

    // ── 步骤 1：内部提交计划 ──
    // snapped index、RAM gate 和可执行性只服务 image submit，不进入通用 statistics。
    const auto submitPlan = BuildSubmitPlan(
        image,
        cropData,
        removalMode,
        availableRamBytes);
    result.SetStatistics(submitPlan.diagnostics);
    result.SetFailureReason(submitPlan.diagnostics.GetFailureReason());

    if (!submitPlan.canExecute) {
        result.SetMessage(submitPlan.diagnostics.GetValidationMessage());
        return result;
    }

    // ── 步骤 2：体素提取 ──
    // 通过 ExtractVOI → ImageChangeInformation 产出独立 image submit image。
    // VOI 只能输出 index 轴对齐子体；若 widget box 是旋转盒，再按同一份
    // modelToBox 判定清掉盒外体素，避免提交结果比 preview 多出 AABB 角落内容。
    auto submitImage = GetExtractedImage(image, submitPlan.snappedIndexBounds);
    if (!submitImage) {
        result.SetFailureReason(OrthogonalCropFailureReason::SubmitImageCreationFailed);
        result.SetMessage("Failed to build image submit image.");
        return result;
    }
    SetSubmitOutsideBoxToBackground(submitImage, cropData);

    // ── 步骤 3：偏移补偿 ──
    // 提取后 origin 改变，把位移量累加进 globalOffsetMatrix
    // 保证上层共享坐标语义连续
    double originalOrigin[3] = { 0.0, 0.0, 0.0 };
    image->GetOrigin(originalOrigin);
    double newOrigin[3] = { 0.0, 0.0, 0.0 };
    submitImage->GetOrigin(newOrigin);

    CropDataModel submitCropData = cropData;
    double submitBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    submitImage->GetBounds(submitBounds);
    const CropBoundsDouble6Array submitModelBounds = {
        submitBounds[0], submitBounds[1],
        submitBounds[2], submitBounds[3],
        submitBounds[4], submitBounds[5]
    };
    submitCropData.SetBoxToModelMatrixFromBounds(submitModelBounds);
    submitCropData.SetModelBounds(submitModelBounds);
    submitCropData.SetGlobalOffsetMatrix(
        GetUpdatedOffsetMatrix(
            cropData.GetGlobalOffsetMatrix(),
            {
                newOrigin[0] - originalOrigin[0],
                newOrigin[1] - originalOrigin[1],
                newOrigin[2] - originalOrigin[2]
            }));

    // ── 步骤 4：结果回填 ──
    // image submit image 本身就是新主数据，关闭交互裁切语义
    CropStateModel submitCropState = cropState;
    submitCropState.SetCropEnabled(false);

    result.SetSubmitImage(submitImage);
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(submitCropData));
    result.SetCropDataModel(submitCropData);
    result.SetCropStateModel(submitCropState);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::SubmitExtractVOI);
    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    return result;
}

OrthogonalCropResult OrthogonalCropAlgorithm::GetResult(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    OrthogonalCropResult result;
    result.SetCropStateModel(request.GetCropStateModel());

    // ── 请求归一化 ──
    // request 只携带 boxToModelMatrix；cropData 额外派生 model AABB 供后端执行。
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const bool allowPartialOverlap =
        request.GetExecutionMode() == CropExecutionMode::PreviewArtifact
        || (request.GetExecutionMode() == CropExecutionMode::Submit
            && request.GetRemovalMode() == CropRemovalMode::KeepInside);
    if (!BuildCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    // ── 执行分支分发：preview artifact vs image submit ──
    // PreviewArtifact → GetPreviewResult → image 2D mask + box 3D outline + diagnostics
    // Submit → GetSubmitResult → image submit image + globalOffsetMatrix
    if (request.GetExecutionMode() == CropExecutionMode::PreviewArtifact) {
        return GetPreviewResult(
            image,
            cropData,
            request.GetCropStateModel(),
            request.GetRemovalMode());
    }

    return GetSubmitResult(
        image,
        cropData,
        request.GetCropStateModel(),
        request.GetRemovalMode(),
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}
