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

// 多条裁切链共用的纯几何算法入口；静态类明确表达纯算法职责，不产生对象生命周期。
class CropGeometryAlgorithm final {
public:
    CropGeometryAlgorithm() = delete;

    static CropMatrixDouble16Array GetIdentityMatrix();

    // 标准裁切盒固定为 [-1, 1]^3；所有请求只携带 boxToInputModelMatrix 作为几何真源。
    static CropBoundsDouble6Array GetCanonicalBounds();

    // 从 active input model 轴对齐 bounds 构造标准盒到 active input model 的矩阵。
    static CropMatrixDouble16Array GetBoxMatrix(
        const CropBoundsDouble6Array& inputModelBounds);
};

class OrthogonalCropAlgorithm {
public:
    OrthogonalCropAlgorithm() = delete;

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
