#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Interaction/CropPlaneWidget.h
// 分类: Service / Widget Controller
// 说明: 管理 vtkImplicitPlaneWidget2，把 VTK 平面事件翻译为裁切模块状态回调。
// =====================================================================

#include "OrthogonalCropTypes.h"

#include <array>
#include <functional>
#include <memory>

class vtkRenderWindowInteractor;

// 只管理平面 widget 生命周期、平面参数与交互事件，不关心裁切算法与结果投递。
class CropPlaneWidget {
public:
    // 同步报告 world 原点、单位法线与交互阶段；回调只观察快照，不拥有 widget。
    using PlaneCallback = std::function<void(
        const CropVectorDouble3Array& worldOrigin,
        const CropVectorDouble3Array& worldNormal,
        CropInteractionPhase phase)>;

    CropPlaneWidget();
    ~CropPlaneWidget();

    CropPlaneWidget(const CropPlaneWidget&) = delete;
    CropPlaneWidget& operator=(const CropPlaneWidget&) = delete;
    CropPlaneWidget(CropPlaneWidget&&) noexcept;
    CropPlaneWidget& operator=(CropPlaneWidget&&) noexcept;

    // 绑定非拥有的 interactor；调用方须保证它在 widget 启用期间有效。
    void SetInteractor(vtkRenderWindowInteractor* interactor);

    // 设置参考 world AABB；合法输入会把当前平面 origin 重置为 bounds 中心，启用时以其合法性作为门槛。
    void SetReferenceWorldBounds(const std::array<double, 6>& worldBounds);

    // 直接设置当前 widget world 平面。
    void SetWidgetWorldPlane(
        const CropVectorDouble3Array& worldOrigin,
        const CropVectorDouble3Array& worldNormal,
        const std::array<double, 2>& worldHalfExtents);

    // 读取当前缓存的 world 平面。
    bool GetCurrentWorldPlane(
        CropVectorDouble3Array& worldOrigin,
        CropVectorDouble3Array& worldNormal) const;

    // 设置 world 平面变化回调。
    void SetPlaneCallback(PlaneCallback callback);

    // 开关 widget；打开时补 observer，并按当前 origin 与 halfExtents 构造 PlaceWidget 可视范围。
    // reference bounds 只参与启用/放置前合法性检查，不直接作为 PlaceWidget 的 bounds。
    bool SetEnabled(bool isEnabled);

    // 查询当前 widget 是否开启。
    bool GetEnabled() const;

private:
    class Impl;
    // 唯一拥有 VTK widget、representation、callback command、observer tag 与 world 平面缓存。
    std::unique_ptr<Impl> m_impl;
};
