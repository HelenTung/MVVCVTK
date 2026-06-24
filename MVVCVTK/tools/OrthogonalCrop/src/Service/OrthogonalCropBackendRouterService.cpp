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

#include <string>
#include <utility>

static bool GetRequestUsesBoxGeometry(const OrthogonalCropRequest& request)
{
    return request.GetGeometryType() == OrthogonalCropGeometryType::Box;
}

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
    if (!GetRequestUsesBoxGeometry(request)) {
        return GetRouterFailureStatistics(
            request,
            OrthogonalCropFailureReason::UnsupportedBackend,
            "Router supports only Box crop geometry.");
    }

    switch (request.GetDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        if (request.GetOperation() != OrthogonalCropOperation::Submit) {
            return GetRouterFailureStatistics(
                request,
                OrthogonalCropFailureReason::UnsupportedBackend,
                "ImageData crop source supports only Submit operation.");
        }
        if (!GetInputImage()) {
            return GetRouterFailureStatistics(
                request,
                OrthogonalCropFailureReason::InputImageMissing,
                "ImageData crop source requires image input data.");
        }
        return OrthogonalCropAlgorithm::GetStatistics(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());

    case OrthogonalCropDataSource::VolumeData:
        if (request.GetOperation() != OrthogonalCropOperation::Preview) {
            return GetRouterFailureStatistics(
                request,
                OrthogonalCropFailureReason::UnsupportedBackend,
                "VolumeData crop source supports only Preview operation.");
        }
        if (!GetInputImage()) {
            return GetRouterFailureStatistics(
                request,
                OrthogonalCropFailureReason::InputImageMissing,
                "VolumeData crop source requires image input data.");
        }
        return OrthogonalCropAlgorithm::GetStatistics(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());

    case OrthogonalCropDataSource::PolyData:
        if (request.GetOperation() != OrthogonalCropOperation::Preview) {
            return GetRouterFailureStatistics(
                request,
                OrthogonalCropFailureReason::UnsupportedBackend,
                "PolyData crop source supports only Preview operation.");
        }
        if (!GetInputPolyData()) {
            return GetRouterFailureStatistics(
                request,
                OrthogonalCropFailureReason::InputPolyDataMissing,
                "PolyData crop source requires polydata input data.");
        }

        return OrthogonalCropAlgorithm::GetStatistics(m_inputPolyData, request);
    default:
        return GetRouterFailureStatistics(
            request,
            OrthogonalCropFailureReason::UnsupportedBackend,
            "Orthogonal crop request has no executable data source.");
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext) const
{
    if (!GetRequestUsesBoxGeometry(request)) {
        return GetRouterFailureResult(
            request,
            resultContext,
            OrthogonalCropFailureReason::UnsupportedBackend,
            "Router supports only Box crop geometry.");
    }

    switch (request.GetDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        if (request.GetOperation() != OrthogonalCropOperation::Submit) {
            return GetRouterFailureResult(
                request,
                resultContext,
                OrthogonalCropFailureReason::UnsupportedBackend,
                "ImageData crop source supports only Submit operation.");
        }
        if (!GetInputImage()) {
            return GetRouterFailureResult(
                request,
                resultContext,
                OrthogonalCropFailureReason::InputImageMissing,
                "ImageData crop source requires image input data.");
        }
        return OrthogonalCropAlgorithm::GetResult(
            m_inputImage,
            request,
            resultContext,
            GetSystemAvailableRamBytes());

    case OrthogonalCropDataSource::VolumeData:
        if (request.GetOperation() != OrthogonalCropOperation::Preview) {
            return GetRouterFailureResult(
                request,
                resultContext,
                OrthogonalCropFailureReason::UnsupportedBackend,
                "VolumeData crop source supports only Preview operation.");
        }
        if (!GetInputImage()) {
            return GetRouterFailureResult(
                request,
                resultContext,
                OrthogonalCropFailureReason::InputImageMissing,
                "VolumeData crop source requires image input data.");
        }
        return OrthogonalCropAlgorithm::GetResult(
            m_inputImage,
            request,
            resultContext,
            GetSystemAvailableRamBytes());

    case OrthogonalCropDataSource::PolyData:
        if (request.GetOperation() != OrthogonalCropOperation::Preview) {
            return GetRouterFailureResult(
                request,
                resultContext,
                OrthogonalCropFailureReason::UnsupportedBackend,
                "PolyData crop source supports only Preview operation.");
        }
        if (!GetInputPolyData()) {
            return GetRouterFailureResult(
                request,
                resultContext,
                OrthogonalCropFailureReason::InputPolyDataMissing,
                "PolyData crop source requires polydata input data.");
        }

        return OrthogonalCropAlgorithm::GetResult(m_inputPolyData, request, resultContext);
    default:
        return GetRouterFailureResult(
            request,
            resultContext,
            OrthogonalCropFailureReason::UnsupportedBackend,
            "Orthogonal crop request has no executable data source.");
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

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetRouterFailureStatistics(
    const OrthogonalCropRequest& request,
    OrthogonalCropFailureReason failureReason,
    const std::string& message) const
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(request.GetDataSource());
    statistics.SetResolvedOperation(request.GetOperation());
    statistics.SetResolvedGeometryType(request.GetGeometryType());
    statistics.SetResolvedRemovalMode(request.GetRemovalMode());
    statistics.SetFailureReason(failureReason);
    statistics.SetValidationMessage(message);
    return statistics;
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetRouterFailureResult(
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext,
    OrthogonalCropFailureReason failureReason,
    const std::string& message) const
{
    auto result = resultContext;
    const auto statistics = GetRouterFailureStatistics(request, failureReason, message);
    result.SetStatistics(statistics);
    result.SetFailureReason(failureReason);
    result.SetMessage(message);
    result.SetSucceeded(false);
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
