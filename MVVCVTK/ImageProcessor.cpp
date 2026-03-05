#include "ImageProcessor.h"
#include <vtkImageResample.h>

vtkSmartPointer<vtkImageData> ImageProcessor::ApplyDownsampling(vtkImageData* input, int targetDim)
{
    if (!input) return nullptr;

    int dims[3];
    input->GetDimensions(dims);
    int maxDim = std::max({ dims[0], dims[1], dims[2] });

    // 检查是否需要降采样
    if (maxDim <= targetDim) {
        return input; // 不需要处理，直接返回原指针
    }

    // 以最大轴为基准，三轴等比例缩放，保持物理 Bounds 不变
    double factor = static_cast<double>(targetDim) / static_cast<double>(maxDim);

    // --- 执行降采样逻辑 ---
    auto resample = vtkSmartPointer<vtkImageResample>::New();
    resample->SetInputData(input);
    resample->SetAxisMagnificationFactor(0, factor);
    resample->SetAxisMagnificationFactor(1, factor);
    resample->SetAxisMagnificationFactor(2, factor);

    resample->SetInterpolationModeToLinear(); // 线性插值平衡性能与质量
    resample->Update();

    return resample->GetOutput();
}