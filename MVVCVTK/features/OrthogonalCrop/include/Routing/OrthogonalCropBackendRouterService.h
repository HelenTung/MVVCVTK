#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Routing/OrthogonalCropBackendRouterService.h
// 分类: Service / Backend Router
// 说明: 在基于图像输入与网格输入的裁切后端之间做统一分发，屏蔽 UI 层的分支判断。
// =====================================================================
// Router 只根据 request.geometryType / request.operation / request.dataSource 执行已经决定好的目标；
// 默认 request 只是当前 active input 的几何模板，正式目标由 bridge 或调用方写回 request。
// 图像 / 体渲染 / 网格的数据处理都由 OrthogonalCropAlgorithm 执行，router 只做输入选择和错误边界。

#include "Algorithms/OrthogonalCropAlgorithm.h"
#include "Algorithms/PlanarCropAlgorithm.h"

#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>

class OrthogonalCropBackendRouterService {
public:
    // 绑定 image 输入，供交互桥或 main 在数据加载后注入。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);

    // 返回当前 router 绑定的 image 输入。
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 绑定本次 polydata 输入；来源生命周期由调用方负责。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 清空 polydata 输入，避免临时 preview 缓存影响后续 active input 判断。
    void ClearInputPolyData();

    // 返回当前 router 绑定的 polydata 输入。
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;

    // 设置期望优先使用的数据源；若对应输入不存在，则按当前可用输入回退。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 返回本次请求实际会走到的活跃数据源。
    OrthogonalCropDataSource GetActiveDataSource() const;

    // 返回当前活跃数据的 input model bounds。
    std::array<double, 6> GetActiveInputModelBounds() const;

    // 构造与当前活跃输入一致的默认 request 模板。
    // 它只提供初始几何和兜底目标，正式执行前仍由 bridge 写入本次业务选择。
    OrthogonalCropRequest GetDefaultRequest() const;

    // 执行当前请求；request 是本次裁切身份和几何的唯一输入来源。
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

private:
    // Box 路由集中处理动作 + 数据源组合。
    OrthogonalCropResult GetBoxResult(const OrthogonalCropRequest& request) const;

    // Plane 路由集中处理动作 + 数据源组合，实际执行交给 PlanarCropAlgorithm。
    OrthogonalCropResult GetPlaneResult(const OrthogonalCropRequest& request) const;

    // 读取 image model bounds。
    std::array<double, 6> GetImageModelBounds() const;

    // 读取 polyData input model bounds。
    std::array<double, 6> GetPolyDataInputModelBounds() const;

    // 在调用方结果上下文上回填 router 层失败结果，并保留请求三元组。
    OrthogonalCropResult GetRouterFailureResult(
        const OrthogonalCropRequest& request,
        OrthogonalCropFailureReason failureReason,
        const std::string& message) const;

    // 查询系统当前可用物理内存，供 image submit 估算使用。
    std::size_t GetSystemAvailableRamBytes() const;

    // image 输入由 router 统一持有，实际处理直接分发到算法层。
    vtkSmartPointer<vtkImageData> m_inputImage;

    // polydata 输入由调用方按本次请求绑定；router 不判断来源和生命周期。
    vtkSmartPointer<vtkPolyData> m_inputPolyData;

    // 外部偏好的数据源；若对应输入不存在，则会自动回退。
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::ImageData;
};
