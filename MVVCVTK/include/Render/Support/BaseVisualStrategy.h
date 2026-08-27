#pragma once

#include "Data/ImageProcessor.h"
#include "Render/Support/FeatureVisualStrategy.h"

#include <vtkImageResample.h>

#include <algorithm>

// Host 内部策略基类只保留数据处理能力；通用 Feature 策略生命周期位于稳定契约层。
class BaseVisualStrategy : public FeatureVisualStrategy {
protected:
    // 最近一次创建的 producer；返回端口在下次替换该成员或策略析构前有效。
    vtkSmartPointer<vtkImageResample> m_resampleFilter;

    vtkAlgorithmOutput* GetDownsampledOutputPort(
        vtkImageData* input,
        int targetDim = 766)
    {
        if (!input) {
            return nullptr;
        }
        m_resampleFilter = ImageProcessor::GetDownsampledImage(
            input, targetDim);
        return m_resampleFilter
            ? m_resampleFilter->GetOutputPort()
            : nullptr;
    }

    void ClampImageBounds(
        int& x,
        int& y,
        int& z,
        const int dims[3])
    {
        if (!dims) {
            return;
        }
        x = std::max(0, std::min(x, dims[0] - 1));
        y = std::max(0, std::min(y, dims[1] - 1));
        z = std::max(0, std::min(z, dims[2] - 1));
    }
};
