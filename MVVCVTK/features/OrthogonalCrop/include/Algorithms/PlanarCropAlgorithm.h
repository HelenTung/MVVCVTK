#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Algorithms/PlanarCropAlgorithm.h
// 分类: Math / Core Algorithm
// 说明: 平面裁切算法入口；只消费 request 与输入数据，不持有 UI 或渲染状态。
// =====================================================================

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <cstddef>

class PlanarCropAlgorithm {
public:
    PlanarCropAlgorithm() = delete;

    // image 输入入口只接受 Plane + Preview + VolumeData 或 Plane + Submit + ImageData。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // polydata 输入入口只接受 Plane + Preview + PolyData 请求。
    static OrthogonalCropResult GetResult(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request);

private:
    class Impl;
};
