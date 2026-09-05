#include "AppStateTests.h"

#include "App/AppState.h"

#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
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

DataReadyState GetDataReadyState(
    const std::array<double, 3>& spacing,
    const DataGeneration generation,
    const DataBindingRevision bindingRevision,
    const std::uint8_t identityByte = 1)
{
    DataReadyState state;
    state.dataRevision.entityId.bytes[0] = identityByte;
    state.dataRevision.generation = generation;
    state.bindingRevision = bindingRevision;
    state.scalarRange = { -10.0, 42.0 };
    state.spacing = spacing;
    return state;
}

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

    const auto identitySink = std::make_shared<StateEventSink>();
    SharedInteractionState identityState(identitySink);
    const auto committedIdentity = GetDataReadyState(
        { 2.0, 2.0, 2.0 }, 4, 7);
    const bool didCommitIdentity = identityState.SetSpacingData(
        committedIdentity.spacing,
        [&](const std::array<double, 3>&) {
            return std::optional<DataReadyState>{ committedIdentity };
        });
    if (!didCommitIdentity
        || identityState.GetSpacing() != committedIdentity.spacing
        || identityState.GetDataRevision()
            != committedIdentity.dataRevision
        || identityState.GetDataBindingRevision()
            != committedIdentity.bindingRevision
        || identitySink->GetEvents().size() != 1
        || identitySink->GetEvents().front()
            != (UpdateFlags::Spacing | UpdateFlags::DataReady)) {
        std::cerr << "Spacing commits must publish their DataGraph identity atomically.\n";
        ++failureCount;
    }

    SharedInteractionState concurrentSpacingState(nullptr);
    std::promise<void> dataWriteEntered;
    auto dataWriteEnteredFuture = dataWriteEntered.get_future();
    std::promise<void> releaseDataWrite;
    auto releaseDataWriteFuture = releaseDataWrite.get_future();
    bool didSetSpacingData = false;
    std::thread dataWriter([&] {
        didSetSpacingData = concurrentSpacingState.SetSpacingData(
            { 2.0, 2.0, 2.0 },
            [&](const std::array<double, 3>&) {
                dataWriteEntered.set_value();
                releaseDataWriteFuture.wait();
                return std::optional<DataReadyState>{
                    GetDataReadyState({ 2.0, 2.0, 2.0 }, 2, 2) };
            });
    });
    dataWriteEnteredFuture.wait();
    std::promise<void> directWriteStarted;
    auto directWriteStartedFuture = directWriteStarted.get_future();
    std::promise<void> directWriteComplete;
    auto directWriteCompleteFuture = directWriteComplete.get_future();
    std::thread directWriter([&] {
        directWriteStarted.set_value();
        concurrentSpacingState.SetSpacing(3.0, 3.0, 3.0);
        directWriteComplete.set_value();
    });
    directWriteStartedFuture.wait();
    const bool didDirectWriteComplete = directWriteCompleteFuture.wait_for(
        std::chrono::milliseconds(100)) == std::future_status::ready;
    releaseDataWrite.set_value();
    dataWriter.join();
    directWriter.join();
    if (!didSetSpacingData || !didDirectWriteComplete
        || concurrentSpacingState.GetSpacing()
            != std::array<double, 3>{ 2.0, 2.0, 2.0 }
        || concurrentSpacingState.GetDataRevision()
            != GetDataReadyState({ 2.0, 2.0, 2.0 }, 2, 2).dataRevision) {
        std::cerr << "A completed data commit must remain the spacing linearization point.\n";
        ++failureCount;
    }

    SharedInteractionState identityRaceState(nullptr);
    std::promise<void> identityCallbackEntered;
    auto identityCallbackEnteredFuture = identityCallbackEntered.get_future();
    std::promise<void> releaseIdentityCallback;
    auto releaseIdentityCallbackFuture = releaseIdentityCallback.get_future();
    bool didAcceptStaleIdentity = true;
    std::thread identityWriter([&] {
        didAcceptStaleIdentity = identityRaceState.SetSpacingData(
            { 2.0, 2.0, 2.0 },
            [&](const std::array<double, 3>&) {
                identityCallbackEntered.set_value();
                releaseIdentityCallbackFuture.wait();
                return std::optional<DataReadyState>{
                    GetDataReadyState({ 2.0, 2.0, 2.0 }, 2, 2) };
            });
    });
    identityCallbackEnteredFuture.wait();
    const auto winningIdentity = GetDataReadyState(
        { 5.0, 5.0, 5.0 }, 9, 11, 2);
    identityRaceState.SetDataReady(winningIdentity);
    releaseIdentityCallback.set_value();
    identityWriter.join();
    if (didAcceptStaleIdentity
        || identityRaceState.GetDataRevision()
            != winningIdentity.dataRevision
        || identityRaceState.GetDataBindingRevision()
            != winningIdentity.bindingRevision
        || identityRaceState.GetSpacing() != winningIdentity.spacing) {
        std::cerr << "A newer primary identity must reject a stale spacing completion.\n";
        ++failureCount;
    }

    SharedInteractionState reentrantSpacingState(nullptr);
    bool didAcceptInnerDataWrite = true;
    const bool didAcceptOuterDataWrite = reentrantSpacingState.SetSpacingData(
        { 2.0, 2.0, 2.0 },
        [&](const std::array<double, 3>&) {
            didAcceptInnerDataWrite = reentrantSpacingState.SetSpacingData(
                { 3.0, 3.0, 3.0 },
                [&](const std::array<double, 3>&) {
                    return std::optional<DataReadyState>{
                        GetDataReadyState({ 3.0, 3.0, 3.0 }, 3, 3) };
                });
            reentrantSpacingState.SetSpacing(4.0, 4.0, 4.0);
            return std::optional<DataReadyState>{
                GetDataReadyState({ 2.0, 2.0, 2.0 }, 2, 2) };
        });
    if (!didAcceptOuterDataWrite || didAcceptInnerDataWrite
        || reentrantSpacingState.GetSpacing()
            != std::array<double, 3>{ 2.0, 2.0, 2.0 }) {
        std::cerr << "Reentrant spacing data writes must be rejected without deadlock.\n";
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
