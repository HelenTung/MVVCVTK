#include "Host/HostFeatureBindings.h"

#include "AppService.h"
#include "AppState.h"
#include "DataManager.h"
#include "Services/GapAnalysisService.h"

#include <vtkImageData.h>

#include <cmath>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

class HostFeatureBindings::Impl final {
public:
    ~Impl();

    void AttachFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);
    ImageSnapshot GetImageSnapshot() const;
    bool SetImageState(
        ImageState state,
        const ImageSnapshot& expectedSnapshot,
        ImageSnapshot& publishedSnapshot);
    bool StartGap(
        const HostGapStartRequest& request,
        HostCompleteCallback onComplete);
    bool SwitchGapLayer();
    bool ExitGap();
    bool GetGapView() const;
    bool OnHostTick();

private:
    static bool GetImageReady(vtkImageData* image);
    bool GetGapConfigValid(const HostGapConfig& config) const;
    GapVoidParams BuildVoidParams(
        const HostGapVoidConfig& config) const;
    GapSurfaceRequest BuildGapSurfaceRequest(
        const HostGapSurfaceConfig& config) const;
    std::optional<Orientation> GetGapSliceOrient(
        HostRenderViewRole role) const;

    HostCoreServices m_core;
    const HostRenderViewSet* m_renderViews = nullptr;
};

HostFeatureBindings::Impl::~Impl()
{
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ClearView();
    }
}

bool HostFeatureBindings::Impl::GetImageReady(vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }
    int dimensions[3] = { 0, 0, 0 };
    image->GetDimensions(dimensions);
    return dimensions[0] > 0
        && dimensions[1] > 0
        && dimensions[2] > 0;
}

bool HostFeatureBindings::Impl::GetGapConfigValid(
    const HostGapConfig& config) const
{
    if (!std::isfinite(config.surface.dataRangeRatio)
        || !std::isfinite(config.surface.absoluteIsoValue)
        || !std::isfinite(config.voidDetection.grayMin)
        || !std::isfinite(config.voidDetection.grayMax)
        || !std::isfinite(config.voidDetection.minVolumeMM3)
        || !std::isfinite(
            config.voidDetection.angleThresholdDeg)) {
        return false;
    }
    if (config.surface.isoMode
            == HostGapAnalysisIsoMode::DataRangeRatio
        && (config.surface.dataRangeRatio < 0.0
            || config.surface.dataRangeRatio > 1.0)) {
        return false;
    }
    return config.voidDetection.grayMin
            <= config.voidDetection.grayMax
        && config.voidDetection.tensorWindowSize > 0
        && config.voidDetection.erosionIterations >= 0;
}

GapVoidParams HostFeatureBindings::Impl::BuildVoidParams(
    const HostGapVoidConfig& config) const
{
    GapVoidParams params;
    params.grayMin = config.grayMin;
    params.grayMax = config.grayMax;
    params.minVolumeMM3 = config.minVolumeMM3;
    params.angleThresholdDeg = config.angleThresholdDeg;
    params.tensorWindowSize = config.tensorWindowSize;
    params.erosionIterations = config.erosionIterations;
    return params;
}

GapSurfaceRequest HostFeatureBindings::Impl::BuildGapSurfaceRequest(
    const HostGapSurfaceConfig& config) const
{
    GapSurfaceRequest request;
    request.isoMode = config.isoMode
            == HostGapAnalysisIsoMode::AbsoluteValue
        ? GapIsoMode::AbsoluteValue
        : GapIsoMode::DataRangeRatio;
    request.dataRangeRatio = config.dataRangeRatio;
    request.absoluteIsoValue = config.absoluteIsoValue;
    return request;
}

std::optional<Orientation>
HostFeatureBindings::Impl::GetGapSliceOrient(
    const HostRenderViewRole role) const
{
    switch (role) {
    case HostRenderViewRole::TopDownSlice:
        return Orientation::Top_down;
    case HostRenderViewRole::FrontBackSlice:
        return Orientation::Front_back;
    case HostRenderViewRole::LeftRightSlice:
        return Orientation::Left_right;
    default:
        return std::nullopt;
    }
}

void HostFeatureBindings::Impl::AttachFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_core = core;
    m_renderViews = &renderViews;
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ClearView();
    }
}

ImageSnapshot HostFeatureBindings::Impl::GetImageSnapshot() const
{
    return m_core.sharedDataMgr
        ? m_core.sharedDataMgr->GetImageSnapshot()
        : ImageSnapshot{};
}

bool HostFeatureBindings::Impl::SetImageState(
    ImageState state,
    const ImageSnapshot& expectedSnapshot,
    ImageSnapshot& publishedSnapshot)
{
    publishedSnapshot.reset();
    if (!m_core.sharedDataMgr
        || !m_core.sharedState
        || !expectedSnapshot
        || !state.image
        || !m_core.sharedDataMgr->SetCurrentData(
            std::move(state),
            expectedSnapshot,
            publishedSnapshot)) {
        return false;
    }

    // current 已经发布；通知异常或未来的非关键返回值不能把 CAS 伪装成失败。
    try {
        (void)m_core.sharedState->SetImageDataReady(
            publishedSnapshot->scalarRange[0],
            publishedSnapshot->scalarRange[1],
            publishedSnapshot->spacing);
    }
    catch (...) {
    }
    return true;
}

bool HostFeatureBindings::Impl::StartGap(
    const HostGapStartRequest& request,
    HostCompleteCallback onComplete)
{
    if (!m_renderViews
        || !m_core.gapAnalysis
        || !GetGapConfigValid(request.algorithm)
        || (request.targetViews.viewIds.empty()
            && request.targetViews.viewRoles.empty()
            && !request.isDefaultOverlayUsed)) {
        return false;
    }

    auto targetViews = m_renderViews->GetViewsByTargets(
        request.targetViews);
    if (targetViews.empty() && request.isDefaultOverlayUsed) {
        targetViews = m_renderViews->GetGapOverlayViews();
    }
    if (targetViews.empty()) {
        return false;
    }

    std::vector<std::shared_ptr<OverlayService>> meshTargets;
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>>
        sliceTargets;
    for (const auto* view : targetViews) {
        if (!view || !view->service) {
            continue;
        }
        if (m_renderViews->GetRoleIs3DView(view->config.role)) {
            meshTargets.push_back(view->service);
        }
        if (const auto orientation =
            GetGapSliceOrient(view->config.role)) {
            sliceTargets.push_back(
                { *orientation, view->service });
        }
    }

    const auto imageState = GetImageSnapshot();
    if (!imageState || !GetImageReady(imageState->image)) {
        return false;
    }

    GapViewRequest viewRequest;
    viewRequest.inputImage = imageState->image;
    viewRequest.surface = BuildGapSurfaceRequest(
        request.algorithm.surface);
    viewRequest.voidParams = BuildVoidParams(
        request.algorithm.voidDetection);
    viewRequest.meshTargets = std::move(meshTargets);
    viewRequest.sliceTargets = std::move(sliceTargets);
    return m_core.gapAnalysis->StartView(
        std::move(viewRequest), std::move(onComplete));
}

bool HostFeatureBindings::Impl::SwitchGapLayer()
{
    return m_core.gapAnalysis
        && m_core.gapAnalysis->SwitchOverlay();
}

bool HostFeatureBindings::Impl::ExitGap()
{
    return m_core.gapAnalysis
        && m_core.gapAnalysis->ExitView();
}

bool HostFeatureBindings::Impl::GetGapView() const
{
    return m_core.gapAnalysis
        && m_core.gapAnalysis->GetViewOn();
}

bool HostFeatureBindings::Impl::OnHostTick()
{
    if (m_core.gapAnalysis
        && m_core.gapAnalysis->GetDisplayTickNeeded()) {
        m_core.gapAnalysis->OnDisplayTick(nullptr);
    }
    return true;
}

HostFeatureBindings::HostFeatureBindings()
    : m_impl(std::make_unique<Impl>())
{
}

HostFeatureBindings::~HostFeatureBindings() = default;

void HostFeatureBindings::AttachFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_impl->AttachFeatures(core, renderViews);
}

ImageSnapshot HostFeatureBindings::GetImageSnapshot() const
{
    return m_impl ? m_impl->GetImageSnapshot() : ImageSnapshot{};
}

bool HostFeatureBindings::SetImageState(
    ImageState state,
    const ImageSnapshot& expectedSnapshot,
    ImageSnapshot& publishedSnapshot)
{
    return m_impl
        && m_impl->SetImageState(
            std::move(state),
            expectedSnapshot,
            publishedSnapshot);
}

bool HostFeatureBindings::StartGap(
    const HostGapStartRequest& request,
    HostCompleteCallback onComplete)
{
    return m_impl
        && m_impl->StartGap(request, std::move(onComplete));
}

bool HostFeatureBindings::SwitchGapLayer()
{
    return m_impl && m_impl->SwitchGapLayer();
}

bool HostFeatureBindings::ExitGap()
{
    return m_impl && m_impl->ExitGap();
}

bool HostFeatureBindings::GetGapView() const
{
    return m_impl && m_impl->GetGapView();
}

bool HostFeatureBindings::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}
