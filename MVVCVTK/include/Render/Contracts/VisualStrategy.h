#pragma once

#include "App/AppTypes.h"
#include "Render/Contracts/RenderEffect.h"
#include "Render/Internal/RenderResourceCoordinator.h"

#include <vtkActor.h>
#include <vtkDataObject.h>
#include <vtkImageData.h>
#include <vtkProp3D.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <memory>
#include <utility>

// App 只依赖视觉策略行为；具体 VTK 管线类型留在 Render 层内部。
class AbstractVisualStrategy {
public:
    virtual ~AbstractVisualStrategy() = default;

    virtual void SetInputData(vtkSmartPointer<vtkDataObject> data) = 0;
    // 空 mask 表示整卷有效；需要有效域的策略自行覆盖。
    virtual void SetInputMask(vtkSmartPointer<vtkImageData>) {}
    // DataStage 以一笔事务提交 volume 与 mask；普通策略沿用既有两个窄入口。
    virtual bool SetInputData(
        vtkSmartPointer<vtkDataObject> data,
        vtkSmartPointer<vtkImageData> validityMask)
    {
        SetInputData(std::move(data));
        SetInputMask(std::move(validityMask));
        return true;
    }
    virtual void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual bool SetVisualState(
        const RenderParams&,
        UpdateFlags = UpdateFlags::All) { return true; }
    virtual bool SetProductCommit() { return true; }
    virtual RenderTransitionState GetTransitionState() const { return {}; }
    virtual void SetFirstRenderDuration(std::uint64_t) noexcept {}
    virtual int GetPlaneAxis(vtkActor*) { return -1; }
    virtual int GetNavigationAxis() const { return -1; }
    virtual vtkProp3D* GetMainProp() { return nullptr; }
    virtual bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect,
        RenderBindingUse bindingUse) = 0;
    virtual bool DetachRenderEffect(const RenderEffect* effect) = 0;
    virtual bool SetRenderInputStamp(RenderInputStamp inputStamp) = 0;
    virtual RenderInputStamp GetRenderInputStamp() const = 0;
    virtual bool SetRenderEffectUse(RenderBindingUse bindingUse) = 0;
    virtual RenderBindingUse GetRenderEffectUse() const = 0;
    virtual RenderEffectState GetRenderEffectState() const = 0;
    virtual bool SetRenderEffectCommit(std::uint64_t revision) = 0;
    virtual bool ClearRenderEffectStage(std::uint64_t revision) = 0;
};
