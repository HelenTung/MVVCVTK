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

#include <cstddef>

class OrthogonalCropBackendRouterService;

class PlanarCropAlgorithm {
public:
    // 生成平面预览矩形轮廓，供 overlay 和 3D 预览共用。
    static vtkSmartPointer<vtkPolyData> GetOutlinePolyData(
        const CropDataModel& cropData,
        const CropBoundsDouble6Array& inputModelBounds);

private:
    // router 是三元组分发边界；算法执行入口只给 router 调用，避免外部绕过路由组合。
    friend class OrthogonalCropBackendRouterService;

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
