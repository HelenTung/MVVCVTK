#pragma once
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkImageResample.h>
#include <array>
class vtkAlgorithmOutput;
class ImageProcessor {
public:
    // dimensionRatio 是相对原始三轴 dimensions 的统一线性比例，范围为 (0,1]。
    static vtkSmartPointer<vtkImageResample> CreateScaledImage(
        vtkImageData* input,
        double dimensionRatio,
        vtkAlgorithmOutput* inputPort = nullptr);
    // targetDimensions 由加载期 LOD 计划一次确定；producer 不再自行选择挡位尺寸。
    static vtkSmartPointer<vtkImageResample> CreateScaledImage(
        vtkImageData* input,
        const std::array<int, 3>& targetDimensions,
        vtkAlgorithmOutput* inputPort = nullptr);
    // 二值有效域必须保持 0/255，缩放时固定使用最近邻插值。
    static vtkSmartPointer<vtkImageResample> CreateScaledMask(
        vtkImageData* input,
        double dimensionRatio);
    static vtkSmartPointer<vtkImageResample> CreateScaledMask(
        vtkImageData* input,
        const std::array<int, 3>& targetDimensions);
};
