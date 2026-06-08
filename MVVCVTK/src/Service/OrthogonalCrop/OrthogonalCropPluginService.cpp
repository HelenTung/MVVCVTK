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
    request.SetLocalToInputMatrix(GetIdentityMatrixArray());
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
    // ═══ 系统可用物理内存查询 (Windows) ═══
    // 用途: PhysicalCrop 执行前判断是否有足够 RAM
    // Algorithm::GetStatistics 会拿这个值和 expectedBytes 比较
    // 不够时会返回 InsufficientRam 阻止物理裁切执行
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
    // ═══ Image 路径统计：透传给 Algorithm + 注入 RAM 参数 ═══
    // PluginService 不重写算法, 唯一附加值是补上系统 RAM
    // Algorithm 在 PhysicalCrop 场景下会用 RAM 判断是否安全执行
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
