// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropBackendRouterService.cpp
// 分类: Service / Backend Router Implementation
// 说明: 按 request 指定的数据源与后端，把 image / polydata 请求直接分发到算法层。
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
    if (m_preferredDataSource != OrthogonalCropDataSource::Auto) {
        return m_preferredDataSource;
    }

    const bool hasImage = GetInputImage() != nullptr;
    const bool hasPolyData = GetInputPolyData() != nullptr;

    if (hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    return OrthogonalCropDataSource::Auto;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetActiveInputModelBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
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
    if (activeDataSource == OrthogonalCropDataSource::Auto) {
        OrthogonalCropRequest request;
        request.SetDataSource(OrthogonalCropDataSource::Auto);
        request.SetBackend(OrthogonalCropBackend::None);
        return request;
    }

    OrthogonalCropRequest request;
    request.SetDataSource(activeDataSource);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetBoxToInputModelMatrixFromBounds(GetActiveInputModelBounds());
    request.SetBackend(activeDataSource == OrthogonalCropDataSource::PolyData
        ? OrthogonalCropBackend::ClipPreview
        : OrthogonalCropBackend::MaskPreview);
    return request;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetStatistics(const OrthogonalCropRequest& request) const
{
    switch (request.GetBackend()) {
    case OrthogonalCropBackend::MaskPreview:
    case OrthogonalCropBackend::SubmitExtractVOI: {
        if (request.GetDataSource() != OrthogonalCropDataSource::ImageData) {
            auto statistics = GetMissingInputStatistics(request.GetDataSource(), request.GetBackend());
            statistics.SetValidationMessage("Image crop backend requires image input data source.");
            return statistics;
        }

        if (!GetInputImage()) {
            auto statistics = GetMissingInputStatistics(
                OrthogonalCropDataSource::ImageData,
                request.GetBackend());
            statistics.SetValidationMessage("Image crop backend requires image input data.");
            return statistics;
        }

        return OrthogonalCropAlgorithm::GetStatistics(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());
    }
    case OrthogonalCropBackend::ClipPreview: {
        if (request.GetDataSource() != OrthogonalCropDataSource::PolyData) {
            auto statistics = GetMissingInputStatistics(request.GetDataSource(), request.GetBackend());
            statistics.SetValidationMessage("Polydata clip backend requires polydata input data source.");
            return statistics;
        }

        if (!GetInputPolyData()) {
            auto statistics = GetMissingInputStatistics(
                OrthogonalCropDataSource::PolyData,
                OrthogonalCropBackend::ClipPreview);
            statistics.SetValidationMessage("Polydata clip backend requires polydata input data.");
            return statistics;
        }

        return OrthogonalCropAlgorithm::GetStatistics(m_inputPolyData, request);
    }
    case OrthogonalCropBackend::None:
        if (request.GetDataSource() == OrthogonalCropDataSource::ImageData && GetInputImage()) {
            return OrthogonalCropAlgorithm::GetStatistics(
                m_inputImage,
                request,
                GetSystemAvailableRamBytes());
        }
        if (request.GetDataSource() == OrthogonalCropDataSource::PolyData && GetInputPolyData()) {
            return OrthogonalCropAlgorithm::GetStatistics(m_inputPolyData, request);
        }
        return GetMissingInputStatistics(request.GetDataSource(), request.GetBackend());
    default:
        return GetMissingInputStatistics(request.GetDataSource(), request.GetBackend());
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext) const
{
    switch (request.GetBackend()) {
    case OrthogonalCropBackend::MaskPreview:
    case OrthogonalCropBackend::SubmitExtractVOI: {
        if (request.GetDataSource() != OrthogonalCropDataSource::ImageData) {
            auto result = GetMissingInputResult(resultContext);
            result.SetMessage("Image crop backend requires image input data source.");
            return result;
        }

        if (!GetInputImage()) {
            auto result = GetMissingInputResult(resultContext);
            result.SetMessage("Image crop backend requires image input data.");
            return result;
        }

        return OrthogonalCropAlgorithm::GetResult(
            m_inputImage,
            request,
            resultContext,
            GetSystemAvailableRamBytes());
    }
    case OrthogonalCropBackend::ClipPreview: {
        if (request.GetDataSource() != OrthogonalCropDataSource::PolyData) {
            auto result = GetMissingInputResult(
                resultContext,
                OrthogonalCropFailureReason::InputPolyDataMissing);
            result.SetMessage("Polydata clip backend requires polydata input data source.");
            return result;
        }

        if (!GetInputPolyData()) {
            auto result = GetMissingInputResult(
                resultContext,
                OrthogonalCropFailureReason::InputPolyDataMissing);
            result.SetMessage("Polydata clip backend requires polydata input data.");
            return result;
        }

        return OrthogonalCropAlgorithm::GetResult(m_inputPolyData, request, resultContext);
    }
    case OrthogonalCropBackend::None:
        if (request.GetDataSource() == OrthogonalCropDataSource::ImageData && GetInputImage()) {
            return OrthogonalCropAlgorithm::GetResult(
                m_inputImage,
                request,
                resultContext,
                GetSystemAvailableRamBytes());
        }
        if (request.GetDataSource() == OrthogonalCropDataSource::PolyData && GetInputPolyData()) {
            return OrthogonalCropAlgorithm::GetResult(m_inputPolyData, request, resultContext);
        }
        return GetMissingInputResult(resultContext);
    default:
        return GetMissingInputResult(resultContext);
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
    OrthogonalCropBackend backend) const
{
    const bool requiresPolyData = backend == OrthogonalCropBackend::ClipPreview
        || dataSource == OrthogonalCropDataSource::PolyData;

    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(dataSource);
    statistics.SetResolvedBackend(backend);
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
