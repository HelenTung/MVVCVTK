#pragma once

// =====================================================================
// Path: MVVCVTK/include/Math/OrthogonalCrop/OrthogonalCropAlgorithm.h
// 分类: Math / Core Algorithm
// OrthogonalCropAlgorithm.h - 正交裁切独立插件纯算法层
// =====================================================================

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>
#include <string>

class OrthogonalCropAlgorithm {
public:
    // 校验一组输入 bounds 与目标裁切 bounds 是否满足执行前提。
    static bool GetBoundsAreValid(
        const std::array<double, 6>& inputBounds,
        const std::array<double, 6>& rasBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 直接从 image 读取输入 bounds，再复用通用校验逻辑。
    static bool GetBoundsAreValid(
        vtkImageData* image,
        const std::array<double, 6>& rasBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 把 request 归一化为可执行的 CropDataModel。
    static bool GetCropDataModel(
        const std::array<double, 6>& inputBounds,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // image 入口版本的 request 归一化，供 plugin 和 router 直接调用。
    static bool GetCropDataModel(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    // 把物理空间 bounds 吸附到当前 image 的 IJK 体素范围。
    static std::array<int, 6> GetSnappedVoxelBounds(vtkImageData* image, const CropDataModel& cropData);

    // 生成裁切盒 outline，供 overlay 和 3D 预览复用。
    static vtkSmartPointer<vtkPolyData> GetOutlinePolyData(const CropDataModel& cropData);

    // 基于已归一化 cropData 估算输出规模、失败原因与物理裁切可执行性。
    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const CropDataModel& cropData,
        CropRemovalMode removalMode,
        CropExecutionMode executionMode,
        std::size_t availableRamBytes = 0);

    // 直接从 request 获取统计信息，是 UI 和 service 最常走的便捷入口。
    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // 生成虚拟裁切结果：mask、outline、统计以及交互态快照。
    static OrthogonalCropResult GetVirtualCropResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const CropStateModel& cropState,
        CropRemovalMode removalMode);

    // 生成物理裁切结果：derived image、更新后的 cropData 与 offset matrix。
    static OrthogonalCropResult GetPhysicalCropResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const CropStateModel& cropState,
        CropRemovalMode removalMode,
        std::size_t availableRamBytes = 0);

    // 算法总入口：先校验和归一化，再分发到 virtual / physical 两条执行链。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);
};
