// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/PlanarCropAlgorithm.cpp
// 分类: Math / Core Algorithm Implementation
// 说明: 负责平面 request 归一化、平面矩形轮廓和 image submit 产物构建。
// =====================================================================

#include "PlanarCropAlgorithm.h"

#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>

#include <algorithm>
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
    const OrthogonalCropRequest& request,
    const CropVectorDouble3Array& planeNormalInInputModel,
    const CropVectorDouble3Array& planeCenterInInputModel,
    const std::array<double, 2>& planeHalfExtentsInInputModel,
    const CropBoundsDouble6Array& inputModelBounds)
{
    CropDataModel cropData;
    cropData.SetBoxToInputModelMatrix(request.GetBoxToInputModelMatrix());
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
        request,
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

static void SetScalarTupleToBackground(vtkDataArray* scalars, vtkIdType tupleId, const std::vector<double>& backgroundTuple)
{
    if (!scalars || backgroundTuple.empty()) {
        return;
    }

    scalars->SetTuple(tupleId, backgroundTuple.data());
}

static vtkSmartPointer<vtkImageData> GetPlanarMaskImage(
    vtkImageData* image,
    const CropDataModel& cropData,
    CropRemovalMode removalMode)
{
    if (!image) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);

    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    maskImage->CopyStructure(image);
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
    if (!maskPtr) {
        return nullptr;
    }

    const bool keepInside = removalMode == CropRemovalMode::KeepInside;
    const auto planeCenterInInputModel = cropData.GetPlaneCenterInInputModel();
    const auto planeNormalInInputModel = cropData.GetPlaneNormalInInputModel();
    const vtkIdType rowStride = dims[0];
    const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];

    for (int k = 0; k < dims[2]; ++k) {
        for (int j = 0; j < dims[1]; ++j) {
            for (int i = 0; i < dims[0]; ++i) {
                const int index[3] = { i, j, k };
                double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
                image->TransformIndexToPhysicalPoint(index, inputModelPoint);

                const bool isInside = GetPointIsOnNormalSide(
                    inputModelPoint,
                    planeCenterInInputModel,
                    planeNormalInInputModel);
                const bool keepVoxel = keepInside ? isInside : !isInside;
                const vtkIdType linearIndex = static_cast<vtkIdType>(k) * sliceStride
                    + static_cast<vtkIdType>(j) * rowStride
                    + i;
                maskPtr[linearIndex] = keepVoxel ? 255 : 0;
            }
        }
    }

    maskImage->Modified();
    return maskImage;
}

static void SetPlanarRemovedVoxelsToBackground(
    vtkImageData* submitImage,
    const CropDataModel& cropData,
    CropRemovalMode removalMode)
{
    if (!submitImage) {
        return;
    }

    auto scalars = submitImage->GetPointData()
        ? submitImage->GetPointData()->GetScalars()
        : nullptr;
    if (!scalars) {
        return;
    }

    const int componentCount = scalars->GetNumberOfComponents();
    if (componentCount <= 0) {
        return;
    }

    double scalarRange[2] = { 0.0, 0.0 };
    scalars->GetRange(scalarRange);
    std::vector<double> backgroundTuple(static_cast<std::size_t>(componentCount), scalarRange[0]);

    int dims[3] = { 0, 0, 0 };
    submitImage->GetDimensions(dims);

    const bool keepInside = removalMode == CropRemovalMode::KeepInside;
    const auto planeCenterInInputModel = cropData.GetPlaneCenterInInputModel();
    const auto planeNormalInInputModel = cropData.GetPlaneNormalInInputModel();

    for (int k = 0; k < dims[2]; ++k) {
        for (int j = 0; j < dims[1]; ++j) {
            for (int i = 0; i < dims[0]; ++i) {
                int index[3] = { i, j, k };
                double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
                submitImage->TransformIndexToPhysicalPoint(index, inputModelPoint);

                const bool isInside = GetPointIsOnNormalSide(
                    inputModelPoint,
                    planeCenterInInputModel,
                    planeNormalInInputModel);
                const bool keepVoxel = keepInside ? isInside : !isInside;
                if (!keepVoxel) {
                    SetScalarTupleToBackground(
                        scalars,
                        submitImage->ComputePointId(index),
                        backgroundTuple);
                }
            }
        }
    }

    submitImage->Modified();
}

vtkSmartPointer<vtkPolyData> PlanarCropAlgorithm::GetOutlinePolyData(
    const CropDataModel& cropData,
    const CropBoundsDouble6Array& /*inputModelBounds*/)
{
    const auto normal = cropData.GetPlaneNormalInInputModel();
    const auto center = cropData.GetPlaneCenterInInputModel();
    const auto halfExtents = cropData.GetPlaneHalfExtentsInInputModel();
    double normalData[3] = { normal[0], normal[1], normal[2] };
    if (vtkMath::Normalize(normalData) <= PlaneEpsilon) {
        return nullptr;
    }

    const double halfWidth = std::abs(halfExtents[0]);
    const double halfHeight = std::abs(halfExtents[1]);
    if (halfWidth <= PlaneEpsilon || halfHeight <= PlaneEpsilon) {
        return nullptr;
    }

    double referenceUp[3] = { 0.0, 1.0, 0.0 };
    if (std::abs(vtkMath::Dot(normalData, referenceUp)) > 0.99) {
        referenceUp[0] = 0.0;
        referenceUp[1] = 0.0;
        referenceUp[2] = 1.0;
    }

    double axisWidth[3] = { 0.0, 0.0, 0.0 };
    double axisHeight[3] = { 0.0, 0.0, 0.0 };
    vtkMath::Cross(normalData, referenceUp, axisWidth);
    if (vtkMath::Normalize(axisWidth) <= PlaneEpsilon) {
        return nullptr;
    }
    vtkMath::Cross(normalData, axisWidth, axisHeight);
    if (vtkMath::Normalize(axisHeight) <= PlaneEpsilon) {
        return nullptr;
    }

    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(4);

    const double signs[4][2] = {
        { -1.0, -1.0 },
        { 1.0, -1.0 },
        { 1.0, 1.0 },
        { -1.0, 1.0 }
    };
    for (vtkIdType pointIndex = 0; pointIndex < 4; ++pointIndex) {
        double point[3] = { 0.0, 0.0, 0.0 };
        for (int axis = 0; axis < 3; ++axis) {
            point[axis] = center[axis]
                + signs[pointIndex][0] * halfWidth * axisWidth[axis]
                + signs[pointIndex][1] * halfHeight * axisHeight[axis];
        }
        points->SetPoint(pointIndex, point);
    }

    auto lines = vtkSmartPointer<vtkCellArray>::New();
    const vtkIdType rectangleLine[5] = { 0, 1, 2, 3, 0 };
    lines->InsertNextCell(5, rectangleLine);

    auto outline = vtkSmartPointer<vtkPolyData>::New();
    outline->SetPoints(points);
    outline->SetLines(lines);
    return outline;
}

static OrthogonalCropResult GetPlanarPreviewResult(
    const CropDataModel& cropData,
    const OrthogonalCropRequest& request,
    const CropBoundsDouble6Array& inputModelBounds)
{
    auto result = GetPlanarResultFromRequest(request);
    auto statistics = GetPlanarDiagnostics(request);
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);

    result.SetCropDataModel(cropData);
    result.SetStatistics(statistics);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetOutlinePolyData(PlanarCropAlgorithm::GetOutlinePolyData(cropData, inputModelBounds));
    result.SetSucceeded(result.GetOutlinePolyData() != nullptr);
    if (!result.GetSucceeded()) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed);
        statistics.SetValidationMessage("Plane preview outline creation failed.");
        result.SetStatistics(statistics);
        result.SetFailureReason(statistics.GetFailureReason());
        result.SetMessage(statistics.GetValidationMessage());
    }
    return result;
}

static OrthogonalCropResult GetPlanarSubmitResult(
    vtkImageData* image,
    const CropDataModel& cropData,
    const OrthogonalCropRequest& request,
    const CropBoundsDouble6Array& inputModelBounds,
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

    auto submitImage = vtkSmartPointer<vtkImageData>::New();
    submitImage->DeepCopy(image);
    if (!submitImage) {
        return GetFailureResult(
            request,
            OrthogonalCropFailureReason::SubmitImageCreationFailed,
            "Failed to build planar image submit image.",
            &cropData);
    }

    SetPlanarRemovedVoxelsToBackground(
        submitImage,
        cropData,
        request.GetRemovalMode());

    auto maskImage = GetPlanarMaskImage(
        image,
        cropData,
        request.GetRemovalMode());
    if (!maskImage) {
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
    result.SetSubmitImage(submitImage);
    result.SetMaskImage(maskImage);
    result.SetOutlinePolyData(PlanarCropAlgorithm::GetOutlinePolyData(cropData, inputModelBounds));
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
            request,
            inputModelBounds);
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
        request,
        inputModelBounds);
    result.SetSucceeded(result.GetOutlinePolyData() != nullptr);
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
