#pragma once

// =====================================================================
// OrthogonalCropPluginService.h - 正交裁切独立插件服务封装
// =====================================================================

#include "OrthogonalCrop/OrthogonalCropAlgorithm.h"

class OrthogonalCropPluginService {
public:
    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    OrthogonalCropRequest GetDefaultRequest() const;
    std::size_t GetSystemAvailableRamBytes() const;
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

private:
    vtkSmartPointer<vtkImageData> m_inputImage;
};
