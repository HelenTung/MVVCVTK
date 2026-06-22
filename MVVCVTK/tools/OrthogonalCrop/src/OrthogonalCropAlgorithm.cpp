// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/OrthogonalCropAlgorithm.cpp
// 分类: Math / Core Algorithm Implementation
// 说明: 负责 bounds 校验、request 归一化、image 2D mask preview、box 3D outline preview 与 image submit 统计/执行。
// =====================================================================

#include "OrthogonalCropAlgorithm.h"

#include <vtkBox.h>
#include <vtkBoundingBox.h>
#include <vtkCubeSource.h>
#include <vtkDataArray.h>
#include <vtkExtractVOI.h>
#include <vtkGeometryFilter.h>
#include <vtkImageChangeInformation.h>
#include <vtkMatrix4x4.h>
#include <vtkOutlineSource.h>
#include <vtkPointData.h>
#include <vtkTableBasedClipDataSet.h>
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

static std::array<double, 6> GetImageModelBounds(vtkImageData* image)
{
    // vtkImageData::GetBounds 返回 image model 范围；底层仍通过 VTK physical-point API 表达。
    // 这里不再额外做 world/image model 折叠，算法层后续所有 image 校验都直接用 image model 语义。
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!image) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    image->GetBounds(rawBounds);
    return { rawBounds[0], rawBounds[1], rawBounds[2], rawBounds[3], rawBounds[4], rawBounds[5] };
}

static std::array<double, 6> GetPolyDataInputModelBounds(vtkPolyData* polyData)
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!polyData) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    polyData->GetBounds(rawBounds);
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
    const std::array<double, 6>& inputModelBounds,
    const std::array<double, 6>& cropInputModelBounds)
{
    // preview 允许 partial overlap，因此这里只判断两个轴对齐盒是否真正有体积交集。
    return cropInputModelBounds[1] > inputModelBounds[0] + BoundsEpsilon
        && cropInputModelBounds[0] < inputModelBounds[1] - BoundsEpsilon
        && cropInputModelBounds[3] > inputModelBounds[2] + BoundsEpsilon
        && cropInputModelBounds[2] < inputModelBounds[3] - BoundsEpsilon
        && cropInputModelBounds[5] > inputModelBounds[4] + BoundsEpsilon
        && cropInputModelBounds[4] < inputModelBounds[5] - BoundsEpsilon;
}

static bool GetBoundsContained(
    const std::array<double, 6>& inputModelBounds,
    const std::array<double, 6>& cropInputModelBounds)
{
    // 严格校验路径要求裁切盒完整包含在 active input model bounds 内。
    // Image KeepInside submit 会走 partial overlap：后续 snapped index 会 clamp 到有效体素范围。
    return cropInputModelBounds[0] >= inputModelBounds[0] - BoundsEpsilon
        && cropInputModelBounds[1] <= inputModelBounds[1] + BoundsEpsilon
        && cropInputModelBounds[2] >= inputModelBounds[2] - BoundsEpsilon
        && cropInputModelBounds[3] <= inputModelBounds[3] + BoundsEpsilon
        && cropInputModelBounds[4] >= inputModelBounds[4] - BoundsEpsilon
        && cropInputModelBounds[5] <= inputModelBounds[5] + BoundsEpsilon;
}

static std::array<double, 6> GetInputModelBoundsFromBoxToInputModelMatrix(const std::array<double, 16>& boxToInputModelMatrixData)
{
    // 从 boxToInputModelMatrix 派生 active input model AABB；
    // 8 个标准盒角点保留有向盒的真实外包范围，后续用它做校验、index 吸附和统计。
    auto boxToInputModelTransform = vtkSmartPointer<vtkTransform>::New();
    boxToInputModelTransform->SetMatrix(boxToInputModelMatrixData.data());

    vtkBoundingBox inputModelBounds;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const double boxPoint[3] = { static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz) };
                double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
                boxToInputModelTransform->TransformPoint(boxPoint, inputModelPoint);
                inputModelBounds.AddPoint(inputModelPoint);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    inputModelBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

static vtkSmartPointer<vtkTransform> GetBoxToInputModelTransform(const std::array<double, 16>& boxToInputModelMatrixData)
{
    auto boxToInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputModelMatrix->DeepCopy(boxToInputModelMatrixData.data());

    // 输出 outline 时从标准盒 [-1,1]^3 出发，直接用 box -> input model 生成显示几何。
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(boxToInputModelMatrix);
    return transform;
}

static vtkSmartPointer<vtkMatrix4x4> GetInputModelToBoxMatrix(const CropDataModel& cropData)
{
    auto boxToInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputModelMatrix->DeepCopy(cropData.GetBoxToInputModelMatrix().data());

    // inside/outside 判定统一在标准盒空间完成：
    // input model 点乘以 inputModelToBox 后，只需检查三个坐标是否落在 [-1,1]。
    auto inputModelToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToInputModelMatrix, inputModelToBoxMatrix);
    return inputModelToBoxMatrix;
}

static bool GetBoxToInputModelIsAxisAligned(const std::array<double, 16>& boxToInputModelMatrixData)
{
    // 只识别无旋转/无剪切的对角矩阵快路径；任意旋转或剪切仍走标准 inputModelToBox 判断。
    return std::abs(boxToInputModelMatrixData[1]) <= BoundsEpsilon
        && std::abs(boxToInputModelMatrixData[2]) <= BoundsEpsilon
        && std::abs(boxToInputModelMatrixData[4]) <= BoundsEpsilon
        && std::abs(boxToInputModelMatrixData[6]) <= BoundsEpsilon
        && std::abs(boxToInputModelMatrixData[8]) <= BoundsEpsilon
        && std::abs(boxToInputModelMatrixData[9]) <= BoundsEpsilon;
}

static bool GetCanonicalBoxContainsPoint(const double boxPoint[4])
{
    const double invW = std::abs(boxPoint[3]) > 1e-12 ? 1.0 / boxPoint[3] : 1.0;
    return std::abs(boxPoint[0] * invW) <= 1.0 + BoundsEpsilon
        && std::abs(boxPoint[1] * invW) <= 1.0 + BoundsEpsilon
        && std::abs(boxPoint[2] * invW) <= 1.0 + BoundsEpsilon;
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
    plan.diagnostics.SetResolvedBackend(OrthogonalCropBackend::SubmitExtractVOI);

    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string validationMessage;
    const bool allowPartialOverlap = removalMode == CropRemovalMode::KeepInside;
    if (!image) {
        plan.diagnostics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        plan.diagnostics.SetValidationMessage("Input image is null.");
        return plan;
    }

    if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
        GetImageModelBounds(image),
        cropData.GetInputModelBounds(),
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
    // 2D mask preview 只生成 inside/outside 掩码；
    // 执行域先收缩到 snapped AABB，再用标准盒判定避免整图遍历。
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
        maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
        if (!maskPtr) {
            return nullptr;
        }
        const vtkIdType totalVoxelCount = static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
        std::memset(maskPtr, 0, static_cast<std::size_t>(totalVoxelCount));
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
    // mask 用 255 表示保留、0 表示移除；
    // KeepInside 和 RemoveInside 只交换 inside/outside 的写入语义。
    const unsigned char keptValue = 255;
    const unsigned char removedValue = 0;
    const unsigned char insideValue = keepInside ? keptValue : removedValue;
    const unsigned char outsideValue = keepInside ? removedValue : keptValue;

    const vtkIdType totalVoxelCount = static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
    const vtkIdType allocatedVoxelCount = useCompactMask
        ? static_cast<vtkIdType>(width) * height * depth
        : totalVoxelCount;
    std::memset(maskPtr, outsideValue, static_cast<std::size_t>(allocatedVoxelCount));

    if (GetBoxToInputModelIsAxisAligned(cropData.GetBoxToInputModelMatrix())) {
        // 标准盒矩阵退化为 input model 轴对齐盒时，snapped AABB 内部整块都属于 inside。
        if (useCompactMask) {
            std::memset(maskPtr, insideValue, static_cast<std::size_t>(allocatedVoxelCount));
        }
        else {
            const vtkIdType rowStride = dims[0];
            const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];
            for (int k = minK; k <= maxK; ++k) {
                for (int j = minJ; j <= maxJ; ++j) {
                    // 轴对齐盒的每个有效区间在内存中是连续行；
                    // 直接 memset 整行，避免逐体素写入同一语义值。
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

    auto inputModelToBoxMatrix = GetInputModelToBoxMatrix(cropData);

    // 旋转/缩放盒的真实 inside 由“index 体素中心 -> image model -> 标准盒空间”决定。
    // 这里仍旧只在 snapped AABB 范围内遍历，避免为了旋转盒额外扩张执行域。
    const vtkIdType rowStride = useCompactMask ? width : dims[0];
    const vtkIdType sliceStride = useCompactMask
        ? static_cast<vtkIdType>(width) * height
        : static_cast<vtkIdType>(dims[0]) * dims[1];
    auto indexToPhysicalMatrix = image->GetIndexToPhysicalMatrix();
    const double inputModelToBoxStepVector[4] = {
        indexToPhysicalMatrix->GetElement(0, 0),
        indexToPhysicalMatrix->GetElement(1, 0),
        indexToPhysicalMatrix->GetElement(2, 0),
        0.0
    };
    double inputModelToBoxStepResult[4] = { 0.0, 0.0, 0.0, 0.0 };
    inputModelToBoxMatrix->MultiplyPoint(inputModelToBoxStepVector, inputModelToBoxStepResult);

    for (int k = minK; k <= maxK; ++k) {
        for (int j = minJ; j <= maxJ; ++j) {
            const int rowStartIndex[3] = { minI, j, k };
            double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
            // 每一行先把 row 起点 index 转到 image model，
            // 再整体乘 inputModelToBox；后续沿 i 方向只做增量推进，避免每个点都重乘矩阵。
            // image model 底层通过 VTK physical-point API 表达。
            image->TransformIndexToPhysicalPoint(rowStartIndex, inputModelPoint);
            const double inputModelPoint4[4] = { inputModelPoint[0], inputModelPoint[1], inputModelPoint[2], 1.0 };
            double boxPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
            inputModelToBoxMatrix->MultiplyPoint(inputModelPoint4, boxPoint);

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
                    maskPtr[linearIndex] = insideValue;
                }

                boxPoint[0] += inputModelToBoxStepResult[0];
                boxPoint[1] += inputModelToBoxStepResult[1];
                boxPoint[2] += inputModelToBoxStepResult[2];
                boxPoint[3] += inputModelToBoxStepResult[3];
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
    // 同时把新的 image model origin 设置成输出块起点对应的 image model 坐标。
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
        || GetBoxToInputModelIsAxisAligned(cropData.GetBoxToInputModelMatrix())) {
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

    auto inputModelToBoxMatrix = GetInputModelToBoxMatrix(cropData);
    auto indexToPhysicalMatrix = submitImage->GetIndexToPhysicalMatrix();
    const double inputModelToBoxStepVector[4] = {
        indexToPhysicalMatrix->GetElement(0, 0),
        indexToPhysicalMatrix->GetElement(1, 0),
        indexToPhysicalMatrix->GetElement(2, 0),
        0.0
    };
    double inputModelToBoxStepResult[4] = { 0.0, 0.0, 0.0, 0.0 };
    inputModelToBoxMatrix->MultiplyPoint(inputModelToBoxStepVector, inputModelToBoxStepResult);

    for (int k = 0; k < dims[2]; ++k) {
        for (int j = 0; j < dims[1]; ++j) {
            const int rowStartIndex[3] = { 0, j, k };
            double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
            submitImage->TransformIndexToPhysicalPoint(rowStartIndex, inputModelPoint);

            const double inputModelPoint4[4] = { inputModelPoint[0], inputModelPoint[1], inputModelPoint[2], 1.0 };
            double boxPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
            inputModelToBoxMatrix->MultiplyPoint(inputModelPoint4, boxPoint);

            for (int i = 0; i < dims[0]; ++i) {
                if (!GetCanonicalBoxContainsPoint(boxPoint)) {
                    int index[3] = { i, j, k };
                    scalars->SetTuple(
                        submitImage->ComputePointId(index),
                        backgroundTuple.data());
                }

                boxPoint[0] += inputModelToBoxStepResult[0];
                boxPoint[1] += inputModelToBoxStepResult[1];
                boxPoint[2] += inputModelToBoxStepResult[2];
                boxPoint[3] += inputModelToBoxStepResult[3];
            }
        }
    }

    submitImage->Modified();
}

static vtkSmartPointer<vtkPolyData> GetOutlinePolyDataInternal(const CropDataModel& cropData)
{
    // box 3D outline preview 的几何真源同样是标准盒 [-1,1]^3 + boxToInputModelMatrix。
    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    const auto canonicalBounds = GetCanonicalCropBoxBounds();
    cube->SetBounds(
        canonicalBounds[0], canonicalBounds[1],
        canonicalBounds[2], canonicalBounds[3],
        canonicalBounds[4], canonicalBounds[5]);
    cube->Update();

    auto boxToInputModelTransform = GetBoxToInputModelTransform(cropData.GetBoxToInputModelMatrix());
    auto transformFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    transformFilter->SetInputConnection(cube->GetOutputPort());
    transformFilter->SetTransform(boxToInputModelTransform);
    transformFilter->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(transformFilter->GetOutput());
    return output;
}
bool OrthogonalCropAlgorithm::GetBoundsAreValid(
    const std::array<double, 6>& inputModelBounds,
    const std::array<double, 6>& cropInputModelBounds,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // allowPartialOverlap 表示裁切盒只要和输入有真实体积交集即可。
    // 预览执行链和 image KeepInside submit 都允许该语义；后者会提取交集范围。
    // inputModelBounds 是当前输入数据在 active input model 下的 AABB，
    // cropInputModelBounds 是 boxToInputModelMatrix 派生出的裁切盒外接 AABB；
    // 二者已在同一坐标系里，因此这里只判断正体积、相交或包含关系。
    if (!GetBoundsHavePositiveVolume(inputModelBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = GetBoundsMessage("Input model bounds are invalid:", inputModelBounds);
        return false;
    }

    if (!GetBoundsHavePositiveVolume(cropInputModelBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = GetBoundsMessage("Crop input model bounds are invalid:", cropInputModelBounds);
        return false;
    }

    const bool inRange = allowPartialOverlap
        ? GetBoundsOverlap(inputModelBounds, cropInputModelBounds)
        : GetBoundsContained(inputModelBounds, cropInputModelBounds);
    if (!inRange) {
        failureReason = OrthogonalCropFailureReason::BoundsOutOfRange;
        message = allowPartialOverlap
            ? "Crop input model bounds do not overlap the active input model bounds."
            : "Crop input model bounds exceed the active input model bounds.";
        return false;
    }

    failureReason = OrthogonalCropFailureReason::None;
    message.clear();
    return true;
}

bool OrthogonalCropAlgorithm::GetCropDataModel(
    const std::array<double, 6>& inputModelBounds,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // 将 request 归一化为后端统一的 cropData；
    // boxToInputModelMatrix 继续作为精确有向盒真源，inputModelBounds 只派生为校验和粗范围缓存。
    cropData = CropDataModel();
    cropData.SetBoxToInputModelMatrix(request.GetBoxToInputModelMatrix());
    cropData.SetInputModelBounds(GetInputModelBoundsFromBoxToInputModelMatrix(request.GetBoxToInputModelMatrix()));

    return GetBoundsAreValid(inputModelBounds, cropData.GetInputModelBounds(), failureReason, message, allowPartialOverlap);
}

static bool BuildCropDataModel(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // image 重载只负责把 vtkImageData 的 input model bounds 作为 inputModelBounds 传下去；
    // 真正的 request -> cropData 归一化仍由通用重载完成。
    if (!image) {
        failureReason = OrthogonalCropFailureReason::InputImageMissing;
        message = "Input image is null.";
        return false;
    }

    return OrthogonalCropAlgorithm::GetCropDataModel(
        GetImageModelBounds(image),
        request,
        cropData,
        failureReason,
        message,
        allowPartialOverlap);
}

static bool BuildCropDataModel(
    vtkPolyData* polyData,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // polydata 重载只负责提供输入网格自身的 input model bounds；
    // request -> cropData 的几何归一化继续与 image 路径共享同一套算法。
    if (!polyData) {
        failureReason = OrthogonalCropFailureReason::InputPolyDataMissing;
        message = "Input polydata is null.";
        return false;
    }

    return OrthogonalCropAlgorithm::GetCropDataModel(
        GetPolyDataInputModelBounds(polyData),
        request,
        cropData,
        failureReason,
        message,
        allowPartialOverlap);
}

static std::array<int, 6> GetSnappedIndexBounds(vtkImageData* image, const CropDataModel& cropData)
{
    // image 路径最终都要落到体素级执行，因此先把已经折叠回 image model 的 bounds
    // 通过 vtkImageData 原生的 image-model -> continuous-index 变换吸附到 index 整数区间。
    // 这里显式变换 8 个角点，避免 direction 非单位矩阵时只按各轴独立换算导致包围盒失真。
    // 最终结果语义是一个“完整覆盖 crop bounds 的整数 index 包围盒”，供统计和执行路径共用。
    std::array<int, 6> indexBounds = { 0, 0, 0, 0, 0, 0 };
    if (!image) {
        return indexBounds;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);

    const auto inputModelBounds = cropData.GetInputModelBounds();
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
                const double inputModelPoint[3] = {
                    inputModelBounds[sx == 0 ? 0 : 1],
                    inputModelBounds[sy == 0 ? 2 : 3],
                    inputModelBounds[sz == 0 ? 4 : 5]
                };
                // input model 点落在 index 的哪个连续子区间；VTK API 名里的 PhysicalPoint 对应 image model。
                double continuousIndex[3] = { 0.0, 0.0, 0.0 };
                image->TransformPhysicalPointToContinuousIndex(inputModelPoint, continuousIndex);
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
        indexBounds[axis * 2 + 0] = std::clamp(std::min(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0));
        indexBounds[axis * 2 + 1] = std::clamp(std::max(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0));
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
    OrthogonalCropBackend backend,
    std::size_t availableRamBytes)
{
    // Statistics 只对外报告数据源、后端和失败诊断。
    // image submit 的 snapped index / RAM gate 属于内部执行计划，不再挂在 statistics 上。
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);

    switch (backend) {
    case OrthogonalCropBackend::None: {
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string validationMessage;
        if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
            GetImageModelBounds(image),
            cropData.GetInputModelBounds(),
            failureReason,
            validationMessage,
            true)) {
            statistics.SetFailureReason(failureReason);
            statistics.SetValidationMessage(validationMessage);
            return statistics;
        }
        statistics.SetResolvedBackend(OrthogonalCropBackend::None);
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        return statistics;
    }
    case OrthogonalCropBackend::MaskPreview: {
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string validationMessage;
        if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
            GetImageModelBounds(image),
            cropData.GetInputModelBounds(),
            failureReason,
            validationMessage,
            true)) {
            statistics.SetFailureReason(failureReason);
            statistics.SetValidationMessage(validationMessage);
            return statistics;
        }
        statistics.SetResolvedBackend(OrthogonalCropBackend::MaskPreview);
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        return statistics;
    }
    case OrthogonalCropBackend::SubmitExtractVOI:
        return BuildSubmitPlan(image, cropData, removalMode, availableRamBytes).diagnostics;
    case OrthogonalCropBackend::ClipPreview:
        statistics.SetResolvedBackend(backend);
        statistics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
        statistics.SetValidationMessage("Image algorithm does not support polydata clip preview backend.");
        return statistics;
    default:
        statistics.SetResolvedBackend(backend);
        statistics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
        statistics.SetValidationMessage("Image algorithm received an unsupported crop backend.");
        return statistics;
    }
}

OrthogonalCropStatistics OrthogonalCropAlgorithm::GetStatistics(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    // request 入口的职责非常窄：先做一次 request -> cropData 归一化，
    // 再把 backend/removal/RAM 约束带进 cropData 版本的统计核心。
    OrthogonalCropStatistics statistics;
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const bool allowPartialOverlap =
        request.GetBackend() != OrthogonalCropBackend::SubmitExtractVOI
        || (request.GetBackend() == OrthogonalCropBackend::SubmitExtractVOI
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
        request.GetBackend(),
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}

static OrthogonalCropResult GetPreviewResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const OrthogonalCropResult& resultContext,
    CropRemovalMode removalMode)
{
    // 复用调用方已经确定好的结果身份；
    // 后续 mask、diagnostics、outline 都围绕同一份 cropData 回填。
    auto result = resultContext;
    result.SetCropDataModel(cropData);

    // 预览允许裁切盒只与输入部分重叠；
    // 这样用户拖到边界外时仍能看到有效交集的反馈。
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string validationMessage;
    if (!OrthogonalCropAlgorithm::GetBoundsAreValid(
        GetImageModelBounds(image),
        cropData.GetInputModelBounds(),
        failureReason,
        validationMessage,
        true)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(validationMessage);
        return result;
    }
    // 将 input model 角点吸附为 index bounds；
    // mask 执行只需要覆盖裁切盒可能影响到的体素范围。
    const auto snappedIndexBounds = GetSnappedIndexBounds(image, cropData);

    // 按 removal mode 生成 mask；
    // KeepInside 用 compact ROI 降低分配成本，RemoveInside 保留 full-volume 以表达盒外补集。
    result.SetMaskImage(::GetMaskImage(
        image,
        cropData,
        snappedIndexBounds,
        removalMode));

    // 统计信息只记录数据源、后端和失败诊断；
    // mask 产物由 result 持有，避免把大对象挂进 diagnostics。
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    statistics.SetResolvedBackend(OrthogonalCropBackend::MaskPreview);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetStatistics(statistics);
    result.SetFailureReason(statistics.GetFailureReason());

    // outline 使用 active input model 坐标回填；
    // overlay 可以直接与主数据矩阵对齐显示。
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
    const OrthogonalCropResult& resultContext,
    CropRemovalMode removalMode,
    std::size_t availableRamBytes)
{
    // image submit 产出新的主数据 image；
    // submit 结果只回填新的 cropData 和 image，避免把提交后的显示同步语义塞回算法层。
    auto result = resultContext;
    result.SetCropDataModel(cropData);

    // 提交计划只服务 image submit；
    // snapped index、RAM gate 和可执行性留在内部，避免污染通用 statistics 语义。
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

    // 提取独立的 image submit image；
    // VOI 只能得到 index AABB，旋转盒还要用 inputModelToBox 清掉盒外角落，保证 submit 与 preview 语义一致。
    auto submitImage = GetExtractedImage(image, submitPlan.snappedIndexBounds);
    if (!submitImage) {
        result.SetFailureReason(OrthogonalCropFailureReason::SubmitImageCreationFailed);
        result.SetMessage("Failed to build image submit image.");
        return result;
    }
    SetSubmitOutsideBoxToBackground(submitImage, cropData);

    CropDataModel submitCropData = cropData;
    double submitBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    submitImage->GetBounds(submitBounds);
    const CropBoundsDouble6Array submitInputModelBounds = {
        submitBounds[0], submitBounds[1],
        submitBounds[2], submitBounds[3],
        submitBounds[4], submitBounds[5]
    };
    submitCropData.SetBoxToInputModelMatrixFromBounds(submitInputModelBounds);
    submitCropData.SetInputModelBounds(submitInputModelBounds);

    // 回填提交结果并关闭交互裁切语义；
    // submit image 将成为新主数据，旧 preview 状态不能继续沿用。
    CropStateModel submitCropState = resultContext.GetCropStateModel();
    submitCropState.SetCropEnabled(false);

    result.SetSubmitImage(submitImage);
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(submitCropData));
    result.SetCropDataModel(submitCropData);
    result.SetCropStateModel(submitCropState);
    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    return result;
}

static OrthogonalCropStatistics GetPolyDataDiagnostics(
    OrthogonalCropBackend backend)
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    statistics.SetResolvedBackend(backend);

    switch (backend) {
    case OrthogonalCropBackend::None:
    case OrthogonalCropBackend::ClipPreview:
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        return statistics;
    case OrthogonalCropBackend::MaskPreview:
    case OrthogonalCropBackend::SubmitExtractVOI:
        statistics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
        statistics.SetValidationMessage("Polydata algorithm does not support image crop backend.");
        return statistics;
    default:
        statistics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
        statistics.SetValidationMessage("Polydata algorithm received an unsupported crop backend.");
        return statistics;
    }
}

static vtkSmartPointer<vtkPolyData> GetClippedPolyDataInternal(
    vtkPolyData* polyData,
    const CropDataModel& cropData,
    CropRemovalMode removalMode)
{
    if (!polyData) {
        return nullptr;
    }

    // cropData 已归一化，activeInputModelToBox transform 负责把 active input model 点送回 [-1,1]^3。
    auto clipFunction = vtkSmartPointer<vtkBox>::New();
    const auto canonicalBounds = GetCanonicalCropBoxBounds();
    clipFunction->SetBounds(
        canonicalBounds[0], canonicalBounds[1],
        canonicalBounds[2], canonicalBounds[3],
        canonicalBounds[4], canonicalBounds[5]);

    auto activeInputModelToBoxTransform = vtkSmartPointer<vtkTransform>::New();
    activeInputModelToBoxTransform->SetMatrix(cropData.GetBoxToInputModelMatrix().data());
    activeInputModelToBoxTransform->Inverse();
    clipFunction->SetTransform(activeInputModelToBoxTransform);

    auto clipFilter = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
    clipFilter->SetInputData(polyData);
    clipFilter->SetClipFunction(clipFunction);
    if (removalMode == CropRemovalMode::KeepInside) {
        clipFilter->InsideOutOn();
    }
    else {
        clipFilter->InsideOutOff();
    }

    auto geometryFilter = vtkSmartPointer<vtkGeometryFilter>::New();
    geometryFilter->SetInputConnection(clipFilter->GetOutputPort());
    geometryFilter->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(geometryFilter->GetOutput());
    return output;
}

static OrthogonalCropResult GetClipPreviewResult(
    vtkPolyData* polyData,
    const CropDataModel& cropData,
    const OrthogonalCropResult& resultContext,
    CropRemovalMode removalMode)
{
    // polydata preview 与 image preview 共用 result contract；
    // 算法层直接产出 clip polydata，bridge 只负责显示返回的 artifact。
    auto result = resultContext;
    result.SetCropDataModel(cropData);

    auto clipped = GetClippedPolyDataInternal(polyData, cropData, removalMode);
    if (!clipped) {
        result.SetFailureReason(OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed);
        result.SetMessage("Failed to build clipped polydata.");
        return result;
    }

    const auto statistics = GetPolyDataDiagnostics(OrthogonalCropBackend::ClipPreview);
    result.SetStatistics(statistics);
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(cropData));
    result.SetClipPolyData(clipped);
    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    return result;
}

OrthogonalCropResult OrthogonalCropAlgorithm::GetResult(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext,
    std::size_t fallbackAvailableRamBytes)
{
    auto result = resultContext;

    // 将 request 归一化为 cropData；
    // request 只携带 boxToInputModelMatrix，cropData 再派生 input model AABB 供后端执行。
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const bool allowPartialOverlap =
        request.GetBackend() != OrthogonalCropBackend::SubmitExtractVOI
        || (request.GetBackend() == OrthogonalCropBackend::SubmitExtractVOI
            && request.GetRemovalMode() == CropRemovalMode::KeepInside);
    if (!BuildCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    // 按 request.backend 分发到 preview 或 submit；
    // preview 产出显示 artifact，submit 产出新的 image。
    if (request.GetBackend() == OrthogonalCropBackend::None) {
        OrthogonalCropStatistics statistics;
        statistics.SetResolvedDataSource(resultContext.GetResolvedDataSource());
        statistics.SetResolvedBackend(resultContext.GetResolvedBackend());
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);

        result.SetStatistics(statistics);
        result.SetCropDataModel(cropData);
        result.SetOutlinePolyData(GetOutlinePolyDataInternal(cropData));
        result.SetSucceeded(true);
        result.SetFailureReason(OrthogonalCropFailureReason::None);
        return result;
    }

    if (request.GetBackend() == OrthogonalCropBackend::MaskPreview) {
        return GetPreviewResult(
            image,
            cropData,
            resultContext,
            request.GetRemovalMode());
    }

    if (request.GetBackend() == OrthogonalCropBackend::SubmitExtractVOI) {
        return GetSubmitResult(
            image,
            cropData,
            resultContext,
            request.GetRemovalMode(),
            GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
    }

    // image 算法只执行 image preview / image submit；
    // 遇到 polydata 或未来新增后端时必须失败返回，避免产生“成功但没有产物”的假结果。
    auto fallbackResult = resultContext;
    const auto diagnostics = GetImageDiagnostics(
        image,
        cropData,
        request.GetRemovalMode(),
        request.GetBackend(),
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
    fallbackResult.SetCropDataModel(cropData);
    fallbackResult.SetStatistics(diagnostics);
    fallbackResult.SetFailureReason(diagnostics.GetFailureReason());
    fallbackResult.SetMessage(diagnostics.GetValidationMessage());
    fallbackResult.SetSucceeded(false);
    return fallbackResult;
}

OrthogonalCropStatistics OrthogonalCropAlgorithm::GetStatistics(
    vtkPolyData* polyData,
    const OrthogonalCropRequest& request)
{
    OrthogonalCropStatistics statistics;
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!BuildCropDataModel(polyData, request, cropData, failureReason, message, true)) {
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        statistics.SetResolvedBackend(request.GetBackend());
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    return GetPolyDataDiagnostics(request.GetBackend());
}

OrthogonalCropResult OrthogonalCropAlgorithm::GetResult(
    vtkPolyData* polyData,
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext)
{
    auto result = resultContext;

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!BuildCropDataModel(polyData, request, cropData, failureReason, message, true)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    if (request.GetBackend() == OrthogonalCropBackend::None) {
        const auto statistics = GetPolyDataDiagnostics(OrthogonalCropBackend::None);
        result.SetStatistics(statistics);
        result.SetCropDataModel(cropData);
        result.SetOutlinePolyData(GetOutlinePolyDataInternal(cropData));
        result.SetSucceeded(true);
        result.SetFailureReason(OrthogonalCropFailureReason::None);
        return result;
    }

    if (request.GetBackend() == OrthogonalCropBackend::ClipPreview) {
        return GetClipPreviewResult(
            polyData,
            cropData,
            resultContext,
            request.GetRemovalMode());
    }

    auto fallbackResult = resultContext;
    const auto diagnostics = GetPolyDataDiagnostics(request.GetBackend());
    fallbackResult.SetCropDataModel(cropData);
    fallbackResult.SetStatistics(diagnostics);
    fallbackResult.SetFailureReason(diagnostics.GetFailureReason());
    fallbackResult.SetMessage(diagnostics.GetValidationMessage());
    fallbackResult.SetSucceeded(false);
    return fallbackResult;
}
