#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Routing/CropRouter.h
// 分类: Service / Backend Router
// 说明: 在基于图像输入与网格输入的裁切后端之间做统一分发，屏蔽 UI 层的分支判断。
// =====================================================================
// Router 只根据 request.geometryType / request.operation / request.dataSource 执行已经决定好的目标；
// 默认 request 只是当前 active input 的几何模板，正式目标由 bridge 或调用方写回 request。
// 图像 / 体渲染 / 网格的数据处理都由 OrthogonalCropAlgorithm 执行，router 只做输入选择和错误边界。

#include "OrthogonalCropTypes.h"

#include <vtkSmartPointer.h>

#include <array>
#include <memory>

class vtkImageData;
class vtkPolyData;

class CropRouter {
public:
    CropRouter();
    ~CropRouter();

    CropRouter(const CropRouter&) = delete;
    CropRouter& operator=(const CropRouter&) = delete;
    CropRouter(CropRouter&&) noexcept;
    CropRouter& operator=(CropRouter&&) noexcept;

    // 绑定 image 输入；router 通过 vtkSmartPointer 保持共享引用，直到替换或自身销毁。
    void SetInputImage(vtkSmartPointer<vtkImageData> image);

    // 只报告 image 输入是否可用，不向 router 外泄漏可变 VTK 对象。
    bool GetImageReady() const;

    // 绑定 polydata 输入；router 通过 vtkSmartPointer 保持共享引用，ClearInputPolyData 显式释放本侧引用。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 清空 polydata 输入，避免临时 preview 缓存影响后续 active input 判断。
    void ClearInputPolyData();

    // 只报告 polydata 输入是否可用，不向 router 外泄漏可变 VTK 对象。
    bool GetPolyDataReady() const;

    // 设置期望优先使用的数据源；若对应输入不存在，则按当前可用输入回退。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 返回本次请求实际会走到的活跃数据源。
    OrthogonalCropDataSource GetActiveDataSource() const;

    // 返回当前活跃数据的 input model bounds。
    std::array<double, 6> GetActiveInputModelBounds() const;

    // 用当前 active dataSource/bounds 构造 Box + KeepInside + Preview 模板。
    // ImageData 不支持 Preview；正式执行前 bridge 必须按业务选择改为 Submit，或切换到可预览数据源。
    OrthogonalCropRequest GetDefaultRequest() const;

    // 执行当前请求；request 是本次裁切身份和几何的唯一输入来源。
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

private:
    class Impl;
    // 唯一拥有 router 输入缓存和数据源选择状态；随 CropRouter 移动并在析构时统一释放。
    std::unique_ptr<Impl> m_impl;
};
