// =====================================================================
// Path: MVVCVTK/src/Math/OrthogonalCrop/OrthogonalCropAlgorithm.cpp
// 分类: Math / Core Algorithm Implementation
// 说明: 负责 bounds 校验、request 归一化、虚拟 mask 生成、物理提取与统计估算。
// =====================================================================

#include "OrthogonalCrop/OrthogonalCropAlgorithm.h"

#include <vtkBoundingBox.h>
#include <vtkCubeSource.h>
#include <vtkExtractVOI.h>
#include <vtkImageChangeInformation.h>
#include <vtkMatrix4x4.h>
#include <vtkOutlineSource.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace {
// 统一的 bounds 比较容差，避免浮点误差导致边界判断抖动。
constexpr double BoundsEpsilon = 1e-6;

std::array<double, 6> GetImageBounds(vtkImageData* image)
{
    // vtkImageData::GetBounds 返回的就是 image physical 坐标范围；
    // 这里不再额外做 world/model 折叠，算法层后续所有 image 校验都直接用这套坐标语义。
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!image) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    image->GetBounds(rawBounds);
    return { rawBounds[0], rawBounds[1], rawBounds[2], rawBounds[3], rawBounds[4], rawBounds[5] };
}

bool GetBoundsHavePositiveVolume(const std::array<double, 6>& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

std::string GetBoundsMessage(const char* prefix, const std::array<double, 6>& bounds)
{
    std::ostringstream stream;
    stream << prefix << " ["
        << bounds[0] << ", " << bounds[1] << "; "
        << bounds[2] << ", " << bounds[3] << "; "
        << bounds[4] << ", " << bounds[5] << "]";
    return stream.str();
}

bool GetBoundsOverlap(const std::array<double, 6>& inputBounds, const std::array<double, 6>& rasBounds)
{
    // preview 允许 partial overlap，因此这里只判断两个轴对齐盒是否真正有体积交集。
    return rasBounds[1] > inputBounds[0] + BoundsEpsilon
        && rasBounds[0] < inputBounds[1] - BoundsEpsilon
        && rasBounds[3] > inputBounds[2] + BoundsEpsilon
        && rasBounds[2] < inputBounds[3] - BoundsEpsilon
        && rasBounds[5] > inputBounds[4] + BoundsEpsilon
        && rasBounds[4] < inputBounds[5] - BoundsEpsilon;
}

bool GetBoundsContained(const std::array<double, 6>& inputBounds, const std::array<double, 6>& rasBounds)
{
    // physical crop 需要稳定的 derived 数据范围，因此要求裁切盒完整包含在输入 bounds 内。
    return rasBounds[0] >= inputBounds[0] - BoundsEpsilon
        && rasBounds[1] <= inputBounds[1] + BoundsEpsilon
        && rasBounds[2] >= inputBounds[2] - BoundsEpsilon
        && rasBounds[3] <= inputBounds[3] + BoundsEpsilon
        && rasBounds[4] >= inputBounds[4] - BoundsEpsilon
        && rasBounds[5] <= inputBounds[5] + BoundsEpsilon;
}

std::array<double, 6> GetRasBoundsFromBoxToInputMatrix(const std::array<double, 16>& boxToInputMatrixData)
{
    // boxToInputMatrix 是标准盒 [-1,1]^3 到输入空间的唯一几何真源；
    // AABB 只通过 8 个标准角点派生，用于统一校验、IJK 吸附和统计范围。
    auto boxToInputTransform = vtkSmartPointer<vtkTransform>::New();
    boxToInputTransform->SetMatrix(boxToInputMatrixData.data());

    vtkBoundingBox rasBounds;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const double boxPoint[3] = { static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz) };
                double inputPoint[3] = { 0.0, 0.0, 0.0 };
                boxToInputTransform->TransformPoint(boxPoint, inputPoint);
                rasBounds.AddPoint(inputPoint);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    rasBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

std::array<double, 16> GetSourceToTargetMatrixArray(vtkMatrix4x4* sourceToTargetMatrix)
{
    if (!sourceToTargetMatrix) {
        return GetIdentityMatrixArray();
    }

    std::array<double, 16> sourceToTargetMatrixData = { 0.0 };
    vtkMatrix4x4::DeepCopy(sourceToTargetMatrixData.data(), sourceToTargetMatrix);
    return sourceToTargetMatrixData;
}

vtkSmartPointer<vtkTransform> GetBoxToInputTransform(const std::array<double, 16>& boxToInputMatrixData)
{
    auto boxToInputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMatrix->DeepCopy(boxToInputMatrixData.data());

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(boxToInputMatrix);
    return transform;
}

vtkSmartPointer<vtkMatrix4x4> GetInputToBoxMatrix(const CropDataModel& cropData)
{
    auto boxToInputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMatrix->DeepCopy(cropData.GetBoxToInputMatrix().data());

    auto inputToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToInputMatrix, inputToBoxMatrix);
    return inputToBoxMatrix;
}

bool GetBoxToInputIsAxisAligned(const std::array<double, 16>& boxToInputMatrixData)
{
    // 只识别无旋转/无剪切的对角矩阵快路径；任意旋转或剪切仍走标准 inputToBox 判断。
    return std::abs(boxToInputMatrixData[1]) <= BoundsEpsilon
        && std::abs(boxToInputMatrixData[2]) <= BoundsEpsilon
        && std::abs(boxToInputMatrixData[4]) <= BoundsEpsilon
        && std::abs(boxToInputMatrixData[6]) <= BoundsEpsilon
        && std::abs(boxToInputMatrixData[8]) <= BoundsEpsilon
        && std::abs(boxToInputMatrixData[9]) <= BoundsEpsilon;
}

bool GetCanonicalBoxContainsPoint(const double boxPoint[4])
{
    const double invW = std::abs(boxPoint[3]) > 1e-12 ? 1.0 / boxPoint[3] : 1.0;
    return std::abs(boxPoint[0] * invW) <= 1.0 + BoundsEpsilon
        && std::abs(boxPoint[1] * invW) <= 1.0 + BoundsEpsilon
        && std::abs(boxPoint[2] * invW) <= 1.0 + BoundsEpsilon;
}

std::array<double, 16> GetUpdatedOffsetMatrix(
    const std::array<double, 16>& sourceToTargetMatrixData,
    const std::array<double, 3>& translation)
{
    // physical crop 会改变 derived image 的 origin，
    // 这里把这次位移继续累加进 globalOffsetMatrix，保证上层共享坐标语义保持连续。
    auto sourceToTargetMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    sourceToTargetMatrix->DeepCopy(sourceToTargetMatrixData.data());

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->PostMultiply();
    transform->SetMatrix(sourceToTargetMatrix);
    transform->Translate(translation[0], translation[1], translation[2]);

    return GetSourceToTargetMatrixArray(transform->GetMatrix());
}

std::size_t GetImageVoxelCount(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    return static_cast<std::size_t>(dims[0])
        * static_cast<std::size_t>(dims[1])
        * static_cast<std::size_t>(dims[2]);
}

std::size_t GetImageBytesPerVoxel(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    return static_cast<std::size_t>(image->GetScalarSize())
        * static_cast<std::size_t>(image->GetNumberOfScalarComponents());
}

std::size_t GetVoxelCountFromIjkBounds(const std::array<int, 6>& ijkBounds)
{
    const std::size_t sizeI = static_cast<std::size_t>(ijkBounds[1] - ijkBounds[0] + 1);
    const std::size_t sizeJ = static_cast<std::size_t>(ijkBounds[3] - ijkBounds[2] + 1);
    const std::size_t sizeK = static_cast<std::size_t>(ijkBounds[5] - ijkBounds[4] + 1);
    return sizeI * sizeJ * sizeK;
}

std::size_t GetEffectiveAvailableRamBytes(
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    return request.GetAvailableRamBytes() != 0
        ? request.GetAvailableRamBytes()
        : fallbackAvailableRamBytes;
}

vtkSmartPointer<vtkImageData> GetVirtualMaskImage(
    vtkImageData* image,
    const CropDataModel& cropData,
    const std::array<int, 6>& ijkBounds,
    CropRemovalMode removalMode,
    std::size_t* insideVoxelCount = nullptr)
{
    // virtual mask 的结果不是导出新体数据，而是构造一张用于预览的 inside/outside 掩码。
    // 整体流程分两段：
    // 1. 先用 snapped IJK bounds 把执行域收缩到最小候选 AABB
    // 2. 再用 inputToBox 把体素中心归一化到标准盒 [-1,1]^3 做 inside 判定
    if (!image) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);

    const int minI = std::max(0, ijkBounds[0]);
    const int maxI = std::min(dims[0] - 1, ijkBounds[1]);
    const int minJ = std::max(0, ijkBounds[2]);
    const int maxJ = std::min(dims[1] - 1, ijkBounds[3]);
    const int minK = std::max(0, ijkBounds[4]);
    const int maxK = std::min(dims[2] - 1, ijkBounds[5]);
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
        if (insideVoxelCount) {
            *insideVoxelCount = 0;
        }
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

    if (GetBoxToInputIsAxisAligned(cropData.GetBoxToInputMatrix())) {
        // 标准盒矩阵退化为输入空间轴对齐盒时，snapped AABB 内部整块都属于 inside。
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

        if (insideVoxelCount) {
            *insideVoxelCount = static_cast<std::size_t>(width) * height * depth;
        }
        maskImage->Modified();
        return maskImage;
    }

    auto inputToBoxMatrix = GetInputToBoxMatrix(cropData);

    // 旋转/缩放盒的真实 inside 由“体素中心 -> 输入 physical 空间 -> 标准盒空间”决定。
    // 这里仍旧只在 snapped AABB 范围内遍历，避免为了旋转盒额外扩张执行域。
    std::size_t countedInsideVoxelCount = 0;
    const vtkIdType rowStride = useCompactMask ? width : dims[0];
    const vtkIdType sliceStride = useCompactMask
        ? static_cast<vtkIdType>(width) * height
        : static_cast<vtkIdType>(dims[0]) * dims[1];
    auto indexToPhysicalMatrix = image->GetIndexToPhysicalMatrix();
    const double inputToBoxStepVector[4] = {
        indexToPhysicalMatrix->GetElement(0, 0),
        indexToPhysicalMatrix->GetElement(1, 0),
        indexToPhysicalMatrix->GetElement(2, 0),
        0.0
    };
    double inputToBoxStepResult[4] = { 0.0, 0.0, 0.0, 0.0 };
    inputToBoxMatrix->MultiplyPoint(inputToBoxStepVector, inputToBoxStepResult);

    for (int k = minK; k <= maxK; ++k) {
        for (int j = minJ; j <= maxJ; ++j) {
            const int rowStartIndex[3] = { minI, j, k };
            double inputPoint[3] = { 0.0, 0.0, 0.0 };
            // 每一行先把 row 起点 index 转到输入 physical 空间，
            // 再整体乘 inputToBox；后续沿 i 方向只做增量推进，避免每个点都重乘矩阵。
            image->TransformIndexToPhysicalPoint(rowStartIndex, inputPoint);
            const double inputPoint4[4] = { inputPoint[0], inputPoint[1], inputPoint[2], 1.0 };
            double boxPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
            inputToBoxMatrix->MultiplyPoint(inputPoint4, boxPoint);
            
            for (int i = minI; i <= maxI; ++i) {
                const bool isInside = GetCanonicalBoxContainsPoint(boxPoint);

                if (isInside) {
                    ++countedInsideVoxelCount;
                    const vtkIdType linearIndex = useCompactMask
                        ? static_cast<vtkIdType>(k - minK) * sliceStride
                            + static_cast<vtkIdType>(j - minJ) * rowStride
                            + (i - minI)
                        : static_cast<vtkIdType>(k) * sliceStride
                            + static_cast<vtkIdType>(j) * rowStride
                            + i;
                    maskPtr[linearIndex] = insideValue; // 初始化
                }

                boxPoint[0] += inputToBoxStepResult[0];
                boxPoint[1] += inputToBoxStepResult[1];
                boxPoint[2] += inputToBoxStepResult[2];
                boxPoint[3] += inputToBoxStepResult[3];
            }
        }
    }

    maskImage->Modified();
    if (insideVoxelCount) {
        *insideVoxelCount = countedInsideVoxelCount;
    }
    return maskImage;
}

vtkSmartPointer<vtkImageData> GetExtractedImage(
    vtkImageData* image,
    const std::array<int, 6>& ijkBounds)
{
    // physical crop 的目标是导出真正独立的 derived image。
    // 这里先按 snapped IJK 做 vtkExtractVOI，再把 extent 起点归零，
    // 同时把新的 physical origin 设置成输出块起点对应的 physical 坐标。
    if (!image) {
        return nullptr;
    }

    const int width = ijkBounds[1] - ijkBounds[0] + 1;
    const int height = ijkBounds[3] - ijkBounds[2] + 1;
    const int depth = ijkBounds[5] - ijkBounds[4] + 1;
    if (width <= 0 || height <= 0 || depth <= 0) {
        return nullptr;
    }

    const int outputStartIndex[3] = { ijkBounds[0], ijkBounds[2], ijkBounds[4] };
    double outputOrigin[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(outputStartIndex, outputOrigin);

    // 这里让 vtkExtractVOI 只负责提取体素子块，
    // vtkImageChangeInformation 再只修正输出 extent/origin，不改真实体素内容。
    auto extract = vtkSmartPointer<vtkExtractVOI>::New();
    extract->SetInputData(image);
    extract->SetVOI(
        ijkBounds[0], ijkBounds[1],
        ijkBounds[2], ijkBounds[3],
        ijkBounds[4], ijkBounds[5]);

    auto normalizeInformation = vtkSmartPointer<vtkImageChangeInformation>::New();
    normalizeInformation->SetInputConnection(extract->GetOutputPort());
    normalizeInformation->SetOutputExtentStart(0, 0, 0);
    normalizeInformation->SetOutputOrigin(outputOrigin);
    normalizeInformation->Update();

    auto output = vtkSmartPointer<vtkImageData>::New();
    output->ShallowCopy(normalizeInformation->GetOutput());
    return output;
}

vtkSmartPointer<vtkPolyData> GetOutlinePolyDataInternal(const CropDataModel& cropData)
{
    // outline 的几何真源同样是标准盒 [-1,1]^3 + boxToInputMatrix。
    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    const auto canonicalBounds = GetCanonicalCropBoxBounds();
    cube->SetBounds(
        canonicalBounds[0], canonicalBounds[1],
        canonicalBounds[2], canonicalBounds[3],
        canonicalBounds[4], canonicalBounds[5]);
    cube->Update();

    auto boxToInputTransform = GetBoxToInputTransform(cropData.GetBoxToInputMatrix());
    auto transformFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    transformFilter->SetInputConnection(cube->GetOutputPort());
    transformFilter->SetTransform(boxToInputTransform);
    transformFilter->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(transformFilter->GetOutput());
    return output;
}
} // namespace

bool OrthogonalCropAlgorithm::GetBoundsAreValid(
    const std::array<double, 6>& inputBounds,
    const std::array<double, 6>& rasBounds,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // allowPartialOverlap 只在 virtual crop 中启用，表示 preview 允许盒子部分超出输入范围，
    // 但 physical crop 仍要求完整包含，避免导出的 derived image 语义不稳定。
    // 这里默认调用方已经把 inputBounds 和 crop bounds 放到了同一坐标系里，不再做二次折叠。
    if (!GetBoundsHavePositiveVolume(inputBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = GetBoundsMessage("Input bounds are invalid:", inputBounds);
        return false;
    }

    if (!GetBoundsHavePositiveVolume(rasBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = GetBoundsMessage("Crop bounds are invalid:", rasBounds);
        return false;
    }

    const bool inRange = allowPartialOverlap
        ? GetBoundsOverlap(inputBounds, rasBounds)
        : GetBoundsContained(inputBounds, rasBounds);
    if (!inRange) {
        failureReason = OrthogonalCropFailureReason::BoundsOutOfRange;
        message = allowPartialOverlap
            ? "Crop bounds do not overlap the active input bounds."
            : "Crop bounds exceed the active input bounds.";
        return false;
    }

    failureReason = OrthogonalCropFailureReason::None;
    message.clear();
    return true;
}

bool OrthogonalCropAlgorithm::GetBoundsAreValid(
    vtkImageData* image,
    const std::array<double, 6>& rasBounds,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // image 重载只是把 vtkImageData 的 physical bounds 提出来，再复用统一的 bounds 判定逻辑。
    if (!image) {
        failureReason = OrthogonalCropFailureReason::InputImageMissing;
        message = "Input image is null.";
        return false;
    }

    return GetBoundsAreValid(GetImageBounds(image), rasBounds, failureReason, message, allowPartialOverlap);
}

bool OrthogonalCropAlgorithm::GetCropDataModel(
    const std::array<double, 6>& inputBounds,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // request 的几何真源只有 boxToInputMatrix；cropData 只额外缓存由它派生出的输入空间 AABB。
    cropData = CropDataModel();
    cropData.SetGlobalOffsetMatrix(request.GetGlobalOffsetMatrix());
    cropData.SetBoxToInputMatrix(request.GetBoxToInputMatrix());
    cropData.SetRasBounds(GetRasBoundsFromBoxToInputMatrix(request.GetBoxToInputMatrix()));

    return GetBoundsAreValid(inputBounds, cropData.GetRasBounds(), failureReason, message, allowPartialOverlap);
}

bool OrthogonalCropAlgorithm::GetCropDataModel(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message,
    bool allowPartialOverlap)
{
    // image 重载只负责把 vtkImageData 的 physical bounds 作为输入 bounds 传下去；
    // 真正的 request -> cropData 归一化仍由通用重载完成。
    if (!image) {
        failureReason = OrthogonalCropFailureReason::InputImageMissing;
        message = "Input image is null.";
        return false;
    }

    return GetCropDataModel(GetImageBounds(image), request, cropData, failureReason, message, allowPartialOverlap);
}

std::array<int, 6> OrthogonalCropAlgorithm::GetSnappedVoxelBounds(vtkImageData* image, const CropDataModel& cropData)
{
    // image 路径最终都要落到体素级执行，因此先把已经折叠回 image physical/model 空间的 bounds
    // 通过 vtkImageData 原生的 physical->continuous-index 变换吸附到 IJK 整数区间。
    // 这里显式变换 8 个角点，避免 direction 非单位矩阵时只按各轴独立换算导致包围盒失真。
    // 最终结果语义是一个“完整覆盖 crop bounds 的整数 IJK 包围盒”，供统计和执行路径共用。
    std::array<int, 6> ijkBounds = { 0, 0, 0, 0, 0, 0 };
    if (!image) {
        return ijkBounds;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);

    const auto rasBounds = cropData.GetRasBounds();
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
                const double physicalPoint[3] = {
                    rasBounds[sx == 0 ? 0 : 1],
                    rasBounds[sy == 0 ? 2 : 3],
                    rasBounds[sz == 0 ? 4 : 5]
                };
				// 物理模型坐标系下点rasBounds 落在 IJK 里的哪个子区间
                double continuousIndex[3] = { 0.0, 0.0, 0.0 };
                image->TransformPhysicalPointToContinuousIndex(physicalPoint, continuousIndex);
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
        ijkBounds[axis * 2 + 0] = std::clamp(std::min(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0)); // min
        ijkBounds[axis * 2 + 1] = std::clamp(std::max(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0)); // max
    }

    return ijkBounds;
}

vtkSmartPointer<vtkPolyData> OrthogonalCropAlgorithm::GetOutlinePolyData(const CropDataModel& cropData)
{
    return GetOutlinePolyDataInternal(cropData);
}

OrthogonalCropStatistics OrthogonalCropAlgorithm::GetStatistics(
    vtkImageData* image,
    const CropDataModel& cropData,
    CropRemovalMode removalMode,
    CropExecutionMode executionMode,
    std::size_t availableRamBytes)
{
    // 统计核心只做三件事：
    // 1. 确认 crop bounds 在输入 physical 坐标范围内是否合法
    // 2. 把 crop bounds 吸附成 snapped IJK 包围盒
    // 3. 按 execution mode 推导 inside/output 规模和物理执行可行性
    OrthogonalCropStatistics statistics;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string validationMessage;
    if (!GetBoundsAreValid(
        image,
        cropData.GetRasBounds(),
        failureReason,
        validationMessage,
        executionMode == CropExecutionMode::VirtualCrop)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(validationMessage);
        return statistics;
    }

    const auto snappedIjkBounds = GetSnappedVoxelBounds(image, cropData);
    const std::size_t totalVoxelCount = GetImageVoxelCount(image);
    std::size_t insideVoxelCount = GetVoxelCountFromIjkBounds(snappedIjkBounds);
    const std::size_t bytesPerVoxel = GetImageBytesPerVoxel(image);

    if (executionMode == CropExecutionMode::VirtualCrop
        && !GetBoxToInputIsAxisAligned(cropData.GetBoxToInputMatrix())) {
        // 对旋转/缩放盒，virtual crop 的 inside 语义应以真实标准盒体素数为准，
        // 不能继续沿用 snapped AABB 的包围盒体素数，否则 GetStatistics 与 GetResult 的统计会分叉。
        int dims[3] = { 0, 0, 0 };
        image->GetDimensions(dims);

        const int minI = std::max(0, snappedIjkBounds[0]);
        const int maxI = std::min(dims[0] - 1, snappedIjkBounds[1]);
        const int minJ = std::max(0, snappedIjkBounds[2]);
        const int maxJ = std::min(dims[1] - 1, snappedIjkBounds[3]);
        const int minK = std::max(0, snappedIjkBounds[4]);
        const int maxK = std::min(dims[2] - 1, snappedIjkBounds[5]);
        if (minI > maxI || minJ > maxJ || minK > maxK) {
            insideVoxelCount = 0;
        }
        else {
            auto inputToBoxMatrix = GetInputToBoxMatrix(cropData);

            insideVoxelCount = 0;
            auto indexToPhysicalMatrix = image->GetIndexToPhysicalMatrix();
            const double inputToBoxStepVector[4] = {
                indexToPhysicalMatrix->GetElement(0, 0),
                indexToPhysicalMatrix->GetElement(1, 0),
                indexToPhysicalMatrix->GetElement(2, 0),
                0.0
            };
            double inputToBoxStepResult[4] = { 0.0, 0.0, 0.0, 0.0 };
            inputToBoxMatrix->MultiplyPoint(inputToBoxStepVector, inputToBoxStepResult);

            for (int k = minK; k <= maxK; ++k) {
                for (int j = minJ; j <= maxJ; ++j) {
                    const int rowStartIndex[3] = { minI, j, k };
                    double inputPoint[3] = { 0.0, 0.0, 0.0 };
                    image->TransformIndexToPhysicalPoint(rowStartIndex, inputPoint);
                    const double inputPoint4[4] = { inputPoint[0], inputPoint[1], inputPoint[2], 1.0 };
                    double boxPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
                    inputToBoxMatrix->MultiplyPoint(inputPoint4, boxPoint);

                    for (int i = minI; i <= maxI; ++i) {
                        const bool isInside = GetCanonicalBoxContainsPoint(boxPoint);

                        if (isInside) {
                            ++insideVoxelCount;
                        }

                        boxPoint[0] += inputToBoxStepResult[0];
                        boxPoint[1] += inputToBoxStepResult[1];
                        boxPoint[2] += inputToBoxStepResult[2];
                        boxPoint[3] += inputToBoxStepResult[3];
                    }
                }
            }
        }
    }

    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    statistics.SetTotalVoxelCount(totalVoxelCount);
    statistics.SetInsideVoxelCount(insideVoxelCount);
    statistics.SetSnappedIjkBounds(snappedIjkBounds);

    if (executionMode == CropExecutionMode::VirtualCrop) {
        // virtual crop 会按 removal mode 决定 mask 粒度：
        // - KeepInside：只分配 snapped ROI 大小的 mask
        // - RemoveInside：仍保留 full-volume mask，以表达盒外补集
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
        const std::size_t virtualMaskVoxelCount = removalMode == CropRemovalMode::KeepInside
            ? GetVoxelCountFromIjkBounds(snappedIjkBounds)
            : totalVoxelCount;
        statistics.SetOutputVoxelCount(virtualMaskVoxelCount);
        statistics.SetEstimatedRamUsageBytes(virtualMaskVoxelCount);
        statistics.SetCanExecutePhysicalCrop(true);
        return statistics;
    }

    if (removalMode == CropRemovalMode::RemoveInside) {
        // 物理 RemoveInside 不做重采样补洞，因此明确维持“不支持”的旧约束。
        statistics.SetFailureReason(OrthogonalCropFailureReason::PhysicalRemoveInsideUnsupported);
        statistics.SetValidationMessage(
            "Physical crop with RemoveInside is intentionally blocked because it cannot preserve a contiguous derived volume without resampling.");
        statistics.SetCanExecutePhysicalCrop(false);
        return statistics;
    }

    const std::size_t outputVoxelCount = insideVoxelCount;
    const std::size_t estimatedRamUsageBytes = outputVoxelCount * bytesPerVoxel;
    statistics.SetOutputVoxelCount(outputVoxelCount);
    statistics.SetEstimatedRamUsageBytes(estimatedRamUsageBytes);
    const bool canExecute = availableRamBytes == 0 || estimatedRamUsageBytes <= availableRamBytes;
    statistics.SetCanExecutePhysicalCrop(canExecute);
    if (!canExecute) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::InsufficientRam);
        statistics.SetValidationMessage("Estimated physical crop memory usage exceeds currently available RAM.");
    }

    return statistics;
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
    const bool allowPartialOverlap = request.GetExecutionMode() == CropExecutionMode::VirtualCrop;
    if (!GetCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    // 对外暴露的 request 入口最终先归一化，再转入 cropData 版本的统计核心。
    return GetStatistics(
        image,
        cropData,
        request.GetRemovalMode(),
        request.GetExecutionMode(),
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}

OrthogonalCropResult OrthogonalCropAlgorithm::GetVirtualCropResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const CropStateModel& cropState,
    CropRemovalMode removalMode)
{
    // ── 步骤 1：构造基本结果骨架 ──
    // 预先填充数据源/后端标记、几何快照与交互状态
    // 后续 steps 逐步回填 mask / statistics / outline
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
    result.SetCropDataModel(cropData);
    result.SetCropStateModel(cropState);

    // ── 步骤 2：校验 bounds 合法性 ──
    // allowPartialOverlap=true，预览允许盒子部分超出输入边界
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string validationMessage;
    if (!GetBoundsAreValid(image, cropData.GetRasBounds(), failureReason, validationMessage, true)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(validationMessage);
        return result;
    }
    // 8个ras模型坐标角点转换为六维盒边界，只需要六维盒边界即可表示出来八个角点
    const auto snappedIjkBounds = GetSnappedVoxelBounds(image, cropData);
    const std::size_t totalVoxelCount = GetImageVoxelCount(image);
    std::size_t insideVoxelCount = 0;

    // ── 步骤 3a：生成 mask ──
    // KeepInside  → ROI 大小 compact mask（memset 快路径）
    // RemoveInside → full-volume mask（标记盒外区域）
    result.SetVirtualMaskImage(::GetVirtualMaskImage(
        image,
        cropData,
        snappedIjkBounds,
        removalMode,
        &insideVoxelCount));

    // ── 步骤 3b：回填 statistics ──
    // 包含 total/inside/output voxel 计数、snapped IJK 边界、RAM 估算
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    statistics.SetTotalVoxelCount(totalVoxelCount);
    statistics.SetInsideVoxelCount(insideVoxelCount);
    const std::size_t virtualMaskVoxelCount = removalMode == CropRemovalMode::KeepInside
        ? GetVoxelCountFromIjkBounds(snappedIjkBounds)
        : totalVoxelCount;
    statistics.SetOutputVoxelCount(virtualMaskVoxelCount);
    statistics.SetEstimatedRamUsageBytes(virtualMaskVoxelCount);
    statistics.SetSnappedIjkBounds(snappedIjkBounds);
    statistics.SetCanExecutePhysicalCrop(true);
    result.SetStatistics(statistics);
    result.SetFailureReason(statistics.GetFailureReason());

    // ── 步骤 3c：回填 outline 与最终状态 ──
    // outline 落在后端输入坐标系下，供 overlay 直接消费
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(cropData));
    result.SetSucceeded(result.GetVirtualMaskImage() != nullptr);
    if (!result.GetSucceeded()) {
        result.SetFailureReason(OrthogonalCropFailureReason::VirtualMaskCreationFailed);
        result.SetMessage("Failed to build virtual crop mask.");
    }
    return result;
}

OrthogonalCropResult OrthogonalCropAlgorithm::GetPhysicalCropResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const CropStateModel& cropState,
    CropRemovalMode removalMode,
    std::size_t availableRamBytes)
{
    // physical crop 返回的是新的 derived image，
    // 因此除了提取体素数据，还必须同步修正 bounds 和 globalOffsetMatrix。
    // 结果说明：derived image 是新的主数据快照，derivedCropData 记录它自己的 physical bounds 与位移补偿。
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    result.SetCropDataModel(cropData);
    result.SetCropStateModel(cropState);

    // ── 步骤 1：统计预估与可行性判断 ──
    // 综合 removal mode / RAM 约束判断是否能安全执行 physical crop
    const auto statistics = GetStatistics(
        image,
        cropData,
        removalMode,
        CropExecutionMode::PhysicalCrop,
        availableRamBytes);
    result.SetStatistics(statistics);
    result.SetFailureReason(statistics.GetFailureReason());

    if (!statistics.GetCanExecutePhysicalCrop()) {
        result.SetMessage(statistics.GetValidationMessage());
        return result;
    }

    // ── 步骤 2：体素提取 ──
    // 通过 ExtractVOI → ImageChangeInformation 产出独立 derived image
    auto derivedImage = GetExtractedImage(image, statistics.GetSnappedIjkBounds());
    if (!derivedImage) {
        result.SetFailureReason(OrthogonalCropFailureReason::DerivedImageCreationFailed);
        result.SetMessage("Failed to build derived cropped image.");
        return result;
    }

    // ── 步骤 3：偏移补偿 ──
    // 提取后 origin 改变，把位移量累加进 globalOffsetMatrix
    // 保证上层共享坐标语义连续
    double originalOrigin[3] = { 0.0, 0.0, 0.0 };
    image->GetOrigin(originalOrigin);
    double newOrigin[3] = { 0.0, 0.0, 0.0 };
    derivedImage->GetOrigin(newOrigin);

    CropDataModel derivedCropData = cropData;
    double derivedBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    derivedImage->GetBounds(derivedBounds);
    const CropBoundsDouble6Array derivedRasBounds = {
        derivedBounds[0], derivedBounds[1],
        derivedBounds[2], derivedBounds[3],
        derivedBounds[4], derivedBounds[5]
    };
    derivedCropData.SetBoxToInputMatrixFromBounds(derivedRasBounds);
    derivedCropData.SetRasBounds(derivedRasBounds);
    derivedCropData.SetGlobalOffsetMatrix(
        GetUpdatedOffsetMatrix(
            cropData.GetGlobalOffsetMatrix(),
            {
                newOrigin[0] - originalOrigin[0],
                newOrigin[1] - originalOrigin[1],
                newOrigin[2] - originalOrigin[2]
            }));

    // ── 步骤 4：结果回填 ──
    // derived image 本身就是新主数据，关闭交互裁切语义
    CropStateModel derivedCropState = cropState;
    derivedCropState.SetCropEnabled(false);

    result.SetDerivedImage(derivedImage);
    result.SetOutlinePolyData(GetOutlinePolyDataInternal(derivedCropData));
    result.SetCropDataModel(derivedCropData);
    result.SetCropStateModel(derivedCropState);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
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
    // request 只携带 boxToInputMatrix；cropData 额外派生输入空间 AABB 供后端执行。
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const bool allowPartialOverlap = request.GetExecutionMode() == CropExecutionMode::VirtualCrop;
    if (!GetCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    // ── 执行分支分发：VirtualCrop vs PhysicalCrop ──
    // VirtualCrop  → GetVirtualCropResult  → mask + outline + statistics（预览用，不复制体数据）
    // PhysicalCrop → GetPhysicalCropResult → derived image + globalOffsetMatrix（可独立使用的新数据）
    if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
        return GetVirtualCropResult(
            image,
            cropData,
            request.GetCropStateModel(),
            request.GetRemovalMode());
    }

    return GetPhysicalCropResult(
        image,
        cropData,
        request.GetCropStateModel(),
        request.GetRemovalMode(),
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}
