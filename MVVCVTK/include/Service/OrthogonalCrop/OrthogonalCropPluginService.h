#pragma once

// =====================================================================
// OrthogonalCropPluginService.h — 正交裁切独立插件服务封装
// =====================================================================
// 用法对齐 GapAnalysisService 的“独立算法模块”思路：
// - SetInputImage：绑定一次输入体数据；
// - GetDefaultRequest：生成与当前输入体积一致的默认请求；
// - GetStatistics：只做校验/估算，不产生派生体；
// - GetResult：按 request 执行虚拟裁切或物理裁切。
//
// 该服务可直接放在 main、测试代码或前端 ViewModel 中持有，
// 不依赖当前 MedicalVizService 主链，也不接管任何 UI 交互流。

#include "OrthogonalCrop/OrthogonalCropAlgorithm.h"

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

class OrthogonalCropPluginService {
public:
    // 记录当前 image backend 要使用的输入体数据。
    void SetInputImage(vtkSmartPointer<vtkImageData> image)
    {
        m_inputImage = std::move(image);
    }

    // 返回当前 image backend 已绑定的输入对象。
    vtkSmartPointer<vtkImageData> GetInputImage() const
    {
        return m_inputImage;
    }

    // 根据当前输入图像生成一份“全幅范围、虚拟裁切、保留内部”的默认 request。
    // 上层通常从这里起步，再覆写 boundsMode、removalMode 或 executionMode。
    OrthogonalCropRequest GetDefaultRequest() const
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

    // 读取系统当前可用物理内存，供 physical crop 的 RAM 估算使用。
    // 查询失败时返回 0，表示后续由上层或算法层按“未知但不阻断”处理。
    std::size_t GetSystemAvailableRamBytes() const
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

    // image-only 统计入口：把当前输入和系统 RAM 信息一起转交给算法层。
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const
    {
        return OrthogonalCropAlgorithm::GetStatistics(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());
    }

    // image-only 结果入口：统一走算法层，并补入当前机器可用 RAM 作为 physical crop 参考。
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const
    {
        return OrthogonalCropAlgorithm::GetResult(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());
    }

private:
    // image backend 当前持有的输入体数据；整个插件服务的默认 request、统计和结果都围绕它展开。
    vtkSmartPointer<vtkImageData> m_inputImage;
};
