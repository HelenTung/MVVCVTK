#pragma once
#include "Host/Types/HostSessionTypes.h"
#include "Host/HostFeature.h"
#include "App/Services/AppPorts.h"
#include "Interaction/InteractionPorts.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>
class AbstractViewContext;
struct ViewCameraState;
class RenderBindPort;

// Registry 独占的单 View 参与者；仅在 owner thread 执行，不制定跨 View 顺序。
struct HostRenderViewRuntime final {
    HostRenderViewConfig config;
    AppPorts app;
    std::shared_ptr<AppDataStagePort> dataStage;
    InteractionPorts interaction;
    std::shared_ptr<RenderBindPort> renderBind;
    std::shared_ptr<FeatureViewService> featureView;
    std::shared_ptr<OverlayService> overlay;
    std::shared_ptr<AppTaskControlPort> taskControl;
    std::shared_ptr<AbstractViewContext> context;
    std::uint64_t appliedEpoch = 0;
    std::uint64_t renderedEpoch = 0;
    std::uint64_t pendingRenderEpoch = 0;
    bool isAvailable = false;
    HostRenderViewState BuildViewState() const;
    HostRenderViewState BuildViewState(const AppViewState& appState) const;
    HostSceneViewState BuildSceneViewState(std::vector<std::string> featureIds) const;
    bool GetIntentStampValid(const RenderInputStamp& expected) const;
    bool CollectUpdates() const;
    bool SendPendingUpdates() const;
    bool SetRenderNeeded() const;
    bool ResetRenderNeeded() const;
    bool SendRender(std::uint64_t epoch);
    void SendCompletions() const noexcept;
    static std::optional<HostRenderMode> GetHostViewMode(VizMode mode);
private:
    static HostCameraState GetHostCamera(const ViewCameraState& source);
};
