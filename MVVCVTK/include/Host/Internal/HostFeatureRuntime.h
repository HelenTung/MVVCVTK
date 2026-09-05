#pragma once
#include "Host/HostFeature.h"
#include <functional>
#include <memory>
#include <thread>

class AbstractDataManager;
class SharedInteractionState;
class HostViewRuntimeRegistry;
class HostFrameCoordinator;

// Session 独占；只接收挂载所需能力，不回调整个 Session。
class HostFeatureRuntime final {
public:
    struct Ports final {
        HostViewRuntimeRegistry* views = nullptr;
        HostInputPort* input = nullptr;
        std::weak_ptr<AbstractDataManager> data;
        std::weak_ptr<SharedInteractionState> state;
        std::shared_ptr<HostFrameCoordinator> frames;
        std::thread::id ownerThread;
        std::function<bool(std::function<void()>)> onOwnerComplete;
    };
    enum class DetachResult { Rejected, Detached, StopPending };
    HostFeatureRuntime();
    ~HostFeatureRuntime();
    HostFeatureRuntime(const HostFeatureRuntime&) = delete;
    HostFeatureRuntime& operator=(const HostFeatureRuntime&) = delete;
    bool StartOwner(Ports ports);
    bool StopOwner();
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    DetachResult DetachFeature(const HostFeature& feature);
    bool DetachFeatures();
    void SendFeatureTicks() noexcept;
    bool GetIsEmpty() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
