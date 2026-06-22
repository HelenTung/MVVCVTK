#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/Service/OrthogonalCropPluginService.h
// 分类: Service / Image Plugin Facade
// OrthogonalCropPluginService.h - 正交裁切独立插件服务封装
// 说明: 只处理 vtkImageData 路径，向上提供默认 request、诊断与执行入口。
// =====================================================================

#include "OrthogonalCropAlgorithm.h"

class OrthogonalCropPluginService {
public:
    // 绑定当前 image 输入。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);

    // 返回当前绑定的 image 输入。
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 基于当前 image 构造默认 request。
    OrthogonalCropRequest GetDefaultRequest() const;

    // 查询系统当前可用物理内存，供 submit 估算使用。
    std::size_t GetSystemAvailableRamBytes() const;

    // 查询 image 路径诊断结果。
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;

    // 执行 image 路径裁切，并把产物填入调用方已构造的结果上下文。
    OrthogonalCropResult GetResult(
        const OrthogonalCropRequest& request,
        const OrthogonalCropResult& resultContext) const;

private:
    // image-only plugin 唯一持有的输入对象。
    vtkSmartPointer<vtkImageData> m_inputImage;
};
