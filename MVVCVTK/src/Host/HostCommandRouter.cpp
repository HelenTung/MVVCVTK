#include "Host/HostCommandRouter.h"

#include "AppService.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
template <typename>
constexpr bool isUnknownCommand = false;
}

class HostCommandRouter::Impl final {
public:
    Impl(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews,
        std::shared_ptr<HostFeatureBindings> featureBindings);
    ~Impl() = default;

    bool DispatchCommand(HostCommand command) const;

private:
    bool LoadVolume(
        InitialVolumeLoadConfig initialVolume,
        std::function<void(bool isSuccess)> loadComplete) const;
    bool ReloadVolume(
        HostVolumeBufferRequest volumeBuffer,
        std::function<void(bool isSuccess)> reloadComplete) const;
    std::function<void(bool)> BuildLoadComplete(
        std::function<void(bool isSuccess)> onComplete) const;
    std::function<void(bool)> BuildReloadComplete(
        std::function<void(bool isSuccess)> onComplete) const;
    bool ExportData(
        HostDataExportConfig dataExportConfig,
        std::function<void(bool isSuccess)> onComplete) const;
    bool SetViewConfig(const HostViewConfig& viewConfig) const;
    // 非拥有引用：core 和 view set 由 session 持有，声明顺序保证它们晚于 router 析构。
    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
    // 弱引用避免 router 延长 feature 生命周期；命令入口必须允许绑定层已卸载时 lock 失败。
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
};

HostCommandRouter::Impl::Impl(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::shared_ptr<HostFeatureBindings> featureBindings)
    : m_core(&core)
    , m_renderViews(&renderViews)
    , m_featureBindings(std::move(featureBindings))
{
}

bool HostCommandRouter::Impl::DispatchCommand(HostCommand command) const
{
    return std::visit(
        [this](auto&& typedCommand) -> bool {
            using Command = std::decay_t<decltype(typedCommand)>;
            if constexpr (std::is_same_v<Command, std::monostate>) {
                std::cerr << "[Host] Command dispatch skipped: command was not specified." << std::endl;
                return false;
            }
            else if constexpr (std::is_same_v<Command, HostLoadCommand>) {
                return LoadVolume(
                    std::move(typedCommand.request),
                    std::move(typedCommand.onComplete));
            }
            else if constexpr (std::is_same_v<Command, HostReloadCommand>) {
                return ReloadVolume(
                    std::move(typedCommand.request),
                    std::move(typedCommand.onComplete));
            }
            else if constexpr (std::is_same_v<Command, HostExportCommand>) {
                return ExportData(
                    std::move(typedCommand.request),
                    std::move(typedCommand.onComplete));
            }
            else if constexpr (std::is_same_v<Command, HostViewCommand>) {
                return SetViewConfig(typedCommand.request);
            }
            else if constexpr (std::is_same_v<Command, HostFeatureCommand>) {
                const auto bindings = m_featureBindings.lock();
                if (!bindings) {
                    std::cerr << "[Host] Feature command dispatch skipped: feature bindings are not ready." << std::endl;
                    return false;
                }
                return bindings->SendCommand(std::move(typedCommand));
            }
            else {
                static_assert(
                    isUnknownCommand<Command>,
                    "Unknown host command");
            }
        },
        std::move(command));
}

bool HostCommandRouter::Impl::LoadVolume(
    InitialVolumeLoadConfig initialVolume,
    std::function<void(bool isSuccess)> loadComplete) const
{
    if (!initialVolume.isInitialLoadEnabled) {
        return true;
    }
    if (!m_core || !m_renderViews) {
        std::cerr << "[Host] Initial volume load skipped: host services are not ready." << std::endl;
        return false;
    }
    if (initialVolume.filePath.empty()) {
        std::cerr << "[Host] Initial volume load skipped: file path was not specified." << std::endl;
        return false;
    }
    if (!initialVolume.geometry) {
        std::cerr << "[Host] Initial volume load skipped: volume geometry was not specified." << std::endl;
        return false;
    }

    const auto* primaryView = m_renderViews->GetPrimaryView();
    if (!primaryView || !primaryView->service) {
        std::cerr << "[Host] Initial volume load skipped: primary render view is missing." << std::endl;
        return false;
    }

    primaryView->service->LoadFileAsync(
        initialVolume.filePath,
        initialVolume.geometry->spacing,
        initialVolume.geometry->origin,
        BuildLoadComplete(std::move(loadComplete)));
    return true;
}

bool HostCommandRouter::Impl::ReloadVolume(
    HostVolumeBufferRequest volumeBuffer,
    std::function<void(bool isSuccess)> reloadComplete) const
{
    if (!m_core || !m_renderViews) {
        std::cerr << "[Host] Volume buffer reload skipped: host services are not ready.\n";
        return false;
    }
    if (!volumeBuffer.geometry) {
        std::cerr << "[Host] Volume buffer reload skipped: volume geometry was not specified.\n";
        return false;
    }

    std::size_t voxelCount = 1;
    for (const int dimension : volumeBuffer.dimensions) {
        if (dimension <= 0
            || voxelCount > std::numeric_limits<std::size_t>::max()
                / static_cast<std::size_t>(dimension)) {
            std::cerr << "[Host] Volume buffer reload skipped: dimensions were invalid.\n";
            return false;
        }
        voxelCount *= static_cast<std::size_t>(dimension);
    }
    if (volumeBuffer.voxels.size() != voxelCount) {
        std::cerr << "[Host] Volume buffer reload skipped: voxel count did not match dimensions.\n";
        return false;
    }

    const auto* primaryView = m_renderViews->GetPrimaryView();
    if (!primaryView || !primaryView->service) {
        std::cerr << "[Host] Volume buffer reload skipped: primary render view is missing.\n";
        return false;
    }

    // service 在返回前复制 voxels，异步任务不会借用 volumeBuffer 的存储。
    return primaryView->service->ReloadFromBufferAsync(
        volumeBuffer.voxels.data(),
        volumeBuffer.dimensions,
        volumeBuffer.geometry->spacing,
        volumeBuffer.geometry->origin,
        BuildReloadComplete(std::move(reloadComplete)));
}

std::function<void(bool)> HostCommandRouter::Impl::BuildLoadComplete(
    std::function<void(bool isSuccess)> onComplete) const
{
    const auto featureBindings = m_featureBindings;
    return [
        featureBindings,
        onComplete = std::move(onComplete)
    ](bool isSuccess) mutable {
        if (const auto lockedBindings = featureBindings.lock()) {
            if (isSuccess) {
                lockedBindings->SendCropInput();
            }
            else {
                lockedBindings->ClearCropInput();
            }
        }
        if (onComplete) {
            onComplete(isSuccess);
        }
    };
}

std::function<void(bool)> HostCommandRouter::Impl::BuildReloadComplete(
    std::function<void(bool isSuccess)> onComplete) const
{
    const auto featureBindings = m_featureBindings;
    return [
        featureBindings,
        onComplete = std::move(onComplete)
    ](bool isSuccess) mutable {
        // Reload 失败不会替换 current image，因此保留现有 Crop input；成功后才刷新快照。
        if (isSuccess) {
            if (const auto lockedBindings = featureBindings.lock()) {
                lockedBindings->SendCropInput();
            }
        }
        if (onComplete) {
            onComplete(isSuccess);
        }
    };
}

bool HostCommandRouter::Impl::ExportData(
    HostDataExportConfig dataExportConfig,
    std::function<void(bool isSuccess)> onComplete) const
{
    if (!m_renderViews) {
        std::cerr << "[Host] Data export skipped: host render views are not ready." << std::endl;
        return false;
    }

    if (dataExportConfig.hasVolumeExport == dataExportConfig.hasSliceExport) {
        std::cerr << "[Host] Data export skipped: exactly one export request flag must be true." << std::endl;
        return false;
    }

    if (dataExportConfig.hasVolumeExport) {
        if (dataExportConfig.transformedDataOutputPath.empty()) {
            std::cerr << "[Host] Transformed data export skipped: output path was not configured." << std::endl;
            return false;
        }

        const auto* exportView = m_renderViews->GetPrimaryView();
        if (!exportView || !exportView->service) {
            std::cerr << "[Host] Transformed data export skipped: primary render view is missing." << std::endl;
            return false;
        }

        exportView->service->ExportDataAsync(
            dataExportConfig.transformedDataOutputPath,
            std::move(onComplete));
        return true;
    }

    if (dataExportConfig.sliceOutputDir.empty()) {
        std::cerr << "[Host] Slice image export skipped: output directory was not configured." << std::endl;
        return false;
    }

    const auto* exportView = m_renderViews->GetViewBySelector(
        dataExportConfig.sliceSourceViewId,
        dataExportConfig.isSliceRoleUsed,
        dataExportConfig.sliceSourceRole);

    if (!exportView || !exportView->service) {
        std::cerr << "[Host] Slice image export skipped: source render view is missing." << std::endl;
        return false;
    }
    if (!m_renderViews->GetRoleIsSliceView(exportView->config.role)) {
        std::cerr << "[Host] Slice image export skipped: source render view is not a slice view." << std::endl;
        return false;
    }

    exportView->service->ExportSlicesAsync(
        dataExportConfig.sliceOutputDir,
        dataExportConfig.sliceAngleDeg,
        std::move(onComplete));
    return true;
}

bool HostCommandRouter::Impl::SetViewConfig(const HostViewConfig& viewConfig) const
{
    // optional 字段组成一次命令快照；先完成目标和输入校验，保证拒绝路径没有部分写入。
    const bool hasViewEdit =
        viewConfig.mode.has_value()
        || viewConfig.material.has_value()
        || viewConfig.opacity.has_value()
        || viewConfig.tfNodes.has_value()
        || viewConfig.iso.has_value()
        || viewConfig.background.has_value()
        || viewConfig.spacing.has_value()
        || viewConfig.windowLevel.has_value();

    if (!m_renderViews) {
        std::cerr << "[Host] View config skipped: host render views are not ready." << std::endl;
        return false;
    }
    if (!hasViewEdit) {
        std::cerr << "[Host] View config skipped: no config value was provided." << std::endl;
        return false;
    }

    const auto* targetView = m_renderViews->GetViewBySelector(
        viewConfig.viewId,
        viewConfig.isViewRoleUsed,
        viewConfig.viewRole);

    if (!targetView || !targetView->service) {
        std::cerr << "[Host] View config skipped: target render view is missing." << std::endl;
        return false;
    }

    const auto& service = targetView->service;
    if (viewConfig.mode) {
        // mode 同时属于 service 管线意图和 context 输入/相机策略；必须先确认两个 owner 都可写。
        if (!targetView->context) {
            std::cerr << "[Host] View mode config skipped: render context is missing." << std::endl;
            return false;
        }
        service->SetVizMode(*viewConfig.mode);
        targetView->context->SetCameraStyle(*viewConfig.mode);
    }
    // 其余 optional 字段只有 service 一个 owner，可按请求中实际携带的值独立分发。
    if (viewConfig.material) {
        service->SetMaterial(*viewConfig.material);
    }
    if (viewConfig.opacity) {
        service->SetOpacity(*viewConfig.opacity);
    }
    if (viewConfig.tfNodes) {
        service->SetTransferFunction(*viewConfig.tfNodes);
    }
    if (viewConfig.iso) {
        service->SetIsoThreshold(*viewConfig.iso);
    }
    if (viewConfig.background) {
        service->SetBackground(*viewConfig.background);
    }
    if (viewConfig.spacing) {
        service->SetSpacing(
            (*viewConfig.spacing)[0],
            (*viewConfig.spacing)[1],
            (*viewConfig.spacing)[2]);
    }
    if (viewConfig.windowLevel) {
        service->SetWindowLevel(
            viewConfig.windowLevel->windowWidth,
            viewConfig.windowLevel->windowCenter);
    }
    return true; // 配置已分发；service 的实际管线同步由后续主线程 tick 消费。
}

HostCommandRouter::HostCommandRouter(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::shared_ptr<HostFeatureBindings> featureBindings)
    : m_impl(std::make_unique<HostCommandRouter::Impl>(
        core,
        renderViews,
        std::move(featureBindings)))
{
}

HostCommandRouter::~HostCommandRouter() = default;

bool HostCommandRouter::DispatchCommand(HostCommand command) const
{
    return m_impl && m_impl->DispatchCommand(std::move(command));
}
