#pragma once

#include "Render/Internal/RulerMetrics.h"

#include <memory>

class vtkRenderer;

// 单 View 独占；只挂载自身 prop，随 owner thread 上的 View 生命周期解除绑定。
class RulerOverlay final {
public:
    RulerOverlay();
    ~RulerOverlay();
    RulerOverlay(const RulerOverlay&) = delete;
    RulerOverlay& operator=(const RulerOverlay&) = delete;
    void AttachRenderer(vtkRenderer* renderer);
    void DetachRenderer();
    void SetInput(const RulerInput& input, const RulerParams& params);
    const RulerState& GetState() const;
    void ClearInput();
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
