#include "Host/HostFrameCoordinator.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct FrameProbe final {
    std::vector<std::string> events;
    std::vector<HostFrameIntent> intents;
    HostFrameStageStatus stageStatus = HostFrameStageStatus::Ready;
    bool isCollectSet = true;
    bool isIntentSet = true;
    bool isFeatureApplySet = true;
    bool isRenderSent = true;
    bool isRenderPending = false;
    bool isViewAApplied = false;
    bool isViewBApplied = false;
    bool wasRenderBeforeApply = false;
    std::size_t renderAttemptCount = 0;
    std::size_t completionCount = 0;
    std::uint64_t committedDataVersion = 1;

    HostFrameCoordinator::Callbacks GetCallbacks()
    {
        HostFrameCoordinator::Callbacks callbacks;
        callbacks.collectUpdates = [this]() {
            events.push_back("collect-updates");
            return isCollectSet;
        };
        callbacks.sendFeatureTicks = [this]() {
            events.push_back("tick-features");
        };
        callbacks.setIntents = [this](
            const std::vector<HostFrameIntent>& values) {
            events.push_back("set-intents");
            intents = values;
            return isIntentSet;
        };
        callbacks.applyFeatureUpdates = [this]() {
            events.push_back("apply-feature-updates");
            return isFeatureApplySet;
        };
        callbacks.buildStage = [this](const std::uint64_t) {
            events.push_back("stage-view-a");
            isViewAApplied = true;
            events.push_back("stage-view-b");
            isViewBApplied = true;
            events.push_back("validate");
            return stageStatus;
        };
        callbacks.setCommit = [this](const std::uint64_t epoch) {
            committedDataVersion = 2;
            events.push_back("commit-" + std::to_string(epoch));
        };
        callbacks.sendRender = [this](const std::uint64_t epoch) {
            ++renderAttemptCount;
            wasRenderBeforeApply = wasRenderBeforeApply
                || !isViewAApplied || !isViewBApplied;
            events.push_back(
                "render-view-a-" + std::to_string(epoch));
            events.push_back(
                "render-view-b-" + std::to_string(epoch));
            isRenderPending = !isRenderSent;
            return isRenderSent;
        };
        callbacks.getRenderPending = [this]() {
            return isRenderPending;
        };
        callbacks.sendCompletions = [this]() {
            events.push_back("app-complete");
            events.push_back("host-complete");
            ++completionCount;
        };
        callbacks.clearStage = [this]() {
            events.push_back("clear-stage");
        };
        return callbacks;
    }
};

FeatureSceneDelta GetDelta(
    std::uint64_t requestId,
    FeatureScenePriority priority = FeatureScenePriority::Scene)
{
    FeatureSceneDelta delta;
    delta.viewIds = { "view-a", "view-b" };
    delta.requestId = requestId;
    delta.priority = priority;
    return delta;
}

bool GetSequenceValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(7, probe.GetCallbacks());
    if (!coordinator.Enqueue(
            "feature.overlay",
            GetDelta(1, FeatureScenePriority::Overlay))
        || !coordinator.Enqueue(
            "feature.scene",
            GetDelta(2, FeatureScenePriority::Scene))) {
        return false;
    }

    const auto status = coordinator.FlushOnOwnerTick(true);
    const std::vector<std::string> expected = {
        "collect-updates",
        "tick-features",
        "set-intents",
        "apply-feature-updates",
        "stage-view-a",
        "stage-view-b",
        "validate",
        "commit-1",
        "render-view-a-1",
        "render-view-b-1",
        "app-complete",
        "host-complete"
    };
    return status == HostFrameCoordinator::FlushStatus::Completed
        && probe.events == expected
        && !probe.wasRenderBeforeApply
        && probe.intents.size() == 2
        && probe.intents[0].featureId == "feature.scene"
        && probe.intents[1].featureId == "feature.overlay"
        && probe.intents[0].sessionGeneration == 7
        && probe.intents[0].baseSceneEpoch == 0
        && coordinator.GetCommittedEpoch() == 1
        && probe.completionCount == 1;
}

bool GetLatestWinsValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(3, probe.GetCallbacks());
    auto reversed = GetDelta(4);
    std::reverse(reversed.viewIds.begin(), reversed.viewIds.end());
    return coordinator.Enqueue("feature", GetDelta(1))
        && coordinator.Enqueue("feature", std::move(reversed))
        && coordinator.Enqueue("feature", GetDelta(2))
        && coordinator.FlushOnOwnerTick(false)
            == HostFrameCoordinator::FlushStatus::Completed
        && probe.intents.size() == 1
        && probe.intents.front().delta.requestId == 4;
}

bool GetFrozenBatchValid()
{
    FrameProbe probe;
    HostFrameCoordinator* coordinator = nullptr;
    auto callbacks = probe.GetCallbacks();
    const auto setIntents = callbacks.setIntents;
    callbacks.setIntents = [&probe, &coordinator, setIntents](
        const std::vector<HostFrameIntent>& intents) {
        const bool isSet = setIntents(intents);
        if (coordinator) {
            (void)coordinator->Enqueue(
                "feature.next",
                GetDelta(9, FeatureScenePriority::Refinement));
        }
        return isSet;
    };

    HostFrameCoordinator value(5, std::move(callbacks));
    coordinator = &value;
    if (!value.Enqueue("feature.current", GetDelta(1))
        || value.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Completed
        || probe.intents.size() != 1
        || probe.intents.front().featureId != "feature.current") {
        return false;
    }

    probe.intents.clear();
    probe.events.clear();
    probe.isViewAApplied = false;
    probe.isViewBApplied = false;
    return value.FlushOnOwnerTick(false)
            == HostFrameCoordinator::FlushStatus::Completed
        && probe.intents.size() == 1
        && probe.intents.front().featureId == "feature.next"
        && probe.intents.front().baseSceneEpoch == 1
        && value.GetCommittedEpoch() == 2;
}

bool GetFeatureTickVisibleInSameFrameValid()
{
    FrameProbe probe;
    HostFrameCoordinator* coordinator = nullptr;
    auto callbacks = probe.GetCallbacks();
    callbacks.sendFeatureTicks = [&probe, &coordinator]() {
        probe.events.push_back("tick-features");
        if (coordinator) {
            (void)coordinator->Enqueue(
                "feature.tick", GetDelta(12));
        }
    };
    HostFrameCoordinator value(9, std::move(callbacks));
    coordinator = &value;
    return value.FlushOnOwnerTick(true)
            == HostFrameCoordinator::FlushStatus::Completed
        && probe.intents.size() == 1
        && probe.intents.front().featureId == "feature.tick"
        && probe.intents.front().delta.requestId == 12
        && value.GetCommittedEpoch() == 1
        && probe.renderAttemptCount == 1
        && probe.completionCount == 1;
}

bool GetFailureBeforeCommitValid()
{
    FrameProbe collectProbe;
    collectProbe.isCollectSet = false;
    HostFrameCoordinator collectCoordinator(
        1, collectProbe.GetCallbacks());
    if (!collectCoordinator.Enqueue("feature", GetDelta(1))
        || collectCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Failed
        || collectCoordinator.GetCommittedEpoch() != 0
        || collectProbe.renderAttemptCount != 0
        || collectProbe.completionCount != 0
        || collectProbe.events.back() != "clear-stage") {
        return false;
    }
    collectProbe.isCollectSet = true;
    if (collectCoordinator.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Completed
        || collectProbe.intents.size() != 1
        || collectCoordinator.GetCommittedEpoch() != 1) {
        return false;
    }

    FrameProbe tickProbe;
    bool isTickThrowing = true;
    auto tickCallbacks = tickProbe.GetCallbacks();
    tickCallbacks.sendFeatureTicks = [&tickProbe, &isTickThrowing]() {
        tickProbe.events.push_back("tick-features");
        if (isTickThrowing) throw 1;
    };
    HostFrameCoordinator tickCoordinator(1, std::move(tickCallbacks));
    if (!tickCoordinator.Enqueue("feature", GetDelta(1))
        || tickCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Failed
        || tickCoordinator.GetCommittedEpoch() != 0
        || tickProbe.renderAttemptCount != 0) {
        return false;
    }
    isTickThrowing = false;
    if (tickCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Completed
        || tickProbe.intents.size() != 1
        || tickCoordinator.GetCommittedEpoch() != 1) {
        return false;
    }

    FrameProbe intentProbe;
    intentProbe.isIntentSet = false;
    HostFrameCoordinator intentCoordinator(
        1, intentProbe.GetCallbacks());
    if (!intentCoordinator.Enqueue("feature", GetDelta(1))
        || intentCoordinator.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Failed
        || intentCoordinator.GetCommittedEpoch() != 0
        || intentProbe.renderAttemptCount != 0
        || intentProbe.completionCount != 0
        || intentProbe.events.back() != "clear-stage") {
        return false;
    }
    intentProbe.isIntentSet = true;
    intentProbe.intents.clear();
    if (intentCoordinator.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Completed
        || intentProbe.intents.size() != 1
        || intentProbe.intents.front().delta.requestId != 1
        || intentCoordinator.GetCommittedEpoch() != 1) {
        return false;
    }

    FrameProbe applyProbe;
    applyProbe.isFeatureApplySet = false;
    HostFrameCoordinator applyCoordinator(
        1, applyProbe.GetCallbacks());
    if (!applyCoordinator.Enqueue("feature", GetDelta(2))
        || applyCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Failed
        || applyCoordinator.GetCommittedEpoch() != 0
        || applyProbe.renderAttemptCount != 0
        || applyProbe.completionCount != 0) {
        return false;
    }
    applyProbe.isFeatureApplySet = true;
    applyProbe.intents.clear();
    if (applyCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Completed
        || applyProbe.intents.size() != 1
        || applyProbe.intents.front().delta.requestId != 2) {
        return false;
    }

    FrameProbe stageProbe;
    stageProbe.stageStatus = HostFrameStageStatus::Failed;
    HostFrameCoordinator stageCoordinator(
        1, stageProbe.GetCallbacks());
    if (!stageCoordinator.Enqueue("feature", GetDelta(3))
        || stageCoordinator.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Failed
        || stageCoordinator.GetCommittedEpoch() != 0
        || stageProbe.renderAttemptCount != 0
        || stageProbe.completionCount != 0
        || stageProbe.events.back() != "clear-stage") {
        return false;
    }
    stageProbe.stageStatus = HostFrameStageStatus::Ready;
    stageProbe.intents.clear();
    return stageCoordinator.FlushOnOwnerTick(false)
            == HostFrameCoordinator::FlushStatus::Completed
        && stageProbe.intents.size() == 1
        && stageProbe.intents.front().delta.requestId == 3;
}

bool GetThrownPhaseRetryValid()
{
    FrameProbe intentProbe;
    HostFrameCoordinator* intentCoordinator = nullptr;
    bool isIntentThrowing = true;
    auto intentCallbacks = intentProbe.GetCallbacks();
    const auto setIntents = intentCallbacks.setIntents;
    intentCallbacks.setIntents = [
        &intentProbe, &intentCoordinator, &isIntentThrowing, setIntents](
            const std::vector<HostFrameIntent>& intents) {
        const bool isSet = setIntents(intents);
        if (isIntentThrowing) {
            isIntentThrowing = false;
            if (intentCoordinator) {
                (void)intentCoordinator->Enqueue(
                    "feature.next", GetDelta(9));
            }
            throw std::runtime_error("set intents failure");
        }
        return isSet;
    };
    HostFrameCoordinator intentValue(1, std::move(intentCallbacks));
    intentCoordinator = &intentValue;
    if (!intentValue.Enqueue("feature.retry", GetDelta(1))
        || intentValue.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Failed
        || intentValue.GetCommittedEpoch() != 0) {
        return false;
    }
    intentProbe.intents.clear();
    if (intentValue.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Completed
        || intentProbe.intents.size() != 1
        || intentProbe.intents.front().featureId != "feature.retry"
        || intentValue.GetCommittedEpoch() != 1) {
        return false;
    }
    intentProbe.intents.clear();
    if (intentValue.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::Completed
        || intentProbe.intents.size() != 1
        || intentProbe.intents.front().featureId != "feature.next"
        || intentValue.GetCommittedEpoch() != 2) {
        return false;
    }

    FrameProbe applyProbe;
    bool isApplyThrowing = true;
    auto applyCallbacks = applyProbe.GetCallbacks();
    const auto applyUpdates = applyCallbacks.applyFeatureUpdates;
    applyCallbacks.applyFeatureUpdates = [
        &isApplyThrowing, applyUpdates]() {
        if (isApplyThrowing) {
            isApplyThrowing = false;
            throw std::runtime_error("apply failure");
        }
        return applyUpdates();
    };
    HostFrameCoordinator applyCoordinator(1, std::move(applyCallbacks));
    if (!applyCoordinator.Enqueue("feature.retry", GetDelta(2))
        || applyCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Failed
        || applyCoordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Completed
        || applyProbe.intents.size() != 1
        || applyProbe.intents.front().delta.requestId != 2) {
        return false;
    }

    FrameProbe stageProbe;
    bool isStageThrowing = true;
    auto stageCallbacks = stageProbe.GetCallbacks();
    const auto buildStage = stageCallbacks.buildStage;
    stageCallbacks.buildStage = [&isStageThrowing, buildStage](
        const std::uint64_t epoch) {
        if (isStageThrowing) {
            isStageThrowing = false;
            throw std::runtime_error("stage failure");
        }
        return buildStage(epoch);
    };
    HostFrameCoordinator stageCoordinator(1, std::move(stageCallbacks));
    return stageCoordinator.Enqueue("feature.retry", GetDelta(3))
        && stageCoordinator.FlushOnOwnerTick(false)
            == HostFrameCoordinator::FlushStatus::Failed
        && stageCoordinator.FlushOnOwnerTick(false)
            == HostFrameCoordinator::FlushStatus::Completed
        && stageProbe.intents.size() == 1
        && stageProbe.intents.front().delta.requestId == 3;
}

bool GetUnchangedCompletionValid()
{
    FrameProbe probe;
    probe.stageStatus = HostFrameStageStatus::Unchanged;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    return coordinator.FlushOnOwnerTick(true)
            == HostFrameCoordinator::FlushStatus::Completed
        && coordinator.GetCommittedEpoch() == 0
        && probe.renderAttemptCount == 0
        && probe.completionCount == 1;
}

bool GetRenderRetryValid()
{
    FrameProbe probe;
    probe.isRenderSent = false;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    if (!coordinator.Enqueue("feature", GetDelta(1))
        || coordinator.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::RenderPending
        || coordinator.GetCommittedEpoch() != 1
        || probe.committedDataVersion != 2
        || probe.completionCount != 0
        || probe.renderAttemptCount != 1) {
        return false;
    }

    const auto stageCount = static_cast<std::size_t>(std::count(
        probe.events.begin(), probe.events.end(), "stage-view-a"));
    probe.isRenderSent = true;
    if (coordinator.FlushOnOwnerTick(true)
            != HostFrameCoordinator::FlushStatus::Completed
        || coordinator.GetCommittedEpoch() != 1
        || probe.committedDataVersion != 2
        || probe.renderAttemptCount != 2
        || probe.completionCount != 1
        || static_cast<std::size_t>(std::count(
            probe.events.begin(), probe.events.end(), "stage-view-a"))
            != stageCount) {
        return false;
    }

    FrameProbe thrownProbe;
    bool isRenderThrowing = true;
    auto callbacks = thrownProbe.GetCallbacks();
    const auto sendRender = callbacks.sendRender;
    callbacks.sendRender = [&isRenderThrowing, sendRender](
        const std::uint64_t epoch) {
        if (isRenderThrowing) {
            isRenderThrowing = false;
            throw std::runtime_error("render failure");
        }
        return sendRender(epoch);
    };
    HostFrameCoordinator thrownCoordinator(1, std::move(callbacks));
    if (!thrownCoordinator.Enqueue("feature", GetDelta(1))
        || thrownCoordinator.FlushOnOwnerTick(false)
            != HostFrameCoordinator::FlushStatus::RenderPending
        || thrownCoordinator.GetCommittedEpoch() != 1
        || thrownProbe.completionCount != 0) {
        return false;
    }
    const auto thrownStageCount = static_cast<std::size_t>(std::count(
        thrownProbe.events.begin(), thrownProbe.events.end(), "stage-view-a"));
    return thrownCoordinator.FlushOnOwnerTick(false)
            == HostFrameCoordinator::FlushStatus::Completed
        && thrownProbe.completionCount == 1
        && static_cast<std::size_t>(std::count(
            thrownProbe.events.begin(), thrownProbe.events.end(),
            "stage-view-a")) == thrownStageCount;
}

bool GetStopPreemptionValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    if (!coordinator.Enqueue("feature", GetDelta(1))) return false;
    coordinator.Stop();
    return !coordinator.Enqueue("feature", GetDelta(2))
        && coordinator.FlushOnOwnerTick(true)
            == HostFrameCoordinator::FlushStatus::Stopped
        && coordinator.GetCommittedEpoch() == 0
        && probe.renderAttemptCount == 0
        && probe.completionCount == 0;
}

bool GetCallbackStopValid()
{
    // 每个可重入的提交前阶段停止后，不得再提交或调用 Render。
    for (int phase = 0; phase < 4; ++phase) {
        FrameProbe probe;
        auto callbacks = probe.GetCallbacks();
        HostFrameCoordinator* owner = nullptr;
        if (phase == 0) callbacks.collectUpdates = [&]() { owner->Stop(); return true; };
        if (phase == 1) callbacks.sendFeatureTicks = [&]() { owner->Stop(); };
        if (phase == 2) callbacks.applyFeatureUpdates = [&]() { owner->Stop(); return true; };
        if (phase == 3) callbacks.buildStage = [&](std::uint64_t) {
            owner->Stop(); return HostFrameStageStatus::Ready;
        };
        HostFrameCoordinator coordinator(1, std::move(callbacks));
        owner = &coordinator;
        if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Stopped
            || coordinator.GetCommittedEpoch() != 0 || probe.renderAttemptCount != 0
            || probe.completionCount != 0) return false;
    }
    return true;
}

bool GetInputValidationValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    auto missingRequest = GetDelta(0);
    auto duplicateView = GetDelta(1);
    duplicateView.viewIds.push_back("view-a");
    auto targetMany = GetDelta(1);
    targetMany.scope = FeatureSceneScope::TargetViewOnly;
    return !coordinator.Enqueue("", GetDelta(1))
        && !coordinator.Enqueue("feature", std::move(missingRequest))
        && !coordinator.Enqueue("feature", std::move(duplicateView))
        && !coordinator.Enqueue("feature", std::move(targetMany));
}

bool GetDisplayBatchValid()
{
    FrameProbe probe;
    probe.isRenderSent = false;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    std::vector<int> completed;
    const auto ready = []() -> std::optional<bool> { return true; };
    if (!coordinator.SendDisplayComplete(ready, [&](bool value) { completed.push_back(value ? 1 : -1); })
        || coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::RenderPending
        || !completed.empty()) return false;
    if (!coordinator.SendDisplayComplete(ready, [&](bool value) { completed.push_back(value ? 2 : -2); })) return false;
    probe.isRenderSent = true;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || completed != std::vector<int>{1}) return false;
    return coordinator.FlushOnOwnerTick(true) == HostFrameCoordinator::FlushStatus::Completed
        && completed == std::vector<int>{1, 2};
}

bool GetDisplayReentryValid()
{
    FrameProbe probe;
    auto callbacks = probe.GetCallbacks();
    HostFrameCoordinator* frames = nullptr;
    bool isEnqueued = false;
    int completed = 0;
    const auto ready = []() -> std::optional<bool> { return true; };
    callbacks.collectUpdates = [&] {
        if (!isEnqueued) {
            isEnqueued = frames->SendDisplayComplete(ready, [&](bool value) {
                if (value) ++completed;
                (void)frames->SendDisplayComplete(ready, [&](bool next) { if (next) ++completed; });
            });
        }
        return true;
    };
    HostFrameCoordinator coordinator(1, std::move(callbacks));
    frames = &coordinator;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || !isEnqueued || completed != 0) return false;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || completed != 1) return false;
    return coordinator.FlushOnOwnerTick(true) == HostFrameCoordinator::FlushStatus::Completed
        && completed == 2;
}

bool GetDisplayPendingValid()
{
    FrameProbe probe;
    std::optional<bool> applied;
    int completed = 0;
    bool result = true;
    int readyCount = 0;
    auto callbacks = probe.GetCallbacks();
    callbacks.sendReadyCompletions = [&] { ++readyCount; };
    HostFrameCoordinator coordinator(1, std::move(callbacks));
    if (!coordinator.SendDisplayComplete([&] { return applied; }, [&](bool value) {
        ++completed;
        result = value;
    })) return false;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || completed != 0 || readyCount != 1) return false;
    probe.isRenderSent = false;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::RenderPending
        || readyCount != 2) return false;
    coordinator.Stop();
    coordinator.Stop();
    return completed == 1 && !result
        && !coordinator.SendDisplayComplete([]() -> std::optional<bool> { return true; }, [](bool) {});
}

bool GetDisplayDecisionValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    bool isApplied = true;
    int completed = 0;
    if (!coordinator.SendDisplayComplete([&]() -> std::optional<bool> { return isApplied; },
            [&](bool value) { if (value) ++completed; isApplied = false; })
        || !coordinator.SendDisplayComplete([&]() -> std::optional<bool> { return isApplied; },
            [&](bool value) { if (value) ++completed; })) return false;
    return coordinator.FlushOnOwnerTick(true) == HostFrameCoordinator::FlushStatus::Completed
        && completed == 2;
}

bool GetDisplayFailureValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    int failed = 0;
    int succeeded = 0;
    if (!coordinator.SendDisplayComplete([]() -> std::optional<bool> { return std::nullopt; }, [](bool) {})
        || !coordinator.SendDisplayComplete([]() -> std::optional<bool> { return false; },
            [&](bool value) { if (!value) ++failed; })
        || !coordinator.SendDisplayComplete([]() -> std::optional<bool> { return true; },
            [&](bool value) { if (value) ++succeeded; })) return false;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || failed != 1 || succeeded != 1) return false;
    coordinator.Stop();
    return failed == 1 && succeeded == 1;
}

bool GetRenderStopValid()
{
    FrameProbe probe;
    auto callbacks = probe.GetCallbacks();
    HostFrameCoordinator* frames = nullptr;
    callbacks.sendRender = [&](std::uint64_t) { frames->Stop(); return false; };
    HostFrameCoordinator coordinator(1, std::move(callbacks));
    frames = &coordinator;
    int cancelled = 0;
    if (!coordinator.SendDisplayComplete([]() -> std::optional<bool> { return true; },
        [&](bool value) { if (!value) ++cancelled; })) return false;
    return coordinator.FlushOnOwnerTick(true) == HostFrameCoordinator::FlushStatus::Stopped
        && cancelled == 1;
}

bool GetDisplayCapacityValid()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    std::size_t count = 0;
    const auto waiting = []() -> std::optional<bool> { return std::nullopt; };
    const auto complete = [&](bool value) { if (!value) ++count; };
    for (std::size_t index = 0; index < 1024; ++index) {
        if (!coordinator.SendDisplayComplete(waiting, complete)) return false;
    }
    if (coordinator.SendDisplayComplete(waiting, complete)) return false;
    coordinator.Stop();
    return count == 1024;
}

bool GetDisplayApplyFailureValid()
{
    FrameProbe probe;
    probe.isCollectSet = false;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    int completed = 0;
    if (!coordinator.SendDisplayComplete([]() -> std::optional<bool> { return false; },
        [&](bool value) { if (!value) ++completed; })) return false;
    return coordinator.FlushOnOwnerTick(true) == HostFrameCoordinator::FlushStatus::Failed
        && completed == 1 && probe.renderAttemptCount == 0;
}

bool GetDisplayReentryBounded()
{
    FrameProbe probe;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    std::size_t admitted = 0;
    const auto waiting = []() -> std::optional<bool> { return std::nullopt; };
    if (!coordinator.SendDisplayComplete([]() -> std::optional<bool> { return true; }, [&](bool) {
        for (std::size_t index = 0; index < 1024; ++index) {
            if (coordinator.SendDisplayComplete(waiting, [](bool) {})) ++admitted;
        }
    })) return false;
    for (std::size_t index = 1; index < 1024; ++index) {
        if (!coordinator.SendDisplayComplete(waiting, [](bool) {})) return false;
    }
    const auto status = coordinator.FlushOnOwnerTick(true);
    coordinator.Stop();
    return status == HostFrameCoordinator::FlushStatus::Completed && admitted == 1;
}

bool GetDisplayUnchangedValid()
{
    FrameProbe probe;
    probe.stageStatus = HostFrameStageStatus::Unchanged;
    HostFrameCoordinator coordinator(1, probe.GetCallbacks());
    int completed = 0;
    const auto ready = []() -> std::optional<bool> { return true; };
    const auto onComplete = [&](bool value) { if (value) ++completed; };
    if (!coordinator.SendDisplayComplete(ready, onComplete)
        || coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || completed != 0) return false;
    probe.stageStatus = HostFrameStageStatus::Ready;
    if (coordinator.FlushOnOwnerTick(true) != HostFrameCoordinator::FlushStatus::Completed
        || completed != 1 || probe.renderAttemptCount != 1) return false;
    probe.stageStatus = HostFrameStageStatus::Unchanged;
    return coordinator.SendDisplayComplete(ready, onComplete)
        && coordinator.FlushOnOwnerTick(true) == HostFrameCoordinator::FlushStatus::Completed
        && completed == 2 && probe.renderAttemptCount == 1;
}

} // namespace

int main()
{
    const bool isValid = GetSequenceValid()
        && GetLatestWinsValid()
        && GetFrozenBatchValid()
        && GetFeatureTickVisibleInSameFrameValid()
        && GetFailureBeforeCommitValid()
        && GetThrownPhaseRetryValid()
        && GetUnchangedCompletionValid()
        && GetRenderRetryValid()
        && GetStopPreemptionValid()
        && GetCallbackStopValid()
        && GetInputValidationValid()
        && GetDisplayBatchValid()
        && GetDisplayReentryValid()
        && GetDisplayPendingValid()
        && GetDisplayDecisionValid()
        && GetDisplayFailureValid()
        && GetRenderStopValid()
        && GetDisplayCapacityValid()
        && GetDisplayApplyFailureValid()
        && GetDisplayUnchangedValid()
        && GetDisplayReentryBounded();
    std::cout << (isValid
        ? "PASS: Host frame coordinator protocol\n"
        : "FAIL: Host frame coordinator protocol\n");
    return isValid ? 0 : 1;
}
