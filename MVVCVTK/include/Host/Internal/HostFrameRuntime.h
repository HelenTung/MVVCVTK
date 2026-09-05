#pragma once
#include "Host/HostFrameCoordinator.h"
#include "Host/Internal/HostRenderViewRuntime.h"
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// 帧批次的物化与提交状态；阶段推进仍由 HostFrameCoordinator 负责。
class HostFrameRuntime final {
public:
    HostFrameRuntime(std::vector<HostRenderViewRuntime>& views,
        const std::shared_ptr<FeatureViewLease>& lease,
        std::function<std::vector<std::string>(const HostRenderViewRuntime&)> onFeatureIds);
    void BuildSceneStates();
    bool GetIsBusy() const noexcept { return m_executionDepth != 0; }
    void Clear() noexcept;
    void SetViewUnavailable(std::size_t index) noexcept;
    std::optional<HostSceneViewState> GetSceneState(std::size_t index) const;
    std::vector<HostSceneViewState> GetSceneStates() const;
    bool SetFrameGeneration(std::uint64_t sessionGeneration);
    bool SetFrameIntents(
        const std::vector<HostFrameIntent>& intents);
    bool CollectFrameUpdates();
    bool ApplyFrameUpdates();
    HostFrameStageStatus BuildFrameStage(std::uint64_t nextEpoch);
    void SetFrameCommit(std::uint64_t epoch) noexcept;
    bool SendFrameRender(std::uint64_t epoch);
    bool GetFrameRenderPending() const noexcept;
    void SendFrameCompletions() noexcept;
    void ClearFrameStage() noexcept;
private:
    // 同线程 callback 可重入 Stop；阶段执行期间资源拓扑必须保留。
    class PhaseGuard final {
    public:
        explicit PhaseGuard(std::size_t& depth) : m_depth(depth) { ++m_depth; }
        ~PhaseGuard() { --m_depth; }
        PhaseGuard(const PhaseGuard&) = delete;
        PhaseGuard& operator=(const PhaseGuard&) = delete;
    private:
        std::size_t& m_depth;
    };
    std::size_t m_executionDepth = 0;
    struct FrameStage final {
        std::uint64_t epoch = 0;
        std::vector<HostSceneViewState> sceneStates;
        std::vector<std::size_t> renderOrder;
        std::vector<bool> renderNeeded;
    };
    std::optional<std::size_t> GetViewIndexById(std::string_view viewId) const;
    static int GetRenderPriority(const HostSceneViewState& state) noexcept;
    // 引用的是 Registry::Impl 的成员槽；Clear 在 topology 释放之前执行。
    std::vector<HostRenderViewRuntime>& m_views;
    const std::shared_ptr<FeatureViewLease>& m_lease;
    std::function<std::vector<std::string>(const HostRenderViewRuntime&)> m_onFeatureIds;
    std::vector<HostFrameIntent> m_frameIntents;
    std::optional<FrameStage> m_frameStage;
    std::vector<HostSceneViewState> m_sceneStates;
    std::vector<std::size_t> m_renderOrder;
    std::uint64_t m_sessionGeneration = 0;
    std::uint64_t m_committedEpoch = 0;
};
