#include "ImageProcessor.h"
#include <vtkImageResample.h>

vtkSmartPointer<vtkImageData> ImageProcessor::ApplyDownsampling(vtkImageData* input, int targetDim)
{
    if (!input) return nullptr;

    int dims[3];
    input->GetDimensions(dims);

    // 检查是否需要降采样
    if (dims[0] <= targetDim && dims[1] <= targetDim && dims[2] <= targetDim) {
        return input; // 不需要处理，直接返回原指针
    }

    // --- 执行降采样逻辑 ---
    auto resample = vtkSmartPointer<vtkImageResample>::New();
    resample->SetInputData(input);

    // 计算各轴缩放因子
    // vtkImageResample 会自动调整 Spacing，确保物理空间 Bounds 不变
    double factorX = static_cast<double>(targetDim) / static_cast<double>(dims[0]);
    double factorY = static_cast<double>(targetDim) / static_cast<double>(dims[1]);
    double factorZ = static_cast<double>(targetDim) / static_cast<double>(dims[2]);

    resample->SetAxisMagnificationFactor(0, factorX);
    resample->SetAxisMagnificationFactor(1, factorY);
    resample->SetAxisMagnificationFactor(2, factorZ);

    resample->SetInterpolationModeToLinear(); // 线性插值平衡性能与质量
    resample->Update();

    return resample->GetOutput();
}