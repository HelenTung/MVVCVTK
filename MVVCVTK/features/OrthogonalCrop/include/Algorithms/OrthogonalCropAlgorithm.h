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

#include <array>
#include <cstddef>
#include <string>

class CropRouter;

class OrthogonalCropAlgorithm {
public:
    // 校验 active input model bounds 与目标裁切 input model bounds 是否满足执行前提。
    static bool GetBoundsAreValid(
        const std::array<double, 6>& inputModelBounds,
        const std::array<double, 6>& cropInputModelBounds,
        CropFailure& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 把 request 归一化为可执行的 CropDataModel。
    static bool GetCropDataModel(
        const std::array<double, 6>& inputModelBounds,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        CropFailure& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 生成 box 3D outline preview polydata，供 overlay 和 3D 预览复用。
    static vtkSmartPointer<vtkPolyData> GetOutlinePolyData(const CropDataModel& cropData);

private:
    // router 是三元组分发边界；算法执行入口只给 router 调用，避免外部绕过路由组合。
    friend class CropRouter;

    // image / volume 共用 image 输入入口；算法只补 cropData、产物和诊断。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // polydata 输入入口只处理 router 放行的 Box + Preview + PolyData 请求。
    static OrthogonalCropResult GetResult(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request);
};
