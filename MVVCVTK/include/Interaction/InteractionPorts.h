#pragma once

#include "App/AppTypes.h"

#include <array>
#include <cstdint>
#include <memory>

class vtkActor;
class vtkProp3D;

class RenderUpdatePort {
public:
    virtual ~RenderUpdatePort() = default;

    virtual bool SendUpdates() = 0;
    // Session freeze 后只应用当前线程已经产生的 pending，不领取新 worker。
    virtual bool SendPendingUpdates() = 0;
    // Frame pump 只在所需 Render 完成后调用；实现必须在内部锁外执行 callback。
    virtual void SendCompletions() = 0;
    virtual bool SetRenderNeeded() = 0;
    virtual bool ResetRenderNeeded() = 0;
    virtual bool SetInteractionPhase() { return true; }
    virtual void SetRenderComplete(std::uint64_t) noexcept {}
};

class InteractionStatePort {
public:
    virtual ~InteractionStatePort() = default;

    virtual bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting) = 0;
};

class SliceInputPort {
public:
    virtual ~SliceInputPort() = default;

    virtual bool SetSliceScroll(int delta) = 0;
    virtual int GetPlaneAxis(vtkActor* actor) const = 0;
    virtual bool SetCursorWorld(
        const std::array<double, 3>& worldPosition,
        int axis) = 0;
    virtual std::array<double, 3> GetCursorWorld() const = 0;
    virtual WindowLevelParams GetWindowLevel() const = 0;
    virtual bool SetWindowLevelDrag(
        int totalDx,
        int totalDy,
        int viewWidth,
        int viewHeight,
        double startWidth,
        double startCenter) = 0;
};

class ModelInputPort {
public:
    virtual ~ModelInputPort() = default;

    virtual vtkProp3D* GetMainProp() const = 0;
    virtual std::array<double, 16> GetModelMatrix() const = 0;
    virtual bool SetModelMatrix(
        const std::array<double, 16>& modelToWorld) = 0;
};

struct InteractionPorts final {
    std::shared_ptr<RenderUpdatePort> update;
    std::shared_ptr<InteractionStatePort> state;
    std::shared_ptr<SliceInputPort> slice;
    std::shared_ptr<ModelInputPort> model;
};
