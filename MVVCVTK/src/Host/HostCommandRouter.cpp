#include "Host/HostCommandRouter.h"

#include "AppService.h"
#include "AppTypes.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"
#include "VolumeTypes.h"

#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

class HostCommandRouter::Impl final {
public:
    Impl(const HostCoreServices& core, const HostRenderViewSet& renderViews,
        std::shared_ptr<HostFeatureBindings> featureBindings)
        : m_core(&core), m_renderViews(&renderViews),
          m_featureBindings(std::move(featureBindings)) {}

    bool DispatchCommand(HostCommand command) const;

private:
    bool SendData(HostDataCommand command) const;
    bool SendView(const HostViewCommand& command) const;
    bool SendTool(const HostToolCommand& command) const;
    bool SendCrop(HostCropCommand command) const;
    bool SendGap(HostGapCommand command) const;
    bool LoadFile(HostLoadRequest request, HostCompleteCallback callback) const;
    bool ReloadBuffer(HostReloadRequest request, HostCompleteCallback callback) const;
    bool ExportVolume(HostVolumeExportRequest request, HostCompleteCallback callback) const;
    bool ExportSlices(HostSliceExportRequest request, HostCompleteCallback callback) const;
    std::optional<VolumeLayout> BuildLoadLayout(
        const HostLoadRequest& request) const;
    std::optional<std::array<int, 3>> GetRawDims(
        const std::string& path) const;
    std::optional<VizMode> GetAppViewMode(HostViewMode mode) const;
    std::optional<ToolMode> GetAppToolMode(HostToolMode mode) const;
    std::optional<MaterialParams> BuildAppMaterial(const HostMaterialParams& value) const;
    std::optional<BackgroundColor> BuildAppBackground(const HostBackgroundColor& value) const;
    std::optional<WindowLevelParams> BuildAppWindowLevel(const HostWindowLevelParams& value) const;
    std::optional<std::vector<TFNode>> BuildAppNodes(
        const std::vector<HostTransferNode>& values) const;
    HostCompleteCallback BuildLoadComplete(HostCompleteCallback callback) const;
    HostCompleteCallback BuildReloadComplete(HostCompleteCallback callback) const;

    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
};

bool HostCommandRouter::Impl::DispatchCommand(HostCommand command) const
{
    if (auto* value = std::get_if<HostDataCommand>(&command)) {
        return SendData(std::move(*value));
    }
    if (const auto* value = std::get_if<HostViewCommand>(&command)) {
        return SendView(*value);
    }
    if (const auto* value = std::get_if<HostToolCommand>(&command)) {
        return SendTool(*value);
    }
    if (auto* value = std::get_if<HostCropCommand>(&command)) {
        return SendCrop(std::move(*value));
    }
    if (auto* value = std::get_if<HostGapCommand>(&command)) {
        return SendGap(std::move(*value));
    }
    return false;
}

bool HostCommandRouter::Impl::SendData(HostDataCommand command) const
{
    switch (command.request.action) {
    case HostDataAction::LoadFile:
        if (auto* request = std::get_if<HostLoadRequest>(&command.request.payload)) {
            return LoadFile(std::move(*request), std::move(command.onComplete));
        }
        return false;
    case HostDataAction::ReloadBuffer:
        if (auto* request = std::get_if<HostReloadRequest>(&command.request.payload)) {
            return ReloadBuffer(std::move(*request), std::move(command.onComplete));
        }
        return false;
    case HostDataAction::ExportVolume:
        if (auto* request = std::get_if<HostVolumeExportRequest>(&command.request.payload)) {
            return ExportVolume(std::move(*request), std::move(command.onComplete));
        }
        return false;
    case HostDataAction::ExportSlices:
        if (auto* request = std::get_if<HostSliceExportRequest>(&command.request.payload)) {
            return ExportSlices(std::move(*request), std::move(command.onComplete));
        }
        return false;
    case HostDataAction::None:
        return false;
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
        BuildLoadComplete(std::move(callback)));
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
        std::move(*buffer), BuildReloadComplete(std::move(callback)));
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
    const std::filesystem::path filePath(path);
    std::string extension = filePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".raw") return std::nullopt;

    const std::string stem = filePath.stem().string();
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

bool HostCommandRouter::Impl::ExportVolume(
    HostVolumeExportRequest request, HostCompleteCallback callback) const
{
    const auto* view = m_renderViews ? m_renderViews->GetPrimaryView() : nullptr;
    if (!view || !view->service || request.outputPath.empty()) {
        return false;
    }
    view->service->ExportDataAsync(request.outputPath, std::move(callback));
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

std::optional<VizMode> HostCommandRouter::Impl::GetAppViewMode(HostViewMode mode) const
{
    switch (mode) {
    case HostViewMode::Volume: return VizMode::Volume;
    case HostViewMode::IsoSurface: return VizMode::IsoSurface;
    case HostViewMode::SliceTopDown: return VizMode::SliceTop_down;
    case HostViewMode::SliceFrontBack: return VizMode::SliceFront_back;
    case HostViewMode::SliceLeftRight: return VizMode::SliceLeft_right;
    case HostViewMode::CompositeVolume: return VizMode::CompositeVolume;
    case HostViewMode::CompositeIsoSurface: return VizMode::CompositeIsoSurface;
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
    if (!std::isfinite(value.ambient) || !std::isfinite(value.diffuse)
        || !std::isfinite(value.specular) || !std::isfinite(value.specularPower)
        || !std::isfinite(value.opacity) || value.specularPower < 0.0
        || value.opacity < 0.0 || value.opacity > 1.0) {
        return std::nullopt;
    }
    return MaterialParams{ value.ambient, value.diffuse, value.specular,
        value.specularPower, value.opacity, value.isShadeOn };
}

std::optional<BackgroundColor> HostCommandRouter::Impl::BuildAppBackground(
    const HostBackgroundColor& value) const
{
    if (!std::isfinite(value.r) || !std::isfinite(value.g) || !std::isfinite(value.b)) {
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
    std::vector<TFNode> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (!std::isfinite(value.position) || !std::isfinite(value.opacity)
            || !std::isfinite(value.r) || !std::isfinite(value.g)
            || !std::isfinite(value.b)) {
            return std::nullopt;
        }
        result.push_back({ value.position, value.opacity, value.r, value.g, value.b });
    }
    return result;
}

bool HostCommandRouter::Impl::SendView(const HostViewCommand& command) const
{
    if (command.request.action != HostViewAction::Set) {
        return false;
    }
    const auto* request = std::get_if<HostViewSetRequest>(&command.request.payload);
    const auto* view = request && m_renderViews
        ? m_renderViews->GetViewBySelector(request->targetView) : nullptr;
    if (!request || !view || !view->service) {
        return false;
    }
    const auto mode = request->mode ? GetAppViewMode(*request->mode) : std::optional<VizMode>{};
    const auto material = request->material ? BuildAppMaterial(*request->material)
        : std::optional<MaterialParams>{};
    const auto nodes = request->transferNodes ? BuildAppNodes(*request->transferNodes)
        : std::optional<std::vector<TFNode>>{};
    const auto background = request->background ? BuildAppBackground(*request->background)
        : std::optional<BackgroundColor>{};
    const auto windowLevel = request->windowLevel ? BuildAppWindowLevel(*request->windowLevel)
        : std::optional<WindowLevelParams>{};
    if ((request->mode && !mode) || (request->material && !material)
        || (request->transferNodes && !nodes) || (request->background && !background)
        || (request->windowLevel && !windowLevel)
        || (request->opacity && (!std::isfinite(*request->opacity)
            || *request->opacity < 0.0 || *request->opacity > 1.0))
        || (request->iso && !std::isfinite(*request->iso))) {
        return false;
    }
    if (mode) {
        if (!view->context) return false;
        view->service->SetVizMode(*mode);
        view->context->SetCameraStyle(*mode);
    }
    if (material) view->service->SetMaterial(*material);
    if (request->opacity) view->service->SetOpacity(*request->opacity);
    if (nodes) view->service->SetTransferFunction(*nodes);
    if (request->iso) view->service->SetIsoThreshold(*request->iso);
    if (background) view->service->SetBackground(*background);
    if (request->spacing) {
        const auto& spacing = *request->spacing;
        if (!std::isfinite(spacing[0]) || spacing[0] <= 0.0
            || !std::isfinite(spacing[1]) || spacing[1] <= 0.0
            || !std::isfinite(spacing[2]) || spacing[2] <= 0.0) return false;
        view->service->SetSpacing(spacing[0], spacing[1], spacing[2]);
    }
    if (windowLevel) view->service->SetWindowLevel(
        windowLevel->windowWidth, windowLevel->windowCenter);
    return true;
}

bool HostCommandRouter::Impl::SendTool(const HostToolCommand& command) const
{
    const HostViewTarget* target = nullptr;
    std::optional<HostToolMode> requestedMode;
    switch (command.request.action) {
    case HostToolAction::Set:
        if (const auto* request = std::get_if<HostToolSetRequest>(&command.request.payload)) {
            target = &request->targetView;
            requestedMode = request->toolMode;
        }
        break;
    case HostToolAction::Switch:
        if (const auto* request = std::get_if<HostToolSwitchRequest>(&command.request.payload)) {
            target = &request->targetView;
        }
        break;
    case HostToolAction::None:
        break;
    }
    const auto* view = target && m_renderViews
        ? m_renderViews->GetViewBySelector(*target) : nullptr;
    if (!view || !view->context) return false;
    ToolMode mode = ToolMode::Navigation;
    if (requestedMode) {
        const auto appMode = GetAppToolMode(*requestedMode);
        if (!appMode) return false;
        mode = *appMode;
    } else {
        mode = view->context->GetToolMode() == ToolMode::Navigation
            ? ToolMode::ModelTransform : ToolMode::Navigation;
    }
    view->context->SetToolMode(mode);
    return true;
}

bool HostCommandRouter::Impl::SendCrop(HostCropCommand command) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) return false;
    switch (command.request.action) {
    case HostCropAction::Start:
        if (!command.onComplete) {
            if (const auto* value = std::get_if<HostCropTargetRequest>(
                &command.request.payload)) {
                return bindings->StartCrop(*value);
            }
        }
        return false;
    case HostCropAction::Box:
        if (!command.onComplete) {
            if (const auto* value = std::get_if<HostCropTargetRequest>(
                &command.request.payload)) {
                return bindings->SwitchCropBox(*value);
            }
        }
        return false;
    case HostCropAction::Plane:
        if (!command.onComplete) {
            if (const auto* value = std::get_if<HostCropTargetRequest>(
                &command.request.payload)) {
                return bindings->SwitchCropPlane(*value);
            }
        }
        return false;
    case HostCropAction::Preview:
        if (!command.onComplete) {
            if (const auto* value = std::get_if<HostCropPreviewRequest>(
                &command.request.payload)) {
                return bindings->SwitchCropView(*value);
            }
        }
        return false;
    case HostCropAction::Submit:
        if (const auto* value = std::get_if<HostCropTargetRequest>(
            &command.request.payload)) {
            return bindings->SendCrop(*value, std::move(command.onComplete));
        }
        return false;
    case HostCropAction::Exit:
        return !command.onComplete && std::holds_alternative<std::monostate>(command.request.payload)
            && bindings->ExitCrop();
    case HostCropAction::None:
        return false;
    }
    return false;
}

bool HostCommandRouter::Impl::SendGap(HostGapCommand command) const
{
    const auto bindings = m_featureBindings.lock();
    if (!bindings) return false;
    switch (command.request.action) {
    case HostGapAction::Start:
        if (const auto* value = std::get_if<HostGapStartRequest>(
            &command.request.payload)) {
            return bindings->StartGap(*value, std::move(command.onComplete));
        }
        return false;
    case HostGapAction::Overlay:
        return !command.onComplete && std::holds_alternative<std::monostate>(command.request.payload)
            && bindings->SwitchGapLayer();
    case HostGapAction::Exit:
        return !command.onComplete && std::holds_alternative<std::monostate>(command.request.payload)
            && bindings->ExitGap();
    case HostGapAction::None:
        return false;
    }
    return false;
}

HostCompleteCallback HostCommandRouter::Impl::BuildLoadComplete(
    HostCompleteCallback callback) const
{
    const auto bindings = m_featureBindings;
    return [bindings, callback = std::move(callback)](bool isSuccess) mutable {
        if (const auto value = bindings.lock()) {
            if (isSuccess) value->SendCropInput(); else value->ClearCropInput();
        }
        if (callback) callback(isSuccess);
    };
}

HostCompleteCallback HostCommandRouter::Impl::BuildReloadComplete(
    HostCompleteCallback callback) const
{
    const auto bindings = m_featureBindings;
    return [bindings, callback = std::move(callback)](bool isSuccess) mutable {
        if (isSuccess) if (const auto value = bindings.lock()) value->SendCropInput();
        if (callback) callback(isSuccess);
    };
}

HostCommandRouter::HostCommandRouter(const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::shared_ptr<HostFeatureBindings> featureBindings)
    : m_impl(std::make_unique<Impl>(core, renderViews, std::move(featureBindings))) {}

HostCommandRouter::~HostCommandRouter() = default;

bool HostCommandRouter::DispatchCommand(HostCommand command) const
{
    return m_impl && m_impl->DispatchCommand(std::move(command));
}
