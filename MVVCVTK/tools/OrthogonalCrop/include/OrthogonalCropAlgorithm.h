#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/OrthogonalCropAlgorithm.h
// 分类: Math / Core Algorithm
// OrthogonalCropAlgorithm.h - 正交裁切独立插件纯算法层
// =====================================================================
// 核心执行链：
// 1. 由 request 归一化得到 CropDataModel
// 2. 基于 cropData 做 bounds 校验与执行诊断
// 3. 按 executionMode 分流到 image preview artifact 或 image submit extract
// 4. 把几何数据、image/polydata 产物、诊断信息与交互态重新组装为统一结果模型

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>
#include <string>

class OrthogonalCropAlgorithm {
public:
    // 校验一组数据 model bounds 与目标裁切 model bounds 是否满足执行前提。
    static bool GetBoundsAreValid(
        const std::array<double, 6>& dataModelBounds,
        const std::array<double, 6>& cropModelBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 把 request 归一化为可执行的 CropDataModel。
    static bool GetCropDataModel(
        const std::array<double, 6>& dataModelBounds,
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
    // request 只携带 boxToModelMatrix；算法层会派生 AABB 并折叠成统一 cropData。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);
};
