#include "App/Services/FeatureViewService.h"
#include "Host/Internal/HostRenderViewRuntime.h"
#include "Host/Internal/HostTransferCodec.h"
#include "Host/Internal/HostRulerCodec.h"
#include "Interaction/AbstractViewContext.h"
#include <algorithm>
#include <chrono>
#include <utility>

std::optional<HostRenderMode>
HostRenderViewRuntime::GetHostViewMode(const VizMode mode)
{
    switch (mode) {
    case VizMode::Volume: return HostRenderMode::Volume;
    case VizMode::IsoSurface: return HostRenderMode::IsoSurface;
    case VizMode::SliceTop_down:
        return HostRenderMode::SliceTopDown;
    case VizMode::SliceFront_back:
        return HostRenderMode::SliceFrontBack;
    case VizMode::SliceLeft_right:
        return HostRenderMode::SliceLeftRight;
    case VizMode::CompositeVolume:
        return HostRenderMode::CompositeVolume;
    case VizMode::CompositeIsoSurface:
        return HostRenderMode::CompositeIsoSurface;
    }
    return std::nullopt;
}

HostRenderViewState HostRenderViewRuntime::BuildViewState() const
{
    HostRenderViewState state;
    state.id = config.id;
    state.role = config.role;
    if (!isAvailable || !app.view) return state;

    const auto appState = app.view->GetViewState();
    return BuildViewState(appState);
}

HostRenderViewState HostRenderViewRuntime::BuildViewState(
    const AppViewState& appState) const
{
    HostRenderViewState state;
    state.id = config.id;
    state.role = config.role;
    const auto viewMode = GetHostViewMode(appState.mode);
    if (viewMode) state.viewMode = *viewMode;
    state.material = {
        appState.material.ambient,
        appState.material.diffuse,
        appState.material.specular,
        appState.material.specularPower,
        appState.material.opacity,
        appState.material.isShadeOn
    };
    state.volumeTransferFunction =
        HostTransferCodec::GetHostVolumeTransfer(
            appState.volumeTransferFunction);
    state.isoThreshold = appState.isoThreshold;
    state.background = {
        appState.background.r,
        appState.background.g,
        appState.background.b
    };
    state.spacing = appState.spacing;
    state.windowLevel = {
        appState.windowLevel.windowWidth,
        appState.windowLevel.windowCenter
    };
    state.scalarRange = appState.scalarRange;
    switch (appState.volumeQuality) {
    case VolumeQuality::Auto:
        state.volumeQuality = HostVolumeQuality::Auto;
        break;
    case VolumeQuality::Low:
        state.volumeQuality = HostVolumeQuality::Low;
        break;
    case VolumeQuality::High:
        state.volumeQuality = HostVolumeQuality::High;
        break;
    case VolumeQuality::XHigh:
        state.volumeQuality = HostVolumeQuality::XHigh;
        break;
    case VolumeQuality::Ultra:
        state.volumeQuality = HostVolumeQuality::Ultra;
        break;
    }
    state.isFeatureActive = appState.isFeatureActive;
    state.isInteracting = appState.isInteracting;
    state.cursorWorld = appState.cursorWorld;
    state.visibilityMask = appState.visibilityMask;
    state.ruler = HostRulerCodec::GetParams(appState.ruler);
    state.rulerState = HostRulerCodec::GetState(appState.rulerState);
    state.dataRevision = appState.dataRevision;
    state.bindingRevision = appState.bindingRevision;
    state.isAxesVisible = context
        && context->GetOrientationAxesVisible();
    return state;
}

HostCameraState HostRenderViewRuntime::GetHostCamera(
    const ViewCameraState& source)
{
    HostCameraState target;
    target.position = source.position;
    target.focalPoint = source.focalPoint;
    target.viewUp = source.viewUp;
    target.clippingRange = source.clippingRange;
    target.parallelScale = source.parallelScale;
    target.viewAngle = source.viewAngle;
    target.isParallel = source.isParallel;
    return target;
}

HostSceneViewState HostRenderViewRuntime::BuildSceneViewState(
    std::vector<std::string> featureIds) const
{
    HostSceneViewState state;
    state.id = config.id;
    state.role = config.role;
    state.isAvailable = isAvailable;
    state.sceneEpoch = appliedEpoch;
    state.renderedEpoch = renderedEpoch;
    if (!isAvailable || !app.view) return state;

    const auto appState = app.view->GetViewState();
    state.presentation = BuildViewState(appState);
    state.presentationRevision = appState.revision;
    if (context) {
        const auto camera = context->GetCameraState();
        if (camera) state.camera = GetHostCamera(*camera);
    }
    state.activeFeatureIds = std::move(featureIds);
    return state;
}

bool HostRenderViewRuntime::GetIntentStampValid(
    const RenderInputStamp& expected) const
{
    if (expected.dataRevision == DataRevisionRef{}) return true;
    if (!featureView) return false;
    const auto current = featureView->GetRenderInputStamp();
    return current && *current == expected;
}


bool HostRenderViewRuntime::CollectUpdates() const
{
    if (!isAvailable) return true;
    if (!interaction.update || !app.view || !context) return false;
    try { return interaction.update->SendUpdates(); }
    catch (...) { return false; }
}
bool HostRenderViewRuntime::SendPendingUpdates() const
{
    if (!isAvailable) return true;
    if (!interaction.update || !app.view || !context) return false;
    try { return interaction.update->SendPendingUpdates(); }
    catch (...) { return false; }
}
bool HostRenderViewRuntime::SetRenderNeeded()
{
    if (!interaction.update) return false;
    isFrameRenderNeeded = true;
    return true;
}
bool HostRenderViewRuntime::ResetRenderNeeded()
{
    if (!isAvailable || !interaction.update) return false;
    const bool isAppDirty = interaction.update->ResetRenderNeeded();
    return std::exchange(isFrameRenderNeeded, false) || isAppDirty;
}
bool HostRenderViewRuntime::GetRenderNeeded() const
{
    return isFrameRenderNeeded || (interaction.update && interaction.update->GetRenderNeeded());
}
bool HostRenderViewRuntime::SendRender(const std::uint64_t epoch)
{
    bool isRendered = false;
    try {
        const auto renderStart = std::chrono::steady_clock::now();
        isRendered = isAvailable && context && context->SendRender();
        if (isRendered && interaction.update) {
            const auto durationUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - renderStart).count());
            interaction.update->SetRenderComplete(std::max<std::uint64_t>(1, durationUs));
        }
    }
    catch (...) { isRendered = false; }
    if (isRendered) {
        pendingRenderEpoch = 0;
        renderedEpoch = epoch;
    }
    return isRendered;
}
void HostRenderViewRuntime::SendCompletions() const noexcept
{
    try { if (isAvailable && interaction.update) interaction.update->SendCompletions(); }
    catch (...) {}
}
