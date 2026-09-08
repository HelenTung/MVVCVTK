#include "App/Services/FeatureViewService.h"
#include "Host/Internal/HostRenderViewRuntime.h"
#include "Host/Internal/HostTransferCodec.h"
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

bool HostRenderViewRuntime::GetPresentationEqual(
    const AppViewState& appState, const HostRenderViewState& previous)
{
    static_assert(static_cast<int>(VolumeQuality::Auto) == static_cast<int>(HostVolumeQuality::Auto)
        && static_cast<int>(VolumeQuality::Low) == static_cast<int>(HostVolumeQuality::Low)
        && static_cast<int>(VolumeQuality::High) == static_cast<int>(HostVolumeQuality::High)
        && static_cast<int>(VolumeQuality::XHigh) == static_cast<int>(HostVolumeQuality::XHigh)
        && static_cast<int>(VolumeQuality::Ultra) == static_cast<int>(HostVolumeQuality::Ultra));
    const auto mode = GetHostViewMode(appState.mode);
    const auto& material = appState.material;
    const auto& transfer = appState.volumeTransferFunction;
    const auto& oldTransfer = previous.volumeTransferFunction;
    return mode && *mode == previous.viewMode
        && material.ambient == previous.material.ambient
        && material.diffuse == previous.material.diffuse
        && material.specular == previous.material.specular
        && material.specularPower == previous.material.specularPower
        && material.opacity == previous.material.opacity
        && material.isShadeOn == previous.material.isShadeOn
        && appState.isoThreshold == previous.isoThreshold
        && appState.background.r == previous.background.r
        && appState.background.g == previous.background.g
        && appState.background.b == previous.background.b
        && appState.spacing == previous.spacing
        && appState.windowLevel.windowWidth == previous.windowLevel.windowWidth
        && appState.windowLevel.windowCenter == previous.windowLevel.windowCenter
        && appState.scalarRange == previous.scalarRange
        && static_cast<int>(appState.volumeQuality) == static_cast<int>(previous.volumeQuality)
        && appState.isFeatureActive == previous.isFeatureActive
        && appState.isInteracting == previous.isInteracting
        && appState.cursorWorld == previous.cursorWorld
        && appState.visibilityMask == previous.visibilityMask
        && appState.dataRevision == previous.dataRevision
        && appState.bindingRevision == previous.bindingRevision
        && std::equal(transfer.colorNodes.begin(), transfer.colorNodes.end(),
            oldTransfer.colorNodes.begin(), oldTransfer.colorNodes.end(), [](const auto& a, const auto& b) {
                return a.scalar == b.scalar && a.r == b.r && a.g == b.g && a.b == b.b;
            })
        && std::equal(transfer.opacityNodes.begin(), transfer.opacityNodes.end(),
            oldTransfer.opacityNodes.begin(), oldTransfer.opacityNodes.end(), [](const auto& a, const auto& b) {
                return a.scalar == b.scalar && a.opacity == b.opacity;
            });
}

HostSceneViewState HostRenderViewRuntime::BuildSceneViewState(
    std::vector<std::string> featureIds, const HostSceneViewState* previous) const
{
    HostSceneViewState state;
    state.id = config.id;
    state.role = config.role;
    state.isAvailable = isAvailable;
    state.sceneEpoch = appliedEpoch;
    state.renderedEpoch = renderedEpoch;
    if (!isAvailable || !app.view) return state;

    const auto appState = app.view->GetViewState();
    if (previous && previous->id == config.id && previous->role == config.role
        && previous->isAvailable && previous->presentation
        && previous->presentationRevision == appState.revision
        && GetPresentationEqual(appState, *previous->presentation)) {
        state.presentation = previous->presentation;
        state.presentation->isAxesVisible = context && context->GetOrientationAxesVisible();
    }
    else state.presentation = BuildViewState(appState);
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
