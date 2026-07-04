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

static constexpr double PlaneEpsilon = 1e-8;

static CropBoundsDouble6Array GetImageModelBounds(vtkImageData* image)
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

static CropBoundsDouble6Array GetPolyDataInputModelBounds(vtkPolyData* polyData)
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

static bool GetBoundsHavePositiveVolume(const CropBoundsDouble6Array& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

static std::size_t GetImageBytesPerVoxel(vtkImageData* image)
{
    if (!image) {
        return 0;
    }

    return static_cast<std::size_t>(image->GetScalarSize())
        * static_cast<std::size_t>(image->GetNumberOfScalarComponents());
}

static std::size_t GetImageVoxelCount(vtkImageData* image)
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

static std::size_t GetEffectiveAvailableRamBytes(
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    return request.GetAvailableRamBytes() != 0
        ? request.GetAvailableRamBytes()
        : fallbackAvailableRamBytes;
}

static OrthogonalCropStatistics GetPlanarDiagnostics(const OrthogonalCropRequest& request)
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(request.GetDataSource());
    statistics.SetResolvedOperation(request.GetOperation());
    statistics.SetResolvedGeometryType(request.GetGeometryType());
    statistics.SetResolvedRemovalMode(request.GetRemovalMode());
    return statistics;
}

static OrthogonalCropResult GetPlanarResultFromRequest(const OrthogonalCropRequest& request)
{
    OrthogonalCropResult result;
    result.SetResolvedDataSource(request.GetDataSource());
    result.SetResolvedOperation(request.GetOperation());
    result.SetResolvedGeometryType(request.GetGeometryType());
    result.SetResolvedRemovalMode(request.GetRemovalMode());
    return result;
}

static bool GetNormalizedPlane(
    const OrthogonalCropRequest& request,
    CropVectorDouble3Array& planeNormalInInputModel,
    CropVectorDouble3Array& planeCenterInInputModel,
    OrthogonalCropFailureReason& failureReason,
    std::string& message)
{
    planeNormalInInputModel = request.GetPlaneNormalInInputModel();
    planeCenterInInputModel = request.GetPlaneCenterInInputModel();

    const double length = vtkMath::Norm(planeNormalInInputModel.data());
    if (length <= PlaneEpsilon) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
        message = "Plane normal length is too small.";
        return false;
    }

    for (double& component : planeNormalInInputModel) {
        component /= length;
    }

    failureReason = OrthogonalCropFailureReason::None;
    message.clear();
    return true;
}

static CropDataModel GetPlaneCropDataModel(
    const CropVectorDouble3Array& planeNormalInInputModel,
    const CropVectorDouble3Array& planeCenterInInputModel,
    const std::array<double, 2>& planeHalfExtentsInInputModel,
    const CropBoundsDouble6Array& inputModelBounds)
{
    CropDataModel cropData;
    cropData.SetPlaneNormalInInputModel(planeNormalInInputModel);
    cropData.SetPlaneCenterInInputModel(planeCenterInInputModel);
    cropData.SetPlaneHalfExtentsInInputModel(planeHalfExtentsInInputModel);
    cropData.SetInputModelBounds(inputModelBounds);
    return cropData;
}

static bool GetPlaneCropDataModel(
    const OrthogonalCropRequest& request,
    const CropBoundsDouble6Array& inputModelBounds,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message)
{
    if (!GetBoundsHavePositiveVolume(inputModelBounds)) {
        failureReason = OrthogonalCropFailureReason::InvalidBounds;
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

    cropData = GetPlaneCropDataModel(
        planeNormalInInputModel,
        planeCenterInInputModel,
        request.GetPlaneHalfExtentsInInputModel(),
        inputModelBounds);
    return true;
}

static bool GetPointIsOnNormalSide(
    const double inputModelPoint[3],
    const CropVectorDouble3Array& planeCenterInInputModel,
    const CropVectorDouble3Array& planeNormalInInputModel)
{
    const double offset[3] = {
        inputModelPoint[0] - planeCenterInInputModel[0],
        inputModelPoint[1] - planeCenterInInputModel[1],
        inputModelPoint[2] - planeCenterInInputModel[2]
    };

    return vtkMath::Dot(offset, planeNormalInInputModel.data()) > 0.0;
}

struct PlanarVoxelSideStep {
    std::array<double, 3> planeCenterContinuousIndex = { 0.0, 0.0, 0.0 };
    double iStep = 0.0;
    double jStep = 0.0;
    double kStep = 0.0;
    double boundaryEpsilon = PlaneEpsilon;
};

struct PlanarSubmitImages {
    vtkSmartPointer<vtkImageData> submitImage;
    vtkSmartPointer<vtkImageData> maskImage;
};

enum class PlanarRowSide {
    NormalSide,
    OppositeSide,
    Mixed
};

static OrthogonalCropResult GetFailureResult(
    const OrthogonalCropRequest& request,
    OrthogonalCropFailureReason failureReason,
    const std::string& message,
    const CropDataModel* cropData = nullptr)
{
    auto result = GetPlanarResultFromRequest(request);
    auto statistics = GetPlanarDiagnostics(request);
    statistics.SetFailureReason(failureReason);
    statistics.SetValidationMessage(message);
    result.SetStatistics(statistics);
    result.SetFailureReason(failureReason);
    result.SetMessage(message);
    if (cropData) {
        result.SetCropDataModel(*cropData);
    }
    result.SetSucceeded(false);
    return result;
}

static PlanarVoxelSideStep GetPlanarVoxelSideStep(
    vtkImageData* image,
    const CropDataModel& cropData,
    const int dims[3])
{
    PlanarVoxelSideStep sideStep;
    if (!image) {
        return sideStep;
    }

    const auto planeCenterInInputModel = cropData.GetPlaneCenterInInputModel();
    image->TransformPhysicalPointToContinuousIndex(
        planeCenterInInputModel.data(),
        sideStep.planeCenterContinuousIndex.data());

    const auto planeNormalInInputModel = cropData.GetPlaneNormalInInputModel();
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

static double GetPlanarVoxelSideAtIndex(
    const PlanarVoxelSideStep& sideStep,
    int i,
    int j,
    int k)
{
    return (static_cast<double>(i) - sideStep.planeCenterContinuousIndex[0]) * sideStep.iStep
        + (static_cast<double>(j) - sideStep.planeCenterContinuousIndex[1]) * sideStep.jStep
        + (static_cast<double>(k) - sideStep.planeCenterContinuousIndex[2]) * sideStep.kStep;
}

static bool GetPlanarVoxelIsOnNormalSide(
    vtkImageData* image,
    const PlanarVoxelSideStep& sideStep,
    const CropDataModel& cropData,
    const int index[3],
    double planeSide)
{
    if (std::abs(planeSide) > sideStep.boundaryEpsilon) {
        return planeSide > 0.0;
    }

    double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(index, inputModelPoint);
    return GetPointIsOnNormalSide(
        inputModelPoint,
        cropData.GetPlaneCenterInInputModel(),
        cropData.GetPlaneNormalInInputModel());
}

static PlanarRowSide GetPlanarRowSide(
    double rowStartSide,
    double rowEndSide,
    double boundaryEpsilon)
{
    const double minSide = std::min(rowStartSide, rowEndSide);
    const double maxSide = std::max(rowStartSide, rowEndSide);
    if (minSide > boundaryEpsilon) {
        return PlanarRowSide::NormalSide;
    }
    if (maxSide < -boundaryEpsilon) {
        return PlanarRowSide::OppositeSide;
    }
    return PlanarRowSide::Mixed;
}

static std::vector<unsigned char> GetPlanarBackgroundVoxelBytes(
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

static void SetPlanarSubmitRowToBackground(
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

static void SetPlanarSubmitRowBytes(
    unsigned char* maskRowPtr,
    unsigned char* submitRowPtr,
    const unsigned char* sourceRowPtr,
    const std::vector<unsigned char>& backgroundVoxelBytes,
    std::size_t rowBytes,
    int voxelCount,
    bool keepRow)
{
    const auto maskBytes = static_cast<std::size_t>(voxelCount);
    if (keepRow) {
        std::memset(maskRowPtr, 255, maskBytes);
        std::memcpy(submitRowPtr, sourceRowPtr, rowBytes);
        return;
    }

    std::memset(maskRowPtr, 0, maskBytes);
    SetPlanarSubmitRowToBackground(
        submitRowPtr,
        backgroundVoxelBytes,
        voxelCount);
}

static void SetPlanarSubmitVoxelBytes(
    unsigned char* maskPtr,
    unsigned char* submitPtr,
    const unsigned char* sourcePtr,
    const std::vector<unsigned char>& backgroundVoxelBytes,
    std::size_t bytesPerVoxel,
    bool keepVoxel)
{
    if (keepVoxel) {
        *maskPtr = 255;
        std::memcpy(submitPtr, sourcePtr, bytesPerVoxel);
        return;
    }

    *maskPtr = 0;
    if (!backgroundVoxelBytes.empty()) {
        std::memcpy(submitPtr, backgroundVoxelBytes.data(), bytesPerVoxel);
    }
}

static PlanarSubmitImages GetPlanarSubmitImages(
    vtkImageData* image,
    const CropDataModel& cropData,
    CropRemovalMode removalMode)
{
    PlanarSubmitImages images;
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

    const auto backgroundVoxelBytes = GetPlanarBackgroundVoxelBytes(
        sourceScalars,
        submitScalars,
        submitBytes,
        componentCount,
        bytesPerVoxel);
    const bool keepInside = removalMode == CropRemovalMode::KeepInside;
    const auto sideStep = GetPlanarVoxelSideStep(image, cropData, dims);
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

            const double rowStartSide = GetPlanarVoxelSideAtIndex(sideStep, extent[0], j, k);
            const double rowEndSide = rowStartSide
                + static_cast<double>(dims[0] - 1) * sideStep.iStep;
            const auto rowSide = GetPlanarRowSide(
                rowStartSide,
                rowEndSide,
                sideStep.boundaryEpsilon);
            if (rowSide != PlanarRowSide::Mixed) {
                const bool rowIsInside = rowSide == PlanarRowSide::NormalSide;
                const bool keepRow = keepInside ? rowIsInside : !rowIsInside;
                SetPlanarSubmitRowBytes(
                    maskRowPtr,
                    submitRowPtr,
                    sourceRowPtr,
                    backgroundVoxelBytes,
                    rowBytes,
                    dims[0],
                    keepRow);
                continue;
            }

            double planeSide = rowStartSide;
            for (int iOffset = 0; iOffset < dims[0]; ++iOffset) {
                const int index[3] = { extent[0] + iOffset, j, k };
                const bool isInside = GetPlanarVoxelIsOnNormalSide(
                    image,
                    sideStep,
                    cropData,
                    index,
                    planeSide);
                const bool keepVoxel = keepInside ? isInside : !isInside;
                const auto voxelByteOffset = static_cast<std::size_t>(iOffset) * bytesPerVoxel;
                SetPlanarSubmitVoxelBytes(
                    maskRowPtr + iOffset,
                    submitRowPtr + voxelByteOffset,
                    sourceRowPtr + voxelByteOffset,
                    backgroundVoxelBytes,
                    bytesPerVoxel,
                    keepVoxel);

                planeSide += sideStep.iStep;
            }
        }
    }

    images.submitImage->Modified();
    images.maskImage->Modified();
    return images;
}

static OrthogonalCropResult GetPlanarPreviewResult(
    const CropDataModel& cropData,
    const OrthogonalCropRequest& request)
{
    auto result = GetPlanarResultFromRequest(request);
    auto statistics = GetPlanarDiagnostics(request);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);

    // 平面 preview 的真实语义是无限半空间：dot(point - center, normal) 决定保留/移除侧。
    // 之前生成有限矩形 outline 只是显示参照，会误导用户以为裁切只发生在框内，因此这里不再返回 outline。
    result.SetCropDataModel(cropData);
    result.SetStatistics(statistics);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetSucceeded(true);
    return result;
}

static OrthogonalCropResult GetPlanarSubmitResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const OrthogonalCropRequest& request,
    const CropBoundsDouble6Array& /*inputModelBounds*/,
    std::size_t availableRamBytes)
{
    if (!image) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::InputImageMissing,
            "Input image is null.",
            &cropData);
    }

    const std::size_t voxelCount = GetImageVoxelCount(image);
    const std::size_t estimatedRamUsageBytes =
        voxelCount * (GetImageBytesPerVoxel(image) + sizeof(unsigned char));
    if (availableRamBytes != 0 && estimatedRamUsageBytes > availableRamBytes) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::InsufficientRam,
            "Estimated planar image submit memory usage exceeds currently available RAM.",
            &cropData);
    }

    auto submitImages = GetPlanarSubmitImages(
        image,
        cropData,
        request.GetRemovalMode());
    if (!submitImages.submitImage) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::SubmitImageCreationFailed,
            "Failed to build planar image submit image.",
            &cropData);
    }

    if (!submitImages.maskImage) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::SubmitMaskCreationFailed,
            "Failed to build planar image submit mask.",
            &cropData);
    }

    auto result = GetPlanarResultFromRequest(request);
    auto statistics = GetPlanarDiagnostics(request);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);

    result.SetCropDataModel(cropData);
    result.SetStatistics(statistics);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetSubmitImage(submitImages.submitImage);
    result.SetMaskImage(submitImages.maskImage);
    result.SetSucceeded(true);
    return result;
}

OrthogonalCropResult PlanarCropAlgorithm::GetResult(
    vtkImageData* image,
    const OrthogonalCropRequest& request,
    std::size_t fallbackAvailableRamBytes)
{
    if (!image) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::InputImageMissing,
            "Input image is null.");
    }

    const auto inputModelBounds = GetImageModelBounds(image);
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!GetPlaneCropDataModel(
            request,
            inputModelBounds,
            cropData,
            failureReason,
            message)) {
        return GetFailureResult(request, failureReason, message);
    }

    if (request.GetOperation() == OrthogonalCropOperation::Preview) {
        return GetPlanarPreviewResult(
            cropData,
            request);
    }

    if (request.GetOperation() == OrthogonalCropOperation::Submit) {
        return GetPlanarSubmitResult(
            image,
            cropData,
            request,
            inputModelBounds,
            GetEffectiveAvailableRamBytes(request, fallbackAvailableRamBytes));
    }

    return GetFailureResult(
        request,
        OrthogonalCropFailureReason::UnsupportedBackend,
        "Image algorithm received an unsupported planar crop operation.",
        &cropData);
}

OrthogonalCropResult PlanarCropAlgorithm::GetResult(
    vtkPolyData* polyData,
    const OrthogonalCropRequest& request)
{
    if (!polyData) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::InputPolyDataMissing,
            "Input polydata is null.");
    }

    const auto inputModelBounds = GetPolyDataInputModelBounds(polyData);
    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!GetPlaneCropDataModel(
            request,
            inputModelBounds,
            cropData,
            failureReason,
            message)) {
        return GetFailureResult(request, failureReason, message);
    }

    if (request.GetOperation() != OrthogonalCropOperation::Preview) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::UnsupportedBackend,
            "PolyData algorithm received an unsupported planar crop operation.",
            &cropData);
    }

    auto result = GetPlanarPreviewResult(
        cropData,
        request);
    if (!result.GetSucceeded()) {
        return result;
    }

    auto statistics = result.GetStatistics();
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    statistics.SetValidationMessage("");
    result.SetStatistics(statistics);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetMessage("");
    return result;
}
