#pragma once

// =====================================================================
// OrthogonalCropAlgorithm.h — 正交裁切独立插件纯算法层
// =====================================================================
// 对齐 GapAnalysis 的职责划分：
// - 前处理：把 OrthogonalCropRequest 解析为 CropDataModel；
// - 执行：做边界校验、体素吸附、RAM 估算、虚拟遮罩或物理裁切；
// - 后处理：把派生体、outline、offset matrix 与失败原因写回 result。
//
// 这里不持有线程、DataManager、渲染节点或 UI 状态机；
// 只要求调用方提供 vtkImageData 输入与一次性请求快照。

#include "OrthogonalCropTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include <vtkCubeSource.h>
#include <vtkExtractVOI.h>
#include <vtkImageChangeInformation.h>
#include <vtkImageData.h>
#include <vtkImageMask.h>
#include <vtkMatrix4x4.h>
#include <vtkOutlineSource.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkUnsignedCharArray.h>

namespace OrthogonalCropInternal {

inline bool GetMatrixIsIdentity(const std::array<double, 16>& matrix)
{
    return matrix == GetIdentityMatrixArray();
}

inline std::array<double, 3> GetPointTransformed(
    const std::array<double, 16>& matrix,
    const std::array<double, 3>& point)
{
    auto vtkMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            vtkMatrix->SetElement(row, col, matrix[row * 4 + col]);
        }
    }

    double inPoint[4] = { point[0], point[1], point[2], 1.0 };
    double outPoint[4] = { 0.0, 0.0, 0.0, 0.0 };
    vtkMatrix->MultiplyPoint(inPoint, outPoint);
    return { outPoint[0], outPoint[1], outPoint[2] };
}

inline std::array<double, 6> GetRasBoundsFromLocalDefinition(
    const std::array<double, 3>& localCenter,
    const std::array<double, 3>& localDimensions,
    const std::array<double, 16>& localAlignmentMatrix)
{
    std::array<double, 6> rasBounds = {
        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()
    };

    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const std::array<double, 3> localPoint = {
                    localCenter[0] + (ix == 0 ? -0.5 : 0.5) * localDimensions[0],
                    localCenter[1] + (iy == 0 ? -0.5 : 0.5) * localDimensions[1],
                    localCenter[2] + (iz == 0 ? -0.5 : 0.5) * localDimensions[2]
                };
                const auto rasPoint = GetPointTransformed(localAlignmentMatrix, localPoint);
                rasBounds[0] = std::min(rasBounds[0], rasPoint[0]);
                rasBounds[1] = std::max(rasBounds[1], rasPoint[0]);
                rasBounds[2] = std::min(rasBounds[2], rasPoint[1]);
                rasBounds[3] = std::max(rasBounds[3], rasPoint[1]);
                rasBounds[4] = std::min(rasBounds[4], rasPoint[2]);
                rasBounds[5] = std::max(rasBounds[5], rasPoint[2]);
            }
        }
    }

    return rasBounds;
}

inline std::size_t GetImageVoxelCount(vtkImageData* image)
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

inline std::size_t GetImageBytesPerVoxel(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    return static_cast<std::size_t>(image->GetScalarSize())
        * static_cast<std::size_t>(image->GetNumberOfScalarComponents());
}

inline std::array<int, 6> GetSnappedIjkBounds(vtkImageData* image, const CropDataModel& cropData)
{
    std::array<int, 6> ijkBounds = { 0, 0, 0, 0, 0, 0 };
    if (!image) {
        return ijkBounds;
    }

    int dims[3] = { 0, 0, 0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    double spacing[3] = { 1.0, 1.0, 1.0 };
    image->GetDimensions(dims);
    image->GetOrigin(origin);
    image->GetSpacing(spacing);

    const auto rasBounds = cropData.GetRasBounds();
    for (int axis = 0; axis < 3; ++axis) {
        const double minCoord = rasBounds[axis * 2 + 0];
        const double maxCoord = rasBounds[axis * 2 + 1];
        const double axisSpacing = std::abs(spacing[axis]) > 1e-12 ? spacing[axis] : 1.0;
        const int minIndex = static_cast<int>(std::llround((minCoord - origin[axis]) / axisSpacing));
        const int maxIndex = static_cast<int>(std::llround((maxCoord - origin[axis]) / axisSpacing));
        ijkBounds[axis * 2 + 0] = std::clamp(std::min(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0));
        ijkBounds[axis * 2 + 1] = std::clamp(std::max(minIndex, maxIndex), 0, std::max(dims[axis] - 1, 0));
    }

    return ijkBounds;
}

inline std::size_t GetVoxelCountFromIjkBounds(const std::array<int, 6>& ijkBounds)
{
    const std::size_t sizeI = static_cast<std::size_t>(ijkBounds[1] - ijkBounds[0] + 1);
    const std::size_t sizeJ = static_cast<std::size_t>(ijkBounds[3] - ijkBounds[2] + 1);
    const std::size_t sizeK = static_cast<std::size_t>(ijkBounds[5] - ijkBounds[4] + 1);
    return sizeI * sizeJ * sizeK;
}

inline std::array<double, 16> GetUpdatedOffsetMatrix(
    const std::array<double, 16>& currentMatrix,
    const std::array<double, 3>& translation)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrix->SetElement(row, col, currentMatrix[row * 4 + col]);
        }
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->PostMultiply();
    transform->SetMatrix(matrix);
    transform->Translate(translation[0], translation[1], translation[2]);

    std::array<double, 16> updatedMatrix = { 0.0 };
    auto updatedVtkMatrix = transform->GetMatrix();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            updatedMatrix[row * 4 + col] = updatedVtkMatrix->GetElement(row, col);
        }
    }
    return updatedMatrix;
}

inline std::size_t GetEffectiveAvailableRamBytes(
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    return request.GetAvailableRamBytes() != 0
        ? request.GetAvailableRamBytes()
        : fallbackAvailableRamBytes;
}

inline vtkSmartPointer<vtkImageData> GetVirtualMaskImage(
    vtkImageData* image,
    const CropDataModel& cropData,
    const std::array<int, 6>& ijkBounds,
    CropRemovalMode removalMode,
    std::size_t* insideVoxelCount = nullptr)
{
    if (!image) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    double spacing[3] = { 1.0, 1.0, 1.0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetDimensions(dims);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);

    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    maskImage->SetDimensions(dims);
    maskImage->SetSpacing(spacing);
    maskImage->SetOrigin(origin);
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
    const unsigned char insideValue = removalMode == CropRemovalMode::KeepInside ? 255 : 0;
    const unsigned char outsideValue = removalMode == CropRemovalMode::KeepInside ? 0 : 255;
    const vtkIdType totalVoxelCount = static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
    std::memset(maskPtr, outsideValue, static_cast<std::size_t>(totalVoxelCount));

    const int minI = std::max(0, ijkBounds[0]);
    const int maxI = std::min(dims[0] - 1, ijkBounds[1]);
    const int minJ = std::max(0, ijkBounds[2]);
    const int maxJ = std::min(dims[1] - 1, ijkBounds[3]);
    const int minK = std::max(0, ijkBounds[4]);
    const int maxK = std::min(dims[2] - 1, ijkBounds[5]);
    if (minI > maxI || minJ > maxJ || minK > maxK) {
        if (insideVoxelCount) {
            *insideVoxelCount = 0;
        }
        maskImage->Modified();
        return maskImage;
    }

    const bool useLocalBounds = cropData.GetLocalAlignmentEnabled();

    if (!useLocalBounds) {
        const int width = maxI - minI + 1;
        const int height = maxJ - minJ + 1;
        const int depth = maxK - minK + 1;
        const vtkIdType rowStride = dims[0];
        const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];
        for (int k = minK; k <= maxK; ++k) {
            for (int j = minJ; j <= maxJ; ++j) {
                const vtkIdType rowStart = static_cast<vtkIdType>(k) * sliceStride
                    + static_cast<vtkIdType>(j) * rowStride
                    + minI;
                std::memset(maskPtr + rowStart, insideValue, static_cast<std::size_t>(width));
            }
        }

        if (insideVoxelCount) {
            *insideVoxelCount = static_cast<std::size_t>(width) * height * depth;
        }
        maskImage->Modified();
        return maskImage;
    }

    auto modelToLocalMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    auto localToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    const auto& localAlignmentMatrix = cropData.GetLocalAlignmentMatrix();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            localToModelMatrix->SetElement(row, col, localAlignmentMatrix[row * 4 + col]);
        }
    }
    vtkMatrix4x4::Invert(localToModelMatrix, modelToLocalMatrix);

    std::array<double, 16> modelToLocal = { 0.0 };
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            modelToLocal[row * 4 + col] = modelToLocalMatrix->GetElement(row, col);
        }
    }

    const auto localCenter = cropData.GetLocalCenter();
    const auto localDimensions = cropData.GetLocalDimensions();
    const std::array<double, 3> localHalfDimensions = {
        localDimensions[0] * 0.5,
        localDimensions[1] * 0.5,
        localDimensions[2] * 0.5
    };
    std::size_t countedInsideVoxelCount = 0;
    const vtkIdType rowStride = dims[0];
    const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];
    const double stepX = modelToLocal[0] * spacing[0];
    const double stepY = modelToLocal[4] * spacing[0];
    const double stepZ = modelToLocal[8] * spacing[0];
    const double stepW = modelToLocal[12] * spacing[0];

    for (int k = minK; k <= maxK; ++k) {
        const double modelZ = origin[2] + spacing[2] * static_cast<double>(k);
        for (int j = minJ; j <= maxJ; ++j) {
            const double modelX = origin[0] + spacing[0] * static_cast<double>(minI);
            const double modelY = origin[1] + spacing[1] * static_cast<double>(j);
            double localXRaw = modelToLocal[0] * modelX + modelToLocal[1] * modelY + modelToLocal[2] * modelZ + modelToLocal[3];
            double localYRaw = modelToLocal[4] * modelX + modelToLocal[5] * modelY + modelToLocal[6] * modelZ + modelToLocal[7];
            double localZRaw = modelToLocal[8] * modelX + modelToLocal[9] * modelY + modelToLocal[10] * modelZ + modelToLocal[11];
            double localWRaw = modelToLocal[12] * modelX + modelToLocal[13] * modelY + modelToLocal[14] * modelZ + modelToLocal[15];

            for (int i = minI; i <= maxI; ++i) {
                const double invW = std::abs(localWRaw) > 1e-12 ? 1.0 / localWRaw : 1.0;
                const double localX = localXRaw * invW;
                const double localY = localYRaw * invW;
                const double localZ = localZRaw * invW;
                const bool isInside = localX >= localCenter[0] - localHalfDimensions[0]
                    && localX <= localCenter[0] + localHalfDimensions[0]
                    && localY >= localCenter[1] - localHalfDimensions[1]
                    && localY <= localCenter[1] + localHalfDimensions[1]
                    && localZ >= localCenter[2] - localHalfDimensions[2]
                    && localZ <= localCenter[2] + localHalfDimensions[2];

                if (isInside) {
                    ++countedInsideVoxelCount;
                    const vtkIdType linearIndex = static_cast<vtkIdType>(k) * sliceStride
                        + static_cast<vtkIdType>(j) * rowStride
                        + i;
                    maskPtr[linearIndex] = insideValue;
                }

                localXRaw += stepX;
                localYRaw += stepY;
                localZRaw += stepZ;
                localWRaw += stepW;
            }
        }
    }

    maskImage->Modified();
    if (insideVoxelCount) {
        *insideVoxelCount = countedInsideVoxelCount;
    }
    return maskImage;
}

inline vtkSmartPointer<vtkImageData> GetImageLinkedToMask(
    vtkImageData* image,
    vtkImageData* maskImage,
    double maskedOutputValue = 0.0,
    bool invertMask = false)
{
    if (!image || !maskImage) {
        return nullptr;
    }

    auto maskFilter = vtkSmartPointer<vtkImageMask>::New();
    maskFilter->SetImageInputData(image);
    maskFilter->SetMaskInputData(maskImage);
    maskFilter->SetMaskedOutputValue(maskedOutputValue);
    maskFilter->SetNotMask(invertMask ? 1 : 0);
    maskFilter->Update();

    auto output = vtkSmartPointer<vtkImageData>::New();
    output->ShallowCopy(maskFilter->GetOutput());
    return output;
}

inline vtkSmartPointer<vtkImageData> GetExtractedImage(
    vtkImageData* image,
    const std::array<int, 6>& ijkBounds)
{
    if (!image) {
        return nullptr;
    }

    const int width = ijkBounds[1] - ijkBounds[0] + 1;
    const int height = ijkBounds[3] - ijkBounds[2] + 1;
    const int depth = ijkBounds[5] - ijkBounds[4] + 1;
    if (width <= 0 || height <= 0 || depth <= 0) {
        return nullptr;
    }

    double spacing[3] = { 1.0, 1.0, 1.0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetSpacing(spacing);
    image->GetOrigin(origin);

    const double outputOrigin[3] = {
        origin[0] + spacing[0] * ijkBounds[0],
        origin[1] + spacing[1] * ijkBounds[2],
        origin[2] + spacing[2] * ijkBounds[4]
    };

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

inline vtkSmartPointer<vtkImageData> GetPhysicalCroppedImage(
    vtkImageData* image,
    const std::array<int, 6>& ijkBounds)
{
    return GetExtractedImage(image, ijkBounds);
}

inline vtkSmartPointer<vtkPolyData> GetOutlinePolyData(const CropDataModel& cropData)
{
    if (cropData.GetLocalAlignmentEnabled()) {
        auto cubeSource = vtkSmartPointer<vtkCubeSource>::New();
        const auto localCenter = cropData.GetLocalCenter();
        const auto localDimensions = cropData.GetLocalDimensions();
        cubeSource->SetCenter(localCenter[0], localCenter[1], localCenter[2]);
        cubeSource->SetXLength(localDimensions[0]);
        cubeSource->SetYLength(localDimensions[1]);
        cubeSource->SetZLength(localDimensions[2]);
        cubeSource->Update();

        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        const auto& localAlignmentMatrix = cropData.GetLocalAlignmentMatrix();
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                matrix->SetElement(row, col, localAlignmentMatrix[row * 4 + col]);
            }
        }

        auto transform = vtkSmartPointer<vtkTransform>::New();
        transform->SetMatrix(matrix);

        auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        filter->SetTransform(transform);
        filter->SetInputConnection(cubeSource->GetOutputPort());
        filter->Update();
        return filter->GetOutput();
    }

    auto outlineSource = vtkSmartPointer<vtkOutlineSource>::New();
    const auto rasBounds = cropData.GetRasBounds();
    outlineSource->SetBounds(
        rasBounds[0], rasBounds[1],
        rasBounds[2], rasBounds[3],
        rasBounds[4], rasBounds[5]);
    outlineSource->Update();
    return outlineSource->GetOutput();
}

} // namespace OrthogonalCropInternal

class OrthogonalCropAlgorithm {
public:
    // 直接校验一份目标 bounds 是否既自洽，又完全落在输入 bounds 内部。
    // 这个重载不依赖具体数据对象，适合 router/polydata 路径复用统一边界判断。
    static bool GetBoundsAreValid(
        const std::array<double, 6>& inputBounds,
        const std::array<double, 6>& rasBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false)
    {
        if (!(rasBounds[0] < rasBounds[1] && rasBounds[2] < rasBounds[3] && rasBounds[4] < rasBounds[5])) {
            failureReason = OrthogonalCropFailureReason::InvalidBounds;
            message = "Crop bounds are invalid because min is not smaller than max.";
            return false;
        }

        bool overlapsInput = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (rasBounds[axis * 2 + 1] < inputBounds[axis * 2 + 0]
                || rasBounds[axis * 2 + 0] > inputBounds[axis * 2 + 1]) {
                overlapsInput = false;
            }

            if (rasBounds[axis * 2 + 0] < inputBounds[axis * 2 + 0]
                || rasBounds[axis * 2 + 1] > inputBounds[axis * 2 + 1]) {
                if (allowPartialOverlap) {
                    continue;
                }

                failureReason = OrthogonalCropFailureReason::BoundsOutOfRange;
                message = "Crop bounds exceed the source volume bounds.";
                return false;
            }
        }

        if (allowPartialOverlap && !overlapsInput) {
            failureReason = OrthogonalCropFailureReason::BoundsOutOfRange;
            message = "Crop bounds do not overlap the source volume bounds.";
            return false;
        }

        failureReason = OrthogonalCropFailureReason::None;
        message.clear();
        return true;
    }

    // 以 vtkImageData 为输入做 bounds 校验；先读原图 bounds，再复用通用重载。
    // 这是 image 路径最常用的前置保护，避免空图像或越界裁切继续往下执行。
    static bool GetBoundsAreValid(
        vtkImageData* image,
        const std::array<double, 6>& rasBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false)
    {
        if (!image) {
            failureReason = OrthogonalCropFailureReason::InputImageMissing;
            message = "Input image is null.";
            return false;
        }

        double imageBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        image->GetBounds(imageBounds);
        return GetBoundsAreValid(
            {
                imageBounds[0], imageBounds[1],
                imageBounds[2], imageBounds[3],
                imageBounds[4], imageBounds[5]
            },
            rasBounds,
            failureReason,
            message,
            allowPartialOverlap);
    }

    // 把一次 request 解析为稳定的 CropDataModel。
    // 这一步会统一处理四种 boundsMode，并把局部坐标定义最终还原成世界坐标 bounds。
    static bool GetCropDataModel(
        const std::array<double, 6>& inputBounds,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false)
    {
        cropData = CropDataModel();
        cropData.SetGlobalOffsetMatrix(request.GetGlobalOffsetMatrix());
        cropData.SetLocalAlignmentMatrix(request.GetLocalAlignmentMatrix());

        switch (request.GetBoundsMode()) {
        case CropBoundsMode::InputVolumeBounds:
            cropData.SetLocalAlignmentEnabled(false);
            cropData.SetRasBounds(inputBounds);
            cropData.SetLocalCenter({ 0.0, 0.0, 0.0 });
            cropData.SetLocalDimensions(cropData.GetDimensions());
            break;
        case CropBoundsMode::MinMaxCoordinates:
            cropData.SetLocalAlignmentEnabled(false);
            cropData.SetRasBounds(request.GetRasBounds());
            break;
        case CropBoundsMode::CenterAndDimensions:
            cropData.SetLocalAlignmentEnabled(false);
            cropData.SetCenterAndDimensions(request.GetCenter(), request.GetDimensions());
            break;
        case CropBoundsMode::LocalCenterAndDimensions:
            cropData.SetLocalAlignmentEnabled(true);
            cropData.SetLocalCenter(request.GetLocalCenter());
            cropData.SetLocalDimensions(request.GetLocalDimensions());
            cropData.SetRasBounds(OrthogonalCropInternal::GetRasBoundsFromLocalDefinition(
                request.GetLocalCenter(),
                request.GetLocalDimensions(),
                request.GetLocalAlignmentMatrix()));
            break;
        }

        return GetBoundsAreValid(inputBounds, cropData.GetRasBounds(), failureReason, message, allowPartialOverlap);
    }

    // image 路径版本的 request 归一化入口；先读取 image bounds，再复用通用重载。
    static bool GetCropDataModel(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false)
    {
        if (!image) {
            failureReason = OrthogonalCropFailureReason::InputImageMissing;
            message = "Input image is null.";
            return false;
        }

        double imageBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        image->GetBounds(imageBounds);
        return GetCropDataModel(
            {
                imageBounds[0], imageBounds[1],
                imageBounds[2], imageBounds[3],
                imageBounds[4], imageBounds[5]
            },
            request,
            cropData,
            failureReason,
            message,
            allowPartialOverlap);
    }

    // 把世界坐标裁切盒吸附到 image 的整数 IJK 边界。
    // 后面的统计、virtual mask 和 physical crop 都直接建立在这份 snapped bounds 上。
    static std::array<int, 6> GetSnappedVoxelBounds(vtkImageData* image, const CropDataModel& cropData)
    {
        return OrthogonalCropInternal::GetSnappedIjkBounds(image, cropData);
    }

    static vtkSmartPointer<vtkImageData> GetImageLinkedToMask(
        vtkImageData* image,
        vtkImageData* maskImage,
        double maskedOutputValue = 0.0,
        bool invertMask = false)
    {
        return OrthogonalCropInternal::GetImageLinkedToMask(
            image,
            maskImage,
            maskedOutputValue,
            invertMask);
    }

    static vtkSmartPointer<vtkImageData> GetExtractedImage(
        vtkImageData* image,
        const std::array<int, 6>& ijkBounds)
    {
        return OrthogonalCropInternal::GetExtractedImage(image, ijkBounds);
    }

    // 基于已经归一化好的 cropData 做纯统计估算。
    // 它不生成结果图像，只回答“inside 有多大、输出会多大、物理裁切是否允许继续做”。
    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const CropDataModel& cropData,
        CropRemovalMode removalMode,
        CropExecutionMode executionMode,
        std::size_t availableRamBytes = 0)
    {
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
        const std::size_t totalVoxelCount = OrthogonalCropInternal::GetImageVoxelCount(image);
        const std::size_t insideVoxelCount = OrthogonalCropInternal::GetVoxelCountFromIjkBounds(snappedIjkBounds);
        const std::size_t bytesPerVoxel = OrthogonalCropInternal::GetImageBytesPerVoxel(image);

        statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        statistics.SetTotalVoxelCount(totalVoxelCount);
        statistics.SetInsideVoxelCount(insideVoxelCount);
        statistics.SetSnappedIjkBounds(snappedIjkBounds);

        if (executionMode == CropExecutionMode::VirtualCrop) {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
            statistics.SetOutputVoxelCount(totalVoxelCount);
            statistics.SetEstimatedRamUsageBytes(totalVoxelCount);
            statistics.SetCanExecutePhysicalCrop(true);
            return statistics;
        }

        if (removalMode == CropRemovalMode::RemoveInside) {
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

    // 从 request 直接得到统计信息的便捷入口；内部会先把 request 解析成 cropData。
    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0)
    {
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

        return GetStatistics(
            image,
            cropData,
            request.GetRemovalMode(),
            request.GetExecutionMode(),
            OrthogonalCropInternal::GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
    }

    // 构建 virtual crop 结果：保留原图拓扑，只返回 mask、outline 和配套统计。
    // 这条路径不会生成真正脱离原图的派生体数据。
    static OrthogonalCropResult GetVirtualCropResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const CropStateModel& cropState,
        CropRemovalMode removalMode)
    {
        OrthogonalCropResult result;
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
        result.SetCropDataModel(cropData);
        result.SetCropStateModel(cropState);

        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string validationMessage;
        if (!GetBoundsAreValid(image, cropData.GetRasBounds(), failureReason, validationMessage, true)) {
            result.SetFailureReason(failureReason);
            result.SetMessage(validationMessage);
            return result;
        }

        const auto statistics = GetStatistics(image, cropData, removalMode, CropExecutionMode::VirtualCrop);
        result.SetStatistics(statistics);
        result.SetFailureReason(statistics.GetFailureReason());
        std::size_t exactInsideVoxelCount = statistics.GetInsideVoxelCount();
        result.SetVirtualMaskImage(OrthogonalCropInternal::GetVirtualMaskImage(
            image,
            cropData,
            statistics.GetSnappedIjkBounds(),
            removalMode,
            &exactInsideVoxelCount));
        if (cropData.GetLocalAlignmentEnabled()) {
            auto exactStatistics = statistics;
            exactStatistics.SetInsideVoxelCount(exactInsideVoxelCount);
            result.SetStatistics(exactStatistics);
        }
        result.SetOutlinePolyData(OrthogonalCropInternal::GetOutlinePolyData(cropData));
        result.SetSucceeded(result.GetVirtualMaskImage() != nullptr);
        if (!result.GetSucceeded()) {
            result.SetFailureReason(OrthogonalCropFailureReason::VirtualMaskCreationFailed);
            result.SetMessage("Failed to build virtual crop mask.");
        }
        return result;
    }

    // 构建 physical crop 结果：真正提取连续子体，并更新派生结果的 origin/bounds/offset matrix。
    // 只有通过统计阶段的合法性和内存判断后，才会进入这条路径。
    static OrthogonalCropResult GetPhysicalCropResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const CropStateModel& cropState,
        CropRemovalMode removalMode,
        std::size_t availableRamBytes = 0)
    {
        OrthogonalCropResult result;
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        result.SetCropDataModel(cropData);
        result.SetCropStateModel(cropState);

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

        auto derivedImage = OrthogonalCropInternal::GetPhysicalCroppedImage(
            image,
            statistics.GetSnappedIjkBounds());
        if (!derivedImage) {
            result.SetFailureReason(OrthogonalCropFailureReason::DerivedImageCreationFailed);
            result.SetMessage("Failed to build derived cropped image.");
            return result;
        }

        double originalOrigin[3] = { 0.0, 0.0, 0.0 };
        image->GetOrigin(originalOrigin);
        double newOrigin[3] = { 0.0, 0.0, 0.0 };
        derivedImage->GetOrigin(newOrigin);

        CropDataModel derivedCropData = cropData;
        double derivedBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        derivedImage->GetBounds(derivedBounds);
        derivedCropData.SetRasBounds({
            derivedBounds[0], derivedBounds[1],
            derivedBounds[2], derivedBounds[3],
            derivedBounds[4], derivedBounds[5]
        });
        derivedCropData.SetGlobalOffsetMatrix(
            OrthogonalCropInternal::GetUpdatedOffsetMatrix(
                cropData.GetGlobalOffsetMatrix(),
                {
                    newOrigin[0] - originalOrigin[0],
                    newOrigin[1] - originalOrigin[1],
                    newOrigin[2] - originalOrigin[2]
                }));

        CropStateModel derivedCropState = cropState;
        derivedCropState.SetCropEnabled(false);

        result.SetDerivedImage(derivedImage);
        result.SetOutlinePolyData(OrthogonalCropInternal::GetOutlinePolyData(derivedCropData));
        result.SetCropDataModel(derivedCropData);
        result.SetCropStateModel(derivedCropState);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        result.SetSucceeded(true);
        result.SetFailureReason(OrthogonalCropFailureReason::None);
        return result;
    }

    // image 路径总入口：先统计和校验，再根据 executionMode 分发到 virtual 或 physical 结果构建。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0)
    {
        OrthogonalCropResult result;
        result.SetCropStateModel(request.GetCropStateModel());

        const auto statistics = GetStatistics(image, request, fallbackAvailableRamBytes);
        result.SetStatistics(statistics);
        result.SetFailureReason(statistics.GetFailureReason());
        if (statistics.GetFailureReason() != OrthogonalCropFailureReason::None) {
            result.SetMessage(statistics.GetValidationMessage());
            return result;
        }

        CropDataModel cropData;
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string message;
        const bool allowPartialOverlap = request.GetExecutionMode() == CropExecutionMode::VirtualCrop;
        if (!GetCropDataModel(image, request, cropData, failureReason, message, allowPartialOverlap)) {
            result.SetFailureReason(failureReason);
            result.SetMessage(message);
            return result;
        }

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
            OrthogonalCropInternal::GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
    }
};
