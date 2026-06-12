#pragma once

// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/Service/OrthogonalCropBackendRouterService.h
// 分类: Service / Backend Router
// 说明: 在 image 与 polydata 两条裁切后端之间做统一分发，屏蔽 UI 层的分支判断。
// =====================================================================
// 路由主链路：
// 1. 根据 preferredDataSource 与当前已绑定输入确定 active data source
// 2. 把默认 request、轻量预览、统计查询和结果执行都收口到同一组入口
// 3. image 路径直接委托给 OrthogonalCropPluginService
// 4. polydata 路径把 request 归一化为 cropData，再生成 implicit function 做 clip
// 5. 两条路径最终都回填到统一的 OrthogonalCropResult / OrthogonalCropStatistics

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

    // 返回当前活跃数据的 model bounds。
    std::array<double, 6> GetActiveModelBounds() const;

    // 构造与当前活跃输入一致的默认 request。
    OrthogonalCropRequest GetDefaultRequest() const;

    // 查询当前请求的统计信息，内部会按数据源分发。
    // 调用方只需要提交统一 request，不需要提前知道自己会落到 image 还是 polydata 后端。
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;

    // 执行当前请求并返回完整结果，内部会按数据源分发。
    // 结果对象会补齐 resolved source / backend，供交互桥和 overlay 直接消费。
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

    // 执行 3D outline guide preview：只归一化 request->cropData 并生成 outline，不跑 2D mask / 3D clip / 统计。
    OrthogonalCropResult GetLightweightPreviewResult(const OrthogonalCropRequest& request) const;

private:
    // polydata 路径统一从 cropData 直接生成 clipped polydata，内部复用 clip 管道。
    vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        const CropDataModel& cropData,
        CropRemovalMode removalMode) const;

    // 读取 image 输入 bounds。
    std::array<double, 6> GetImageBounds() const;

    // 读取 polydata 输入 bounds。
    std::array<double, 6> GetPolyDataBounds() const;

    // 统一构造“输入缺失”统计结果。
    OrthogonalCropStatistics GetMissingInputStatistics() const;

    // 统一构造“输入缺失”执行结果。
    OrthogonalCropResult GetMissingInputResult() const;

    // polydata 路径把 request 归一化为 cropData 的内部 helper。
    bool GetPolyDataCropDataModel(
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message) const;

    // 用已完成的 clipped polydata 回填统计信息，避免重复执行 clip。
    OrthogonalCropStatistics GetPolyDataStatisticsFromClipped(vtkPolyData* clipped) const;

    // image 路径统计接口，最终委托给 image-only plugin。
    OrthogonalCropStatistics GetImageStatistics(const OrthogonalCropRequest& request) const;

    // polydata 路径统计接口，会实际做一次 clip 来估算输出规模。
    OrthogonalCropStatistics GetPolyDataStatistics(const OrthogonalCropRequest& request) const;

    // image 路径完整结果接口。
    OrthogonalCropResult GetImageResult(const OrthogonalCropRequest& request) const;

    // polydata 路径完整结果接口。
    OrthogonalCropResult GetPolyDataResult(const OrthogonalCropRequest& request) const;

    // 组合持有 image-only plugin；router 的 image 分支全部委托给它。
    OrthogonalCropPluginService m_imageService;

    // polydata 输入单独缓存在 router 中，由 polydata 分支直接消费。
    vtkSmartPointer<vtkPolyData> m_inputPolyData;

    // polydata 统计/结果路径复用的 clip filter。
    mutable vtkSmartPointer<vtkTableBasedClipDataSet> m_polyDataClipFilter;

    // polydata 统计/结果路径复用的 geometry filter。
    mutable vtkSmartPointer<vtkGeometryFilter> m_polyDataGeometryFilter;

    // 最近一次 polydata clip 的输入几何快照；命中时可直接复用 clipped 输出。
    mutable CropDataModel m_cachedPolyDataCropData;

    // 最近一次 polydata clip 对应的 removal mode。
    mutable CropRemovalMode m_cachedPolyDataRemovalMode = CropRemovalMode::KeepInside;

    // 最近一次 polydata clip 对应的原始输入指针；输入变更时缓存必须失效。
    mutable vtkPolyData* m_cachedPolyDataInput = nullptr;

    // 最近一次 polydata clip 的输出结果；供 statistics/result 连续调用复用。
    mutable vtkSmartPointer<vtkPolyData> m_cachedClippedPolyData;

    // 当前是否持有可用的 polydata clip 缓存。
    mutable bool m_hasCachedPolyDataClip = false;

    // 外部偏好的数据源；若对应输入不存在，则会自动回退。
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::Auto;
};
