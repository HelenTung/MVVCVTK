// =====================================================================
// Path: MVVCVTK/src/Service/OrthogonalCrop/OrthogonalCropPluginService.cpp
// 分类: Service / Image Plugin Facade Implementation
// 说明: 为 image 路径提供默认 request、RAM 估算与算法转发。
// =====================================================================

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

    // 默认 request 始终以当前 image 全体 bounds 为基准，
    // 供交互桥在进入模式时派生出第一版 preview request。
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
    // 物理裁切的 RAM 审核走操作系统可用物理内存，保持与旧实现一致。
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
    // plugin service 不重写算法逻辑，只补上系统 RAM 这一层环境参数。
    return OrthogonalCropAlgorithm::GetStatistics(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}

OrthogonalCropResult OrthogonalCropPluginService::GetResult(const OrthogonalCropRequest& request) const
{
    // 完整执行同样直接透传到算法层，保持 service 仅做封装而不做行为变更。
    return OrthogonalCropAlgorithm::GetResult(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}
