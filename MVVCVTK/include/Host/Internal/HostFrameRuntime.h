#pragma once
#include "Host/HostFrameCoordinator.h"
#include "Host/Internal/HostRenderViewRuntime.h"
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <vector>

struct GridGeometry3D;

// 帧批次的物化与提交状态；阶段推进仍由 HostFrameCoordinator 负责。
class HostFrameRuntime final {
public:
    HostFrameRuntime(std::vector<HostRenderViewRuntime>& views,
        const std::shared_ptr<FeatureViewLease>& lease,
        std::function<std::vector<std::string>(const HostRenderViewRuntime&)> onFeatureIds);
    void BuildSceneStates();
    void SetDataRead(std::shared_ptr<const TrustedDataReadPort> data);
    void SetDriveMode(HostDriveMode mode) noexcept;
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
    HostRenderResult SendFrameRender(const HostRenderRequest& request,
        const std::function<bool()>& getIsRunning);
    std::vector<std::string> GetRenderViewIds() const;
    bool GetFrameRenderPending() const noexcept;
    void SendFrameCompletions() noexcept;
    void ClearFrameStage() noexcept;
private:
    class RenderObserver;
    void SetRulerState(std::size_t index) noexcept;
    bool m_isHostDriven = false;
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
        DataGraphSnapshot graph;
        std::map<std::pair<std::string, std::string>, HostFrameIntent> bindings;
        std::vector<HostSceneViewState> sceneStates;
        std::vector<std::size_t> renderOrder;
        std::vector<bool> renderNeeded;
        std::vector<std::size_t> dirtyIndices;
    };
    std::optional<std::size_t> GetViewIndexById(std::string_view viewId) const;
    static bool GetSceneInputsValid(const DataGraphSnapshot& graph,
        const FeatureSceneDelta& delta);
    static std::optional<DataRevisionRef> GetDataSource(
        const DataGraphSnapshot& graph, const DataRevisionRef& data);
    static bool GetGridCompatible(const GridGeometry3D& data,
        const GridGeometry3D& source, bool isLabel);
    static bool GetDisplayInputsValid(const DataGraphSnapshot& graph,
        const FeatureSceneDelta& delta);
    static int GetRenderPriority(const HostSceneViewState& state) noexcept;
    // 引用的是 Registry::Impl 的成员槽；Clear 在 topology 释放之前执行。
    std::vector<HostRenderViewRuntime>& m_views;
    const std::shared_ptr<FeatureViewLease>& m_lease;
    std::function<std::vector<std::string>(const HostRenderViewRuntime&)> m_onFeatureIds;
    std::vector<HostFrameIntent> m_frameIntents;
    std::optional<FrameStage> m_frameStage;
    std::shared_ptr<const TrustedDataReadPort> m_dataRead;
    DataGraphSnapshot m_sceneGraph;
    std::map<std::pair<std::string, std::string>, HostFrameIntent> m_sceneBindings;
    std::vector<HostSceneViewState> m_sceneStates;
    std::vector<std::size_t> m_renderOrder;
    std::uint64_t m_sessionGeneration = 0;
    std::uint64_t m_committedEpoch = 0;
};
