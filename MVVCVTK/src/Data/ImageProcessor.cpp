#include "ImageProcessor.h"

vtkSmartPointer<vtkImageResample> ImageProcessor::GetDownsampledImage(vtkImageData* input, int targetDim)
{
    if (!input) return nullptr;

    int dims[3];
    input->GetDimensions(dims);
    int maxDim = std::max({ dims[0], dims[1], dims[2] });
    auto resample = vtkSmartPointer<vtkImageResample>::New();
    // 无需降采样时仍返回统一的 resample 管线，只把三轴倍率设为 1.0。
    if (maxDim <= targetDim) {
        resample->SetInputData(input);
		resample->SetAxisMagnificationFactor(0, 1.0);
		resample->SetAxisMagnificationFactor(1, 1.0);
        resample->SetAxisMagnificationFactor(2, 1.0);
        return resample; 
    }

    // 以最大轴为基准，三轴等比例缩放，保持物理 Bounds 不变
    double factor = static_cast<double>(targetDim) / static_cast<double>(maxDim);

    // 以同一倍率缩放三轴，避免改变体素的长宽高比例。
    resample->SetInputData(input);
    resample->SetAxisMagnificationFactor(0, factor);
    resample->SetAxisMagnificationFactor(1, factor);
    resample->SetAxisMagnificationFactor(2, factor);

    resample->SetInterpolationModeToLinear(); // 线性插值平衡性能与质量
    //resample->Update();

    return resample;
}

vtkSmartPointer<vtkImageResample>
ImageProcessor::GetDownsampledMask(
    vtkImageData* input,
    int targetDim)
{
    auto resample = GetDownsampledImage(
        input, targetDim);
    if (resample) {
        resample->SetInterpolationModeToNearestNeighbor();
    }
    return resample;
}
