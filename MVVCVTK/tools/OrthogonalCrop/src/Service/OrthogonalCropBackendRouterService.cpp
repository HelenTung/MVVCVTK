// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropBackendRouterService.cpp
// 分类: Service / Backend Router Implementation
// 说明: 按 request 指定的裁切类型 / 动作 / 数据源，把图像 / 体渲染 / 网格请求直接分发到算法层。
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

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(const OrthogonalCropRequest& request) const
{
    // 公开入口只分发裁切类型；动作和数据源留在对应几何的内部路由里，避免多套入口表达同一件事。
    switch (request.GetGeometryType()) {
    case OrthogonalCropGeometryType::Box:
        return GetBoxResult(request);

    default:
        return GetRouterFailureResult(
            request,
            OrthogonalCropFailureReason::UnsupportedBackend,
            "Router supports only Box crop geometry.");
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetBoxResult(const OrthogonalCropRequest& request) const
{
    // Box 路由只放行当前三种真实路径：体预览、网格预览、图像提交；其它组合统一停在 router。
    switch (request.GetOperation()) {
    case OrthogonalCropOperation::Preview:
        switch (request.GetDataSource()) {
        case OrthogonalCropDataSource::VolumeData:
            if (!GetInputImage()) {
                return GetRouterFailureResult(
                    request,
                    OrthogonalCropFailureReason::InputImageMissing,
                    "Image-backed crop route requires image input data.");
            }
            return OrthogonalCropAlgorithm::GetResult(
                m_inputImage,
                request,
                GetSystemAvailableRamBytes());

        case OrthogonalCropDataSource::PolyData:
            if (!GetInputPolyData()) {
                return GetRouterFailureResult(
                    request,
                    OrthogonalCropFailureReason::InputPolyDataMissing,
                    "PolyData crop route requires polydata input data.");
            }
            return OrthogonalCropAlgorithm::GetResult(m_inputPolyData, request);

        default:
            break;
        }
        break;

    case OrthogonalCropOperation::Submit:
        switch (request.GetDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            if (!GetInputImage()) {
                return GetRouterFailureResult(
                    request,
                    OrthogonalCropFailureReason::InputImageMissing,
                    "Image-backed crop route requires image input data.");
            }
            return OrthogonalCropAlgorithm::GetResult(
                m_inputImage,
                request,
                GetSystemAvailableRamBytes());

        default:
            break;
        }
        break;

    default:
        break;
    }

    return GetRouterFailureResult(
        request,
        OrthogonalCropFailureReason::UnsupportedBackend,
        "Router has no executable path for this crop route.");
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

OrthogonalCropResult OrthogonalCropBackendRouterService::GetRouterFailureResult(
    const OrthogonalCropRequest& request,
    OrthogonalCropFailureReason failureReason,
    const std::string& message) const
{
    OrthogonalCropResult result;
    result.SetResolvedDataSource(request.GetDataSource());
    result.SetResolvedOperation(request.GetOperation());
    result.SetResolvedGeometryType(request.GetGeometryType());
    result.SetResolvedRemovalMode(request.GetRemovalMode());

    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(request.GetDataSource());
    statistics.SetResolvedOperation(request.GetOperation());
    statistics.SetResolvedGeometryType(request.GetGeometryType());
    statistics.SetResolvedRemovalMode(request.GetRemovalMode());
    statistics.SetFailureReason(failureReason);
    statistics.SetValidationMessage(message);

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
