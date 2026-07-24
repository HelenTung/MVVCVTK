#pragma once

#include "AppInterfaces.h"
#include "Host/Types/HostValueTypes.h"
#include "Interaction/InteractionTypes.h"

#include <functional>
#include <string>
#include <string_view>

class HostRenderViewSet;

struct HostInputBinding final {
    std::string featureId;
    HostViewTargets targetViews;
    std::function<InteractionResult(const InteractionEvent&)> onInput;
};

class HostInputPort {
public:
    virtual ~HostInputPort() noexcept = default;

    virtual bool AttachInput(HostInputBinding binding) = 0;
    virtual bool DetachInput(std::string_view featureId) = 0;
};

struct HostFeatureContext final {
    const HostRenderViewSet* renderViews = nullptr;
    std::function<ImageSnapshot()> getImageSnapshot;
    std::function<bool(
        ImageState,
        const ImageSnapshot&,
        ImageSnapshot&)> setImageState;
    HostInputPort* inputPort = nullptr;
    std::function<bool(std::function<void()>)> sendOwnerComplete;
};

class HostFeature {
public:
    virtual ~HostFeature() noexcept = default;

    virtual std::string_view GetFeatureId() const noexcept = 0;
    virtual bool AttachHost(const HostFeatureContext& context) = 0;
    virtual bool DetachHost() = 0;
    virtual bool OnHostTick() = 0;
};
