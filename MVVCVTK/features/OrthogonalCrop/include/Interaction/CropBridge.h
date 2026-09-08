#pragma once

#include "OrthogonalCropTypes.h"
#include "Interaction/CropHistoryQueue.h"
#include "Algorithms/CropAlgorithm.h"
#include "App/Services/FeatureViewService.h"
#include "Host/Types/HostViewTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class vtkRenderWindowInteractor;
class vtkRenderer;

struct CropViewRequest final {
    vtkRenderWindowInteractor* interactor = nullptr;
    vtkRenderer* renderer = nullptr;
    std::weak_ptr<const FeatureViewLease> lease;
    std::shared_ptr<FeatureViewService> referenceService;
    std::vector<std::shared_ptr<FeatureViewService>> targetServices;
};

using CropCandidateCallback =
    std::function<void(CropMaterializationCandidate)>;

class CropBridge final {
private:
    class Impl;

public:
    class SourceCommit final {
    public:
        ~SourceCommit();
        SourceCommit(SourceCommit&&) noexcept;
        SourceCommit& operator=(SourceCommit&&) noexcept;
        SourceCommit(const SourceCommit&) = delete;
        SourceCommit& operator=(const SourceCommit&) = delete;
    private:
        friend class CropBridge;
        friend class CropBridge::Impl;
        class Impl;
        explicit SourceCommit(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> m_impl;
    };
    CropBridge();
    void SetWorkAvailable(std::function<void()> onWorkAvailable);
    ~CropBridge();

    CropBridge(const CropBridge&) = delete;
    CropBridge& operator=(const CropBridge&) = delete;

    bool StartView(const CropViewRequest& request);
    // Host 已冻结输入候选时，把 input 与 view binding 作为一次事务提交。
    bool StartView(
        const CropViewRequest& request,
        CropInputSnapshot input);
    bool ClearBindings();
    bool SetCropInput(CropInputSnapshot input);
    CropEditAdmission SendRequest(CropEditRequest request);
    CropHistorySnapshot GetHistory(CropNodeId after = 0,std::size_t limit = 1000) const;
    CropPruneImpact GetPruneImpact(const CropPruneRequest& request) const;
    std::optional<CropNodeSnapshot> GetNode(CropNodeId node) const;
    std::optional<CropEditOutcome> GetOutcome(CropRequestId id) const;
    CropInputSnapshot GetSource() const;
    std::optional<CropViewPreviewState> GetViewState(const FeatureViewService* service) const;
    void ForgetOutcome(CropRequestId requestId);
    bool GetResultsValid(const std::vector<CropResultRecord>& results) const;
    void SetResults(std::vector<CropResultRecord>&& results) noexcept;
    CropDocumentArchive GetArchive() const;
    bool CancelPending();
    bool ClearDocument();
    bool GetSourceTransitionNeeded() const;
    // isQueued=true 时准备队首命令；否则为显式 Root 返回/已有节点选择。
    std::optional<SourceCommit> BuildSourceCommit(CropNodeId nodeId,bool isQueued);
    bool GetSourceCommitReady(const SourceCommit& prepared) const noexcept;
    void SetSourceCommit(SourceCommit&& prepared) noexcept;
    void SetSourceCommitFailed(SourceCommit&& prepared,CropFailure failure);
    bool SwitchCropBox();
    bool SwitchCropPlane();
    bool SwitchCropCylinder();
    bool SwitchCropSphere();
    bool SetCropMode(CropRemovalMode removalMode);
    bool PreviousCrop();
    bool NextCrop();
    bool SetCropNode(CropNodeId nodeId);
    bool ExitCrop();
    bool GetCropActive() const;
    // binding 生命周期独立于 widget 编辑态；Exit 后仍可导航 committed history。
    bool GetCropBound() const;
    CropHistoryState GetCropHistory() const;

    bool RefreshWidgetTransform();
    bool GetShaderTickNeeded() const;
    bool SendShaderCommit();
    // 从固定 Root 的明确节点路径做融合物化，不生成节点级中间 mask。
    bool BuildCropResult(
        CropInputSnapshot rootInput,
        CropCandidateCallback onComplete);
    bool BuildCropResult(CropNodeId nodeId,CropCandidateCallback onComplete);
    bool BuildCropResult(CropNodeId nodeId,CropBuildOptions options,CropRequestId requestId,CropCandidateCallback onComplete);
    bool GetBuildTickNeeded() const;
    FeatureOperationState GetExecutionState() const;
    bool SendBuildResult();

private:
    std::unique_ptr<Impl> m_impl;
};
