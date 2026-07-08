#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Algorithms/PlanarCropAlgorithm.h
// 分类: Math / Core Algorithm
// 说明: 平面裁切算法入口；只消费 request 与输入数据，不持有 UI 或渲染状态。
// =====================================================================

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>

struct PlanarVoxelStep {
    std::array<double, 3> planeIndex = { 0.0, 0.0, 0.0 };
    double iStep = 0.0;
    double jStep = 0.0;
    double kStep = 0.0;
    double boundaryEpsilon = 1e-8;
};

struct PlanarSubmitImages {
    vtkSmartPointer<vtkImageData> submitImage;
    vtkSmartPointer<vtkImageData> maskImage;
};

enum class PlanarRowSide {
    NormalSide,
    OppositeSide,
    Mixed
};

class PlanarCropAlgorithm {
public:
    // image / volume 共用 image 输入入口；preview 只返回几何快照，submit 返回 image 与 mask。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // polydata 输入入口只处理 router 放行的 Plane + Preview + PolyData 请求。
    static OrthogonalCropResult GetResult(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request);
};
