#include "AppStateTests.h"

#include "App/AppState.h"

#include <array>
#include <iostream>
#include <memory>
#include <vector>

namespace {

class StateEventSink final : public IStateEventSink {
public:
    void SendFlags(UpdateFlags flags) override
    {
        m_events.push_back(flags);
    }

    const std::vector<UpdateFlags>& GetEvents() const
    {
        return m_events;
    }

private:
    std::vector<UpdateFlags> m_events;
};

}

int AppStateSuite::GetFailCount() const
{
    int failureCount = 0;
    const auto sink = std::make_shared<StateEventSink>();
    SharedInteractionState state(sink);

    auto material = state.GetMaterial();
    material.opacity = 0.42;
    state.SetMaterial(material);
    state.SetMaterial(material);
    if (sink->GetEvents().size() != 1
        || sink->GetEvents().front() != UpdateFlags::Material) {
        std::cerr << "App state must broadcast one material diff.\n";
        ++failureCount;
    }

    state.SetSpacing(0.5, 0.75, 1.25);
    state.SetSpacing(0.5, 0.75, 1.25);
    if (sink->GetEvents().size() != 2
        || sink->GetEvents().back() != UpdateFlags::Spacing) {
        std::cerr << "App state must broadcast one spacing diff.\n";
        ++failureCount;
    }

    const auto rangeSink = std::make_shared<StateEventSink>();
    SharedInteractionState rangeState(rangeSink);
    rangeState.SetScalarRange(-10.0, 42.0);
    const auto scalarRange = rangeState.GetScalarRange();
    const auto dataRange = rangeState.GetDataRange();
    if (scalarRange != std::array<double, 2>{ -10.0, 42.0 }
        || dataRange != scalarRange) {
        std::cerr << "Scalar range getter must mirror SetScalarRange and its compatibility alias.\n";
        ++failureCount;
    }

    const InteractionSource viewerSource{ "Viewer2D", "primary" };
    const InteractionSource cropSource{ "OrthogonalCrop", "box" };
    if (!state.SetInteracting(viewerSource, true)
        || !state.SetInteracting(viewerSource, true)
        || !state.SetInteracting(cropSource, true)
        || !state.GetIsInteracting()
        || sink->GetEvents().size() != 3
        || sink->GetEvents().back() != UpdateFlags::RenderRate) {
        std::cerr << "Interaction sources must publish only the empty-to-active boundary.\n";
        ++failureCount;
    }
    if (!state.SetInteracting(viewerSource, false)
        || !state.GetIsInteracting()
        || sink->GetEvents().size() != 3) {
        std::cerr << "One source must not clear another active interaction.\n";
        ++failureCount;
    }
    if (!state.SetInteracting(cropSource, false)
        || state.GetIsInteracting()
        || sink->GetEvents().size() != 4
        || sink->GetEvents().back() != UpdateFlags::RenderRate) {
        std::cerr << "The final source exit must publish the active-to-empty boundary.\n";
        ++failureCount;
    }
    if (state.SetInteracting({ "", "invalid" }, true)
        || sink->GetEvents().size() != 4) {
        std::cerr << "Empty interaction identities must be rejected without state changes.\n";
        ++failureCount;
    }

    VolumeTransferFunction initialFunction;
    initialFunction.colorNodes = {
        { 0.1, 0.0, 0.0, 0.0 },
        { 0.9, 1.0, 1.0, 1.0 }
    };
    initialFunction.opacityNodes = {
        { 0.1, 0.0 },
        { 0.9, 1.0 }
    };
    if (!state.SetVolumeTransferFunction(initialFunction)
        || sink->GetEvents().size() != 5
        || sink->GetEvents().back()
            != UpdateFlags::VolumeTransfer
        || state.GetVolumeTransferFunction().colorNodes.size() != 2) {
        std::cerr << "The actual scalar transfer snapshot must be the only TF state.\n";
        ++failureCount;
    }
    (void)state.SetVolumeTransferFunction(initialFunction);
    if (sink->GetEvents().size() != 5) {
        std::cerr << "An equal transfer snapshot must be a no-op.\n";
        ++failureCount;
    }

    VolumeTransferFunction manualFunction;
    manualFunction.colorNodes = {
        { 0.0, 0.75, 0.75, 0.75 },
        { 1.0, 0.75, 0.75, 0.75 }
    };
    manualFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 1.0, 1.0 }
    };
    const std::size_t eventCount = sink->GetEvents().size();
    (void)state.SetVolumeTransferFunction(manualFunction);
    (void)state.SetVolumeTransferFunction(manualFunction);
    if (sink->GetEvents().size() != eventCount + 1
        || sink->GetEvents().back()
            != UpdateFlags::VolumeTransfer) {
        std::cerr << "Equal TF nodes must not publish a second TF event.\n";
        ++failureCount;
    }

    const auto transferSink = std::make_shared<StateEventSink>();
    ViewPresentationState transferState(transferSink);
    VolumeTransferFunction autoFunction;
    autoFunction.colorNodes = {
        { -20.0, 0.75, 0.75, 0.75 },
        { 40.0, 0.75, 0.75, 0.75 }
    };
    autoFunction.opacityNodes = {
        { -20.0, 0.0 },
        { 40.0, 1.0 }
    };
    if (!transferState.GetTransferAuto()
        || !transferState.SetAutoTransfer(autoFunction)
        || !transferState.GetTransferAuto()
        || transferState.GetVolumeTransferFunction().colorNodes.size() != 2) {
        std::cerr << "Auto TF values must keep their auto source.\n";
        ++failureCount;
    }
    const std::size_t autoEventCount = transferSink->GetEvents().size();
    if (!transferState.SetVolumeTransferFunction(autoFunction)
        || transferState.GetTransferAuto()
        || transferSink->GetEvents().size() != autoEventCount + 1
        || transferSink->GetEvents().back()
            != UpdateFlags::VolumeTransfer) {
        std::cerr << "An explicit TF write must change intent even when nodes are equal.\n";
        ++failureCount;
    }
    VolumeTransferFunction nextAuto = autoFunction;
    nextAuto.colorNodes.back().scalar = 80.0;
    nextAuto.opacityNodes.back().scalar = 80.0;
    if (transferState.SetAutoTransfer(nextAuto)
        || transferState.GetTransferAuto()
        || transferState.GetVolumeTransferFunction().colorNodes.back().scalar
            != 40.0) {
        std::cerr << "An auto refresh must not overwrite explicit TF intent.\n";
        ++failureCount;
    }
    return failureCount;
}
