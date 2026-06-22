#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/OrthogonalCropAlgorithm.h
// 分类: Math / Core Algorithm
// OrthogonalCropAlgorithm.h - 正交裁切独立插件纯算法层
// =====================================================================
// 算法层只消费 request 与输入数据；
// 它先归一化 cropData，再按 request.backend 产出 image preview、image submit 或 polydata preview 结果。
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

    // 直接从 request 获取 image 诊断信息，是 UI 和 service 最常走的便捷入口。
    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // image 算法入口：先校验和归一化，再按 request.backend 填充上层已构造的 result。
    // resultContext 已经带有数据源、后端和交互态，算法层只补 cropData、产物和诊断。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        const OrthogonalCropResult& resultContext,
        std::size_t fallbackAvailableRamBytes = 0);

    // 直接从 request 获取 polydata 诊断信息。
    static OrthogonalCropStatistics GetStatistics(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request);

    // polydata 算法入口：按 request.backend 生成 3D clip preview 结果。
    static OrthogonalCropResult GetResult(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request,
        const OrthogonalCropResult& resultContext);
};
