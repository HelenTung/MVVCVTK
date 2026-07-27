#include "ImageProcessor.h"

#include <algorithm>

vtkSmartPointer<vtkImageResample> ImageProcessor::GetDownsampledImage(
    vtkImageData* input,
    int targetDim,
    vtkAlgorithmOutput* inputPort)
{
    if (!input || targetDim <= 0) return nullptr;

    int dims[3];
    input->GetDimensions(dims);
    const int maxDim = std::max({ dims[0], dims[1], dims[2] });
    if (maxDim <= 0) return nullptr;
    auto resample = vtkSmartPointer<vtkImageResample>::New();
    resample->SetInterpolationModeToLinear();
    // 无需降采样时仍返回统一的 resample 管线，只把三轴倍率设为 1.0。
    if (maxDim <= targetDim) {
        if (inputPort) resample->SetInputConnection(inputPort);
        else resample->SetInputData(input);
		resample->SetAxisMagnificationFactor(0, 1.0);
		resample->SetAxisMagnificationFactor(1, 1.0);
        resample->SetAxisMagnificationFactor(2, 1.0);
        return resample; 
    }

    // 以最大轴为基准，三轴等比例缩放，保持物理 Bounds 不变
    const double factor = static_cast<double>(targetDim) / static_cast<double>(maxDim);

    // 以同一倍率缩放三轴，避免改变体素的长宽高比例。
    if (inputPort) resample->SetInputConnection(inputPort);
    else resample->SetInputData(input);
    resample->SetAxisMagnificationFactor(0, factor);
    resample->SetAxisMagnificationFactor(1, factor);
    resample->SetAxisMagnificationFactor(2, factor);

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
