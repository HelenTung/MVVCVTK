#include "ImageProcessor.h"

#include <algorithm>
#include <cmath>

vtkSmartPointer<vtkImageResample> ImageProcessor::CreateScaledImage(
    vtkImageData* input,
    const double dimensionRatio,
    vtkAlgorithmOutput* inputPort)
{
    if (!input
        || !std::isfinite(dimensionRatio)
        || dimensionRatio <= 0.0
        || dimensionRatio > 1.0) {
        return nullptr;
    }

    int sourceDimensions[3]{};
    input->GetDimensions(sourceDimensions);
    std::array<int, 3> targetDimensions{};
    for (std::size_t axis = 0; axis < targetDimensions.size(); ++axis) {
        if (sourceDimensions[axis] <= 0) return nullptr;
        targetDimensions[axis] = std::max(
            1,
            static_cast<int>(std::ceil(
                static_cast<double>(sourceDimensions[axis])
                * dimensionRatio)));
    }
    return CreateScaledImage(input, targetDimensions, inputPort);
}

vtkSmartPointer<vtkImageResample> ImageProcessor::CreateScaledImage(
    vtkImageData* input,
    const std::array<int, 3>& targetDimensions,
    vtkAlgorithmOutput* inputPort)
{
    if (!input) return nullptr;
    int sourceDimensions[3]{};
    input->GetDimensions(sourceDimensions);
    for (std::size_t axis = 0; axis < targetDimensions.size(); ++axis) {
        if (sourceDimensions[axis] <= 0
            || targetDimensions[axis] <= 0
            || targetDimensions[axis] > sourceDimensions[axis]) {
            return nullptr;
        }
    }

    auto resample = vtkSmartPointer<vtkImageResample>::New();
    resample->SetInterpolationModeToLinear();
    // 每个计划尺寸都相对原始输入计算，禁止基于上一 LOD 累积降采样。
    if (inputPort) resample->SetInputConnection(inputPort);
    else resample->SetInputData(input);
    for (std::size_t axis = 0; axis < targetDimensions.size(); ++axis) {
        resample->SetAxisMagnificationFactor(
            static_cast<int>(axis),
            static_cast<double>(targetDimensions[axis])
                / static_cast<double>(sourceDimensions[axis]));
    }
    return resample;
}

vtkSmartPointer<vtkImageResample>
ImageProcessor::CreateScaledMask(
    vtkImageData* input,
    const double dimensionRatio)
{
    auto resample = CreateScaledImage(input, dimensionRatio);
    if (resample) {
        resample->SetInterpolationModeToNearestNeighbor();
    }
    return resample;
}

vtkSmartPointer<vtkImageResample>
ImageProcessor::CreateScaledMask(
    vtkImageData* input,
    const std::array<int, 3>& targetDimensions)
{
    auto resample = CreateScaledImage(input, targetDimensions);
    if (resample) {
        resample->SetInterpolationModeToNearestNeighbor();
    }
    return resample;
}
