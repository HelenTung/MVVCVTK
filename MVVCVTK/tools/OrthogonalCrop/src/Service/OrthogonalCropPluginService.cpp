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
    request.SetExecutionMode(CropExecutionMode::PreviewArtifact);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());

    if (!m_inputImage) {
        return request;
    }

    // 默认 request 始终以当前 image 全体 bounds 为基准，
    // 供交互桥在进入模式时派生出第一版 preview request。
    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    m_inputImage->GetBounds(bounds);
    request.SetBoxToModelMatrixFromBounds({
        bounds[0], bounds[1],
        bounds[2], bounds[3],
        bounds[4], bounds[5]
    });
    return request;
}

std::size_t OrthogonalCropPluginService::GetSystemAvailableRamBytes() const
{
#ifdef _WIN32
    // ═══ 系统可用物理内存查询 (Windows) ═══
    // 用途: Submit 内部执行计划判断是否有足够 RAM。
    // 不够时会返回 InsufficientRam 阻止 image submit 执行。
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
    // ═══ Image 路径诊断：透传给 Algorithm + 注入 RAM 参数 ═══
    // PluginService 不重写算法, 唯一附加值是补上系统 RAM。
    // Algorithm 在 Submit 场景下通过内部 submit plan 判断是否安全执行。
    return OrthogonalCropAlgorithm::GetStatistics(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}

OrthogonalCropResult OrthogonalCropPluginService::GetResult(const OrthogonalCropRequest& request) const
{
    // ═══ Image 路径执行：透传给 Algorithm + 注入 RAM 参数 ═══
    // PluginService 是 image-only 的轻量 Facade, 三层角色:
    //   1) 持有 m_inputImage (由 Router::SetInputImage 注入)
    //   2) 查询系统可用 RAM (Windows: GlobalMemoryStatusEx)
    //   3) 透传给 OrthogonalCropAlgorithm 静态方法
    // 不做任何 request→cropData 归一化, 不做结果回填
    return OrthogonalCropAlgorithm::GetResult(
        m_inputImage,
        request,
        GetSystemAvailableRamBytes());
}
