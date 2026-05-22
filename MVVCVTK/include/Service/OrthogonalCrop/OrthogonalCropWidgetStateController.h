#pragma once

#include "OrthogonalCrop/OrthogonalCropTypes.h"

#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCommand.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>
#include <utility>

class OrthogonalCropWidgetStateController;

// 轻量 VTK 回调适配器，只负责把 widget 事件转回 controller。
class OrthogonalCropWidgetStateCallback : public vtkCommand {
public:
    static OrthogonalCropWidgetStateCallback* New() { return new OrthogonalCropWidgetStateCallback(); }

    // controller 保持所有权，本类只缓存反向通知所需的 owner 指针。
    void SetOwner(OrthogonalCropWidgetStateController* owner)
    {
        m_owner = owner;
    }

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override;

private:
    OrthogonalCropWidgetStateController* m_owner = nullptr;
};

// 只负责 vtkBoxWidget2 的显示、启停和 bounds 状态同步，不做裁切计算。
class OrthogonalCropWidgetStateController {
public:
    using BoundsChangedCallback = std::function<void(const std::array<double, 6>& bounds, CropInteractionPhase phase)>;

    // 初始化 widget 及其外观样式，让 bridge 只操作高层状态接口。
    OrthogonalCropWidgetStateController()
    {
        m_widget = vtkSmartPointer<vtkBoxWidget2>::New();
        m_representation = vtkSmartPointer<vtkBoxRepresentation>::New();
        m_representation->SetPlaceFactor(1.0);
        m_representation->GetOutlineProperty()->SetColor(1.0, 0.15, 0.10);
        m_representation->GetOutlineProperty()->SetLineWidth(2.0);
        m_representation->GetSelectedOutlineProperty()->SetColor(1.0, 0.55, 0.20);
        m_representation->GetSelectedOutlineProperty()->SetLineWidth(2.5);
        m_widget->SetRepresentation(m_representation);

        m_callbackCommand = vtkSmartPointer<OrthogonalCropWidgetStateCallback>::New();
        m_callbackCommand->SetOwner(this);
    }

    // widget 实际挂在哪个窗口由外部决定，controller 只接收 interactor。
    void SetInteractor(vtkRenderWindowInteractor* interactor)
    {
        m_interactor = interactor;
        m_widget->SetInteractor(interactor);
    }

    // 设置完整参考范围；当前 bounds 尚未初始化时会回退到这个范围。
    void SetReferenceBounds(const std::array<double, 6>& bounds)
    {
        if (!GetBoundsAreValid(bounds)) {
            return;
        }

        m_referenceBounds = bounds;
        // 如果当前 bounds 还没初始化，就让参考范围同时作为首次显示范围。
        if (!GetBoundsAreValid(m_currentBounds)) {
            m_currentBounds = bounds;
        }
        // representation 始终以 controller 缓存状态为准，避免参考范围和当前范围错位。
        m_representation->PlaceWidget(m_currentBounds.data());
    }

    // 外部可直接恢复或重设当前 widget bounds，而不需要重新建 widget。
    void SetWidgetBounds(const std::array<double, 6>& bounds)
    {
        if (!GetBoundsAreValid(bounds)) {
            return;
        }

        m_currentBounds = bounds;
        // 每次外部直接改 bounds，都立即同步回 representation。
        m_representation->PlaceWidget(m_currentBounds.data());
    }

    // 对外暴露 controller 缓存的当前 bounds，避免外部直接读 VTK 内部对象。
    const std::array<double, 6>& GetCurrentBounds() const
    {
        return m_currentBounds;
    }

    // bounds 变化由 bridge 注册回调接收，用于驱动 preview 同步。
    void SetBoundsChangedCallback(BoundsChangedCallback callback)
    {
        m_boundsChangedCallback = std::move(callback);
    }

    // 启停只控制 UI 层；若 interactor 或 bounds 不合法，则拒绝启用。
    bool SetEnabled(bool enabled)
    {
        if (enabled && !m_interactor) {
            return false;
        }

        // 先保证 observer 已经就绪，避免 widget 打开后收不到事件。
        EnsureObserversAdded();

        if (enabled) {
            // 当前 bounds 无效时，退回到 reference bounds 作为首次显示盒。
            if (!GetBoundsAreValid(m_currentBounds)) {
                m_currentBounds = m_referenceBounds;
            }

            if (!GetBoundsAreValid(m_currentBounds)) {
                return false;
            }

            // 先把 representation 放到目标位置，再真正打开 widget。
            m_representation->PlaceWidget(m_currentBounds.data());
            m_widget->On();
        }
        else {
            // 关闭时不清空 bounds，只撤销交互占用，便于下次重新打开复用上一状态。
            m_widget->Off();
        }

        m_enabled = enabled;
        return true;
    }

    // 返回当前 widget 是否处于激活状态。
    bool GetEnabled() const
    {
        return m_enabled;
    }

private:
    friend class OrthogonalCropWidgetStateCallback;

    // 统一判定 min/max bounds 是否有效。
    static bool GetBoundsAreValid(const std::array<double, 6>& bounds)
    {
        return bounds[0] < bounds[1]
            && bounds[2] < bounds[3]
            && bounds[4] < bounds[5];
    }

    // 把 VTK 原生事件语义映射成项目内部的交互阶段。
    static CropInteractionPhase GetInteractionPhaseFromEvent(unsigned long eventId)
    {
        switch (eventId) {
        case vtkCommand::StartInteractionEvent:
            return CropInteractionPhase::Dragging;
        case vtkCommand::InteractionEvent:
            return CropInteractionPhase::Dragging;
        case vtkCommand::EndInteractionEvent:
            return CropInteractionPhase::Released;
        default:
            return CropInteractionPhase::Idle;
        }
    }

    // observer 只注册一次，避免多次启停 widget 后事件重复触发。
    void EnsureObserversAdded()
    {
        if (m_observersAdded) {
            return;
        }

        // 只监听一次开始、过程、结束三类核心交互事件，其他语义统一由外层 bridge 派生。
        m_widget->AddObserver(vtkCommand::StartInteractionEvent, m_callbackCommand);
        m_widget->AddObserver(vtkCommand::InteractionEvent, m_callbackCommand);
        m_widget->AddObserver(vtkCommand::EndInteractionEvent, m_callbackCommand);
        m_observersAdded = true;
    }

    // 每次事件都先同步一份最新 bounds，再把 phase 连同 bounds 上报给外部。
    void HandleWidgetEvent(unsigned long eventId)
    {
        const auto rawBounds = m_representation->GetBounds();
        if (!rawBounds) {
            return;
        }

        // 先从 VTK representation 拷贝一份稳定快照，避免把内部指针直接暴露给外部。
        const std::array<double, 6> bounds = {
            rawBounds[0], rawBounds[1],
            rawBounds[2], rawBounds[3],
            rawBounds[4], rawBounds[5]
        };
        if (!GetBoundsAreValid(bounds)) {
            return;
        }

        // 先更新 controller 自己的缓存，再往外发通知，确保回调里读取到的是最新状态。
        m_currentBounds = bounds;
        if (m_boundsChangedCallback) {
            m_boundsChangedCallback(m_currentBounds, GetInteractionPhaseFromEvent(eventId));
        }
    }

    // 当前 widget 绑定的 interactor。
    vtkRenderWindowInteractor* m_interactor = nullptr;
    // VTK widget 本体，负责拾取和交互。
    vtkSmartPointer<vtkBoxWidget2> m_widget;
    // widget 的可视表现和 bounds 读写都依赖 representation。
    vtkSmartPointer<vtkBoxRepresentation> m_representation;
    // 唯一的 VTK 回调对象。
    vtkSmartPointer<OrthogonalCropWidgetStateCallback> m_callbackCommand;
    // 向 bridge 上报 bounds/phase 变化的回调。
    BoundsChangedCallback m_boundsChangedCallback;
    // 当前 UI 是否已启用。
    bool m_enabled = false;
    // observer 是否已经绑定到 widget。
    bool m_observersAdded = false;
    // 当前有效的 widget bounds 缓存。
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 输入对象的完整参考 bounds，用于首次放置或回退。
    std::array<double, 6> m_referenceBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};

inline void OrthogonalCropWidgetStateCallback::Execute(vtkObject* caller, unsigned long eventId, void* callData)
{
    (void)caller;
    (void)callData;
    if (m_owner) {
        m_owner->HandleWidgetEvent(eventId);
    }
}
