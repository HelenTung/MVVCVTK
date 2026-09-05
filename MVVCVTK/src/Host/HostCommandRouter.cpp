#include "Host/HostCommandRouter.h"
#include "Host/Types/HostRequestTypes.h"
#include "Platform/Path.h"

#include "App/AppTypes.h"
#include "App/Services/AppPorts.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Internal/HostTransferCodec.h"
#include "Interaction/AbstractViewContext.h"
#include "Interaction/InteractionPorts.h"
#include "Data/VolumeTypes.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

// 宿主协议到应用服务的唯一请求适配层：
// - 只做具体 Request 识别、Host 类型到 App 类型转换和目标视图选择；
// - 不保存业务状态，也不直接执行裁切/间隙算法；
// - 只保存 View runtime 发放的弱目录，不取得 runtime 容器。
class HostCommandRouter::Impl final {
public:
    explicit Impl(std::weak_ptr<IHostViewDirectory> directory)
        : m_directory(std::move(directory))
    {
    }

    bool Dispatch(
        HostRequest&& request,
        HostCompleteCallback onComplete) const;

private:
    struct ViewCandidate final {
        std::shared_ptr<AppViewPort> viewPort;
        std::shared_ptr<RenderUpdatePort> updatePort;
        std::shared_ptr<AbstractViewContext> context;
        std::function<bool()> stopView;
        std::optional<VizMode> mode;
        std::optional<MaterialParams> material;
        std::optional<double> opacity;
        std::optional<VolumeTransferFunction>
            volumeTransferFunction;
        std::optional<double> iso;
        std::optional<BackgroundColor> background;
        std::optional<WindowLevelParams> windowLevel;
        std::optional<VolumeQuality> volumeQuality;
        std::optional<HostVisibilityParams> visibility;
        std::optional<bool> isAxesVisible;
    };

    bool SetView(const HostViewSetRequest& request) const;
    bool SetSession(const HostSessionSetRequest& request) const;
    bool ResetView(const HostViewResetRequest& request) const;
    bool SetTool(const HostToolSetRequest& request) const;
    bool SwitchTool(const HostToolSwitchRequest& request) const;
    std::optional<HostDataRoute> GetDataRoute(
        const HostViewTarget& target) const;
    std::optional<HostViewRoute> GetViewRoute(
        const HostViewTarget& target) const;
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
    std::optional<VolumeQuality> GetAppQuality(
        HostVolumeQuality quality) const;
    std::optional<BackgroundColor> BuildAppBackground(const HostBackgroundColor& value) const;
    std::optional<WindowLevelParams> BuildAppWindowLevel(const HostWindowLevelParams& value) const;
    std::optional<ViewCandidate> BuildViewCandidate(
        const HostViewSetRequest& request) const;
    bool GetUnitValid(double value) const;

    std::weak_ptr<IHostViewDirectory> m_directory;
};

bool HostCommandRouter::Impl::Dispatch(
    HostRequest&& request,
    HostCompleteCallback onComplete) const
{
    const auto sendSync = [&onComplete](const bool isSucceeded) {
        if (!onComplete) return isSucceeded;
        try { onComplete(isSucceeded); }
        catch (...) {
            // 同步命令已经完成；调用方回调异常不能把已识别请求改写为拒绝。
        }
        return true;
    };

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
        return sendSync(SetView(*value));
    }
    if (const auto* value = dynamic_cast<const HostSessionSetRequest*>(
        &request)) {
        return sendSync(SetSession(*value));
    }
    if (const auto* value = dynamic_cast<const HostViewResetRequest*>(
        &request)) {
        return sendSync(ResetView(*value));
    }
    if (const auto* value = dynamic_cast<const HostToolSetRequest*>(
        &request)) {
        return sendSync(SetTool(*value));
    }
    if (const auto* value = dynamic_cast<const HostToolSwitchRequest*>(
        &request)) {
        return sendSync(SwitchTool(*value));
    }
    return false;
}

std::optional<HostDataRoute>
HostCommandRouter::Impl::GetDataRoute(
    const HostViewTarget& target) const
{
    const auto directory = m_directory.lock();
    return directory
        ? directory->GetDataRoute(target)
        : std::optional<HostDataRoute>{};
}

std::optional<HostViewRoute>
HostCommandRouter::Impl::GetViewRoute(
    const HostViewTarget& target) const
{
    const auto directory = m_directory.lock();
    return directory
        ? directory->GetViewRoute(target)
        : std::optional<HostViewRoute>{};
}

bool HostCommandRouter::Impl::LoadFile(
    HostLoadRequest request, HostCompleteCallback callback) const
{
    if (request.filePath.empty()
        || (request.metadata.source.kind != ImageSourceKind::RawFile
            && request.metadata.source.kind
                != ImageSourceKind::TiffSeries)) {
        return false;
    }
    const auto route = GetDataRoute(HostViewTarget{});
    const auto data = route ? route->data.lock() : nullptr;
    if (!data) {
        return false;
    }
    auto layout = BuildLoadLayout(request);
    if (!layout) return false;
    return data->LoadFileAsync(
        std::move(request.filePath), std::move(*layout),
        std::move(callback)) == TaskAdmissionResult::Accepted;
}

bool HostCommandRouter::Impl::ReloadBuffer(
    HostReloadRequest request, HostCompleteCallback callback) const
{
    if (request.metadata.source.kind != ImageSourceKind::Memory) {
        return false;
    }
    const auto route = GetDataRoute(HostViewTarget{});
    const auto data = route ? route->data.lock() : nullptr;
    if (!data) {
        return false;
    }
    auto layout = VolumeLayout::Create(
        request.geometry.dimensions,
        request.geometry.spacing,
        request.geometry.origin,
        request.geometry.direction,
        std::move(request.metadata));
    if (!layout) return false;
    auto buffer = VolumeBuffer::Create(
        std::move(request.voxels), std::move(*layout));
    if (!buffer) return false;
    return data->ReloadFromBufferAsync(
        std::move(*buffer), std::move(callback))
        == TaskAdmissionResult::Accepted;
}

std::optional<VolumeLayout> HostCommandRouter::Impl::BuildLoadLayout(
    const HostLoadRequest& request) const
{
    auto dimensions = request.geometry.dimensions;
    if (dimensions == std::array<int, 3>{ 0, 0, 0 }) {
        if (request.metadata.source.kind
            != ImageSourceKind::RawFile) {
            return std::nullopt;
        }
        auto rawDimensions = GetRawDims(request.filePath);
        if (!rawDimensions) return std::nullopt;
        dimensions = *rawDimensions;
    }
    ImageMetadata metadata = request.metadata;
    if (metadata.source.uri.empty()) {
        metadata.source.uri = request.filePath;
    }
    return VolumeLayout::Create(
        dimensions,
        request.geometry.spacing,
        request.geometry.origin,
        request.geometry.direction,
        std::move(metadata));
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
    if (request.outputPath.empty()) {
        return false;
    }
    const auto route = GetDataRoute(request.sourceView);
    const auto data = route ? route->data.lock() : nullptr;
    if (!data) {
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
        switch (route->mode) {
        case HostRenderMode::Volume:
        case HostRenderMode::CompositeVolume:
            extension = ".raw";
            break;
        case HostRenderMode::IsoSurface:
        case HostRenderMode::CompositeIsoSurface:
            extension = ".ply";
            break;
        case HostRenderMode::SliceTopDown:
        case HostRenderMode::SliceFrontBack:
        case HostRenderMode::SliceLeftRight:
            return false;
        }
    }
    return data->ExportDataAsync(
        std::move(request.outputPath),
        std::move(extension),
        std::move(callback)) == TaskAdmissionResult::Accepted;
}

bool HostCommandRouter::Impl::ExportSlices(
    HostSliceExportRequest request, HostCompleteCallback callback) const
{
    const bool hasSource = !request.sourceView.viewId.empty()
        || request.sourceView.isViewRoleUsed;
    const auto route = hasSource
        ? GetDataRoute(request.sourceView)
        : std::optional<HostDataRoute>{};
    const bool isSlice = route
        && (route->role == HostRenderViewRole::TopDownSlice
            || route->role == HostRenderViewRole::FrontBackSlice
            || route->role == HostRenderViewRole::LeftRightSlice);
    const auto data = route ? route->data.lock() : nullptr;
    if (!data || request.outputDir.empty()
        || !isSlice
        || (request.angleDeg && !std::isfinite(*request.angleDeg))) {
        return false;
    }
    return data->ExportSlicesAsync(
        std::move(request.outputDir),
        request.angleDeg,
        std::move(callback)) == TaskAdmissionResult::Accepted;
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

std::optional<VolumeQuality>
HostCommandRouter::Impl::GetAppQuality(
    const HostVolumeQuality quality) const
{
    switch (quality) {
    case HostVolumeQuality::Auto:
        return VolumeQuality::Auto;
    case HostVolumeQuality::Low:
        return VolumeQuality::Low;
    case HostVolumeQuality::High:
        return VolumeQuality::High;
    case HostVolumeQuality::XHigh:
        return VolumeQuality::XHigh;
    case HostVolumeQuality::Ultra:
        return VolumeQuality::Ultra;
    }
    return std::nullopt;
}

std::optional<HostCommandRouter::Impl::ViewCandidate>
HostCommandRouter::Impl::BuildViewCandidate(
    const HostViewSetRequest& request) const
{
    const auto route = GetViewRoute(request.targetView);
    const auto view = route ? route->view.lock() : nullptr;
    const auto update = route ? route->update.lock() : nullptr;
    if (!view || !update) {
        return std::nullopt;
    }

    ViewCandidate candidate;
    candidate.viewPort = view;
    candidate.updatePort = update;
    candidate.context = route->context.lock();
    candidate.stopView = route->stopView;
    candidate.mode = request.mode ? GetAppViewMode(*request.mode)
        : std::optional<VizMode>{};
    candidate.material = request.material ? BuildAppMaterial(*request.material)
        : std::optional<MaterialParams>{};
    if (request.materialPreset) {
        candidate.material = GetMaterialPreset(*request.materialPreset);
    }
    candidate.opacity = request.opacity;
    candidate.volumeTransferFunction =
        request.volumeTransferFunction
        ? HostTransferCodec::BuildVolumeTransferFunction(
            *request.volumeTransferFunction)
        : std::optional<VolumeTransferFunction>{};
    candidate.iso = request.iso;
    candidate.background = request.background
        ? BuildAppBackground(*request.background)
        : std::optional<BackgroundColor>{};
    candidate.windowLevel = request.windowLevel
        ? BuildAppWindowLevel(*request.windowLevel)
        : std::optional<WindowLevelParams>{};
    candidate.volumeQuality = request.volumeQuality
        ? GetAppQuality(*request.volumeQuality)
        : std::optional<VolumeQuality>{};
    candidate.visibility = request.visibility;
    candidate.isAxesVisible = request.isAxesVisible;

    if ((request.mode && (!candidate.mode || !candidate.context))
        || (request.material && !candidate.material)
        || (request.materialPreset && !candidate.material)
        || (request.opacity && !GetUnitValid(*request.opacity))
        || (request.volumeTransferFunction
            && !candidate.volumeTransferFunction)
        || (request.iso && !std::isfinite(*request.iso))
        || (request.background && !candidate.background)
        || (request.windowLevel && !candidate.windowLevel)
        || (request.volumeQuality && !candidate.volumeQuality)
        || (request.isAxesVisible && !candidate.context)) {
        return std::nullopt;
    }
    if ((request.materialPreset
            && (request.material || request.opacity))
        ) {
        return std::nullopt;
    }

    if (candidate.material && candidate.opacity) {
        candidate.material->opacity = *candidate.opacity;
        candidate.opacity.reset();
    }

    const VizMode effectiveMode = candidate.mode
        ? *candidate.mode : candidate.viewPort->GetViewState().mode;
    const bool isQualityMode = effectiveMode == VizMode::Volume
        || effectiveMode == VizMode::CompositeVolume
        || effectiveMode == VizMode::IsoSurface
        || effectiveMode == VizMode::CompositeIsoSurface;
    if (!isQualityMode
        && candidate.volumeQuality) {
        return std::nullopt;
    }

    return candidate;
}

bool HostCommandRouter::Impl::GetUnitValid(double value) const
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool HostCommandRouter::Impl::SetSession(
    const HostSessionSetRequest& request) const
{
    if (!request.spacing && !request.cursor) return false;

    AppSessionUpdate update;
    update.spacing = request.spacing;
    if (request.spacing
        && std::any_of(
            request.spacing->begin(), request.spacing->end(),
            [](const double value) {
                return !std::isfinite(value) || value <= 0.0;
            })) {
        return false;
    }
    if (request.cursor) {
        if (request.cursor->axis < -1 || request.cursor->axis > 2
            || std::any_of(
                request.cursor->world.begin(),
                request.cursor->world.end(),
                [](const double value) {
                    return !std::isfinite(value);
                })) {
            return false;
        }
        update.cursorWorld = request.cursor->world;
        update.cursorAxis = static_cast<AppCursorAxis>(
            request.cursor->axis);
    }

    const auto directory = m_directory.lock();
    const auto session = directory
        ? directory->GetSessionPort().lock() : nullptr;
    return session && session->SendSessionUpdate(update);
}

bool HostCommandRouter::Impl::SetView(
    const HostViewSetRequest& request) const
{
    // 1. 先在局部候选中解析目标并完成所有字段校验。
    // 2. App port 内部以完整快照保证字段级强回滚。
    // 3. Host 再按 App → camera → axes → dirty 提交，失败按逆序补偿。
    // 4. 补偿失败时停用目标 view，禁止继续暴露半状态。
    auto candidate = BuildViewCandidate(request);
    if (!candidate) return false;

    AppViewUpdate update;
    update.mode = candidate->mode;
    update.material = candidate->material;
    update.opacity = candidate->opacity;
    update.volumeTransferFunction =
        candidate->volumeTransferFunction;
    update.isoThreshold = candidate->iso;
    update.background = candidate->background;
    update.windowLevel = candidate->windowLevel;
    update.volumeQuality = candidate->volumeQuality;
    if (candidate->visibility) {
        const auto& visibility = *candidate->visibility;
        AppVisibilityUpdate appVisibility;
        appVisibility.isPlanes3DVisible =
            visibility.isPlanes3DVisible;
        appVisibility.isCrosshairVisible =
            visibility.isCrosshairVisible;
        appVisibility.isRulerVisible =
            visibility.isRulerVisible;
        update.visibility = std::move(appVisibility);
    }

    const AppViewState oldState = candidate->viewPort->GetViewState();
    const bool oldAxes = candidate->context
        && candidate->context->GetOrientationAxesVisible();
    if (!candidate->viewPort->SendViewUpdate(update)) return false;
    const AppViewState nextState = candidate->viewPort->GetViewState();
    if (nextState.revision <= oldState.revision) {
        if (candidate->stopView) (void)candidate->stopView();
        return false;
    }

    const auto stopOnRestoreFail = [&]() {
        if (candidate->stopView) (void)candidate->stopView();
    };
    const auto restore = [&](const bool hasAxesChanged) {
        bool isRestored = true;
        if (hasAxesChanged && candidate->context) {
            isRestored = candidate->context
                ->SetOrientationAxesVisible(oldAxes) && isRestored;
        }
        if (candidate->mode && candidate->context) {
            isRestored = candidate->context
                ->SetCameraStyle(oldState.mode) && isRestored;
        }
        isRestored = candidate->viewPort->SetViewState(
            oldState, nextState.revision) && isRestored;
        if (!isRestored) stopOnRestoreFail();
        return isRestored;
    };

    if (candidate->mode
        && !candidate->context->SetCameraStyle(*candidate->mode)) {
        (void)restore(false);
        return false;
    }
    if (candidate->isAxesVisible) {
        if (!candidate->context->SetOrientationAxesVisible(
                *candidate->isAxesVisible)) {
            (void)restore(true);
            return false;
        }
        // 方向轴属于 context，不发布 SharedState flags；显式标脏让 Qt Timer 产生下一帧。
        if (!candidate->updatePort->SetRenderNeeded()) {
            (void)restore(true);
            return false;
        }
    }
    return true;
}

bool HostCommandRouter::Impl::ResetView(
    const HostViewResetRequest& request) const
{
    const auto route = GetViewRoute(request.targetView);
    const auto view = route ? route->view.lock() : nullptr;
    const auto context = route ? route->context.lock() : nullptr;
    const auto update = route ? route->update.lock() : nullptr;
    if (!view || !context || !update) {
        return false;
    }

    const AppViewState oldState = view->GetViewState();
    const auto oldCamera = context->GetCameraState();
    if (!oldCamera) return false;

    AppViewUpdate viewUpdate;
    viewUpdate.windowLevelMode = WindowLevelMode::Auto;
    if (!view->SendViewUpdate(viewUpdate)) return false;
    const AppViewState nextState = view->GetViewState();
    if (nextState.revision <= oldState.revision) {
        if (route->stopView) (void)route->stopView();
        return false;
    }

    const auto restore = [&](const bool hasCameraChanged) {
        bool isRestored = true;
        if (hasCameraChanged) {
            isRestored = context->SetCameraState(*oldCamera)
                && isRestored;
        }
        isRestored = view->SetViewState(
            oldState, nextState.revision) && isRestored;
        if (!isRestored && route->stopView) {
            (void)route->stopView();
        }
        return isRestored;
    };

    if (!context->ResetCamera()) {
        (void)restore(true);
        return false;
    }
    // 相机不发布 App 状态事件；显式补帧与窗宽窗位提交组成同一 Host 事务。
    if (update->SetRenderNeeded()) return true;
    (void)restore(true);
    return false;
}

bool HostCommandRouter::Impl::SetTool(
    const HostToolSetRequest& request) const
{
    const auto route = GetViewRoute(request.targetView);
    const auto context = route ? route->context.lock() : nullptr;
    if (!context) return false;
    const auto mode = GetAppToolMode(request.toolMode);
    if (!mode) return false;
    return context->SetToolMode(*mode);
}

bool HostCommandRouter::Impl::SwitchTool(
    const HostToolSwitchRequest& request) const
{
    const auto route = GetViewRoute(request.targetView);
    const auto context = route ? route->context.lock() : nullptr;
    if (!context) return false;
    const ToolMode mode =
        context->GetToolMode() == ToolMode::Navigation
        ? ToolMode::ModelTransform
        : ToolMode::Navigation;
    return context->SetToolMode(mode);
}

HostCommandRouter::HostCommandRouter(
    std::weak_ptr<IHostViewDirectory> directory)
    : m_impl(std::make_unique<Impl>(std::move(directory)))
{
}

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
