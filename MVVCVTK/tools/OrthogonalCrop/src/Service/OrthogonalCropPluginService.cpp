// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropPluginService.cpp
// 分类: Service / Image Plugin Facade Implementation
// 说明: 为 image 路径提供默认 request、系统 RAM 查询与算法转发。
// =====================================================================

#include "OrthogonalCropPluginService.h"

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
    request.SetDataSource(OrthogonalCropDataSource::ImageData);
    request.SetBackend(OrthogonalCropBackend::MaskPreview);
    request.SetRemovalMode(CropRemovalMode::KeepInside);

    if (!m_inputImage) {
        return request;
    }

    // 默认 request 始终以当前 image 全体 bounds 为基准，
    // 供交互桥在进入模式时派生出第一版 preview request。
    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    m_inputImage->GetBounds(bounds);
    request.SetBoxToInputModelMatrixFromBounds({
        bounds[0], bounds[1],
        bounds[2], bounds[3],
        bounds[4], bounds[5]
    });
    return request;
}

std::size_t OrthogonalCropPluginService::GetSystemAvailableRamBytes() const
{
#ifdef _WIN32
    // 查询 Windows 可用物理内存；
    // image submit 用它做 RAM gate，内存不足时提前阻止昂贵提取。
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
    // image 诊断直接委托算法层；
    // PluginService 只补充系统 RAM，让 submit plan 能判断是否安全执行。
    return OrthogonalCropAlgorithm::GetStatistics(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}

OrthogonalCropResult OrthogonalCropPluginService::GetResult(
    const OrthogonalCropRequest& request,
    const OrthogonalCropResult& resultContext) const
{
    // image 执行直接委托算法层；
    // PluginService 只持有输入 image 并注入 RAM，不重写 result 身份或 request 归一化。
    return OrthogonalCropAlgorithm::GetResult(
        m_inputImage,
        request,
        resultContext,
        GetSystemAvailableRamBytes());
}
