#pragma once

// =====================================================================
// Path: MVVCVTK/include/Service/OrthogonalCrop/OrthogonalCropWidgetStateController.h
// 分类: Service / Widget Controller
// 说明: 管理 vtkBoxWidget2 与 box representation，把 VTK 事件翻译为裁切模块状态回调。
// =====================================================================

#include "OrthogonalCrop/OrthogonalCropTypes.h"

#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCommand.h>
#include <vtkPlanes.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>

class OrthogonalCropWidgetStateController;

// VTK observer 适配器：把原始事件转交给 WidgetStateController 处理。
class OrthogonalCropWidgetStateCallback : public vtkCommand {
public:
    static OrthogonalCropWidgetStateCallback* New();

    // 绑定回调所有者。
    void SetOwner(OrthogonalCropWidgetStateController* owner);

    // VTK 事件入口。
    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override;

private:
    // 事件真正的业务处理者。
    OrthogonalCropWidgetStateController* m_owner = nullptr;
};

// 只管理 widget 生命周期、bounds 与交互事件，不关心裁切算法与结果投递。
class OrthogonalCropWidgetStateController {
public:
    // 向上层报告 bounds 与交互阶段的统一回调类型。
    using BoundsChangedCallback = std::function<void(const std::array<double, 6>& bounds, CropInteractionPhase phase)>;

    OrthogonalCropWidgetStateController();

    // 绑定 widget 所属 interactor。
    void SetInteractor(vtkRenderWindowInteractor* interactor);

    // 设置参考 bounds；当前 bounds 无效时会回退到它。
    void SetReferenceBounds(const std::array<double, 6>& bounds);

    // 直接设置当前 widget bounds。
    void SetWidgetBounds(const std::array<double, 6>& bounds);

    // 返回当前缓存的 widget bounds。
    const std::array<double, 6>& GetCurrentBounds() const;

    // 读取当前 representation 的 6 个裁切平面。
    bool GetPlanes(vtkPlanes* planes) const;

    // 设置 bounds 变化回调。
    void SetBoundsChangedCallback(BoundsChangedCallback callback);

    // 开关 widget；打开时会自动补 observer 并 place 到当前 bounds。
    bool SetEnabled(bool enabled);

    // 查询当前 widget 是否开启。
    bool GetEnabled() const;

private:
    friend class OrthogonalCropWidgetStateCallback;

    // 检查一组 bounds 是否有正体积。
    static bool GetBoundsAreValid(const std::array<double, 6>& bounds);

    // 把 VTK 事件类型统一映射为模块内部交互阶段。
    static CropInteractionPhase GetInteractionPhaseFromEvent(unsigned long eventId);

    // 懒加载绑定 widget observer，避免重复 AddObserver。
    void EnsureObserversAdded();

    // 处理一次 widget 事件并向上抛出 bounds 回调。
    void HandleWidgetEvent(unsigned long eventId);

    // widget 当前所属 interactor。
    vtkRenderWindowInteractor* m_interactor = nullptr;

    // 真正参与交互的 vtkBoxWidget2。
    vtkSmartPointer<vtkBoxWidget2> m_widget;

    // widget 的可视外观与 bounds 数据载体。
    vtkSmartPointer<vtkBoxRepresentation> m_representation;

    // VTK observer 适配器。
    vtkSmartPointer<OrthogonalCropWidgetStateCallback> m_callbackCommand;

    // 向交互桥上报 bounds / phase 的回调。
    BoundsChangedCallback m_boundsChangedCallback;

    // 当前 widget 开关状态。
    bool m_enabled = false;

    // observer 是否已经绑定过。
    bool m_observersAdded = false;

    // 当前缓存的有效 bounds。
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    // 当前参考 bounds，作为 currentBounds 无效时的回退来源。
    std::array<double, 6> m_referenceBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};
