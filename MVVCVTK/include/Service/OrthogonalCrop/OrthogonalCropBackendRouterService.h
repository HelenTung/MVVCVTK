#pragma once

// =====================================================================
// Path: MVVCVTK/include/Service/OrthogonalCrop/OrthogonalCropBackendRouterService.h
// 分类: Service / Backend Router
// 说明: 在 image 与 polydata 两条裁切后端之间做统一分发，屏蔽 UI 层的分支判断。
// =====================================================================
// 路由主链路：
// 1. 根据 preferredDataSource 与当前已绑定输入确定 active data source
// 2. 把默认 request、统计查询和结果执行都收口到同一组入口
// 3. image 路径直接委托给 OrthogonalCropPluginService
// 4. polydata 路径把 request 归一化为 cropData，再生成 implicit function 做 clip
// 5. 两条路径最终都回填到统一的 OrthogonalCropResult / OrthogonalCropStatistics

#include "OrthogonalCrop/OrthogonalCropPluginService.h"

#include <vtkImplicitFunction.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <string>

class OrthogonalCropBackendRouterService {
public:
    // 预初始化阶段绑定 image 输入，供交互桥或 main 在数据加载后注入。
    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image);

    // 对外保留的 image 输入设置接口，行为与 PreInit 版本一致。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);

    // 返回当前 router 绑定的 image 输入。
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 预初始化阶段绑定 polydata 输入。
    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 对外保留的 polydata 输入设置接口。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 返回当前 router 绑定的 polydata 输入。
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;

    // 设置期望优先使用的数据源，Auto 时会按当前可用输入自动回退。
    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 对外保留的首选数据源设置接口。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 返回本次请求实际会走到的活跃数据源。
    OrthogonalCropDataSource GetActiveDataSource() const;

    // 返回当前活跃输入的原始空间 bounds。
    std::array<double, 6> GetActiveInputBounds() const;

    // 构造与当前活跃输入一致的默认 request。
    OrthogonalCropRequest GetDefaultRequest() const;

    // 查询当前请求的统计信息，内部会按数据源分发。
    // 调用方只需要提交统一 request，不需要提前知道自己会落到 image 还是 polydata 后端。
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;

    // 执行当前请求并返回完整结果，内部会按数据源分发。
    // 结果对象会补齐 resolved source / backend，供交互桥和 overlay 直接消费。
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

    // 对已有 VTK implicit function 做通用 polydata clip。
    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        vtkImplicitFunction* clipFunction,
        CropRemovalMode removalMode);

    // 从 cropData 推导 implicit function，再执行 polydata clip。
    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        const CropDataModel& cropData,
        CropRemovalMode removalMode);

protected:
    // 把 cropData 转换成具体的 VTK implicit function，供 polydata 路径复用。
    static vtkSmartPointer<vtkImplicitFunction> GetClipFunction(const CropDataModel& cropData);

private:
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

    // 外部偏好的数据源；若对应输入不存在，则会自动回退。
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::Auto;
};
