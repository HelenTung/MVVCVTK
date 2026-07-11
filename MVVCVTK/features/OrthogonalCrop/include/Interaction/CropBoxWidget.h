#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Interaction/CropBoxWidget.h
// 分类: Service / Widget Controller
// 说明: 管理 vtkBoxWidget2 与 box representation，把 VTK 事件翻译为裁切模块状态回调。
// =====================================================================
// 控制器只维护 widget 生命周期、bounds 和交互阶段；
// 它不生成 request、不执行算法、不刷新 preview，让交互桥基于纯状态决定何时刷新裁切预览。

#include "OrthogonalCropTypes.h"

#include <array>
#include <functional>
#include <memory>

class vtkRenderWindowInteractor;

// 只管理 widget 生命周期、bounds 与交互事件，不关心裁切算法与结果投递。
class CropBoxWidget {
public:
    // 同步报告当前 world AABB（[minX,maxX,minY,maxY,minZ,maxZ]）与交互阶段；回调不转移 widget 所有权。
    using BoundsCallback = std::function<void(const std::array<double, 6>& worldBounds, CropInteractionPhase phase)>;

    CropBoxWidget();
    ~CropBoxWidget();

    CropBoxWidget(const CropBoxWidget&) = delete;
    CropBoxWidget& operator=(const CropBoxWidget&) = delete;
    CropBoxWidget(CropBoxWidget&&) noexcept;
    CropBoxWidget& operator=(CropBoxWidget&&) noexcept;

    // 绑定非拥有的 interactor；调用方须保证它在 widget 启用期间有效。
    void SetInteractor(vtkRenderWindowInteractor* interactor);

    // 设置参考 world bounds；当前 world bounds 无效时会回退到它。
    void SetReferenceWorldBounds(const std::array<double, 6>& worldBounds);

    // 直接设置当前 widget world bounds。
    void SetWidgetWorldBounds(const std::array<double, 6>& worldBounds);

    // 返回当前缓存的 widget world AABB；调用方用它做状态显示、回调同步和下一次 PlaceWidget 的兜底来源。
    const std::array<double, 6>& GetCurrentWorldBounds() const;

    // 读取当前 widget 的有向盒定义。
    // baseCenter / baseSize 描述最近一次 PlaceWidget 的 world AABB 基准盒，
    // baseToNow 描述这个基准盒经过交互后的旋转、缩放和平移，
    // 调用方据此重建真实有向盒，避免把旋转后的 GetBounds 外接框误当成裁切几何。
    bool GetCurrentWorldBox(
        CropVectorDouble3Array& baseCenter,
        CropVectorDouble3Array& baseSize,
        CropMatrixDouble16Array& baseToNow) const;

    // 设置 world bounds 变化回调。
    void SetBoundsCallback(BoundsCallback callback);

    // 开关 widget；打开时会自动补 observer 并 place 到当前 world bounds。
    // 返回 false 表示当前还不满足安全启用条件，例如缺少 interactor 或有效 world bounds。
    bool SetEnabled(bool isEnabled);

    // 查询当前 widget 是否开启。
    bool GetEnabled() const;

private:
    class Impl;
    // 唯一拥有 VTK widget、representation、callback command、observer tag 与交互几何缓存。
    std::unique_ptr<Impl> m_impl;
};
