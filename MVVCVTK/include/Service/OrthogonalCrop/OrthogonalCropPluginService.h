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

#include <memory>
#include <utility>

class IOrthogonalCropService {
public:
    virtual ~IOrthogonalCropService() = default;

    virtual void SetInputImage(vtkSmartPointer<vtkImageData> image) = 0;
    virtual vtkSmartPointer<vtkImageData> GetInputImage() const = 0;
    virtual OrthogonalCropRequest GetDefaultRequest() const = 0;
    virtual std::size_t GetSystemAvailableRamBytes() const = 0;
    virtual OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const = 0;
    virtual OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const = 0;
};

class OrthogonalCropPluginService : public IOrthogonalCropService {
public:
    void SetInputImage(vtkSmartPointer<vtkImageData> image) override
    {
        m_inputImage = std::move(image);
    }

    vtkSmartPointer<vtkImageData> GetInputImage() const override
    {
        return m_inputImage;
    }

    OrthogonalCropRequest GetDefaultRequest() const override
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

    std::size_t GetSystemAvailableRamBytes() const override
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

    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const override
    {
        return OrthogonalCropAlgorithm::GetStatistics(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());
    }

    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const override
    {
        return OrthogonalCropAlgorithm::GetResult(
            m_inputImage,
            request,
            GetSystemAvailableRamBytes());
    }

private:
    vtkSmartPointer<vtkImageData> m_inputImage;
};
