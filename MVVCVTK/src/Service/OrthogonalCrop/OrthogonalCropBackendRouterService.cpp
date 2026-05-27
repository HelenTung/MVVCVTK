#include "OrthogonalCrop/OrthogonalCropBackendRouterService.h"

#include <vtkBox.h>
#include <vtkDoubleArray.h>
#include <vtkGeometryFilter.h>
#include <vtkMatrix4x4.h>
#include <vtkPlanes.h>
#include <vtkPoints.h>
#include <vtkTableBasedClipDataSet.h>

#include <cmath>
#include <cstddef>
#include <utility>

namespace {
std::array<double, 3> GetPointTransformed(
    const std::array<double, 16>& matrix,
    const std::array<double, 3>& point)
{
    const double x = matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3];
    const double y = matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7];
    const double z = matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11];
    const double w = matrix[12] * point[0] + matrix[13] * point[1] + matrix[14] * point[2] + matrix[15];
    const double invW = std::abs(w) > 1e-12 ? 1.0 / w : 1.0;
    return { x * invW, y * invW, z * invW };
}

std::array<double, 3> GetNormalTransformed(
    const std::array<double, 16>& modelToLocalMatrix,
    const std::array<double, 3>& localNormal)
{
    std::array<double, 3> normal = {
        modelToLocalMatrix[0] * localNormal[0] + modelToLocalMatrix[4] * localNormal[1] + modelToLocalMatrix[8] * localNormal[2],
        modelToLocalMatrix[1] * localNormal[0] + modelToLocalMatrix[5] * localNormal[1] + modelToLocalMatrix[9] * localNormal[2],
        modelToLocalMatrix[2] * localNormal[0] + modelToLocalMatrix[6] * localNormal[1] + modelToLocalMatrix[10] * localNormal[2]
    };
    const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (length > 1e-12) {
        normal[0] /= length;
        normal[1] /= length;
        normal[2] /= length;
    }
    return normal;
}

std::array<double, 16> GetMatrixInverted(const std::array<double, 16>& matrixData)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrix->SetElement(row, col, matrixData[row * 4 + col]);
        }
    }
    matrix->Invert();

    std::array<double, 16> inverted = { 0.0 };
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            inverted[row * 4 + col] = matrix->GetElement(row, col);
        }
    }
    return inverted;
}
} // namespace

void OrthogonalCropBackendRouterService::CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_imageService.SetInputImage(std::move(image));
}

void OrthogonalCropBackendRouterService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    CropPreInit_SetInputImage(std::move(image));
}

vtkSmartPointer<vtkImageData> OrthogonalCropBackendRouterService::GetInputImage() const
{
    return m_imageService.GetInputImage();
}

void OrthogonalCropBackendRouterService::CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_inputPolyData = std::move(polyData);
}

void OrthogonalCropBackendRouterService::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    CropPreInit_SetInputPolyData(std::move(polyData));
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetInputPolyData() const
{
    return m_inputPolyData;
}

void OrthogonalCropBackendRouterService::CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_preferredDataSource = dataSource;
}

void OrthogonalCropBackendRouterService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    CropPreInit_SetPreferredDataSource(dataSource);
}

OrthogonalCropDataSource OrthogonalCropBackendRouterService::GetActiveDataSource() const
{
    const bool hasImage = GetInputImage() != nullptr;
    const bool hasPolyData = m_inputPolyData != nullptr;

    if (m_preferredDataSource == OrthogonalCropDataSource::PolyData && hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    if (m_preferredDataSource == OrthogonalCropDataSource::ImageData && hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    return OrthogonalCropDataSource::Auto;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetActiveInputBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        return GetImageBounds();
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataBounds();
    default:
        return bounds;
    }
}

OrthogonalCropRequest OrthogonalCropBackendRouterService::GetDefaultRequest() const
{
    if (GetActiveDataSource() == OrthogonalCropDataSource::ImageData) {
        return m_imageService.GetDefaultRequest();
    }

    OrthogonalCropRequest request;
    request.SetBoundsMode(CropBoundsMode::InputVolumeBounds);
    request.SetExecutionMode(CropExecutionMode::VirtualCrop);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());
    request.SetLocalAlignmentMatrix(GetIdentityMatrixArray());
    request.SetLocalCenter({ 0.0, 0.0, 0.0 });

    const auto bounds = GetActiveInputBounds();
    request.SetRasBounds(bounds);
    request.SetCenter({
        (bounds[0] + bounds[1]) * 0.5,
        (bounds[2] + bounds[3]) * 0.5,
        (bounds[4] + bounds[5]) * 0.5
    });
    request.SetDimensions({
        bounds[1] - bounds[0],
        bounds[3] - bounds[2],
        bounds[5] - bounds[4]
    });
    request.SetLocalDimensions(request.GetDimensions());
    return request;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetStatistics(const OrthogonalCropRequest& request) const
{
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        return GetImageStatistics(request);
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataStatistics(request);
    default:
        return GetMissingInputStatistics();
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(const OrthogonalCropRequest& request) const
{
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        return GetImageResult(request);
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataResult(request);
    default:
        return GetMissingInputResult();
    }
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetClippedPolyData(
    vtkPolyData* polyData,
    vtkImplicitFunction* clipFunction,
    CropRemovalMode removalMode)
{
    if (!polyData || !clipFunction) {
        return nullptr;
    }

    auto clip = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
    clip->SetInputData(polyData);
    clip->SetClipFunction(clipFunction);
    if (removalMode == CropRemovalMode::KeepInside) {
        clip->InsideOutOn();
    }
    else {
        clip->InsideOutOff();
    }
    clip->Update();

    auto geometry = vtkSmartPointer<vtkGeometryFilter>::New();
    geometry->SetInputConnection(clip->GetOutputPort());
    geometry->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(geometry->GetOutput());
    return output;
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetClippedPolyData(
    vtkPolyData* polyData,
    const CropDataModel& cropData,
    CropRemovalMode removalMode)
{
    return GetClippedPolyData(polyData, GetClipFunction(cropData), removalMode);
}

vtkSmartPointer<vtkImplicitFunction> OrthogonalCropBackendRouterService::GetClipFunction(const CropDataModel& cropData)
{
    if (!cropData.GetLocalAlignmentEnabled()) {
        auto box = vtkSmartPointer<vtkBox>::New();
        const auto rasBounds = cropData.GetRasBounds();
        box->SetBounds(
            rasBounds[0], rasBounds[1],
            rasBounds[2], rasBounds[3],
            rasBounds[4], rasBounds[5]);
        return box;
    }

    auto planes = vtkSmartPointer<vtkPlanes>::New();
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto normals = vtkSmartPointer<vtkDoubleArray>::New();
    points->SetNumberOfPoints(6);
    normals->SetNumberOfComponents(3);
    normals->SetNumberOfTuples(6);

    const auto& localToModel = cropData.GetLocalAlignmentMatrix();
    const auto modelToLocal = GetMatrixInverted(localToModel);
    const auto localCenter = cropData.GetLocalCenter();
    const auto localDimensions = cropData.GetLocalDimensions();

    int planeIndex = 0;
    for (int axis = 0; axis < 3; ++axis) {
        for (int signIndex = 0; signIndex < 2; ++signIndex) {
            const double sign = signIndex == 0 ? -1.0 : 1.0;
            auto localPoint = localCenter;
            localPoint[axis] += sign * localDimensions[axis] * 0.5;

            std::array<double, 3> localNormal = { 0.0, 0.0, 0.0 };
            localNormal[axis] = sign;

            const auto modelPoint = GetPointTransformed(localToModel, localPoint);
            const auto modelNormal = GetNormalTransformed(modelToLocal, localNormal);
            points->SetPoint(planeIndex, modelPoint[0], modelPoint[1], modelPoint[2]);
            normals->SetTuple3(planeIndex, modelNormal[0], modelNormal[1], modelNormal[2]);
            ++planeIndex;
        }
    }

    planes->SetPoints(points);
    planes->SetNormals(normals);
    return planes;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetImageBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    auto image = GetInputImage();
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

std::array<double, 6> OrthogonalCropBackendRouterService::GetPolyDataBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!m_inputPolyData) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    m_inputPolyData->GetBounds(rawBounds);
    return {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetMissingInputStatistics() const
{
    OrthogonalCropStatistics statistics;
    statistics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
    statistics.SetValidationMessage("No active crop input data is bound.");
    return statistics;
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetMissingInputResult() const
{
    OrthogonalCropResult result;
    result.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
    result.SetMessage("No active crop input data is bound.");
    return result;
}

bool OrthogonalCropBackendRouterService::GetPolyDataCropDataModel(
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message) const
{
    if (!m_inputPolyData) {
        failureReason = OrthogonalCropFailureReason::InputPolyDataMissing;
        message = "Input polydata is null.";
        return false;
    }

    return OrthogonalCropAlgorithm::GetCropDataModel(
        GetPolyDataBounds(),
        request,
        cropData,
        failureReason,
        message);
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetPolyDataStatisticsFromClipped(vtkPolyData* clipped) const
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);

    if (!m_inputPolyData) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
        statistics.SetValidationMessage("Input polydata is null.");
        return statistics;
    }

    if (!clipped) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
        statistics.SetValidationMessage("Failed to build clipped polydata.");
        return statistics;
    }

    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    statistics.SetTotalVoxelCount(static_cast<std::size_t>(m_inputPolyData->GetNumberOfCells()));
    statistics.SetInsideVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
    statistics.SetOutputVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
    statistics.SetCanExecutePhysicalCrop(true);
    return statistics;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetImageStatistics(const OrthogonalCropRequest& request) const
{
    auto statistics = m_imageService.GetStatistics(request);
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
    }
    else {
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
    }

    return statistics;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetPolyDataStatistics(const OrthogonalCropRequest& request) const
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);

    if (!m_inputPolyData) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
        statistics.SetValidationMessage("Input polydata is null.");
        return statistics;
    }

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    auto clipped = GetClippedPolyData(m_inputPolyData, cropData, request.GetRemovalMode());
    if (!clipped) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
        statistics.SetValidationMessage("Failed to build clipped polydata.");
        return statistics;
    }

    return GetPolyDataStatisticsFromClipped(clipped);
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetImageResult(const OrthogonalCropRequest& request) const
{
    auto result = m_imageService.GetResult(request);
    result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop
        && result.GetResolvedBackend() == OrthogonalCropResolvedBackend::None) {
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
    }
    return result;
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetPolyDataResult(const OrthogonalCropRequest& request) const
{
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);
    result.SetCropStateModel(request.GetCropStateModel());

    if (!m_inputPolyData) {
        result.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
        result.SetMessage("Input polydata is null.");
        return result;
    }

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    auto clipped = GetClippedPolyData(m_inputPolyData, cropData, request.GetRemovalMode());
    if (!clipped) {
        result.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
        result.SetMessage("Failed to build clipped polydata.");
        return result;
    }

    const auto statistics = GetPolyDataStatisticsFromClipped(clipped);

    result.SetStatistics(statistics);
    result.SetCropDataModel(cropData);
    result.SetOutlinePolyData(OrthogonalCropAlgorithm::GetOutlinePolyData(cropData));
    result.SetDerivedPolyData(clipped);
    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    return result;
}
