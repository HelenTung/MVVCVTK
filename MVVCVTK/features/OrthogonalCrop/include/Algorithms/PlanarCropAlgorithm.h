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
    // 平面中心经 image physical-to-continuous-index 变换后的 [i, j, k] 坐标。
    std::array<double, 3> planeIndex = { 0.0, 0.0, 0.0 };
    // 单位 i/j/k index 增量在平面单位法线上的有符号 physical 投影。
    double iStep = 0.0;
    double jStep = 0.0;
    double kStep = 0.0;
    // side = (i-planeIndex[0])*iStep + (j-planeIndex[1])*jStep + (k-planeIndex[2])*kStep；
    // epsilon 按三轴完整遍历幅度缩放，|side| 不超过它时回退到精确 physical-point 判断。
    double boundaryEpsilon = 1e-8;
};

struct PlanarSubmitImages {
    // 与输入结构、标量类型和分量数一致；保留 voxel 复制原值，移除 voxel 写入标量范围最小值。
    vtkSmartPointer<vtkImageData> submitImage;
    // 与 submitImage 对齐的单分量 VTK_UNSIGNED_CHAR mask：255 为保留，0 为移除。
    vtkSmartPointer<vtkImageData> maskImage;
};

enum class PlanarRowSide {
    // 整行都在法线正侧，可按 Inside 批量处理。
    NormalSide,
    // 整行都在法线负侧，可按 Outside 批量处理。
    OppositeSide,
    // 行穿过 epsilon 带，必须逐 voxel 判侧。
    Mixed
};

class PlanarCropAlgorithm {
public:
    // image 输入入口只接受 Plane + Preview + VolumeData 或 Plane + Submit + ImageData。
    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    // polydata 输入入口只接受 Plane + Preview + PolyData 请求。
    static OrthogonalCropResult GetResult(
        vtkPolyData* polyData,
        const OrthogonalCropRequest& request);
};
