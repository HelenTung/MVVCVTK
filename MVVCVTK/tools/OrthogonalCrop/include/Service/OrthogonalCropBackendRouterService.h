#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/Service/OrthogonalCropBackendRouterService.h
// 分类: Service / Backend Router
// 说明: 在 image 与 polydata 两条裁切后端之间做统一分发，屏蔽 UI 层的分支判断。
// =====================================================================
// Router 只根据 request.dataSource / request.backend 执行已经决定好的后端目标；
// 默认 request 只是当前 active input 的几何模板，正式目标由 bridge 或调用方写回 request。
// image 路径委托 OrthogonalCropPluginService，polydata 路径在 Router 内归一化并执行 clip，
// 两条路径最终都回填统一的 OrthogonalCropResult / OrthogonalCropStatistics。

#include "OrthogonalCropPluginService.h"

#include <vtkImplicitFunction.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <string>

class vtkGeometryFilter;
class vtkTableBasedClipDataSet;

class OrthogonalCropBackendRouterService {
public:
    // 绑定 image 输入，供交互桥或 main 在数据加载后注入。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);

    // 返回当前 router 绑定的 image 输入。
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 绑定 polydata 输入，同时清空 clip 缓存。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 返回当前 router 绑定的 polydata 输入。
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;

    // 设置期望优先使用的数据源，Auto 时会按当前可用输入自动回退。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 返回本次请求实际会走到的活跃数据源。
    OrthogonalCropDataSource GetActiveDataSource() const;

    // 返回当前活跃数据的 input model bounds。
    std::array<double, 6> GetActiveInputModelBounds() const;

    // 构造与当前活跃输入一致的默认 request 模板。
    // 它只提供初始几何和兜底目标，正式执行前仍由 bridge 写入本次业务选择。
    OrthogonalCropRequest GetDefaultRequest() const;

    // 查询当前请求的诊断信息，按 request 已指定的数据源和后端分发。
    // 调用方必须先把业务选择写进 request，router 只负责校验和转发。
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;

    // 执行当前请求并填充调用方给定的结果上下文，按 request 已指定的数据源和后端分发。
    // resultContext 已携带 resolved source / backend，router 只校验和转发，不再决定结果身份。
    OrthogonalCropResult GetResult(
        const OrthogonalCropRequest& request,
        const OrthogonalCropResult& resultContext) const;

private:
    // polydata 路径统一从 cropData 直接生成 clipped polydata，内部复用 clip 管道。
    vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        const CropDataModel& cropData,
        CropRemovalMode removalMode) const;

    // 读取 image model bounds。
    std::array<double, 6> GetImageModelBounds() const;

    // 读取 polyData input model bounds。
    std::array<double, 6> GetPolyDataInputModelBounds() const;

    // 统一构造“输入缺失”诊断结果，并按目标后端保留准确失败原因。
    OrthogonalCropStatistics GetMissingInputStatistics(
        OrthogonalCropDataSource dataSource = OrthogonalCropDataSource::Auto,
        OrthogonalCropBackend backend = OrthogonalCropBackend::None) const;

    // 在调用方结果上下文上回填“输入缺失”执行结果，并按目标后端保留准确失败原因。
    OrthogonalCropResult GetMissingInputResult(
        const OrthogonalCropResult& resultContext,
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::InputImageMissing) const;

    // polydata 路径把 request 归一化为 cropData 的内部 helper。
    bool GetPolyDataCropDataModel(
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message) const;

    // 用已完成的 clipped polydata 回填诊断信息，避免重复执行 clip。
    OrthogonalCropStatistics GetPolyDataStatisticsFromClipped(vtkPolyData* clipped) const;

    // polydata 路径诊断接口，会实际做一次 clip 来确认可执行性。
    OrthogonalCropStatistics GetPolyDataStatistics(const OrthogonalCropRequest& request) const;

    // polydata 路径完整结果接口，在既有 result 上下文里补齐 clip 产物。
    OrthogonalCropResult GetPolyDataResult(
        const OrthogonalCropRequest& request,
        const OrthogonalCropResult& resultContext) const;

    // 组合持有 image-only plugin；router 的 image 分支全部委托给它。
    OrthogonalCropPluginService m_imageService;

    // polydata 输入单独缓存在 router 中，由 polydata 分支直接消费。
    vtkSmartPointer<vtkPolyData> m_inputPolyData;

    // polydata 诊断/结果路径复用的 clip filter。
    mutable vtkSmartPointer<vtkTableBasedClipDataSet> m_polyDataClipFilter;

    // polydata 诊断/结果路径复用的 geometry filter。
    mutable vtkSmartPointer<vtkGeometryFilter> m_polyDataGeometryFilter;

    // 最近一次 polydata clip 的输入几何快照；命中时可直接复用 clipped 输出。
    mutable CropDataModel m_cachedPolyDataCropData;

    // 最近一次 polydata clip 对应的 removal mode。
    mutable CropRemovalMode m_cachedPolyDataRemovalMode = CropRemovalMode::KeepInside;

    // 最近一次 polydata clip 对应的原始输入指针；输入变更时缓存必须失效。
    mutable vtkPolyData* m_cachedPolyDataInput = nullptr;

    // 最近一次 polydata clip 的输出结果；供 diagnostics/result 连续调用复用。
    mutable vtkSmartPointer<vtkPolyData> m_cachedClippedPolyData;

    // 当前是否持有可用的 polydata clip 缓存。
    mutable bool m_hasCachedPolyDataClip = false;

    // 外部偏好的数据源；若对应输入不存在，则会自动回退。
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::Auto;
};
