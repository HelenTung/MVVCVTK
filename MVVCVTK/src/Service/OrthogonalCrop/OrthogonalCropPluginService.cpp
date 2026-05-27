#include "OrthogonalCrop/OrthogonalCropPluginService.h"

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

void OrthogonalCropPluginService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_inputImage = std::move(image);
}

vtkSmartPointer<vtkImageData> OrthogonalCropPluginService::GetInputImage() const
{
    return m_inputImage;
}

OrthogonalCropRequest OrthogonalCropPluginService::GetDefaultRequest() const
{
    OrthogonalCropRequest request;
    request.SetBoundsMode(CropBoundsMode::InputVolumeBounds);
    request.SetExecutionMode(CropExecutionMode::VirtualCrop);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());
    request.SetLocalAlignmentMatrix(GetIdentityMatrixArray());
    request.SetLocalCenter({ 0.0, 0.0, 0.0 });

    if (!m_inputImage) {
        return request;
    }

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    m_inputImage->GetBounds(bounds);
    request.SetRasBounds({
        bounds[0], bounds[1],
        bounds[2], bounds[3],
        bounds[4], bounds[5]
    });
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

std::size_t OrthogonalCropPluginService::GetSystemAvailableRamBytes() const
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

OrthogonalCropStatistics OrthogonalCropPluginService::GetStatistics(const OrthogonalCropRequest& request) const
{
    return OrthogonalCropAlgorithm::GetStatistics(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}

OrthogonalCropResult OrthogonalCropPluginService::GetResult(const OrthogonalCropRequest& request) const
{
    return OrthogonalCropAlgorithm::GetResult(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}
