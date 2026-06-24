// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/OrthogonalCropAlgorithm.cpp
// 分类: Math / Core Algorithm Implementation
// 说明: 负责 request 归一化、box preview 几何结果与 image submit 产物构建。
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
    // 判断标准盒的三条局部轴在 input model 中是否仍分别落在单一坐标轴上。
    // 允许轴交换、90 度旋转和翻转；若某一列混入多个分量，或多个 box 轴占用同一 input model 轴，
    // 就说明存在任意角旋转、剪切或退化，不能走 AABB 快路径。
    bool inputModelAxisUsed[3] = { false, false, false };
    for (int boxAxis = 0; boxAxis < 3; ++boxAxis) {
        int mappedInputModelAxis = -1;
        for (int inputModelAxis = 0; inputModelAxis < 3; ++inputModelAxis) {
            const double component = boxToInputModelMatrixData[inputModelAxis * 4 + boxAxis];
            if (std::abs(component) <= BoundsEpsilon) {
                continue;
            }

            if (mappedInputModelAxis >= 0) {
                return false;
            }
            mappedInputModelAxis = inputModelAxis;
        }

        if (mappedInputModelAxis < 0 || inputModelAxisUsed[mappedInputModelAxis]) {
            return false;
        }
        inputModelAxisUsed[mappedInputModelAxis] = true;
    }

    return true;
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

static OrthogonalCropStatistics GetAlgorithmDiagnostics(
    OrthogonalCropDataSource dataSource,
    OrthogonalCropOperation operation,
    OrthogonalCropGeometryType geometryType,
    CropRemovalMode removalMode)
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(dataSource);
    statistics.SetResolvedOperation(operation);
    statistics.SetResolvedGeometryType(geometryType);
    statistics.SetResolvedRemovalMode(removalMode);
    return statistics;
}

static SubmitPlan BuildSubmitPlan(
    vtkImageData* image,
    const CropDataModel& cropData,
    OrthogonalCropGeometryType geometryType,
    CropRemovalMode removalMode,
    std::size_t availableRamBytes)
{
    SubmitPlan plan;
    plan.diagnostics = GetAlgorithmDiagnostics(
        OrthogonalCropDataSource::ImageData,
        OrthogonalCropOperation::Submit,
        geometryType,
        removalMode);

    if (!image) {
        plan.diagnostics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        plan.diagnostics.SetValidationMessage("Input image is null.");
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
    // submit mask 只表达 inside/outside 语义；
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

OrthogonalCropStatistics OrthogonalCropAlgorithm::GetStatistics(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    auto statistics = GetAlgorithmDiagnostics(
        request.GetDataSource(),
        request.GetOperation(),
        request.GetGeometryType(),
        request.GetRemovalMode());

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const bool isSubmit = request.GetOperation() == OrthogonalCropOperation::Submit;
    const bool allowPartialOverlap =
        !isSubmit || request.GetRemovalMode() == CropRemovalMode::KeepInside;
    if (!BuildCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    if (request.GetOperation() == OrthogonalCropOperation::Submit) {
        return BuildSubmitPlan(
            image,
            cropData,
            request.GetGeometryType(),
            request.GetRemovalMode(),
            GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes)).diagnostics;
    }

    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    return statistics;
}

static OrthogonalCropResult GetBoxPreviewResult(
    const CropDataModel& cropData,
    const OrthogonalCropResult& resultContext)
{
    auto result = resultContext;
    result.SetCropDataModel(cropData);

    auto statistics = GetAlgorithmDiagnostics(
        resultContext.GetResolvedDataSource(),
        OrthogonalCropOperation::Preview,
        resultContext.GetResolvedGeometryType(),
        resultContext.GetResolvedRemovalMode());
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetStatistics(statistics);
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(cropData));
    result.SetSucceeded(result.GetOutlinePolyData() != nullptr);
    result.SetFailureReason(statistics.GetFailureReason());
    if (!result.GetSucceeded()) {
        result.SetFailureReason(OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed);
        result.SetMessage("Box preview outline creation failed.");
    }
    return result;
}

static OrthogonalCropResult GetSubmitResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const OrthogonalCropResult& resultContext,
    OrthogonalCropGeometryType geometryType,
    CropRemovalMode removalMode,
    std::size_t availableRamBytes)
{
    auto result = resultContext;
    result.SetCropDataModel(cropData);

    const auto submitPlan = BuildSubmitPlan(
        image,
        cropData,
        geometryType,
        removalMode,
        availableRamBytes);
    result.SetStatistics(submitPlan.diagnostics);
    result.SetFailureReason(submitPlan.diagnostics.GetFailureReason());

    if (!submitPlan.canExecute) {
        result.SetMessage(submitPlan.diagnostics.GetValidationMessage());
        return result;
    }

    auto submitImage = GetExtractedImage(image, submitPlan.snappedIndexBounds);
    if (!submitImage) {
        result.SetFailureReason(OrthogonalCropFailureReason::SubmitImageCreationFailed);
        result.SetMessage("Failed to build image submit image.");
        return result;
    }
    SetSubmitOutsideBoxToBackground(submitImage, cropData);

    auto submitMaskImage = ::GetMaskImage(
        image,
        cropData,
        submitPlan.snappedIndexBounds,
        removalMode);
    if (!submitMaskImage) {
        result.SetFailureReason(OrthogonalCropFailureReason::SubmitMaskCreationFailed);
        result.SetMessage("Failed to build image submit mask.");
        return result;
    }

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

    result.SetSubmitImage(submitImage);
    result.SetMaskImage(submitMaskImage);
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(submitCropData));
    result.SetCropDataModel(submitCropData);
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
    const bool isSubmit = request.GetOperation() == OrthogonalCropOperation::Submit;
    // 预览允许裁切盒只与输入有交集；提交时只有 KeepInside 会截取交集范围。
    // RemoveInside submit 不使用交集语义；范围完整包含时，submit 计划再返回“不支持”。
    const bool allowPartialOverlap =
        !isSubmit || request.GetRemovalMode() == CropRemovalMode::KeepInside;
    if (!BuildCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        auto statistics = GetAlgorithmDiagnostics(
            request.GetDataSource(),
            request.GetOperation(),
            request.GetGeometryType(),
            request.GetRemovalMode());
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        result.SetStatistics(statistics);
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    if (request.GetOperation() == OrthogonalCropOperation::Preview) {
        return GetBoxPreviewResult(
            cropData,
            resultContext);
    }

    if (request.GetOperation() == OrthogonalCropOperation::Submit) {
        return GetSubmitResult(
            image,
            cropData,
            resultContext,
            request.GetGeometryType(),
            request.GetRemovalMode(),
            GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
    }

    auto fallbackResult = resultContext;
    auto diagnostics = GetAlgorithmDiagnostics(
        request.GetDataSource(),
        request.GetOperation(),
        request.GetGeometryType(),
        request.GetRemovalMode());
    diagnostics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
    diagnostics.SetValidationMessage("Image algorithm received an unsupported crop operation.");
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
    auto statistics = GetAlgorithmDiagnostics(
        request.GetDataSource(),
        request.GetOperation(),
        request.GetGeometryType(),
        request.GetRemovalMode());
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!BuildCropDataModel(polyData, request, cropData, failureReason, message, true)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    return statistics;
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
        auto statistics = GetAlgorithmDiagnostics(
            request.GetDataSource(),
            request.GetOperation(),
            request.GetGeometryType(),
            request.GetRemovalMode());
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        result.SetStatistics(statistics);
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    if (request.GetOperation() == OrthogonalCropOperation::Preview) {
        return GetBoxPreviewResult(
            cropData,
            resultContext);
    }

    auto fallbackResult = resultContext;
    auto diagnostics = GetAlgorithmDiagnostics(
        request.GetDataSource(),
        request.GetOperation(),
        request.GetGeometryType(),
        request.GetRemovalMode());
    diagnostics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
    diagnostics.SetValidationMessage("PolyData algorithm received an unsupported crop operation.");
    fallbackResult.SetCropDataModel(cropData);
    fallbackResult.SetStatistics(diagnostics);
    fallbackResult.SetFailureReason(diagnostics.GetFailureReason());
    fallbackResult.SetMessage(diagnostics.GetValidationMessage());
    fallbackResult.SetSucceeded(false);
    return fallbackResult;
}
