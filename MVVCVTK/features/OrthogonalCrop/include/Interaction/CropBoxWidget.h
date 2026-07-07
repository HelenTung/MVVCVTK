#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Interaction/CropBoxWidget.h
// 分类: Service / Widget Controller
// 说明: 管理 vtkBoxWidget2 与 box representation，把 VTK 事件翻译为裁切模块状态回调。
// =====================================================================
// 控制器只维护 widget 生命周期、bounds 和交互阶段；
// 它不生成 request、不执行算法、不刷新 preview，让交互桥基于纯状态决定何时刷新裁切预览。

#include "OrthogonalCropTypes.h"

#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCommand.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>

class CropBoxWidget;

// VTK observer 适配器：把原始事件转交给 WidgetStateController 处理。
class CropBoxCallback : public vtkCommand {
public:
    static CropBoxCallback* New();

    // 绑定回调所有者。
    void SetOwner(CropBoxWidget* owner);

    // VTK 事件入口。
    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override;

private:
    // 事件真正的业务处理者。
    CropBoxWidget* m_owner = nullptr;
};

// 只管理 widget 生命周期、bounds 与交互事件，不关心裁切算法与结果投递。
class CropBoxWidget {
public:
    // 向上层报告 world bounds 与交互阶段的统一回调类型。
    using BoundsCallback = std::function<void(const std::array<double, 6>& worldBounds, CropInteractionPhase phase)>;

    CropBoxWidget();

    // 绑定 widget 所属 interactor。
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
    friend class CropBoxCallback;

    // 检查一组 bounds 是否有正体积。
    static bool GetBoundsAreValid(const std::array<double, 6>& bounds);

    // 把 VTK 事件类型统一映射为模块内部交互阶段。
    static CropInteractionPhase GetEventPhase(unsigned long eventId);

    // 懒加载绑定 widget observer，避免重复 AddObserver。
    void AttachObservers();

    // 处理一次 widget 事件并向上抛出 bounds 回调。
    void OnWidgetEvent(unsigned long eventId);

    // 根据交互阶段刷新 widget 线框样式；
    // VTK 拖动单面时默认高亮 face 而不是 outline，本控制器把反馈统一收口到线框，避免出现实体面染色。
    void SetVisualState(CropInteractionPhase phase);

    // widget 当前所属 interactor。
    vtkRenderWindowInteractor* m_interactor = nullptr;

    // 真正参与交互的 vtkBoxWidget2。
    vtkSmartPointer<vtkBoxWidget2> m_widget;

    // widget 的可视外观与 bounds 数据载体。
    vtkSmartPointer<vtkBoxRepresentation> m_representation;

    // VTK observer 适配器。
    vtkSmartPointer<CropBoxCallback> m_callbackCommand;

    // 向交互桥上报 world bounds / phase 的回调。
    BoundsCallback m_boundsCallback;

    // 当前 widget 开关状态。
    bool m_isEnabled = false;

    // observer 是否已经绑定过。
    bool m_hasObservers = false;

    // 缓存 widget 当前 world AABB，用于状态回调、日志、重进裁切和 PlaceWidget 兜底；
    // 旋转交互后它只是外接框，因此不能作为真实裁切盒姿态。
    std::array<double, 6> m_currentWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    // 记录最近一次 PlaceWidget 使用的 world AABB，作为反解当前有向盒的固定基准；
    // 当前 polydata 角点只表达交互后的姿态，必须配合这个基准盒才能求回完整 affine。
    std::array<double, 6> m_baseBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    // 当前参考 world bounds，作为 currentWorldBounds 无效时的回退来源。
    std::array<double, 6> m_referenceWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};
