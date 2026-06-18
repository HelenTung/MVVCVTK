#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/OrthogonalCropAlgorithm.h
// 分类: Math / Core Algorithm
// OrthogonalCropAlgorithm.h - 正交裁切独立插件纯算法层
// =====================================================================
// 算法层只消费 request 与输入数据；
// 它先归一化 cropData，再按 executionMode 产出 preview artifact 或 image submit 结果。
// 统一结果模型承载几何、产物、诊断和交互态，避免 service / UI 分散理解算法细节。

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>
#include <string>

class OrthogonalCropAlgorithm {
public:
    // 校验 active input model bounds 与目标裁切 input model bounds 是否满足执行前提。
    static bool GetBoundsAreValid(
        const std::array<double, 6>& inputModelBounds,
        const std::array<double, 6>& cropInputModelBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 把 request 归一化为可执行的 CropDataModel。
    static bool GetCropDataModel(
        const std::array<double, 6>& inputModelBounds,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 生成 box 3D outline preview polydata，供 overlay 和 3D 预览复用。
    static vtkSmartPointer<vtkPolyData> GetOutlinePolyData(const CropDataModel& cropData);

    // 直接从 request 获取诊断信息，是 UI 和 service 最常走的便捷入口。
    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // 算法总入口：先校验和归一化，再分发到 preview artifact / image submit 两条执行链。
    // request 只携带 boxToInputModelMatrix；算法层会派生 AABB 并折叠成统一 cropData。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);
};
