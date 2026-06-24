// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropBackendRouterService.cpp
// 分类: Service / Backend Router Implementation
// 说明: 按 request 指定的数据源与后端，把图像 / 体渲染 / 网格请求直接分发到算法层。
// =====================================================================

#include "OrthogonalCropBackendRouterService.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef GetMessage
#undef GetMessage
#endif
#endif

#include <utility>

void OrthogonalCropBackendRouterService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_inputImage = std::move(image);
}

vtkSmartPointer<vtkImageData> OrthogonalCropBackendRouterService::GetInputImage() const
{
    return m_inputImage;
}

void OrthogonalCropBackendRouterService::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_inputPolyData = std::move(polyData);
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetInputPolyData() const
{
    return m_inputPolyData;
}

void OrthogonalCropBackendRouterService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_preferredDataSource = dataSource;
}

OrthogonalCropDataSource OrthogonalCropBackendRouterService::GetActiveDataSource() const
{
    const bool hasImage = GetInputImage() != nullptr;
    const bool hasPolyData = GetInputPolyData() != nullptr;

    if (m_preferredDataSource == OrthogonalCropDataSource::ImageData && hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (m_preferredDataSource == OrthogonalCropDataSource::VolumeData && hasImage) {
        return OrthogonalCropDataSource::VolumeData;
    }

    if (m_preferredDataSource == OrthogonalCropDataSource::PolyData && hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    if (hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    return m_preferredDataSource;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetActiveInputModelBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
    case OrthogonalCropDataSource::VolumeData:
        return GetImageModelBounds();
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataInputModelBounds();
    default:
        return bounds;
    }
}

OrthogonalCropRequest OrthogonalCropBackendRouterService::GetDefaultRequest() const
{
    const auto activeDataSource = GetActiveDataSource();
    OrthogonalCropRequest request;
    request.SetDataSource(activeDataSource);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetBoxToInputModelMatrixFromBounds(GetActiveInputModelBounds());
    request.SetOperation(OrthogonalCropOperation::Preview);
    return request;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetStatistics(const OrthogonalCropRequest& request) const
{
    switch (request.GetDataSource()) {
    case OrthogonalCropDataSource::ImageData:
    case OrthogonalCropDataSource::VolumeData:
        if (request.GetDataSource() == OrthogonalCropDataSource::VolumeData
            && request.GetOperation() != OrthogonalCropOperation::Preview) {
            auto statistics = GetMissingInputStatistics(
                request.GetDataSource(),
                request.GetOperation());
            statistics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
            statistics.SetValidationMessage("Volume crop data source only supports preview operation.");
            return statistics;
        }

        if (!GetInputImage()) {
            auto statistics = GetMissingInputStatistics(
                request.GetDataSource(),
                request.GetOperation());
            statistics.SetValidationMessage("Image-backed crop data source requires image input data.");
            return statistics;
        }

        return OrthogonalCropAlgorithm::GetStatistics(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());
    case OrthogonalCropDataSource::PolyData:
        if (!GetInputPolyData()) {
            auto statistics = GetMissingInputStatistics(
                OrthogonalCropDataSource::PolyData,
                request.GetOperation());
            statistics.SetValidationMessage("Polydata crop data source requires polydata input data.");
            return statistics;
        }

        return OrthogonalCropAlgorithm::GetStatistics(m_inputPolyData, request);
    default: {
        auto statistics = GetMissingInputStatistics(request.GetDataSource(), request.GetOperation());
        statistics.SetFailureReason(OrthogonalCropFailureReason::UnsupportedBackend);
        statistics.SetValidationMessage("Orthogonal crop request has no executable data source.");
        return statistics;
    }
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext) const
{
    switch (request.GetDataSource()) {
    case OrthogonalCropDataSource::ImageData:
    case OrthogonalCropDataSource::VolumeData:
        if (request.GetDataSource() == OrthogonalCropDataSource::VolumeData
            && request.GetOperation() != OrthogonalCropOperation::Preview) {
            auto result = GetMissingInputResult(resultContext, OrthogonalCropFailureReason::UnsupportedBackend);
            result.SetMessage("Volume crop data source only supports preview operation.");
            return result;
        }

        if (!GetInputImage()) {
            auto result = GetMissingInputResult(
                resultContext,
                OrthogonalCropFailureReason::InputImageMissing);
            result.SetMessage("Image-backed crop data source requires image input data.");
            return result;
        }

        return OrthogonalCropAlgorithm::GetResult(
            m_inputImage,
            request,
            resultContext,
            GetSystemAvailableRamBytes());
    case OrthogonalCropDataSource::PolyData:
        if (!GetInputPolyData()) {
            auto result = GetMissingInputResult(
                resultContext,
                OrthogonalCropFailureReason::InputPolyDataMissing);
            result.SetMessage("Polydata crop data source requires polydata input data.");
            return result;
        }

        return OrthogonalCropAlgorithm::GetResult(m_inputPolyData, request, resultContext);
    default: {
        auto result = GetMissingInputResult(resultContext, OrthogonalCropFailureReason::UnsupportedBackend);
        result.SetMessage("Orthogonal crop request has no executable data source.");
        return result;
    }
    }
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetImageModelBounds() const
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

std::array<double, 6> OrthogonalCropBackendRouterService::GetPolyDataInputModelBounds() const
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

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetMissingInputStatistics(
    OrthogonalCropDataSource dataSource,
    OrthogonalCropOperation operation) const
{
    const bool requiresPolyData = dataSource == OrthogonalCropDataSource::PolyData;

    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(dataSource);
    statistics.SetResolvedOperation(operation);
    statistics.SetFailureReason(
        requiresPolyData
            ? OrthogonalCropFailureReason::InputPolyDataMissing
            : OrthogonalCropFailureReason::InputImageMissing);
    statistics.SetValidationMessage(
        requiresPolyData
            ? "No active polydata crop input is bound."
            : "No active image crop input is bound.");
    return statistics;
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetMissingInputResult(
    const OrthogonalCropResult& resultContext,
    OrthogonalCropFailureReason failureReason) const
{
    auto result = resultContext;
    result.SetFailureReason(failureReason);
    result.SetMessage("No active crop input data is bound.");
    return result;
}

std::size_t OrthogonalCropBackendRouterService::GetSystemAvailableRamBytes() const
{
#ifdef _WIN32
    MEMORYSTATUSEX memoryStatus = {};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus) != 0) {
        return static_cast<std::size_t>(memoryStatus.ullAvailPhys);
    }
#endif
    return 0;
}
