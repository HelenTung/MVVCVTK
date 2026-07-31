#include "AppStateTests.h"

#include "App/AppState.h"

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

    const std::vector<TFNode> presetNodes{
        { 0.1, 0.0, 0.0, 0.0, 0.0 },
        { 0.9, 1.0, 1.0, 1.0, 1.0 }
    };
    state.SetTransferPresetIntent(TransferPreset::Percentile);
    if (!state.SetTransferPresetNodes(
            TransferPreset::Percentile, 7, presetNodes)
        || state.GetTransferPreset() != TransferPreset::Percentile
        || sink->GetEvents().size() != 5
        || sink->GetEvents().back() != UpdateFlags::TF) {
        std::cerr << "Percentile intent and resolved nodes must commit as shared TF state.\n";
        ++failureCount;
    }
    state.SetTFNodes(presetNodes);
    if (state.GetTransferPreset() != TransferPreset::Manual
        || state.SetTransferPresetNodes(
            TransferPreset::Percentile, 6, presetNodes)
        || sink->GetEvents().size() != 5) {
        std::cerr << "Manual TF must cancel preset intent and reject stale preset results.\n";
        ++failureCount;
    }

    const std::vector<TFNode> manualNodes{
        { 0.0, 0.0, 0.75, 0.75, 0.75 },
        { 1.0, 1.0, 0.75, 0.75, 0.75 }
    };
    const std::size_t eventCount = sink->GetEvents().size();
    state.SetTFNodes(manualNodes);
    state.SetTFNodes(manualNodes);
    if (sink->GetEvents().size() != eventCount + 1
        || sink->GetEvents().back() != UpdateFlags::TF) {
        std::cerr << "Equal TF nodes must not publish a second TF event.\n";
        ++failureCount;
    }
    return failureCount;
}
