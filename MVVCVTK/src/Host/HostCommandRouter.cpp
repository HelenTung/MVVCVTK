#include "Host/HostCommandRouter.h"
#include "Host/Types/HostRequestTypes.h"
#include "Platform/Path.h"

#include "AppService.h"
#include "AppTypes.h"
#include "Host/HostCoreServices.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"
#include "VolumeTypes.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

// 宿主协议到应用服务的唯一请求适配层：
// - 只做具体 Request 识别、Host 类型到 App 类型转换和目标视图选择；
// - 不保存业务状态，也不直接执行裁切/间隙算法；
// - m_core、m_renderViews 为会话期非拥有观察指针。
class HostCommandRouter::Impl final {
public:
    Impl(const HostCoreServices& core, const HostRenderViewSet& renderViews)
        : m_core(&core), m_renderViews(&renderViews) {}

    bool Dispatch(
        HostRequest&& request,
        HostCompleteCallback onComplete) const;

private:
    struct ViewCandidate final {
        const HostRenderViewRuntime* view = nullptr;
        std::shared_ptr<VizService> service;
        std::shared_ptr<StdRenderContext> context;
        std::optional<VizMode> mode;
        std::optional<MaterialParams> material;
        std::optional<double> opacity;
        std::optional<std::vector<TFNode>> nodes;
        std::optional<double> iso;
        std::optional<BackgroundColor> background;
        std::optional<std::array<double, 3>> spacing;
        std::optional<WindowLevelParams> windowLevel;
        std::optional<VolumeQualityParams> volumeQuality;
        std::optional<std::vector<GradientOpacityNode>> gradientOpacity;
        std::optional<TransferPreset> transferPreset;
        std::optional<bool> isDenoiseOn;
        std::optional<HostCursorParams> cursor;
        std::optional<HostVisibilityParams> visibility;
        std::optional<bool> isAxesVisible;
    };

    bool SetView(const HostViewSetRequest& request) const;
    bool ResetViewCamera(const HostViewResetRequest& request) const;
    bool SetTool(const HostToolSetRequest& request) const;
    bool SwitchTool(const HostToolSwitchRequest& request) const;
    bool LoadFile(HostLoadRequest request, HostCompleteCallback callback) const;
    bool ReloadBuffer(HostReloadRequest request, HostCompleteCallback callback) const;
    bool ExportData(HostDataExportRequest request, HostCompleteCallback callback) const;
    bool ExportSlices(HostSliceExportRequest request, HostCompleteCallback callback) const;
    std::optional<VolumeLayout> BuildLoadLayout(
        const HostLoadRequest& request) const;
    std::optional<std::array<int, 3>> GetRawDims(
        const std::string& path) const;
    std::optional<VizMode> GetAppViewMode(HostRenderMode mode) const;
    std::optional<ToolMode> GetAppToolMode(HostToolMode mode) const;
    std::optional<MaterialParams> BuildAppMaterial(const HostMaterialParams& value) const;
    std::optional<MaterialParams> GetMaterialPreset(HostMaterialPreset preset) const;
    std::optional<VolumeQualityParams> BuildAppQuality(
        const HostVolumeQualityParams& value) const;
    std::optional<std::vector<GradientOpacityNode>> BuildAppGradient(
        const std::vector<HostGradientOpacityNode>& values) const;
    std::optional<TransferPreset> GetTransferPreset(
        HostTransferPreset preset) const;
    std::optional<BackgroundColor> BuildAppBackground(const HostBackgroundColor& value) const;
    std::optional<WindowLevelParams> BuildAppWindowLevel(const HostWindowLevelParams& value) const;
    std::optional<std::vector<TFNode>> BuildAppNodes(
        const std::vector<HostTransferNode>& values) const;
    std::optional<ViewCandidate> BuildViewCandidate(
        const HostViewSetRequest& request) const;
    bool GetUnitValid(double value) const;

    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
};

bool HostCommandRouter::Impl::Dispatch(
    HostRequest&& request,
    HostCompleteCallback onComplete) const
{
    if (auto* value = dynamic_cast<HostLoadRequest*>(&request)) {
        return LoadFile(
            std::move(*value),
            std::move(onComplete));
    }
    if (auto* value = dynamic_cast<HostReloadRequest*>(&request)) {
        return ReloadBuffer(
            std::move(*value),
            std::move(onComplete));
    }
    if (auto* value = dynamic_cast<HostDataExportRequest*>(&request)) {
        return ExportData(
            std::move(*value),
            std::move(onComplete));
    }
    if (auto* value = dynamic_cast<HostSliceExportRequest*>(&request)) {
        return ExportSlices(
            std::move(*value),
            std::move(onComplete));
    }
    if (const auto* value = dynamic_cast<const HostViewSetRequest*>(
        &request)) {
        return !onComplete && SetView(*value);
    }
    if (const auto* value = dynamic_cast<const HostViewResetRequest*>(
        &request)) {
        return !onComplete && ResetViewCamera(*value);
    }
    if (const auto* value = dynamic_cast<const HostToolSetRequest*>(
        &request)) {
        return !onComplete && SetTool(*value);
    }
    if (const auto* value = dynamic_cast<const HostToolSwitchRequest*>(
        &request)) {
        return !onComplete && SwitchTool(*value);
    }
    return false;
}

bool HostCommandRouter::Impl::LoadFile(
    HostLoadRequest request, HostCompleteCallback callback) const
{
    if (!m_renderViews || request.filePath.empty()) {
        return false;
    }
    const auto* view = m_renderViews->GetPrimaryView();
    if (!view || !view->service) {
        return false;
    }
    auto layout = BuildLoadLayout(request);
    if (!layout) return false;
    return view->service->LoadFileAsync(
        std::move(request.filePath), std::move(*layout),
        std::move(callback));
}

bool HostCommandRouter::Impl::ReloadBuffer(
    HostReloadRequest request, HostCompleteCallback callback) const
{
    if (!m_renderViews) {
        return false;
    }
    const auto* view = m_renderViews->GetPrimaryView();
    if (!view || !view->service) {
        return false;
    }
    auto layout = VolumeLayout::Create(
        request.geometry.dimensions,
        request.geometry.spacing,
        request.geometry.origin);
    if (!layout) return false;
    auto buffer = VolumeBuffer::Create(
        std::move(request.voxels), std::move(*layout));
    if (!buffer) return false;
    return view->service->ReloadFromBufferAsync(
        std::move(*buffer), std::move(callback));
}

std::optional<VolumeLayout> HostCommandRouter::Impl::BuildLoadLayout(
    const HostLoadRequest& request) const
{
    auto dimensions = request.geometry.dimensions;
    if (dimensions == std::array<int, 3>{ 0, 0, 0 }) {
        auto rawDimensions = GetRawDims(request.filePath);
        if (!rawDimensions) return std::nullopt;
        dimensions = *rawDimensions;
    }
    return VolumeLayout::Create(
        dimensions, request.geometry.spacing, request.geometry.origin);
}

std::optional<std::array<int, 3>> HostCommandRouter::Impl::GetRawDims(
    const std::string& path) const
{
    // 仅为未显式提供 dimensions 的 RAW 请求解析文件名尾部 NxMxK；
    // 解析从最后两个 x/X 分隔符向前收集第一段连续数字，前缀可包含其它描述文本。
    const std::filesystem::path filePath = PlatformPath::GetNativePath(path);
    std::string extension = PlatformPath::GetUtf8Path(filePath.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".raw") return std::nullopt;

    const std::string stem = PlatformPath::GetUtf8Path(filePath.stem());
    const auto second = stem.find_last_of("xX");
    if (second == std::string::npos || second + 1 >= stem.size()) {
        return std::nullopt;
    }
    const auto first = stem.find_last_of("xX", second - 1);
    if (first == std::string::npos || first + 1 == second) {
        return std::nullopt;
    }
    std::size_t firstStart = first;
    while (firstStart > 0
        && std::isdigit(static_cast<unsigned char>(stem[firstStart - 1]))) {
        --firstStart;
    }
    if (firstStart == first) return std::nullopt;

    std::array<int, 3> dimensions{};
    const std::array<std::pair<std::size_t, std::size_t>, 3> fields = {
        std::pair{ firstStart, first },
        std::pair{ first + 1, second },
        std::pair{ second + 1, stem.size() }
    };
    // from_chars 必须完整消费每个字段且结果为正，避免部分数字、符号或溢出被静默接受。
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto [beginIndex, endIndex] = fields[index];
        const char* begin = stem.data() + beginIndex;
        const char* end = stem.data() + endIndex;
        const auto result = std::from_chars(begin, end, dimensions[index]);
        if (result.ec != std::errc{} || result.ptr != end
            || dimensions[index] <= 0) {
            return std::nullopt;
        }
    }
    return dimensions;
}

bool HostCommandRouter::Impl::ExportData(
    HostDataExportRequest request, HostCompleteCallback callback) const
{
    if (!m_renderViews || request.outputPath.empty()) {
        return false;
    }
    const bool hasSource =
        !request.sourceView.viewId.empty()
        || request.sourceView.isViewRoleUsed;
    const auto* view = hasSource
        ? m_renderViews->GetViewBySelector(
            request.sourceView)
        : m_renderViews->GetPrimaryView();
    if (!view || !view->service) {
        return false;
    }

    std::string extension;
    if (request.format) {
        // Host 只把唯一格式基元收敛为规范后缀；App/Task 不解释，Data 才选择 writer。
        switch (*request.format) {
        case HostDataExportFormat::Raw:
            extension = ".raw";
            break;
        case HostDataExportFormat::Ply:
            extension = ".ply";
            break;
        case HostDataExportFormat::Stl:
            extension = ".stl";
            break;
        case HostDataExportFormat::Obj:
            extension = ".obj";
            break;
        default:
            return false;
        }
    }
    else {
        // 缺省格式由来源窗口完整收敛，调用方不再通过目录名暗示格式。
        switch (view->service->GetVizMode()) {
        case VizMode::Volume:
        case VizMode::CompositeVolume:
            extension = ".raw";
            break;
        case VizMode::IsoSurface:
        case VizMode::CompositeIsoSurface:
            extension = ".ply";
            break;
        case VizMode::SliceTop_down:
        case VizMode::SliceFront_back:
        case VizMode::SliceLeft_right:
            return false;
        }
    }
    view->service->ExportDataAsync(
        std::move(request.outputPath),
        std::move(extension),
        std::move(callback));
    return true;
}

bool HostCommandRouter::Impl::ExportSlices(
    HostSliceExportRequest request, HostCompleteCallback callback) const
{
    const auto* view = m_renderViews
        ? m_renderViews->GetViewBySelector(request.sourceView) : nullptr;
    if (!view || !view->service || request.outputDir.empty()
        || !m_renderViews->GetRoleIsSliceView(view->config.role)
        || (request.angleDeg && !std::isfinite(*request.angleDeg))) {
        return false;
    }
    view->service->ExportSlicesAsync(
        request.outputDir, request.angleDeg, std::move(callback));
    return true;
}

std::optional<VizMode> HostCommandRouter::Impl::GetAppViewMode(HostRenderMode mode) const
{
    switch (mode) {
    case HostRenderMode::Volume: return VizMode::Volume;
    case HostRenderMode::IsoSurface: return VizMode::IsoSurface;
    case HostRenderMode::SliceTopDown: return VizMode::SliceTop_down;
    case HostRenderMode::SliceFrontBack: return VizMode::SliceFront_back;
    case HostRenderMode::SliceLeftRight: return VizMode::SliceLeft_right;
    case HostRenderMode::CompositeVolume: return VizMode::CompositeVolume;
    case HostRenderMode::CompositeIsoSurface: return VizMode::CompositeIsoSurface;
    }
    return std::nullopt;
}

std::optional<ToolMode> HostCommandRouter::Impl::GetAppToolMode(HostToolMode mode) const
{
    switch (mode) {
    case HostToolMode::Navigation: return ToolMode::Navigation;
    case HostToolMode::ModelTransform: return ToolMode::ModelTransform;
    }
    return std::nullopt;
}

std::optional<MaterialParams> HostCommandRouter::Impl::BuildAppMaterial(
    const HostMaterialParams& value) const
{
    if (!GetUnitValid(value.ambient)
        || !GetUnitValid(value.diffuse)
        || !GetUnitValid(value.specular)
        || !std::isfinite(value.specularPower)
        || value.specularPower < 0.0
        || !GetUnitValid(value.opacity)) {
        return std::nullopt;
    }
    return MaterialParams{ value.ambient, value.diffuse, value.specular,
        value.specularPower, value.opacity, value.isShadeOn };
}

std::optional<BackgroundColor> HostCommandRouter::Impl::BuildAppBackground(
    const HostBackgroundColor& value) const
{
    if (!GetUnitValid(value.r) || !GetUnitValid(value.g)
        || !GetUnitValid(value.b)) {
        return std::nullopt;
    }
    return BackgroundColor{ value.r, value.g, value.b };
}

std::optional<WindowLevelParams> HostCommandRouter::Impl::BuildAppWindowLevel(
    const HostWindowLevelParams& value) const
{
    if (!std::isfinite(value.windowWidth) || value.windowWidth <= 0.0
        || !std::isfinite(value.windowCenter)) {
        return std::nullopt;
    }
    return WindowLevelParams{ value.windowWidth, value.windowCenter };
}

std::optional<std::vector<TFNode>> HostCommandRouter::Impl::BuildAppNodes(
    const std::vector<HostTransferNode>& values) const
{
    if (values.empty()) return std::nullopt;

    std::vector<TFNode> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (!GetUnitValid(value.position) || !GetUnitValid(value.opacity)
            || !GetUnitValid(value.r) || !GetUnitValid(value.g)
            || !GetUnitValid(value.b)
            || (!result.empty() && value.position < result.back().position)) {
            return std::nullopt;
        }
        result.push_back({ value.position, value.opacity, value.r, value.g, value.b });
    }
    return result;
}

std::optional<MaterialParams>
HostCommandRouter::Impl::GetMaterialPreset(HostMaterialPreset preset) const
{
    switch (preset) {
    case HostMaterialPreset::Soft:
        return MaterialParams{ 0.25, 0.65, 0.10, 8.0, 0.80, false };
    case HostMaterialPreset::Dense:
        return MaterialParams{ 0.10, 0.85, 0.25, 20.0, 1.00, false };
    case HostMaterialPreset::Glossy:
        return MaterialParams{ 0.08, 0.65, 0.65, 40.0, 1.00, true };
    }
    return std::nullopt;
}

std::optional<VolumeQualityParams>
HostCommandRouter::Impl::BuildAppQuality(
    const HostVolumeQualityParams& value) const
{
    switch (value.quality) {
    case HostVolumeQuality::Quality:
        return VolumeQualityParams{ VolumeQuality::Quality, 766, 1.0, true };
    case HostVolumeQuality::Custom:
        if (value.maxDimension < 1 || value.maxDimension > 16384
            || !std::isfinite(value.sampleDistance)
            || value.sampleDistance <= 0.0) {
            return std::nullopt;
        }
        return VolumeQualityParams{
            VolumeQuality::Custom,
            value.maxDimension,
            value.sampleDistance,
            value.isJitterOn
        };
    }
    return std::nullopt;
}

std::optional<std::vector<GradientOpacityNode>>
HostCommandRouter::Impl::BuildAppGradient(
    const std::vector<HostGradientOpacityNode>& values) const
{
    std::vector<GradientOpacityNode> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (!std::isfinite(value.gradient) || value.gradient < 0.0
            || !GetUnitValid(value.opacity)
            || (!result.empty()
                && value.gradient < result.back().gradient)) {
            return std::nullopt;
        }
        result.push_back({ value.gradient, value.opacity });
    }
    return result;
}

std::optional<TransferPreset>
HostCommandRouter::Impl::GetTransferPreset(HostTransferPreset preset) const
{
    switch (preset) {
    case HostTransferPreset::Percentile:
        return TransferPreset::Percentile;
    }
    return std::nullopt;
}

std::optional<HostCommandRouter::Impl::ViewCandidate>
HostCommandRouter::Impl::BuildViewCandidate(
    const HostViewSetRequest& request) const
{
    ViewCandidate candidate;
    candidate.view = m_renderViews
        ? m_renderViews->GetViewBySelector(request.targetView) : nullptr;
    if (!candidate.view || !candidate.view->service) return std::nullopt;

    candidate.service = candidate.view->service;
    candidate.context = candidate.view->context;
    candidate.mode = request.mode ? GetAppViewMode(*request.mode)
        : std::optional<VizMode>{};
    candidate.material = request.material ? BuildAppMaterial(*request.material)
        : std::optional<MaterialParams>{};
    if (request.materialPreset) {
        candidate.material = GetMaterialPreset(*request.materialPreset);
    }
    candidate.opacity = request.opacity;
    candidate.nodes = request.transferNodes ? BuildAppNodes(*request.transferNodes)
        : std::optional<std::vector<TFNode>>{};
    candidate.iso = request.iso;
    candidate.background = request.background
        ? BuildAppBackground(*request.background)
        : std::optional<BackgroundColor>{};
    candidate.spacing = request.spacing;
    candidate.windowLevel = request.windowLevel
        ? BuildAppWindowLevel(*request.windowLevel)
        : std::optional<WindowLevelParams>{};
    candidate.volumeQuality = request.volumeQuality
        ? BuildAppQuality(*request.volumeQuality)
        : std::optional<VolumeQualityParams>{};
    candidate.gradientOpacity = request.gradientOpacity
        ? BuildAppGradient(*request.gradientOpacity)
        : std::optional<std::vector<GradientOpacityNode>>{};
    candidate.transferPreset = request.transferPreset
        ? GetTransferPreset(*request.transferPreset)
        : std::optional<TransferPreset>{};
    candidate.isDenoiseOn = request.isDenoiseOn;
    candidate.cursor = request.cursor;
    candidate.visibility = request.visibility;
    candidate.isAxesVisible = request.isAxesVisible;

    if ((request.mode && (!candidate.mode || !candidate.context))
        || (request.material && !candidate.material)
        || (request.materialPreset && !candidate.material)
        || (request.opacity && !GetUnitValid(*request.opacity))
        || (request.transferNodes && !candidate.nodes)
        || (request.transferPreset && !candidate.transferPreset)
        || (request.iso && !std::isfinite(*request.iso))
        || (request.background && !candidate.background)
        || (request.windowLevel && !candidate.windowLevel)
        || (request.volumeQuality && !candidate.volumeQuality)
        || (request.gradientOpacity && !candidate.gradientOpacity)
        || (request.isAxesVisible && !candidate.context)) {
        return std::nullopt;
    }
    if ((request.materialPreset
            && (request.material || request.opacity))
        || (request.transferPreset && request.transferNodes)) {
        return std::nullopt;
    }

    if (candidate.spacing) {
        for (const double value : *candidate.spacing) {
            if (!std::isfinite(value) || value <= 0.0) {
                return std::nullopt;
            }
        }
    }
    if (candidate.cursor) {
        if (candidate.cursor->axis < -1 || candidate.cursor->axis > 2
            || !std::all_of(
                candidate.cursor->world.begin(),
                candidate.cursor->world.end(),
                [](double value) { return std::isfinite(value); })) {
            return std::nullopt;
        }
    }
    if (candidate.material && candidate.opacity) {
        candidate.material->opacity = *candidate.opacity;
        candidate.opacity.reset();
    }

    const VizMode effectiveMode = candidate.mode
        ? *candidate.mode : candidate.service->GetVizMode();
    const bool isVolumeMode = effectiveMode == VizMode::Volume
        || effectiveMode == VizMode::CompositeVolume;
    if (!isVolumeMode
        && (candidate.volumeQuality
            || candidate.gradientOpacity
            || candidate.isDenoiseOn)) {
        return std::nullopt;
    }

    return candidate;
}

bool HostCommandRouter::Impl::GetUnitValid(double value) const
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool HostCommandRouter::Impl::SetView(
    const HostViewSetRequest& request) const
{
    // 1. 先在局部候选中解析目标并完成所有字段校验。
    // 2. 可报告失败的 setter 全部排在 void 状态写入前，并逐项传播失败。
    // 3. 前一个可失败 setter 成功、后一个失败时仍可能留下部分状态；
    //    此处不把调用顺序优化表述为强原子回滚。
    // 4. 全部可失败 setter 成功后，再提交其余已验证状态。
    auto candidate = BuildViewCandidate(request);
    if (!candidate) return false;

    if (candidate->spacing) {
        const auto& spacing = *candidate->spacing;
        if (!candidate->service->SetSpacing(
            spacing[0], spacing[1], spacing[2])) {
            return false;
        }
    }
    if (candidate->volumeQuality
        && !candidate->service->SetVolumeQuality(
            *candidate->volumeQuality)) {
        return false;
    }
    if (candidate->gradientOpacity
        && !candidate->service->SetGradientOpacity(
            *candidate->gradientOpacity)) {
        return false;
    }
    if (candidate->transferPreset
        && !candidate->service->SetTransferPreset(
            *candidate->transferPreset)) {
        return false;
    }
    if (candidate->isDenoiseOn
        && !candidate->service->SetDenoiseOn(
            *candidate->isDenoiseOn)) {
        return false;
    }

    if (candidate->mode) {
        candidate->service->SetVizMode(*candidate->mode);
        candidate->context->SetCameraStyle(*candidate->mode);
    }
    if (candidate->material) candidate->service->SetMaterial(*candidate->material);
    if (candidate->opacity) candidate->service->SetOpacity(*candidate->opacity);
    if (candidate->nodes) candidate->service->SetTransferFunction(*candidate->nodes);
    if (candidate->iso) candidate->service->SetIsoThreshold(*candidate->iso);
    if (candidate->background) candidate->service->SetBackground(*candidate->background);
    if (candidate->windowLevel) candidate->service->SetWindowLevel(
        candidate->windowLevel->windowWidth,
        candidate->windowLevel->windowCenter);
    if (candidate->cursor) {
        candidate->service->SetCursorWorldPosition(
            candidate->cursor->world.data(),
            candidate->cursor->axis);
    }
    if (candidate->visibility) {
        const auto& visibility = *candidate->visibility;
        if (visibility.isPlanes3DVisible.has_value()) {
            candidate->service->SetElementVisible(
                VisFlags::Planes3D,
                *visibility.isPlanes3DVisible);
        }
        if (visibility.isCrosshairVisible.has_value()) {
            candidate->service->SetElementVisible(
                VisFlags::Crosshair,
                *visibility.isCrosshairVisible);
        }
        if (visibility.isRulerVisible.has_value()) {
            candidate->service->SetElementVisible(
                VisFlags::Ruler,
                *visibility.isRulerVisible);
        }
    }
    if (candidate->isAxesVisible) {
        candidate->context->SetOrientationAxesVisible(
            *candidate->isAxesVisible);
        // 方向轴属于 context，不发布 SharedState flags；显式标脏让 Qt Timer 产生下一帧。
        candidate->service->SetDirty();
    }
    return true;
}

bool HostCommandRouter::Impl::ResetViewCamera(
    const HostViewResetRequest& request) const
{
    const auto* view = m_renderViews
        ? m_renderViews->GetViewBySelector(request.targetView) : nullptr;
    if (!view || !view->context || !view->service) {
        return false;
    }
    view->context->ResetCamera();
    // ResetCamera 只修改 renderer camera，不触发共享状态广播。
    view->service->SetDirty();
    return true;
}

bool HostCommandRouter::Impl::SetTool(
    const HostToolSetRequest& request) const
{
    const auto* view = m_renderViews
        ? m_renderViews->GetViewBySelector(request.targetView) : nullptr;
    if (!view || !view->context) return false;
    const auto mode = GetAppToolMode(request.toolMode);
    if (!mode) return false;
    view->context->SetToolMode(*mode);
    return true;
}

bool HostCommandRouter::Impl::SwitchTool(
    const HostToolSwitchRequest& request) const
{
    const auto* view = m_renderViews
        ? m_renderViews->GetViewBySelector(request.targetView) : nullptr;
    if (!view || !view->context) return false;
    const ToolMode mode =
        view->context->GetToolMode() == ToolMode::Navigation
        ? ToolMode::ModelTransform
        : ToolMode::Navigation;
    view->context->SetToolMode(mode);
    return true;
}

HostCommandRouter::HostCommandRouter(const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
    : m_impl(std::make_unique<Impl>(core, renderViews)) {}

HostCommandRouter::~HostCommandRouter() = default;

bool HostCommandRouter::Dispatch(
    HostRequest&& request,
    HostCompleteCallback onComplete) const
{
    return m_impl
        && m_impl->Dispatch(
            std::move(request),
            std::move(onComplete));
}
