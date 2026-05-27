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
constexpr double BoundsEpsilon = 1e-6;

std::array<double, 6> GetImageBounds(vtkImageData* image)
{
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
    return rasBounds[1] > inputBounds[0] + BoundsEpsilon
        && rasBounds[0] < inputBounds[1] - BoundsEpsilon
        && rasBounds[3] > inputBounds[2] + BoundsEpsilon
        && rasBounds[2] < inputBounds[3] - BoundsEpsilon
        && rasBounds[5] > inputBounds[4] + BoundsEpsilon
        && rasBounds[4] < inputBounds[5] - BoundsEpsilon;
}

bool GetBoundsContained(const std::array<double, 6>& inputBounds, const std::array<double, 6>& rasBounds)
{
    return rasBounds[0] >= inputBounds[0] - BoundsEpsilon
        && rasBounds[1] <= inputBounds[1] + BoundsEpsilon
        && rasBounds[2] >= inputBounds[2] - BoundsEpsilon
        && rasBounds[3] <= inputBounds[3] + BoundsEpsilon
        && rasBounds[4] >= inputBounds[4] - BoundsEpsilon
        && rasBounds[5] <= inputBounds[5] + BoundsEpsilon;
}

std::array<double, 6> GetRasBoundsFromLocalDefinition(
    const std::array<double, 3>& localCenter,
    const std::array<double, 3>& localDimensions,
    const std::array<double, 16>& localAlignmentMatrix)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrix->SetElement(row, col, localAlignmentMatrix[row * 4 + col]);
        }
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);

    vtkBoundingBox rasBounds;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const double localPoint[3] = {
                    localCenter[0] + sx * localDimensions[0] * 0.5,
                    localCenter[1] + sy * localDimensions[1] * 0.5,
                    localCenter[2] + sz * localDimensions[2] * 0.5
                };
                double rasPoint[3] = { 0.0, 0.0, 0.0 };
                transform->TransformPoint(localPoint, rasPoint);
                rasBounds.AddPoint(rasPoint);
            }
        }
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    rasBounds.GetBounds(bounds);
    return { bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5] };
}

std::array<double, 16> GetMatrixArray(vtkMatrix4x4* matrix)
{
    if (!matrix) {
        return GetIdentityMatrixArray();
    }

    std::array<double, 16> matrixData = { 0.0 };
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrixData[row * 4 + col] = matrix->GetElement(row, col);
        }
    }
    return matrixData;
}

vtkSmartPointer<vtkTransform> GetArrayTransform(const std::array<double, 16>& matrixData, bool invert = false)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrix->SetElement(row, col, matrixData[row * 4 + col]);
        }
    }
    if (invert) {
        matrix->Invert();
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);
    return transform;
}

std::array<double, 16> GetUpdatedOffsetMatrix(
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

    return GetMatrixArray(transform->GetMatrix());
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
    if (!maskPtr) {
        return nullptr;
    }

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

    if (!cropData.GetLocalAlignmentEnabled()) {
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

vtkSmartPointer<vtkImageData> GetExtractedImage(
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

vtkSmartPointer<vtkPolyData> GetOutlinePolyDataInternal(const CropDataModel& cropData)
{
    if (cropData.GetLocalAlignmentEnabled()) {
        const auto center = cropData.GetLocalCenter();
        const auto dimensions = cropData.GetLocalDimensions();

        auto cube = vtkSmartPointer<vtkCubeSource>::New();
        cube->SetBounds(
            center[0] - dimensions[0] * 0.5,
            center[0] + dimensions[0] * 0.5,
            center[1] - dimensions[1] * 0.5,
            center[1] + dimensions[1] * 0.5,
            center[2] - dimensions[2] * 0.5,
            center[2] + dimensions[2] * 0.5);
        cube->Update();

        auto transform = GetArrayTransform(cropData.GetLocalAlignmentMatrix());
        auto transformFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        transformFilter->SetInputConnection(cube->GetOutputPort());
        transformFilter->SetTransform(transform);
        transformFilter->Update();

        auto output = vtkSmartPointer<vtkPolyData>::New();
        output->ShallowCopy(transformFilter->GetOutput());
        return output;
    }

    const auto rasBounds = cropData.GetRasBounds();
    auto outline = vtkSmartPointer<vtkOutlineSource>::New();
    outline->SetBounds(
        rasBounds[0], rasBounds[1],
        rasBounds[2], rasBounds[3],
        rasBounds[4], rasBounds[5]);
    outline->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(outline->GetOutput());
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
        cropData.SetRasBounds(GetRasBoundsFromLocalDefinition(
            request.GetLocalCenter(),
            request.GetLocalDimensions(),
            request.GetLocalAlignmentMatrix()));
        break;
    }

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
    if (!image) {
        failureReason = OrthogonalCropFailureReason::InputImageMissing;
        message = "Input image is null.";
        return false;
    }

    return GetCropDataModel(GetImageBounds(image), request, cropData, failureReason, message, allowPartialOverlap);
}

std::array<int, 6> OrthogonalCropAlgorithm::GetSnappedVoxelBounds(vtkImageData* image, const CropDataModel& cropData)
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
    const std::size_t insideVoxelCount = GetVoxelCountFromIjkBounds(snappedIjkBounds);
    const std::size_t bytesPerVoxel = GetImageBytesPerVoxel(image);

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

OrthogonalCropStatistics OrthogonalCropAlgorithm::GetStatistics(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
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
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}

OrthogonalCropResult OrthogonalCropAlgorithm::GetVirtualCropResult(
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
    result.SetVirtualMaskImage(GetVirtualMaskImage(
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

    auto derivedImage = GetExtractedImage(image, statistics.GetSnappedIjkBounds());
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
        GetUpdatedOffsetMatrix(
            cropData.GetGlobalOffsetMatrix(),
            {
                newOrigin[0] - originalOrigin[0],
                newOrigin[1] - originalOrigin[1],
                newOrigin[2] - originalOrigin[2]
            }));

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
        GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
}
