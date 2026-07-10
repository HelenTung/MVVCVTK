#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Algorithms/OrthogonalCropAlgorithm.h
// 分类: Math / Core Algorithm
// OrthogonalCropAlgorithm.h - 正交裁切独立插件纯算法层
// =====================================================================
// 算法层只消费 request 与输入数据；
// 它先归一化 cropData，再按 request.geometryType / request.operation / request.dataSource
// 产出体渲染预览、图像提交或网格预览结果。
// 统一结果模型承载几何、产物和诊断，避免 service / UI 分散理解算法细节。

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <cstddef>

class OrthogonalCropAlgorithm {
public:
    // image 输入入口只接受 Box + Preview + VolumeData 或 Box + Submit + ImageData。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // polydata 输入入口只接受 Box + Preview + PolyData 请求。
    static OrthogonalCropResult GetResult(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request);
};
