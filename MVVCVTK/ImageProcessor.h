#pragma once
#include <vtkSmartPointer.h>
#include <vtkImageData.h>

class ImageProcessor {
public:
    /**
     * @brief 如果图像尺寸超过 targetDim，则对其进行降采样，否则返回原图。
     * @param input 输入图像
     * @param targetDim 目标最大维度的体素数量（默认 766）
     * @return 处理后的图像（如果是原图则引用计数+1，如果是新图则是新对象）
     */
    static vtkSmartPointer<vtkImageData> ApplyDownsampling(vtkImageData* input, int targetDim = 766);
};