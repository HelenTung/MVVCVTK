#include "Interaction/CropBridge.h"
#include "Interaction/CropHistoryQueue.h"

#include "Algorithms/CropAlgorithm.h"
#include "App/Services/FeatureViewService.h"
#include "Interaction/CropBoxWidget.h"
#include "Interaction/CropCurveWidget.h"
#include "Interaction/CropPlaneWidget.h"
#include "Render/CropShaderController.h"
#include "Routing/CropRouter.h"

#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr double kVectorTolerance = 1.0e-12;

bool GetBoundsValid(const CropBoundsDouble6Array& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

RenderInputStamp GetInputStamp(const CropInputSnapshot& input)
{
    RenderInputStamp stamp;
    if (input.data) {
        stamp.dataRevision = input.data->self;
    }
    return stamp;
}
CropPointClassification GetRootMaskClass(const CropInputSnapshot& input,const CropVectorDouble3Array& point,const CropVectorDouble3Array& error)
{
    if(!input.data)return CropPointClassification::PrecisionNotMet;
    const auto* image=dynamic_cast<const ImageGrid3DPayload*>(input.data->payload.get());
    if(!image)return CropPointClassification::Kept;
    const auto& geometry=image->GetGeometry();double matrix[9],inverse[9];
    for(int row=0;row<3;++row)for(int col=0;col<3;++col)matrix[row*3+col]=geometry.direction[row*3+col]*geometry.spacing[col];
    const auto determinant=vtkMatrix3x3::Determinant(matrix);
    if(!std::isfinite(determinant)||determinant==0)return CropPointClassification::PrecisionNotMet;
    vtkMatrix3x3::Invert(matrix,inverse);std::array<std::int64_t,3> low{},high{};bool outside=false;
    std::size_t cells=1;
    for(int row=0;row<3;++row) {
        double index=0,uncertainty=0;
        for(int col=0;col<3;++col) {
            if(!std::isfinite(point[col])||!std::isfinite(error[col])||error[col]<0)return CropPointClassification::PrecisionNotMet;
            index+=inverse[row*3+col]*(point[col]-geometry.origin[col]);
            uncertainty+=std::abs(inverse[row*3+col])*(error[col]+32*std::numeric_limits<double>::epsilon()
                *(std::abs(point[col])+std::abs(geometry.origin[col])));
        }
        const double a=std::nextafter(index-uncertainty,-INFINITY),b=std::nextafter(index+uncertainty,INFINITY);
        if(!std::isfinite(a)||!std::isfinite(b))return CropPointClassification::PrecisionNotMet;
        const double minimum=double(geometry.extent[row*2])-0.5,maximum=double(geometry.extent[row*2+1])+0.5;
        if(b<minimum||a>maximum)return CropPointClassification::Removed;
        outside=outside||a<minimum||b>maximum;
        low[row]=static_cast<std::int64_t>(std::clamp(std::floor(a+0.5),double(geometry.extent[row*2]),double(geometry.extent[row*2+1])));
        high[row]=static_cast<std::int64_t>(std::clamp(std::floor(b+0.5),double(geometry.extent[row*2]),double(geometry.extent[row*2+1])));
        const auto count=static_cast<std::size_t>(high[row]-low[row]+1);
        if(count>4096/cells)cells=4097;else cells*=count;
    }
    const auto& mask=image->GetValidityMask();
    if(!mask)return outside?CropPointClassification::BoundaryBand:CropPointClassification::Kept;
    if(cells>4096)return CropPointClassification::PrecisionNotMet;
    bool kept=false,removed=outside;
    for(auto z=low[2];z<=high[2];++z)for(auto y=low[1];y<=high[1];++y)for(auto x=low[0];x<=high[0];++x) {
        const auto offset=(static_cast<std::size_t>(z-geometry.extent[4])*geometry.dimensions[1]
            +static_cast<std::size_t>(y-geometry.extent[2]))*geometry.dimensions[0]+static_cast<std::size_t>(x-geometry.extent[0]);
        if(offset>=mask->size())return CropPointClassification::PrecisionNotMet;
        if((*mask)[offset])kept=true;else removed=true;
        if(kept&&removed)return CropPointClassification::BoundaryBand;
    }
    return kept?CropPointClassification::Kept:CropPointClassification::Removed;
}
CropFailure GetPreviewFailure(RenderEffectFailure failure) {
    if(failure==RenderEffectFailure::PrecisionNotMet)return CropFailure::PrecisionNotMet;
    if(failure==RenderEffectFailure::ResourceLimit)return CropFailure::ResourceLimit;
    return CropFailure::PreviewNotReady;
}

}



namespace {
struct SourceCommitGate final { void* owner=nullptr;bool isPending=false; };
}
class CropBridge::SourceCommit::Impl final {
public:
    std::weak_ptr<SourceCommitGate> gate;
    CropHistory::Stage stage;
    CropShaderPayload payload;
    std::vector<std::shared_ptr<CropShaderEffect>> effects;
    bool isQueued=false;
    bool isCommitted=false;
    ~Impl() {
        if(!isCommitted)for(const auto& effect:effects)effect->ClearSourcePreview(payload.revision);
        if(const auto owner=gate.lock())owner->isPending=false;
    }
};

class CropBridge::Impl final {
public:
    std::function<void()> onWorkAvailable;


    struct TargetBinding final {
        std::shared_ptr<FeatureViewService> service;
        std::shared_ptr<CropShaderEffect> effect;
    };

    struct PendingShader final {
        std::optional<CropHistory::Stage> stage;
        CropShaderPayload payload;
        std::vector<TargetBinding> targets;
        std::vector<TargetBinding> retiredTargets;
        std::shared_ptr<FeatureViewService> nextReferenceService;
        vtkRenderWindowInteractor* nextInteractor = nullptr;
        bool isTargetRebind = false;
        vtkRenderer* nextRenderer = nullptr;
    };

    struct BuildTask final {
        std::shared_future<CropMaterializationCandidate> result;
        std::thread worker;
        CropCandidateCallback callback;
        CropBuildParams params;
        std::shared_ptr<std::atomic<bool>> isCancelled;
        std::shared_ptr<std::atomic<std::uint64_t>> phase;
    };

    Impl();
    ~Impl();

    std::shared_ptr<RenderEffect> GetViewEffect(const FeatureViewService* service) const {
        for(const auto& target:m_targets)if(target.service.get()==service)return target.effect;return {};
    }
    bool StartView(const CropViewRequest& request);
    bool StartView(
        const CropViewRequest& request,
        CropInputSnapshot input);
    bool ClearBindings();
    bool SetCropInput(CropInputSnapshot input);
    bool GetOwnerReady() const { return m_ownerThread == std::this_thread::get_id(); }
    CropEditAdmission SendRequest(CropEditRequest request);
    std::optional<CropViewPreviewState> GetViewState(const FeatureViewService* service) const {
        const auto target=std::find_if(m_targets.begin(),m_targets.end(),[service](const auto& value){return value.service.get()==service;});
        if(target==m_targets.end()||!target->effect)return std::nullopt;
        const auto history=m_tree.GetSnapshot(0,0);
        CropViewPreviewState result;result.requestedHead=history.requestedHead;result.appliedHead=history.appliedHead;
        result.renderedHead=target->effect->GetRenderedNode();result.effect=target->effect->GetState();
        if(result.renderedHead&&!m_tree.GetNode(result.renderedHead))result.renderedHead=0;
        result.precision=target->effect->GetCoordinatePrecision();
        result.isRenderPending=result.effect.isRenderPending||result.renderedHead!=result.appliedHead;
        return result;
    }
    CropPreviewPrecision GetPreviewPrecision(const FeatureViewService* service,const std::vector<CropVectorDouble3Array>& points) const {
        CropPreviewPrecision result;result.documentId=m_tree.GetDocumentId();result.stateRevision=m_tree.GetRevision();
        if(points.size()>256){result.failureReason=CropFailure::ResourceLimit;return result;}
        const auto target=std::find_if(m_targets.begin(),m_targets.end(),[service](const auto& value){return value.service.get()==service;});
        if(target==m_targets.end()||!target->effect)return result;
        const auto stamp=target->service->GetRenderInputStamp();
        if(!stamp||!m_input.data||stamp->dataRevision!=m_input.data->self){result.failureReason=CropFailure::SourceMismatch;return result;}
        result=target->effect->GetPreviewPrecision(points);result.documentId=m_tree.GetDocumentId();result.stateRevision=m_tree.GetRevision();
        if(!m_tree.GetNode(result.renderedHead)){result.failureReason=CropFailure::PreviewNotReady;result.renderedHead=0;result.samples.clear();return result;}
        if(result.failureReason!=CropFailure::None)return result;
        result.keptCount=result.removedCount=result.boundaryBandCount=result.precisionNotMetCount=0;
        for(std::size_t i=0;i<points.size();++i) {
            const auto root=GetRootMaskClass(m_input,points[i],result.coordinates.inputError);auto& value=result.samples[i];
            if(root==CropPointClassification::Removed)value=root;
            else if(value!=CropPointClassification::Removed) {
                if(root==CropPointClassification::PrecisionNotMet)value=root;
                else if(root==CropPointClassification::BoundaryBand&&value!=CropPointClassification::PrecisionNotMet)value=root;
            }
            switch(value) {
                case CropPointClassification::Kept:++result.keptCount;break;
                case CropPointClassification::Removed:++result.removedCount;break;
                case CropPointClassification::BoundaryBand:++result.boundaryBandCount;break;
                case CropPointClassification::PrecisionNotMet:++result.precisionNotMetCount;break;
            }
        }
        return result;
    }
    CropNodeId GetRenderedHead() const {
        CropNodeId result=0;bool first=true;
        for(const auto& target:m_targets) {
            const auto node=target.effect?target.effect->GetRenderedNode():0;
            if(first){result=node;first=false;}else if(result!=node)return 0;
        }
        return result&&m_tree.GetNode(result)?result:0;
    }
    CropHistorySnapshot GetHistory(CropNodeId after,std::size_t limit) const {
        auto history=m_tree.GetSnapshot(after,limit);history.renderedHead=GetRenderedHead();return history;
    }
    CropPruneImpact GetPruneImpact(const CropPruneRequest& request) const { return m_tree.GetPruneImpact(request); }
    std::optional<CropNodeSnapshot> GetNode(CropNodeId node) const { return m_commands.GetNode(m_tree,node); }
    std::optional<CropEditOutcome> GetOutcome(CropRequestId id) const { return m_commands.GetOutcome(id); }
    const CropInputSnapshot& GetSource() const { return m_input; }
    void ForgetOutcome(CropRequestId id) { m_commands.ForgetOutcome(id); }
    bool GetResultsValid(const std::vector<CropResultRecord>& results) const { return m_tree.GetResultsValid(results); }
    void SetResults(std::vector<CropResultRecord>&& results) noexcept { m_tree.SetResults(std::move(results)); }
    void SetSourceCommitFailed(std::unique_ptr<SourceCommit::Impl> prepared,CropFailure failure);
    CropDocumentArchive GetArchive() const { return m_tree.GetArchive(); }
    bool ClearDocument();
    bool CancelPending();
    bool GetSourceTransitionNeeded() const { return !m_commands.GetIsEmpty() && !m_pendingShader && !GetTargetsReady() && !m_sourceGate->isPending; }
    std::unique_ptr<SourceCommit::Impl> BuildSourceCommit(CropNodeId nodeId,bool isQueued);
    bool GetSourceCommitReady(const SourceCommit::Impl& prepared) const noexcept;
    void SetSourceCommit(std::unique_ptr<SourceCommit::Impl> prepared) noexcept;

    bool RefreshWidgetTransform();
    bool SwitchCrop(CropShape geometryType);
    bool SetCropMode(CropRemovalMode removalMode);
    bool PreviousCrop();
    bool NextCrop();
    bool SetCropNode(CropNodeId nodeId);
    bool ExitCrop();
    bool GetCropActive() const;
    bool GetCropBound() const;
    CropHistoryState GetCropHistory() const;
    bool GetShaderTickNeeded() const;
    bool SendShaderCommit();
    bool BuildCropResult(
        CropNodeId nodeId,
        CropCandidateCallback onComplete,CropBuildOptions options={},CropRequestId requestId=0);
    bool GetBuildTickNeeded() const;
    FeatureOperationState GetExecutionState() const;
    bool SendBuildResult();
    bool GetLeaseReady() const;

private:
    bool StartViewInput(
        const CropViewRequest& request,
        std::optional<CropInputSnapshot> input);
    void OnBoxWidget(CropInteractionPhase phase);
    void OnPlaneWidget(CropInteractionPhase phase);
    void OnCurveWidget(CropInteractionPhase phase);
    bool SetCandidate(CropOpItem operation);
    bool SendNextOp();
    bool SetShader(CropHistory::Stage stage);
    CropFailure m_previewFailure=CropFailure::PreviewNotReady;
    void FailPending(CropFailure failure);
    CropEditRequest BuildEditRequest(CropEditKind kind,CropNodeId node) const;
    std::uint64_t CreateShaderRevision() noexcept;
    std::optional<CropOpItem> BuildBoxOp();
    std::optional<CropOpItem> BuildPlaneOp();
    std::optional<CropOpItem> BuildCurveOp();
    bool GetOpSame(
        const CropOpItem& first,
        const CropOpItem& second) const;
    CropBoundsDouble6Array GetWorldBounds() const;
    CropBoundsDouble6Array GetWidgetWorldBounds() const;
    std::optional<CropMatrixDouble16Array> GetWorldToInput() const;
    bool GetShaderCommitted() const;
    bool GetTargetsReady() const;
    bool SetInteraction(
        const InteractionSource& source,
        bool isInteracting);
    bool ClearDragSources();
    bool ClearInteractions();
    bool SetWidgetActive(bool isActive);
    void ClearShaderStage();
    void ClearShader();
    void ClearTargets();
    CropMaterializationCandidate BuildResultFailure(
        const CropBuildParams& params,
        CropFailure failureReason,
        const char* message) const;

    CropRouter m_buildRouter;
    CropCurveWidget m_curveWidget;
    CropBoxWidget m_boxWidget;
    CropPlaneWidget m_planeWidget;
    CropInputSnapshot m_input;
    std::shared_ptr<FeatureViewService> m_referenceService;
    std::weak_ptr<const FeatureViewLease> m_lease;
    std::vector<TargetBinding> m_targets;
    std::shared_ptr<SourceCommitGate> m_sourceGate=std::make_shared<SourceCommitGate>();
    CropHistory m_tree;
    CropHistoryQueue m_commands;
    // 仅为当前已应用路径的渲染缓存，不能用于查找或改写历史。
    std::vector<CropOpItem> m_activePath;
    CropRequestId m_lastRequestId = 0;
    CropNodeId m_editNode = 0;
    CropNodeId m_dragParent = 0;
    std::shared_ptr<const DataResourceLease> m_sourceLease;
    const std::thread::id m_ownerThread = std::this_thread::get_id();
    CropShaderPayload m_activePayload;
    std::optional<PendingShader> m_pendingShader;
    std::optional<BuildTask> m_buildTask;
    std::optional<CropOpItem> m_dragStart;
    CropShape m_geometryType = CropShape::Box;
    CropRemovalMode m_removalMode = CropRemovalMode::None;
    InteractionSource m_boxSource{ "OrthogonalCrop", "" };
    InteractionSource m_planeSource{ "OrthogonalCrop", "" };
    InteractionSource m_curveSource{ "OrthogonalCrop", "" };
    InteractionSource m_commitSource{ "OrthogonalCrop", "" };
    bool m_hasDrag = false;
    std::uint64_t m_nextRevision = 1;
    bool m_isActive = false;
    bool m_isAccepting = true;
};

CropBridge::Impl::Impl()
{
    m_sourceGate->owner=this;
    const std::string bridgeId = std::to_string(
        reinterpret_cast<std::uintptr_t>(this));
    m_boxSource.channelId = bridgeId + ":Box";
    m_planeSource.channelId = bridgeId + ":Plane";
    m_curveSource.channelId = bridgeId + ":Curve";
    m_curveWidget.SetCallback([this](CropInteractionPhase phase){OnCurveWidget(phase);});
    m_curveWidget.SetContextGate([this] {
        if(!GetLeaseReady()||!m_isActive||m_buildTask||m_sourceGate->isPending)return false;
        for(const auto& result:m_tree.GetResults())if(result.status==CropResultStatus::Building)return false;
        return !RefreshWidgetTransform();
    });
    m_commitSource.channelId = bridgeId + ":Commit";
    m_boxWidget.SetBoundsCallback(
        [this](const CropBoundsDouble6Array&, const CropInteractionPhase phase) {
            OnBoxWidget(phase);
        });
    m_planeWidget.SetPlaneCallback(
        [this](const CropVectorDouble3Array&, const CropVectorDouble3Array&, const CropInteractionPhase phase) {
            OnPlaneWidget(phase);
        });
}

CropBridge::Impl::~Impl()
{
    m_sourceGate->owner=nullptr;
    m_isAccepting = false;
    m_isActive = false;
    m_hasDrag = false;
    const bool hasBindings = m_referenceService || !m_targets.empty();
    if (hasBindings) {
        (void)ClearInteractions();
        m_boxWidget.SetEnabled(false);
        m_planeWidget.SetEnabled(false);
        m_curveWidget.SetEnabled(false);
        m_curveWidget.SetContext(nullptr,nullptr);
        m_boxWidget.SetInteractor(nullptr);
        m_planeWidget.SetInteractor(nullptr);
        ClearShader();
        ClearTargets();
    }
    if (m_buildTask) {
        m_buildTask->isCancelled->store(true, std::memory_order_release);
        if (m_buildTask->worker.joinable()) {
            m_buildTask->worker.join();
        }
        m_buildTask.reset();
    }
}

bool CropBridge::Impl::StartView(const CropViewRequest& request)
{
    return StartViewInput(request, std::nullopt);
}

bool CropBridge::Impl::StartView(
    const CropViewRequest& request,
    CropInputSnapshot input)
{
    return StartViewInput(
        request,
        std::optional<CropInputSnapshot>{ std::move(input) });
}

bool CropBridge::Impl::StartViewInput(
    const CropViewRequest& request,
    std::optional<CropInputSnapshot> input)
{
    const auto lease = request.lease.lock();
    if (!m_isAccepting
        || m_sourceGate->isPending
        || m_buildTask
        || !lease
        || !lease->GetIsActive()
        || !lease->GetIsOwnerThread()
        || !request.interactor
        || !request.renderer
        || !request.referenceService
        || (input && !CropAlgorithm::GetInputValid(*input))) {
        return false;
    }
    const auto currentLease = m_lease.lock();
    if (currentLease
        && currentLease.get() != lease.get()
        && m_referenceService) {
        return false;
    }
    m_lease = lease;
    if(request.isCandidateOnly&&!m_targets.empty())return false;
    const bool isInputChanged = input && !m_input.data;
    if (input && m_input.data && input->data->self != m_input.data->self) return false;
    if (isInputChanged && !SetCropInput(*input)) return false;
    std::vector<std::shared_ptr<FeatureViewService>> targetServices;
    for (const auto& service : request.targetServices) {
        if (!service) {
            continue;
        }
        const auto duplicate = std::find_if(
            targetServices.begin(),
            targetServices.end(),
            [&service](const auto& current) { return current.get() == service.get(); });
        if (duplicate == targetServices.end()) {
            targetServices.push_back(service);
        }
    }
    if (targetServices.empty() && !input) {
        targetServices.push_back(request.referenceService);
    }
    if (targetServices.empty()) {
        return false;
    }
    const bool isSameReference =
        m_referenceService.get()
        == request.referenceService.get();
    const bool isSameTargets =
        targetServices.size() == m_targets.size()
        && std::all_of(
            targetServices.begin(),
            targetServices.end(),
            [this](const auto& service) {
                return std::any_of(
                    m_targets.begin(),
                    m_targets.end(),
                    [&service](const auto& target) {
                        return target.service.get()
                            == service.get();
                    });
            });
    if (m_pendingShader) {
        // 历史前缀正在换代时，同一视图的 Start 只是恢复编辑连接；
        // 它不能因为 GPU revision 尚未提交而拒绝后续 Box/Plane 手势。
        if (!isInputChanged
            && !m_pendingShader->isTargetRebind
            && (m_isActive || GetShaderCommitted())
            && isSameReference && isSameTargets) {
            (void)ClearDragSources();
            m_hasDrag = false;
            m_dragStart.reset();
            m_boxWidget.SetInteractor(request.interactor);
            m_planeWidget.SetInteractor(request.interactor);
            m_curveWidget.SetContext(request.interactor,request.renderer);
            m_isActive = true;
            return true;
        }
        return false;
    }
    m_hasDrag = false;
    m_dragStart.reset();

    std::vector<TargetBinding> targets;
    std::vector<TargetBinding> createdTargets;
    for (const auto& service : targetServices) {
        const auto existing = std::find_if(
            m_targets.begin(),
            m_targets.end(),
            [&service](const TargetBinding& target) {
                return target.service.get() == service.get();
            });
        if (existing != m_targets.end()) {
            targets.push_back(*existing);
            continue;
        }
        TargetBinding target;
        target.service = service;
        target.effect = std::make_shared<CropShaderEffect>();
        if (!request.isCandidateOnly&&!target.service->AttachRenderEffect(target.effect)) {
            for (const auto& created : createdTargets) {
                (void)created.service->DetachRenderEffect(
                    created.effect.get());
            }
            return false;
        }
        createdTargets.push_back(target);
        targets.push_back(std::move(target));
    }

    const bool isShaderCommitted = GetShaderCommitted();
    if (!isInputChanged
        && (m_isActive || isShaderCommitted)
        && isSameReference && isSameTargets) {
        (void)ClearInteractions();
        m_boxWidget.SetInteractor(request.interactor);
        m_planeWidget.SetInteractor(request.interactor);
        m_curveWidget.SetContext(request.interactor,request.renderer);
        m_isActive = true;
        try { if (onWorkAvailable) onWorkAvailable(); } catch (...) {}
        return true;
    }

    // 有已提交前缀时，新增目标以同一 table handle 建立新 revision 的 staged/ready/commit；
    // 只有整体成功后才清退旧目标，避免重绑定中出现部分窗口先失去裁切。
    if (!request.isCandidateOnly && !isInputChanged && isShaderCommitted) {
        const std::uint64_t revision = CreateShaderRevision();
        if (!revision) return false;
        CropShaderPayload payload = m_activePayload;
        payload.revision = revision;
        payload.nodeCount = m_activePath.size();
        if (!payload.predicateTable) {
            return false;
        }
        std::vector<TargetBinding> accepted;
        for (const auto& target : targets) {
            if (!target.effect
                || !target.effect->SetCropParams(payload)) {
                for (const auto& current : accepted) {
                    (void)current.effect->ClearCropStage(revision);
                }
                for (const auto& created : createdTargets) {
                    (void)created.service->DetachRenderEffect(
                        created.effect.get());
                }
                return false;
            }
            accepted.push_back(target);
        }
        (void)ClearInteractions();
        m_pendingShader = PendingShader{
            std::nullopt,
            std::move(payload),
            std::move(accepted),
            m_targets,
            request.referenceService,
            request.interactor,
            true,
            request.renderer
        };
        m_isActive = true;
        try { if (onWorkAvailable) onWorkAvailable(); } catch (...) {}
        return true;
    }

    // 所有会失败的 target/effect 准备已经完成；从这里开始连续提交输入和 binding。
    // 输入换代必须同时退休旧 history，不能让旧 predicate table 作用到新数据。
    (void)ClearInteractions();
    ClearShader();

    for (const auto& current : m_targets) {
        const bool isRetained = std::any_of(
            targets.begin(),
            targets.end(),
            [&current](const auto& target) {
                return current.service.get() == target.service.get()
                    && current.effect.get() == target.effect.get();
            });
        if (!isRetained && current.service && current.effect) {
            (void)current.service->DetachRenderEffect(
                current.effect.get());
        }
    }
    m_referenceService = request.referenceService;
    m_targets = std::move(targets);
    m_boxWidget.SetInteractor(request.interactor);
    m_planeWidget.SetInteractor(request.interactor);
    m_curveWidget.SetContext(request.interactor,request.renderer);
    const auto worldBounds = GetWidgetWorldBounds();
    if (GetBoundsValid(worldBounds)) {
        m_boxWidget.SetReferenceWorldBounds(worldBounds);
        m_boxWidget.SetWidgetWorldBounds(worldBounds);
        m_planeWidget.SetReferenceWorldBounds(worldBounds);
    }
    m_isActive = true;
    return true;
}

bool CropBridge::Impl::ClearBindings()
{
    const auto lease = m_lease.lock();
    // StopLease 先关闭业务入口，owner thread 随后仍必须能够完成确定性清理。
    if (!GetOwnerReady() || (lease&&!lease->GetIsOwnerThread()) || (!lease&&!m_targets.empty()) || m_sourceGate->isPending) {
        return false;
    }
    if (m_buildTask) {
        m_buildTask->isCancelled->store(true, std::memory_order_release);
    }
    // VTK Off 可能在拖拽中同步补发 EndInteraction；先关闭业务 gate，
    // 避免清理过程把未完成交互误写成新的 staged/history 操作。
    m_isActive = false;
    m_hasDrag = false;
    m_dragStart.reset();
    (void)ClearInteractions();
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    m_curveWidget.SetEnabled(false);
    m_curveWidget.SetContext(nullptr,nullptr);
    m_boxWidget.SetInteractor(nullptr);
    m_planeWidget.SetInteractor(nullptr);
    ClearShader();
    m_commands.SetCancelled(m_tree);
    m_referenceService.reset();
    ClearTargets();
    m_lease.reset();
    return true;
}

bool CropBridge::Impl::GetLeaseReady() const
{
    const auto lease = m_lease.lock();
    return lease
        && lease->GetIsActive()
        && lease->GetIsOwnerThread();
}

bool CropBridge::Impl::SetCropInput(CropInputSnapshot input)
{
    if (!m_isAccepting || !GetOwnerReady() || !CropAlgorithm::GetInputValid(input)) return false;
    if (m_input.data) return input.data->self == m_input.data->self
        && input.data->payload == m_input.data->payload && input.inputModelBounds == m_input.inputModelBounds;
    auto history = CropHistory::Create(input.data->self);
    if (!history.GetDocumentId()) return false;
    std::shared_ptr<const DataResourceLease> sourceLease;
    if (GetDataEntityIdValid(input.data->lifetimeScope)) {
        const auto access = input.data->lifetime.lock();
        sourceLease = access ? access->StartResourceUse(input.data->self,"crop-document") : nullptr;
        if (!sourceLease) return false;
    }
    m_tree = std::move(history);m_input = std::move(input);m_sourceLease = std::move(sourceLease);
    const auto bounds=GetWidgetWorldBounds();
    if (GetBoundsValid(bounds)) {
        m_boxWidget.SetReferenceWorldBounds(bounds);m_boxWidget.SetWidgetWorldBounds(bounds);
        m_planeWidget.SetReferenceWorldBounds(bounds);
    }
    return true;
}

bool CropBridge::Impl::CancelPending()
{
    if (!GetOwnerReady() || m_sourceGate->isPending) return false;
    if (m_buildTask) m_buildTask->isCancelled->store(true,std::memory_order_release);
    ClearShaderStage();m_commands.SetCancelled(m_tree);
    m_hasDrag=false;m_dragStart.reset();m_dragParent=0;m_editNode=0;
    return true;
}

bool CropBridge::Impl::ClearDocument()
{
    if (!GetOwnerReady() || !m_tree.GetResults().empty() || m_buildTask || m_sourceGate->isPending) return false;
    if (GetCropBound() && !ClearBindings()) return false;
    m_commands.SetCancelled(m_tree);m_tree={};m_input={};m_sourceLease.reset();m_activePath.clear();m_activePayload={};
    return true;
}

std::unique_ptr<CropBridge::SourceCommit::Impl> CropBridge::Impl::BuildSourceCommit(CropNodeId nodeId,bool isQueued)
{
    const bool offline=m_targets.empty()&&!isQueued&&nodeId==m_tree.GetRootId();
    if(!GetOwnerReady() || (!offline&&!GetLeaseReady()) || m_sourceGate->isPending || m_pendingShader
        || (m_targets.empty()&&!offline) || (!isQueued && !m_commands.GetIsEmpty()))return {};
    auto stage=isQueued?m_commands.BuildNext(m_tree):m_tree.BuildSelection(nodeId);
    if (stage.failureReason!=CropFailure::None && isQueued) {
        m_commands.SetFailed(m_tree,stage.failureReason,std::move(stage.impact));
        (void)SetInteraction(m_commitSource,!m_commands.GetIsEmpty());
        return {};
    }
    if(!m_tree.GetStageReady(stage))return {};
    auto table=CropAlgorithm::BuildPredicateTable(stage.operations,stage.operations.size());
    if(!table.isSucceeded||!table.predicateTable)return {};
    const auto revision=CreateShaderRevision();if(!revision)return {};
    auto prepared=std::make_unique<SourceCommit::Impl>();
    prepared->payload={revision,GetInputStamp(m_input),stage.operations.size(),std::move(table.predicateTable)};
    prepared->payload.nodeId=stage.head;
    prepared->stage=std::move(stage);prepared->isQueued=isQueued;
    prepared->effects.reserve(m_targets.size());
    for(const auto& target:m_targets) {
        if(!target.effect || !target.effect->SetSourcePreview(prepared->payload))return {};
        prepared->effects.push_back(target.effect);
    }
    prepared->gate=m_sourceGate;m_sourceGate->isPending=true;
    return prepared;
}

bool CropBridge::Impl::GetSourceCommitReady(const SourceCommit::Impl& prepared) const noexcept
{
    const auto gate=prepared.gate.lock();
    return gate==m_sourceGate && gate->owner==this && gate->isPending && !prepared.isCommitted
        && m_tree.GetStageReady(prepared.stage);
}

void CropBridge::Impl::SetSourceCommit(std::unique_ptr<SourceCommit::Impl> prepared) noexcept
{
    if(!prepared || !GetSourceCommitReady(*prepared))std::terminate();
    for(const auto& effect:prepared->effects)effect->SetSourcePreviewComplete(prepared->payload.revision);
    m_activePath=std::move(prepared->stage.operations);
    if(prepared->isQueued)m_commands.SetComplete(m_tree,std::move(prepared->stage));
    else {m_tree.SetRequestedHead(prepared->stage.head);m_tree.SetCommit(std::move(prepared->stage));}
    m_activePayload=std::move(prepared->payload);m_editNode=0;prepared->isCommitted=true;
}

void CropBridge::Impl::SetSourceCommitFailed(std::unique_ptr<SourceCommit::Impl> prepared,CropFailure failure)
{
    const auto gate=prepared?prepared->gate.lock():nullptr;
    const bool queued=prepared && gate==m_sourceGate && gate->owner==this && !prepared->isCommitted && prepared->isQueued;
    prepared.reset();
    if (queued) m_commands.SetFailed(m_tree,failure);
    (void)SetInteraction(m_commitSource,!m_commands.GetIsEmpty());
}

void CropBridge::SetSourceCommitFailed(SourceCommit&& prepared,CropFailure failure)
{
    m_impl->SetSourceCommitFailed(std::move(prepared.m_impl),failure);
}

std::uint64_t CropBridge::Impl::CreateShaderRevision() noexcept
{
    const auto revision=m_nextRevision;
    if (revision) m_nextRevision=revision==std::numeric_limits<std::uint64_t>::max()?0:revision+1;
    return revision;
}

CropEditRequest CropBridge::Impl::BuildEditRequest(CropEditKind kind,CropNodeId node) const
{
    CropEditRequest request;
    request.documentId=m_tree.GetDocumentId();request.requestId=CropHistory::CreateNodeId();
    request.expectedRevision=m_tree.GetRevision();request.kind=kind;request.nodeId=node;
    return request;
}

CropEditAdmission CropBridge::Impl::SendRequest(CropEditRequest request)
{
    if (!GetLeaseReady() || !GetCropBound() || !m_isAccepting) {
        CropEditAdmission rejected;rejected.failureReason=CropFailure::PreviewNotReady;return rejected;
    }
    const bool building=m_buildTask.has_value()||std::any_of(m_tree.GetResults().begin(),m_tree.GetResults().end(),
        [](const auto& result){return result.status==CropResultStatus::Building;});
    if(building&&!m_commands.GetOutcome(request.requestId)) {
        CropEditAdmission rejected;rejected.requestId=request.requestId;rejected.stateRevision=m_tree.GetRevision();
        rejected.failureReason=CropFailure::Busy;return rejected;
    }
    auto result=m_commands.StartRequest(m_tree,std::move(request));
    if (result.isAccepted && !result.isReplay) {
        m_lastRequestId=result.requestId;
        (void)SetInteraction(m_commitSource,true);
        try { if(onWorkAvailable)onWorkAvailable(); } catch(...) {}
        (void)SendNextOp();
    }
    return result;
}

bool CropBridge::Impl::SwitchCrop(const CropShape geometryType)
{
    if (!m_isActive
        || m_buildTask
        || !CropAlgorithm::GetInputValid(m_input)
        || (geometryType != CropShape::Box && geometryType != CropShape::Plane && geometryType != CropShape::Cylinder && geometryType != CropShape::Sphere)) {
        return false;
    }
    m_geometryType = geometryType;
    m_hasDrag = false;
    m_dragStart.reset();
    (void)ClearDragSources();
    // Switch 结束上一条操作的模式编辑权；下一次有效 Released 会追加历史。
    m_editNode = 0;
    const auto worldBounds = GetWidgetWorldBounds();
    if (!GetBoundsValid(worldBounds)) {
        return false;
    }

    m_boxWidget.SetReferenceWorldBounds(worldBounds);
    m_planeWidget.SetReferenceWorldBounds(worldBounds);
    bool isEnabled = false;
    (void)m_curveWidget.SetEnabled(false);
    if(geometryType==CropShape::Sphere||geometryType==CropShape::Cylinder) {
        m_boxWidget.SetEnabled(false);m_planeWidget.SetEnabled(false);
        const auto modelToWorld=m_referenceService?m_referenceService->GetModelToWorld():std::optional<CropMatrixDouble16Array>{};
        if(!modelToWorld)return false;
        CropOpItem operation;operation.geometryType=geometryType;
        double size=0;
        for(int i=0;i<3;++i) {
            operation.centerInInputModel[i]=m_input.inputModelBounds[2*i]*0.5+m_input.inputModelBounds[2*i+1]*0.5;
            size=std::max(size,m_input.inputModelBounds[2*i+1]-m_input.inputModelBounds[2*i]);
        }
        operation.radius=size>0?size*0.25:1;operation.height=size>0?size*0.5:2;
        isEnabled=m_curveWidget.SetGeometry(operation,*modelToWorld)&&m_curveWidget.SetEnabled(true);
    }
    else if (geometryType == CropShape::Box) {
        m_planeWidget.SetEnabled(false);
        m_boxWidget.SetWidgetWorldBounds(worldBounds);
        isEnabled = m_boxWidget.SetEnabled(true);
    }
    else {
        m_boxWidget.SetEnabled(false);
        const CropVectorDouble3Array origin = {
            (worldBounds[0] + worldBounds[1]) * 0.5,
            (worldBounds[2] + worldBounds[3]) * 0.5,
            (worldBounds[4] + worldBounds[5]) * 0.5
        };
        const CropVectorDouble3Array normal = { 0.0, 0.0, 1.0 };
        const std::array<double, 2> half = {
            (worldBounds[1] - worldBounds[0]) * 0.5,
            (worldBounds[3] - worldBounds[2]) * 0.5
        };
        m_planeWidget.SetWidgetWorldPlane(origin, normal, half);
        isEnabled = m_planeWidget.SetEnabled(true);
    }
    // Widget 仅存在于 reference renderer；切换成功后通过 service 门铃请求
    // 下一帧，避免依赖 vtkBoxWidget2/vtkImplicitPlaneWidget2 的偶然 Render 副作用。
    if (isEnabled && m_referenceService) {
        (void)m_referenceService->SetRenderNeeded();
    }
    return isEnabled;
}

bool CropBridge::Impl::SetCropMode(const CropRemovalMode mode)
{
    if (!m_isActive || (mode!=CropRemovalMode::None && mode!=CropRemovalMode::KeepInside
        && mode!=CropRemovalMode::RemoveInside)) return false;
    if (mode==m_removalMode) return true;
    if (mode!=CropRemovalMode::None && m_editNode) {
        const auto node=m_commands.GetNode(m_tree,m_editNode);
        if (node && node->operation) {
            auto request=BuildEditRequest(CropEditKind::Replace,m_editNode);
            request.operation=*node->operation;request.operation.removalMode=mode;
            const auto accepted=SendRequest(std::move(request));
            if (!accepted.isAccepted) return false;
            m_editNode=accepted.nodeId;
        }
    }
    m_removalMode=mode;
    return true;
}

void CropBridge::Impl::OnBoxWidget(const CropInteractionPhase phase)
{
    if (!GetLeaseReady()) {
        return;
    }
    if (!m_isActive
        || m_geometryType != CropShape::Box
        || m_removalMode == CropRemovalMode::None) {
        (void)SetInteraction(m_boxSource, false);
        m_hasDrag = false;
        m_dragStart.reset();
        return;
    }
    if (phase == CropInteractionPhase::Hover) {
        (void)SetInteraction(m_boxSource, false);
        m_hasDrag = false;
        m_dragStart = BuildBoxOp();
        return;
    }
    if (phase == CropInteractionPhase::Dragging) {
        if (!SetInteraction(m_boxSource, true)) {
            m_hasDrag = false;
            m_dragStart.reset();
            return;
        }
        if (!m_hasDrag) m_dragParent=m_tree.GetRequestedHead();
        m_hasDrag = true;
        return;
    }
    if (phase != CropInteractionPhase::Released) {
        return;
    }

    const bool hasDrag = m_hasDrag;
    m_hasDrag = false;
    auto dragStart = std::move(m_dragStart);
    m_dragStart.reset();
    if (!hasDrag || !dragStart) {
        (void)SetInteraction(m_boxSource, false);
        return;
    }
    auto operation = BuildBoxOp();
    if (!operation || GetOpSame(*dragStart, *operation)) {
        (void)SetInteraction(m_boxSource, false);
        return;
    }
    (void)SetCandidate(std::move(*operation));
    (void)SetInteraction(m_boxSource, false);
}

void CropBridge::Impl::OnPlaneWidget(const CropInteractionPhase phase)
{
    if (!GetLeaseReady()) {
        return;
    }
    if (!m_isActive
        || m_geometryType != CropShape::Plane
        || m_removalMode == CropRemovalMode::None) {
        (void)SetInteraction(m_planeSource, false);
        m_hasDrag = false;
        m_dragStart.reset();
        return;
    }
    if (phase == CropInteractionPhase::Hover) {
        (void)SetInteraction(m_planeSource, false);
        m_hasDrag = false;
        m_dragStart = BuildPlaneOp();
        return;
    }
    if (phase == CropInteractionPhase::Dragging) {
        if (!SetInteraction(m_planeSource, true)) {
            m_hasDrag = false;
            m_dragStart.reset();
            return;
        }
        if (!m_hasDrag) m_dragParent=m_tree.GetRequestedHead();
        m_hasDrag = true;
        return;
    }
    if (phase != CropInteractionPhase::Released) {
        return;
    }

    const bool hasDrag = m_hasDrag;
    m_hasDrag = false;
    auto dragStart = std::move(m_dragStart);
    m_dragStart.reset();
    if (!hasDrag || !dragStart) {
        (void)SetInteraction(m_planeSource, false);
        return;
    }
    auto operation = BuildPlaneOp();
    if (!operation || GetOpSame(*dragStart, *operation)) {
        (void)SetInteraction(m_planeSource, false);
        return;
    }
    (void)SetCandidate(std::move(*operation));
    (void)SetInteraction(m_planeSource, false);
}

std::optional<CropOpItem> CropBridge::Impl::BuildCurveOp()
{
    auto operation=m_curveWidget.GetGeometry();operation.removalMode=m_removalMode;
    const auto geometry=CropGeometry::Build(operation);return geometry?std::optional<CropOpItem>{geometry->GetOperation()}:std::nullopt;
}
void CropBridge::Impl::OnCurveWidget(CropInteractionPhase phase)
{
    if(!GetLeaseReady())return;
    if(!m_isActive||(m_geometryType!=CropShape::Sphere&&m_geometryType!=CropShape::Cylinder)||m_removalMode==CropRemovalMode::None) {
        (void)SetInteraction(m_curveSource,false);m_hasDrag=false;m_dragStart.reset();return;
    }
    if(phase==CropInteractionPhase::Hover) {m_hasDrag=false;m_dragStart=BuildCurveOp();(void)SetInteraction(m_curveSource,false);return;}
    if(phase==CropInteractionPhase::Dragging) {
        if(!SetInteraction(m_curveSource,true)){m_hasDrag=false;m_dragStart.reset();return;}
        if(!m_hasDrag)m_dragParent=m_tree.GetRequestedHead();m_hasDrag=true;
        (void)m_referenceService->SetRenderNeeded();return;
    }
    if(phase!=CropInteractionPhase::Released)return;
    const bool dragged=m_hasDrag;m_hasDrag=false;auto before=std::move(m_dragStart);m_dragStart.reset();
    const auto operation=BuildCurveOp();
    if(dragged&&before&&operation&&!GetOpSame(*before,*operation))(void)SetCandidate(*operation);
    (void)SetInteraction(m_curveSource,false);(void)m_referenceService->SetRenderNeeded();
}

bool CropBridge::Impl::SetCandidate(CropOpItem operation)
{
    if (m_removalMode==CropRemovalMode::None || !m_dragParent) return false;
    auto request=BuildEditRequest(CropEditKind::Append,m_dragParent);
    request.operation=std::move(operation);
    const auto result=SendRequest(std::move(request));
    if (result.isAccepted) m_editNode=result.nodeId;
    return result.isAccepted;
}

bool CropBridge::Impl::SendNextOp()
{
    if (m_pendingShader || m_sourceGate->isPending || m_commands.GetIsEmpty() || !GetTargetsReady()) return false;
    auto stage=m_commands.BuildNext(m_tree);
    if (stage.failureReason!=CropFailure::None) {
        m_commands.SetFailed(m_tree,stage.failureReason,std::move(stage.impact));
        if (m_commands.GetIsEmpty()) (void)SetInteraction(m_commitSource,false);
        return true;
    }
    if (stage.head==m_tree.GetAppliedHead() && GetShaderCommitted()) {
        m_commands.SetComplete(m_tree,std::move(stage));
        if (m_commands.GetIsEmpty()) (void)SetInteraction(m_commitSource,false);
        return true;
    }
    if (!SetShader(std::move(stage))) {
        m_commands.SetFailed(m_tree,m_previewFailure);
        (void)SetInteraction(m_commitSource,!m_commands.GetIsEmpty());
        return false;
    }
    return true;
}

bool CropBridge::Impl::SetShader(CropHistory::Stage stage)
{
    m_previewFailure=CropFailure::PreviewNotReady;
    if (!m_tree.GetStageReady(stage)) return false;
    const auto table=CropAlgorithm::BuildPredicateTable(stage.operations,stage.operations.size());
    if (!table.isSucceeded || !table.predicateTable) {m_previewFailure=table.failureReason;return false;}
    const auto revision=CreateShaderRevision();if(!revision)return false;
    PendingShader pending;pending.payload={revision,GetInputStamp(m_input),stage.operations.size(),table.predicateTable};
    pending.payload.nodeId=stage.head;
    pending.stage=std::move(stage);pending.targets.reserve(m_targets.size());
    for (const auto& target:m_targets) {
        if (!target.effect || !target.effect->SetCropParams(pending.payload)) {
            for(const auto& accepted:pending.targets)(void)accepted.effect->ClearCropStage(revision);
            return false;
        }
        pending.targets.push_back(target);
    }
    if(pending.targets.empty())return false;
    m_pendingShader=std::move(pending);
    try { if(onWorkAvailable)onWorkAvailable(); } catch(...) {}
    return true;
}

bool CropBridge::Impl::PreviousCrop()
{
    const auto node=m_commands.GetNode(m_tree,m_tree.GetRequestedHead());
    return node && node->parentNodeId && SetCropNode(node->parentNodeId);
}

bool CropBridge::Impl::NextCrop()
{
    const auto children=m_tree.GetChildren(m_tree.GetRequestedHead());
    // 多个分支由 Node 请求明确选择，不能任意选最后创建的分支。
    return children.size()==1 && SetCropNode(children.front());
}

bool CropBridge::Impl::SetCropNode(CropNodeId nodeId)
{
    const auto result=SendRequest(BuildEditRequest(CropEditKind::Select,nodeId));
    if(result.isAccepted)m_editNode=0;
    return result.isAccepted;
}

bool CropBridge::Impl::GetShaderTickNeeded() const
{
    return m_pendingShader.has_value() || !m_commands.GetIsEmpty();
}

void CropBridge::Impl::FailPending(CropFailure failure)
{
    const bool hadCommand=m_pendingShader && m_pendingShader->stage.has_value();
    ClearShaderStage();
    if(hadCommand)m_commands.SetFailed(m_tree,failure);
    if(!m_commands.GetIsEmpty())(void)SetInteraction(m_commitSource,true);
}


bool CropBridge::Impl::SendShaderCommit()
{
    if(!m_pendingShader)return SendNextOp();
    auto& pending=*m_pendingShader;
    if(pending.stage&&!m_tree.GetStageReady(*pending.stage)) {
        ClearShaderStage();
        (void)SetInteraction(m_commitSource,!m_commands.GetIsEmpty());
        return SendNextOp();
    }
    bool ready=true;
    for(const auto& target:pending.targets) {
        const auto stamp=target.service->GetRenderInputStamp();
        if(!stamp || *stamp!=pending.payload.sourceStamp) {FailPending(CropFailure::SourceMismatch);return false;}
        const auto state=target.effect->GetState();
        if(state.status==RenderEffectStatus::Failed) {FailPending(GetPreviewFailure(state.failureReason));return false;}
        if(state.stagedRevision!=pending.payload.revision || state.status!=RenderEffectStatus::Ready) {
            ready=false;(void)target.service->SetRenderNeeded();
        }
    }
    if(!ready)return false;
    std::size_t committed=0;
    for(const auto& target:pending.targets) {
        if(!target.effect->StartCropCommit(pending.payload.revision))break;
        ++committed;
    }
    if(committed!=pending.targets.size() || !std::all_of(pending.targets.begin(),pending.targets.end(),
        [&](const auto& target){return target.effect->GetCropCommitReady(pending.payload.revision);})) {
        for(std::size_t index=committed;index>0;--index)(void)pending.targets[index-1].effect->ClearCropCommit(pending.payload.revision);
        FailPending(CropFailure::PreviewNotReady);return false;
    }
    // 全目标已预检，完成和树接管之间没有外部回调及大分配。
    for(const auto& target:pending.targets)if(!target.effect->SetCropComplete(pending.payload.revision))std::terminate();
    if(pending.stage) {
        m_activePath=std::move(pending.stage->operations);
        m_commands.SetComplete(m_tree,std::move(*pending.stage));
    }
    m_activePayload=std::move(pending.payload);
    if(pending.isTargetRebind) {
        for(const auto& retired:pending.retiredTargets) {
            const bool retained=std::any_of(pending.targets.begin(),pending.targets.end(),
                [&](const auto& target){return target.service==retired.service;});
            if(!retained&&retired.service&&retired.effect) {
                (void)retired.effect->ClearCropParams();(void)retired.service->DetachRenderEffect(retired.effect.get());
            }
        }
        (void)ClearInteractions();m_targets=std::move(pending.targets);
        m_referenceService=std::move(pending.nextReferenceService);
        m_boxWidget.SetInteractor(pending.nextInteractor);m_planeWidget.SetInteractor(pending.nextInteractor);
        m_curveWidget.SetContext(pending.nextInteractor,pending.nextRenderer);
    }
    m_pendingShader.reset();
    for(const auto& target:m_targets)if(target.service)(void)target.service->SetRenderNeeded();
    if(m_commands.GetIsEmpty())(void)SetInteraction(m_commitSource,false);
    else (void)SendNextOp();
    return true;
}

std::optional<CropOpItem> CropBridge::Impl::BuildBoxOp()
{
    CropOpItem operation;
    operation.geometryType = CropShape::Box;
    operation.removalMode = m_removalMode;

    CropVectorDouble3Array baseCenter = {};
    CropVectorDouble3Array baseSize = {};
    CropMatrixDouble16Array baseToNow = CropAlgorithm::GetIdentityMatrix();
    if (!m_boxWidget.GetCurrentWorldBox(baseCenter, baseSize, baseToNow)) {
        return std::nullopt;
    }

    vtkNew<vtkMatrix4x4> boxToInitialWorld;
    boxToInitialWorld->Identity();
    for (int axis = 0; axis < 3; ++axis) {
        boxToInitialWorld->SetElement(axis, axis, baseSize[axis] * 0.5);
        boxToInitialWorld->SetElement(axis, 3, baseCenter[axis]);
    }
    vtkNew<vtkMatrix4x4> baseToNowMatrix;
    baseToNowMatrix->DeepCopy(baseToNow.data());
    vtkNew<vtkMatrix4x4> boxToWorld;
    vtkMatrix4x4::Multiply4x4(baseToNowMatrix, boxToInitialWorld, boxToWorld);
    vtkNew<vtkMatrix4x4> worldToInput;
    const auto worldToInputValues=GetWorldToInput();
    if(!worldToInputValues)return std::nullopt;
    worldToInput->DeepCopy(worldToInputValues->data());
    vtkNew<vtkMatrix4x4> boxToInput;
    vtkMatrix4x4::Multiply4x4(worldToInput, boxToWorld, boxToInput);
    vtkMatrix4x4::DeepCopy(operation.boxToInputModelMatrix.data(), boxToInput);
    return operation;
}

std::optional<CropOpItem> CropBridge::Impl::BuildPlaneOp()
{
    CropOpItem operation;
    operation.geometryType = CropShape::Plane;
    operation.removalMode = m_removalMode;

    CropVectorDouble3Array worldOrigin = {};
    CropVectorDouble3Array worldNormal = { 0.0, 0.0, 1.0 };
    if (!m_planeWidget.GetCurrentWorldPlane(worldOrigin, worldNormal)) {
        return std::nullopt;
    }
    vtkNew<vtkMatrix4x4> worldToInput;
    const auto worldToInputValues=GetWorldToInput();
    if(!worldToInputValues)return std::nullopt;
    worldToInput->DeepCopy(worldToInputValues->data());
    const double worldPoint[4] = { worldOrigin[0], worldOrigin[1], worldOrigin[2], 1.0 };
    double inputPoint[4] = {};
    worldToInput->MultiplyPoint(worldPoint, inputPoint);
    if(!std::isfinite(inputPoint[3])||std::abs(inputPoint[3])<=kVectorTolerance)return std::nullopt;
    const double inverseW=1.0/inputPoint[3];
    operation.planeCenterInInputModel = {
        inputPoint[0] * inverseW,
        inputPoint[1] * inverseW,
        inputPoint[2] * inverseW
    };

    vtkNew<vtkMatrix4x4> inputToWorld;
    vtkMatrix4x4::Invert(worldToInput, inputToWorld);
    inputToWorld->Transpose();
    const double worldVector[4] = { worldNormal[0], worldNormal[1], worldNormal[2], 0.0 };
    double inputVector[4] = {};
    inputToWorld->MultiplyPoint(worldVector, inputVector);
    operation.planeNormalInInputModel = {
        inputVector[0], inputVector[1], inputVector[2]
    };
    if (vtkMath::Normalize(operation.planeNormalInInputModel.data()) <= kVectorTolerance) {
        return std::nullopt;
    }
    return operation;
}

bool CropBridge::Impl::GetOpSame(const CropOpItem& first,const CropOpItem& second) const
{ return CropGeometry::GetOperationsSame(first,second); }

CropBoundsDouble6Array CropBridge::Impl::GetWidgetWorldBounds() const
{
    auto bounds=GetWorldBounds();if(!m_referenceService)return {};
    double span=0;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(bounds[2*i])||!std::isfinite(bounds[2*i+1])||bounds[2*i]>bounds[2*i+1])return {};
        span=std::max(span,bounds[2*i+1]-bounds[2*i]);
    }
    const double padding=span>0?span*0.1:1;
    // Widget placement may pad a flat source, while the frozen Root AABB stays exact.
    for(int i=0;i<3;++i)if(bounds[2*i]==bounds[2*i+1]) {bounds[2*i]-=padding;bounds[2*i+1]+=padding;}
    return bounds;
}

CropBoundsDouble6Array CropBridge::Impl::GetWorldBounds() const
{
    CropBoundsDouble6Array bounds = {};
    if (!m_referenceService || !CropAlgorithm::GetInputValid(m_input)) {
        return bounds;
    }
    bounds = {
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()
    };
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<double, 3> modelPoint = {
            m_input.inputModelBounds[(corner & 1) ? 1 : 0],
            m_input.inputModelBounds[(corner & 2) ? 3 : 2],
            m_input.inputModelBounds[(corner & 4) ? 5 : 4]
        };
        const auto worldPoint =
            m_referenceService->GetWorldPosition(modelPoint);
        if (!worldPoint) {
            return {};
        }
        for (int axis = 0; axis < 3; ++axis) {
            bounds[axis * 2] = std::min(
                bounds[axis * 2], (*worldPoint)[axis]);
            bounds[axis * 2 + 1] = std::max(
                bounds[axis * 2 + 1], (*worldPoint)[axis]);
        }
    }
    return bounds;
}

std::optional<CropMatrixDouble16Array> CropBridge::Impl::GetWorldToInput() const
{
    if(!m_referenceService)return std::nullopt;
    const auto modelToWorld=m_referenceService->GetModelToWorld();
    if(!modelToWorld || !std::all_of(modelToWorld->begin(),modelToWorld->end(),[](double v){return std::isfinite(v);})
        || (*modelToWorld)[12]!=0 || (*modelToWorld)[13]!=0 || (*modelToWorld)[14]!=0 || (*modelToWorld)[15]!=1)return std::nullopt;
    vtkNew<vtkMatrix4x4> matrix;matrix->DeepCopy(modelToWorld->data());
    if(!std::isfinite(matrix->Determinant())||matrix->Determinant()==0)return std::nullopt;
    matrix->Invert();CropMatrixDouble16Array values{};vtkMatrix4x4::DeepCopy(values.data(),matrix);
    if(!std::all_of(values.begin(),values.end(),[](double v){return std::isfinite(v);}))return std::nullopt;
    return values;
}

CropMaterializationCandidate CropBridge::Impl::BuildResultFailure(
    const CropBuildParams& params,
    const CropFailure failureReason,
    const char* message) const
{
    CropMaterializationCandidate result;
    result.documentId=params.documentId;result.nodeId=params.nodeId;result.requestId=params.requestId;
    result.failureReason = failureReason;
    result.sourceRevision = params.sourceRevision;
    result.nodeCount = params.nodeCount;
    result.operations = params.operations;
    result.message = message;
    return result;
}

bool CropBridge::Impl::BuildCropResult(
    CropNodeId nodeId,
    CropCandidateCallback onComplete,CropBuildOptions options,CropRequestId requestId)
{
    if (!onComplete) {
        return false;
    }

    const auto input=m_input;
    CropBuildParams params;
    params.documentId=m_tree.GetDocumentId();params.nodeId=nodeId;params.requestId=requestId;
    params.availableRamBytes=options.availableRamBytes;params.meshTolerance=options.meshTolerance;
    params.maxCells=options.maxCells;params.maxDepth=options.maxDepth;
    if(input.data)params.sourceRevision=input.data->self;
    params.operations=m_tree.GetPath(nodeId);params.nodeCount=params.operations.size();
    if(m_buildTask||m_hasDrag) {onComplete(BuildResultFailure(params,CropFailure::Busy,"A crop result build is already running."));return false;}
    if(!m_tree.GetNode(nodeId)) {onComplete(BuildResultFailure(params,CropFailure::NodeNotFound,"The requested crop node does not exist."));return false;}
    if(nodeId==m_tree.GetRootId()) {onComplete(BuildResultFailure(params,CropFailure::NoCropOperations,"Root has no crop operations."));return false;}
    if(!CropAlgorithm::GetInputValid(input)||params.operations.empty()) {
        onComplete(BuildResultFailure(params,CropFailure::BadInput,"The frozen crop source is invalid."));return false;
    }

    const auto tableResult =
        CropAlgorithm::BuildPredicateTable(
            params.operations,
            params.nodeCount);
    if (!tableResult.isSucceeded
        || !tableResult.predicateTable) {
        onComplete(BuildResultFailure(
            params,
            CropFailure::BadInput,
            "Crop absolute predicate prefix is invalid."));
        return false;
    }
    CropShaderPayload payload;
    payload.revision = m_activePayload.revision;
    payload.sourceStamp = GetInputStamp(input);
    payload.nodeCount = params.nodeCount;
    payload.predicateTable =
        tableResult.predicateTable;
    auto isCancelled = std::make_shared<std::atomic<bool>>(false);
    auto task = m_buildRouter.BuildResultTask(
        input,
        params,
        std::move(payload),
        [isCancelled] { return isCancelled->load(std::memory_order_acquire); });
    if (!task) {
        onComplete(BuildResultFailure(
            params,
            CropFailure::VersionMismatch,
            "Crop build snapshot is inconsistent."));
        return false;
    }

    BuildTask active;
    active.isCancelled = std::move(isCancelled);
    active.result = task->get_future().share();
    active.callback = std::move(onComplete);
    active.params = std::move(params);
    active.phase = std::make_shared<std::atomic<std::uint64_t>>(1);
    try {
        active.worker = std::thread(
            [task = std::move(*task), phase = active.phase, onWork = onWorkAvailable]() mutable {
                phase->store(2, std::memory_order_release);
                task();
                phase->store(3, std::memory_order_release);
                try { if (onWork) onWork(); } catch (...) {}
            });
    }
    catch (...) {
        active.callback(BuildResultFailure(
            active.params,
            CropFailure::WorkerStartFailed,
            "Crop build worker could not start."));
        return false;
    }
    if (!active.worker.joinable()) {
        active.callback(BuildResultFailure(
            active.params,
            CropFailure::WorkerStartFailed,
            "Crop build worker is not joinable."));
        return false;
    }
    // worker 独占冻结参数，后续历史分支不会改变本次目标。
    m_buildTask=std::move(active);
    return true;
}

bool CropBridge::Impl::GetBuildTickNeeded() const
{
    return m_buildTask
        && m_buildTask->result.valid()
        && m_buildTask->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

FeatureOperationState CropBridge::Impl::GetExecutionState() const
{
    FeatureOperationState state;
    if (!m_buildTask || !m_buildTask->phase) return state;
    state.stateRevision = m_buildTask->phase->load(std::memory_order_acquire);
    state.status = state.stateRevision == 1 ? FeatureRunStatus::Preparing
        : state.stateRevision == 2 ? FeatureRunStatus::Running : FeatureRunStatus::Ready;
    if (state.status == FeatureRunStatus::Ready) {
        try {
            if (!m_buildTask->result.get().isSucceeded) state.status = FeatureRunStatus::Failed;
        }
        catch (...) { state.status = FeatureRunStatus::Failed; }
    }
    state.progress = state.status == FeatureRunStatus::Ready ? 1.0 : 0.0;
    return state;
}

FeatureOperationState CropBridge::GetExecutionState() const
{
    return m_impl ? m_impl->GetExecutionState() : FeatureOperationState{};
}

bool CropBridge::Impl::SendBuildResult()
{
    if (!GetBuildTickNeeded()) {
        return false;
    }
    auto active = std::move(*m_buildTask);
    m_buildTask.reset();
    CropMaterializationCandidate result;
    try {
        result = active.result.get();
    }
    catch (const std::exception& error) {
        result = BuildResultFailure(
            active.params,
            CropFailure::WorkerFailed,
            error.what());
    }
    catch (...) {
        result = BuildResultFailure(
            active.params,
            CropFailure::WorkerFailed,
            "Crop build worker failed with an unknown exception.");
    }
    if (active.worker.joinable()) {
        active.worker.join();
    }
    // 取消后即使 worker 刚好完成，也不能把该候选发布给已失效输入。
    if (active.isCancelled->load(std::memory_order_acquire)) {
        result = BuildResultFailure(active.params, CropFailure::Cancelled,
            "The crop build was cancelled before publication.");
        result.isCancelled = true;
    }
    if (active.callback) active.callback(std::move(result));
    return true;
}

bool CropBridge::Impl::GetShaderCommitted() const
{
    return m_activePayload.revision!=0 && m_activePayload.sourceStamp==GetInputStamp(m_input)
        && m_activePayload.nodeCount==m_activePath.size() && m_activePayload.predicateTable
        && m_activePayload.predicateTable->operationCount==m_activePath.size();
}

bool CropBridge::Impl::GetTargetsReady() const
{
    if (m_targets.empty()) {
        return false;
    }
    const RenderInputStamp inputStamp =
        GetInputStamp(m_input);
    return GetDataRevisionRefValid(inputStamp.dataRevision)
        && std::all_of(
            m_targets.begin(),
            m_targets.end(),
            [&inputStamp](const auto& target) {
                if (!target.service || !target.effect) {
                    return false;
                }
                const auto stamp =
                    target.service->GetRenderInputStamp();
                return stamp && *stamp == inputStamp;
            });
}



bool CropBridge::Impl::SetWidgetActive(const bool isActive)
{
    if(!isActive)(void)ClearInteractions();
    const bool box=m_boxWidget.SetEnabled(isActive&&m_geometryType==CropShape::Box);
    const bool plane=m_planeWidget.SetEnabled(isActive&&m_geometryType==CropShape::Plane);
    const bool curve=m_curveWidget.SetEnabled(isActive&&(m_geometryType==CropShape::Sphere||m_geometryType==CropShape::Cylinder));
    if(m_referenceService)(void)m_referenceService->SetRenderNeeded();
    return box&&plane&&curve;
}

void CropBridge::Impl::ClearShaderStage()
{
    if (m_pendingShader) {
        for (const auto& target : m_pendingShader->targets) {
            (void)target.effect->ClearCropStage(
                m_pendingShader->payload.revision);
        }
        if (m_pendingShader->isTargetRebind) {
            for (const auto& target : m_pendingShader->targets) {
                const bool isCurrentTarget = std::any_of(
                    m_targets.begin(),
                    m_targets.end(),
                    [&target](const auto& current) {
                        return current.service.get()
                                == target.service.get()
                            && current.effect.get()
                                == target.effect.get();
                    });
                if (isCurrentTarget || !target.service
                    || !target.effect) {
                    continue;
                }
                // 重绑定尚未提交时，新 target 只持有 staged/replay 资源；
                // 取消事务必须把临时 effect 一并拆除，旧 target 继续显示 committed。
                (void)target.effect->ClearCropParams();
                (void)target.service->DetachRenderEffect(
                    target.effect.get());
            }
        }
    }
    m_pendingShader.reset();
    m_hasDrag = false;
    m_dragStart.reset();
    (void)SetInteraction(m_commitSource, false);
}

void CropBridge::Impl::ClearShader()
{
    ClearShaderStage();
    for (const auto& target : m_targets) {
        if (target.effect) {
            (void)target.effect->ClearCropParams();
        }
    }
}

void CropBridge::Impl::ClearTargets()
{
    for (const auto& target : m_targets) {
        if (target.service && target.effect) {
            (void)target.service->DetachRenderEffect(
                target.effect.get());
        }
    }
    m_targets.clear();
}

bool CropBridge::Impl::ExitCrop()
{
    if (!m_isActive) {
        return false;
    }
    // 先拒绝 widget 回调，再关闭 VTK 控件；Off 期间的结束事件只能清理交互态。
    m_isActive = false;
    m_hasDrag = false;
    m_dragStart.reset();
    (void)ClearInteractions();
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    m_curveWidget.SetEnabled(false);
    // 已接纳命令继续完成；Exit 只关闭控件和模式编辑权。
    m_editNode=0;
    if(!m_commands.GetIsEmpty())(void)SetInteraction(m_commitSource,true);
    m_removalMode = CropRemovalMode::None;
    // 当前 committed 节点保持不变；这里只发布一帧，让 reference renderer
    // 在 Timer 渲染链中刷新已经关闭的 Box/Plane 控件。
    if (m_referenceService) {
        (void)m_referenceService->SetRenderNeeded();
    }
    return true;
}

bool CropBridge::Impl::SetInteraction(
    const InteractionSource& source,
    const bool isInteracting)
{
    return m_referenceService
        && m_referenceService->SetInteracting(source, isInteracting);
}

bool CropBridge::Impl::ClearInteractions()
{
    const bool isDragCleared = ClearDragSources();
    if (!m_referenceService) {
        return isDragCleared;
    }
    const bool isCommitCleared =
        m_referenceService->SetInteracting(m_commitSource, false);
    return isDragCleared && isCommitCleared;
}

bool CropBridge::Impl::ClearDragSources()
{
    if (!m_referenceService) {
        return true;
    }
    const bool isBoxCleared =
        m_referenceService->SetInteracting(m_boxSource, false);
    const bool isPlaneCleared =
        m_referenceService->SetInteracting(m_planeSource, false);
    const bool isCurveCleared=m_referenceService->SetInteracting(m_curveSource,false);
    return isBoxCleared && isPlaneCleared && isCurveCleared;
}

bool CropBridge::Impl::GetCropActive() const
{
    return m_isActive;
}

bool CropBridge::Impl::GetCropBound() const
{
    // Render input 是否仍匹配由具体 history 动作检查；Host 用结构 binding
    // 在 Exit 后继续跟踪输入换代，并及时退休旧 history。
    return m_referenceService
        && !m_targets.empty();
}

CropHistoryState CropBridge::Impl::GetCropHistory() const
{
    CropHistoryState state;
    state.nodeCount=m_activePath.size();state.operationCount=m_tree.GetNodeCount()?m_tree.GetNodeCount()-1:0;
    state.editMode=m_removalMode;state.hasEditableOp=m_editNode!=0;state.isEditing=m_isActive;state.isDragging=m_hasDrag;
    state.lastRequestId=m_lastRequestId;state.pendingRequestCount=m_commands.GetPendingCount();
    state.documentId=m_tree.GetDocumentId();state.stateRevision=m_tree.GetRevision();
    state.requestedHead=m_tree.GetRequestedHead();state.appliedHead=m_tree.GetAppliedHead();state.renderedHead=GetRenderedHead();
    return state;
}

std::optional<CropViewPreviewState> CropBridge::GetViewState(const FeatureViewService* service) const {
    return m_impl->GetOwnerReady()?m_impl->GetViewState(service):std::nullopt;
}

CropBridge::CropBridge()
    : m_impl(std::make_unique<Impl>())
{
}

CropBridge::~CropBridge() = default;

bool CropBridge::StartView(const CropViewRequest& request) { return m_impl->StartView(request); }
bool CropBridge::StartView(
    const CropViewRequest& request,
    CropInputSnapshot input)
{
    return m_impl->StartView(request, std::move(input));
}
bool CropBridge::ClearBindings() { return m_impl->ClearBindings(); }
bool CropBridge::SetCropInput(CropInputSnapshot input)
{
    return m_impl->GetOwnerReady()
        && m_impl->SetCropInput(std::move(input));
}
CropEditAdmission CropBridge::SendRequest(CropEditRequest request) { return m_impl->SendRequest(std::move(request)); }
CropHistorySnapshot CropBridge::GetHistory(CropNodeId after,std::size_t limit) const
{ return m_impl->GetOwnerReady()?m_impl->GetHistory(after,limit):CropHistorySnapshot{}; }
CropPruneImpact CropBridge::GetPruneImpact(const CropPruneRequest& request) const
{ return m_impl->GetOwnerReady()?m_impl->GetPruneImpact(request):CropPruneImpact{}; }
std::optional<CropNodeSnapshot> CropBridge::GetNode(CropNodeId node) const
{ return m_impl->GetOwnerReady()?m_impl->GetNode(node):std::nullopt; }
std::optional<CropEditOutcome> CropBridge::GetOutcome(CropRequestId id) const
{ return m_impl->GetOwnerReady()?m_impl->GetOutcome(id):std::nullopt; }
CropInputSnapshot CropBridge::GetSource() const { return m_impl->GetOwnerReady()?m_impl->GetSource():CropInputSnapshot{}; }
std::shared_ptr<RenderEffect> CropBridge::GetViewEffect(const FeatureViewService* service) const {
    return m_impl->GetOwnerReady()?m_impl->GetViewEffect(service):nullptr;
}
bool CropBridge::GetResultsValid(const std::vector<CropResultRecord>& results) const { return m_impl->GetOwnerReady()&&m_impl->GetResultsValid(results); }
void CropBridge::SetResults(std::vector<CropResultRecord>&& results) noexcept { m_impl->SetResults(std::move(results)); }
CropDocumentArchive CropBridge::GetArchive() const { return m_impl->GetOwnerReady()?m_impl->GetArchive():CropDocumentArchive{}; }
bool CropBridge::CancelPending() { return m_impl->CancelPending(); }
bool CropBridge::ClearDocument() { return m_impl->ClearDocument(); }


CropBridge::SourceCommit::SourceCommit(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
CropBridge::SourceCommit::~SourceCommit() = default;
CropBridge::SourceCommit::SourceCommit(SourceCommit&&) noexcept = default;
CropBridge::SourceCommit& CropBridge::SourceCommit::operator=(SourceCommit&&) noexcept = default;
bool CropBridge::GetSourceTransitionNeeded() const {return m_impl->GetLeaseReady()&&m_impl->GetSourceTransitionNeeded();}
std::optional<CropBridge::SourceCommit> CropBridge::BuildSourceCommit(CropNodeId nodeId,bool isQueued)
{
    auto prepared=m_impl->BuildSourceCommit(nodeId,isQueued);
    if(!prepared)return std::nullopt;
    return SourceCommit(std::move(prepared));
}
bool CropBridge::GetSourceCommitReady(const SourceCommit& prepared) const noexcept
{return prepared.m_impl && m_impl->GetSourceCommitReady(*prepared.m_impl);}
void CropBridge::SetSourceCommit(SourceCommit&& prepared) noexcept {m_impl->SetSourceCommit(std::move(prepared.m_impl));}

bool CropBridge::SwitchCropBox()
{
    return m_impl->GetLeaseReady()
        && m_impl->SwitchCrop(CropShape::Box);
}
bool CropBridge::SwitchCropPlane()
{
    return m_impl->GetLeaseReady()
        && m_impl->SwitchCrop(CropShape::Plane);
}
bool CropBridge::SetCropMode(const CropRemovalMode removalMode)
{
    return m_impl->GetLeaseReady()
        && m_impl->SetCropMode(removalMode);
}
bool CropBridge::PreviousCrop()
{
    return m_impl->GetLeaseReady() && m_impl->PreviousCrop();
}
bool CropBridge::NextCrop()
{
    return m_impl->GetLeaseReady() && m_impl->NextCrop();
}
bool CropBridge::SetCropNode(CropNodeId nodeId)
{
    return m_impl->GetLeaseReady()
        && m_impl->SetCropNode(nodeId);
}
bool CropBridge::ExitCrop()
{
    return m_impl->GetLeaseReady() && m_impl->ExitCrop();
}
bool CropBridge::GetCropActive() const
{
    return m_impl->GetLeaseReady() && m_impl->GetCropActive();
}
bool CropBridge::GetCropBound() const
{
    return m_impl->GetLeaseReady() && m_impl->GetCropBound();
}
CropHistoryState CropBridge::GetCropHistory() const
{
    return m_impl->GetOwnerReady()
        ? m_impl->GetCropHistory()
        : CropHistoryState{};
}
bool CropBridge::GetShaderTickNeeded() const
{
    return m_impl->GetLeaseReady()
        && m_impl->GetShaderTickNeeded();
}
bool CropBridge::SendShaderCommit()
{
    return m_impl->GetLeaseReady()
        && m_impl->SendShaderCommit();
}
bool CropBridge::BuildCropResult(
    CropInputSnapshot rootInput,
    CropCandidateCallback onComplete)
{
    return m_impl->GetLeaseReady()
        && rootInput.data && m_impl->GetSource().data
        && rootInput.data->self==m_impl->GetSource().data->self
        && m_impl->BuildCropResult(m_impl->GetCropHistory().appliedHead,std::move(onComplete));
}
bool CropBridge::BuildCropResult(CropNodeId nodeId,CropCandidateCallback onComplete)
{
    return m_impl->GetLeaseReady()&&m_impl->BuildCropResult(nodeId,std::move(onComplete));
}
bool CropBridge::GetBuildTickNeeded() const
{
    return m_impl->GetOwnerReady()
        && m_impl->GetBuildTickNeeded();
}
bool CropBridge::SendBuildResult()
{
    return m_impl->GetOwnerReady()
        && m_impl->SendBuildResult();
}

void CropBridge::SetWorkAvailable(std::function<void()> onWorkAvailable)
{
    m_impl->onWorkAvailable = std::move(onWorkAvailable);
}

bool CropBridge::BuildCropResult(CropNodeId nodeId,CropBuildOptions options,CropRequestId requestId,CropCandidateCallback onComplete)
{ return m_impl&&m_impl->GetLeaseReady()&&m_impl->BuildCropResult(nodeId,std::move(onComplete),options,requestId); }

void CropBridge::ForgetOutcome(CropRequestId id) { if(m_impl&&m_impl->GetOwnerReady())m_impl->ForgetOutcome(id); }

bool CropBridge::SwitchCropCylinder(){return m_impl->GetLeaseReady()&&m_impl->SwitchCrop(CropShape::Cylinder);}
bool CropBridge::SwitchCropSphere(){return m_impl->GetLeaseReady()&&m_impl->SwitchCrop(CropShape::Sphere);}

bool CropBridge::Impl::RefreshWidgetTransform()
{
    if(!GetLeaseReady()||!m_isActive||(m_geometryType!=CropShape::Sphere&&m_geometryType!=CropShape::Cylinder))return false;
    const auto matrix=m_referenceService->GetModelToWorld();
    if(matrix&&m_curveWidget.GetTransformSame(*matrix))return false;
    const auto operation=m_curveWidget.GetGeometry();const bool enabled=m_curveWidget.GetEnabled();
    m_hasDrag=false;m_dragStart.reset();m_dragParent=0;(void)SetInteraction(m_curveSource,false);
    m_curveWidget.SetEnabled(false);
    if(matrix&&m_curveWidget.SetGeometry(operation,*matrix)&&enabled)m_curveWidget.SetEnabled(true);
    (void)m_referenceService->SetRenderNeeded();return true;
}
bool CropBridge::RefreshWidgetTransform(){return m_impl&&m_impl->RefreshWidgetTransform();}

CropPreviewPrecision CropBridge::GetPreviewPrecision(const FeatureViewService* service,const std::vector<CropVectorDouble3Array>& points) const {
    return m_impl->GetOwnerReady()?m_impl->GetPreviewPrecision(service,points):CropPreviewPrecision{};
}
