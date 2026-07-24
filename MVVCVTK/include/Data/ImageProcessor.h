#pragma once
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkImageResample.h>
class ImageProcessor {
public:
    /**
     * @brief 构造连接到 input 的重采样管线；最大维度超过 targetDim 时三轴等比例降采样。
     * @param input 非拥有输入图像；为空时返回 nullptr
     * @param targetDim 目标最大维度的体素数量（默认 766）；必须大于 0，当前实现不校验该前置条件
     * @return 新建的 vtkImageResample filter；无需降采样时三轴倍率均为 1.0，不直接返回 vtkImageData
     */
    static vtkSmartPointer<vtkImageResample> GetDownsampledImage(vtkImageData* input, int targetDim = 766);
    // 二值有效域必须保持 0/255，缩放时固定使用最近邻插值。
    static vtkSmartPointer<vtkImageResample> GetDownsampledMask(
        vtkImageData* input,
        int targetDim = 766);
};
