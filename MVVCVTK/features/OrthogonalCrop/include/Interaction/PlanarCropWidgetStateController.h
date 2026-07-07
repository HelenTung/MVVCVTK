#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Interaction/PlanarCropWidgetStateController.h
// 分类: Service / Widget Controller
// 说明: 管理 vtkImplicitPlaneWidget2，把 VTK 平面事件翻译为裁切模块状态回调。
// =====================================================================

#include "OrthogonalCropTypes.h"

#include <vtkCommand.h>
#include <vtkImplicitPlaneRepresentation.h>
#include <vtkImplicitPlaneWidget2.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>

class PlanarCropWidgetStateController;

// VTK observer 适配器：把原始事件转交给 PlanarCropWidgetStateController 处理。
class PlanarCropWidgetStateCallback : public vtkCommand {
public:
    static PlanarCropWidgetStateCallback* New();

    // 绑定回调所有者。
    void SetOwner(PlanarCropWidgetStateController* owner);

    // VTK 事件入口。
    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override;

private:
    // 事件真正的业务处理者。
    PlanarCropWidgetStateController* m_owner = nullptr;
};

// 只管理平面 widget 生命周期、平面参数与交互事件，不关心裁切算法与结果投递。
class PlanarCropWidgetStateController {
public:
    // 向上层报告 world 平面与交互阶段的统一回调类型。
    using WorldPlaneChangedCallback = std::function<void(
        const CropVectorDouble3Array& worldOrigin,
        const CropVectorDouble3Array& worldNormal,
        CropInteractionPhase phase)>;

    PlanarCropWidgetStateController();

    // 绑定 widget 所属 interactor。
    void SetInteractor(vtkRenderWindowInteractor* interactor);

    // 设置参考 world bounds；当前平面无效时会用 bounds 中心初始化原点。
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
    void SetPlaneCallback(WorldPlaneChangedCallback callback);

    // 开关 widget；打开时会自动补 observer 并 place 到当前 reference bounds。
    bool SetEnabled(bool enabled);

    // 查询当前 widget 是否开启。
    bool GetEnabled() const;

private:
    friend class PlanarCropWidgetStateCallback;

    // 检查一组 bounds 是否有正体积。
    static bool GetBoundsAreValid(const std::array<double, 6>& bounds);

    // 检查并归一化平面法线。
    static bool SetUnitNormal(CropVectorDouble3Array& worldNormal);

    // 把 VTK 事件类型统一映射为模块内部交互阶段。
    static CropInteractionPhase GetEventPhase(unsigned long eventId);

    // 懒加载绑定 widget observer，避免重复 AddObserver。
    void AttachObservers();

    // 将当前平面状态同步到 representation。
    void SetPlaneRep();

    // 处理一次 widget 事件并向上抛出平面回调。
    void OnWidgetEvent(unsigned long eventId);

    // widget 当前所属 interactor。
    vtkRenderWindowInteractor* m_interactor = nullptr;

    // 真正参与交互的 vtkImplicitPlaneWidget2。
    vtkSmartPointer<vtkImplicitPlaneWidget2> m_widget;

    // widget 的可视外观与平面数据载体。
    vtkSmartPointer<vtkImplicitPlaneRepresentation> m_representation;

    // VTK observer 适配器。
    vtkSmartPointer<PlanarCropWidgetStateCallback> m_callbackCommand;

    // 向交互桥上报 world plane / phase 的回调。
    WorldPlaneChangedCallback m_worldPlaneChangedCallback;

    // 当前 widget 开关状态。
    bool m_enabled = false;

    // observer 是否已经绑定过。
    bool m_hasObservers = false;

    // 当前完整模型 world bounds，只作为 reference 输入合法性和默认原点来源；
    // 平面可视尺寸由 bridge 下发的 halfExtents 决定，避免 widget 层重新发明一套尺寸策略。
    std::array<double, 6> m_referenceWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    // 缓存当前 world 平面原点。
    CropVectorDouble3Array m_currentWorldOrigin = { 0.0, 0.0, 0.0 };

    // 缓存当前 world 平面法线。
    CropVectorDouble3Array m_currentWorldNormal = { 0.0, 0.0, 1.0 };

    // 缓存 bridge 下发的平面可视半尺寸；它只影响 VTK 平面控件大小，不改变无限半空间裁切语义。
    std::array<double, 2> m_currentWorldHalfExtents = { 1.0, 1.0 };
};
