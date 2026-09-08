#pragma once
#include "Render/Contracts/RenderEffect.h"
#include <functional>
#include <memory>

class vtkRenderer;

// Owner-thread, private Host implementation. One tracker per renderer records
// actual arrays used by a frame; fences outlive strategies and failed stages.
class RenderFrameLifetime final {
public:
    static std::shared_ptr<RenderFrameLifetime> Create(vtkRenderer* renderer);
    static bool PollAll();
    ~RenderFrameLifetime();
    bool QueueCompletion(std::function<void(RenderFrameOutcome)> callback);
private:
    class Impl;
    explicit RenderFrameLifetime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
