#include "FeatureSupport/WorkLimit.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "../../common/FeatureResultScopes.h"
#include "../../common/FeatureInputReads.h"
#include "Data/DataPayloads.h"
#include "MappedArrayStorage.h"
#include "App/Services/FeatureViewService.h"

#include "Render/Contracts/OverlayService.h"
#include "SurfaceDeterminationService.h"
#include "SurfaceGenerationStore.h"
#include "SurfaceContracts.h"
#include "SurfaceGeometryStorage.h"
#include "MeshVtkStorage.h"
#include "SurfaceOverlayStrategy.h"

#include <vtkCellArray.h>
#include <vtkImageData.h>
#include "Render/Contracts/SlicePlaneState.h"
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "surface-determination";
constexpr std::size_t completionBatchLimit = 8;

bool GetTargetsUsed(const HostViewTargets& targets)
{
    return !targets.viewIds.empty() || !targets.viewRoles.empty();
}

bool GetRoleSupported(const HostRenderViewRole role)
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D
        || role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

bool GetConfigValid(const SurfaceDeterminationConfig& config)
{
    auto params = config.defaultStart;
    if (!GetTargetsUsed(params.targetViews)) {
        params.targetViews.viewRoles.push_back(
            HostRenderViewRole::Primary3D);
    }
    return SurfaceContract::GetInputValid(params);
}

std::array<double, 3> GetSliceNormal(const HostRenderViewRole role)
{
    const auto orientation = role == HostRenderViewRole::TopDownSlice ? Orientation::Top_down
        : role == HostRenderViewRole::FrontBackSlice ? Orientation::Front_back : Orientation::Left_right;
    return SlicePlaneState::Build(orientation, {}).worldNormal;
}
std::shared_ptr<FeatureOverlay> CreateOverlay(
    const HostRenderViewRole role, bool isPreview)
{
    if (role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D) {
        return std::make_shared<SurfaceOverlayStrategy>(isPreview);
    }
    if (role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice) {
        return std::make_shared<SurfaceSliceOverlayStrategy>(
            GetSliceNormal(role), isPreview);
    }
    return nullptr;
}

SurfaceDeterminationResult BuildResult(
    const std::uint64_t requestId,
    const SurfaceResultStatus status,
    const SurfaceFailureReason failureReason,
    const DataRevisionRef sourceRevision,
    const std::uint64_t resultRevision,
    const std::uint64_t pointCount,
    const std::uint32_t objectCount,
    std::string message)
{
    SurfaceDeterminationResult result;
    result.requestId = requestId;
    result.status = status;
    result.failureReason = failureReason;
    result.sourceRevision = sourceRevision;
    result.resultRevision = resultRevision;
    result.pointCount = pointCount;
    result.objectCount = objectCount;
    result.message = std::move(message);
    return result;
}

} // namespace

class SurfaceDeterminationHostFeature::Impl final {
public:
    explicit Impl(SurfaceDeterminationConfig config)
        : m_config(std::move(config))
    {
        m_state.isOverlayVisible = m_config.isOverlayVisible;
        m_stateBeforeRequest = m_state;
    }

    bool AttachHost(const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    SurfaceDeterminationAdmission SendRequest(
        SurfaceDeterminationRequest request,
        SurfaceDeterminationCallback onComplete);
    SurfaceDeterminationState GetState() const;
    SurfacePointDiagnostic GetPointDiagnostic(DataRevisionRef revision, std::uint64_t pointIndex) const;
    SurfaceRestoreState GetResultValidity(DataRevisionRef revision) const;
    std::vector<FeatureOperationState> GetOperationStates() const;
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetSurfaceSnapshot() const;
    std::shared_ptr<const SurfaceGenerationSnapshot> GetSurfaceSnapshot(std::string_view scope) const
        { return m_store.GetCurrentGeneration(scope); }
    std::shared_ptr<const SurfaceGenerationSnapshot> GetSurfaceSnapshot(DataRevisionRef revision) const
        { return m_store.GetGeneration(revision); }
    std::shared_ptr<const SurfaceGenerationSnapshot> GetPreviewSnapshot() const
        { const std::lock_guard<std::mutex> lock(m_stateMutex); return m_preview; }

private:
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureOverlay> overlay;
        std::string viewId;
    };

    struct ObjectJob final {
        std::uint64_t id=0;
        std::shared_ptr<const SurfaceGenerationSnapshot> generation;
        SurfaceObjectRecord object;
        VtkImageGridSnapshot source;
        std::vector<HostFeatureView> views;
        SurfaceDeterminationCallback callback;
        std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
        std::future<std::shared_ptr<const FeatureInternal::SpatialMeshSource>> future;
        ~ObjectJob(){cancelled->store(true);if(future.valid())future.wait();}
    };
    bool ClearObjectSelection();
    void ContinueObjectSelection();
    struct RequestEntry final {
        VtkImageGridSnapshot source;
        std::vector<HostFeatureView> views;
        SurfaceDeterminationCallback onComplete;
        bool isSourceChanged = false;
        DataBinding resultBinding;
        std::shared_ptr<std::atomic<bool>> completeActive;
        FeatureOperationState operation;
        SurfaceDeterminationStartParams params;
        bool isSuperseded = false;
        SurfaceAlgorithmInputs inputs;
        std::shared_ptr<const FeatureInternal::InputReads> readers;
    };

    bool GetIsOwnerThread() const noexcept;
    std::uint64_t GetNextRequestId() noexcept;
    bool GetSourceSame(const VtkImageGridSnapshot& source,
        DataPublishPolicy policy = DataPublishPolicy::RequireCurrentInputs) const;
    bool GetRequestInputsSame(const RequestEntry &request) const;
    std::size_t GetRetainedBytes() const;
    std::shared_ptr<SurfaceGenerationSnapshot> BuildGeneration(const RequestEntry &request,
                                                               SurfaceAlgorithmResult &result,
                                                               std::uint64_t requestId, bool isFormal);
    void SetTransientResult(const RequestEntry& request, SurfaceAlgorithmResult& result, std::uint64_t requestId,
        SurfacePreparedMeshes display);
    bool ClearPreview();
    std::shared_ptr<const SurfaceGenerationSnapshot> GetDisplayGeneration() const;
    bool GetRoiCurrent(const RoiReadSnapshot& roi, DataPublishPolicy policy) const;
    std::vector<HostFeatureView> GetTargetViews(
        const HostViewTargets& targets) const;
    static std::vector<std::string> GetViewIds(
        const std::vector<HostFeatureView>& views);
    void SetState(SurfaceDeterminationState state);
    void SetRequestRunning(
        std::uint64_t requestId,
        DataRevisionRef sourceRevision,
        bool isFirstRequest);
    void SetRequestProgress(const SurfaceRequestProgress& progress);
    void SendComplete(
        SurfaceDeterminationCallback callback,
        SurfaceDeterminationResult result,
        DataBindingRevision bindingRevision = 0,
        std::shared_ptr<std::atomic<bool>> requestActive = {}) const noexcept;
    bool BuildBindings(
        std::shared_ptr<FeatureInternal::SpatialMeshCache> displayData,
        const std::vector<HostFeatureView>& views,
        const VtkImageGridSnapshot& source,
        std::vector<OverlayBinding>& bindings, bool isPreview);
    static void RemoveBindings(
        std::vector<OverlayBinding>& bindings) noexcept;
    bool RemoveDisplay();
    bool SendDisplayDelta();
    bool SetVisibility(bool isVisible);
    bool SetObjectSelection(const SurfaceObjectSelectionRequest& request);
    bool SetObjectPicking(bool enabled);
    InteractionResult OnObjectInput(const InteractionEvent& event);
    bool ClearResult();
    bool ClearResultBinding();
    DataBinding GetResultBinding() const;
    DataBinding GetResultBinding(std::string_view scope) const;
    void SetBindingProjection();
    bool SetDisplayProjection();
    void SetDisplayFailure(std::uint64_t requestId = 0);
    void SetSourceStale();
    void SetRequestComplete(SurfaceJobComplete complete);
    void SetRequestFailed(
        const RequestEntry& request,
        const SurfaceAlgorithmResult& result);
    DataSnapshot SetRequestSucceeded(
        const RequestEntry& request,
        SurfaceAlgorithmResult result,
        std::uint64_t requestId,SurfacePreparedMeshes meshes);

    SurfaceDeterminationConfig m_config;
    mutable std::mutex m_stateMutex;
    SurfaceDeterminationState m_state;
    SurfaceDeterminationState m_stateBeforeRequest;
    FeatureOperationState m_lastOperation;
    FeatureOperationState m_displayOperation;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    FeatureInternal::ResultScopes m_resultScopes;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<SurfaceDeterminationService> m_service;
    SurfaceGenerationStore m_store;
    std::map<std::uint64_t, RequestEntry> m_requests;
    std::string m_activeScope;
    std::shared_ptr<const SurfaceGenerationSnapshot> m_preview;
    VtkImageGridSnapshot m_previewSource;
    std::vector<HostFeatureView> m_previewViews;
    VtkImageGridSnapshot m_activeSource;
    std::vector<HostFeatureView> m_activeViews;
    std::vector<OverlayBinding> m_bindings,m_objectBindings;
    std::unique_ptr<ObjectJob> m_objectJob;
    std::shared_ptr<const SurfaceGenerationSnapshot> m_objectGeneration;
    VtkImageGridSnapshot m_objectSource;
    FeatureOperationState m_objectOperation;
    std::uint64_t m_objectIntent=0;
    std::shared_ptr<FeatureInternal::SpatialMeshCache> m_displayData;
    std::shared_ptr<const FeatureInternal::SpatialMeshSource> m_previewDisplay;
    std::string m_previewDisplayError,m_displayPrepareError;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextRequestId = 1;
    std::uint64_t m_latestRequestId = 0;
    std::uint64_t m_resultRevision = 0;
    DataBinding m_resultBinding;
    bool m_isDisplayPending = false;
    bool m_isAttached = false;
    bool m_isInputAttached=false;
    bool m_isClosing=false,m_pickOwnerActive=false;
    bool m_isDetaching = false;
    bool m_isStaleCleanupPending = false;
    std::shared_ptr<std::atomic<bool>> m_completeActive;
};

bool SurfaceDeterminationHostFeature::Impl::AttachHost(
    const HostFeatureContext& context)
{
    if (m_isAttached || !context.views || !context.data || !context.host
        || !GetConfigValid(m_config)) {
        return false;
    }
    const auto graph = context.data->GetDataGraph();
    if (!graph.view || (graph.view->GetDataFacets(surfaceGenerationType).empty()
        && !context.data->SetDataType(DataTypeDescriptor{
            surfaceGenerationType, { surfaceGenerationFacet },
            [](const IDataPayload& value, std::string&) {
                const auto* payload = dynamic_cast<const SurfaceGenerationPayload*>(&value);
                const auto generation = payload ? payload->GetGeneration() : nullptr;
                return generation && generation->points && generation->triangleIndices
                    && generation->objects && GetDataRevisionRefValid(generation->sourceRevision);
            } }))) return false;
    try {
        m_service = std::make_unique<SurfaceDeterminationService>(
            [weakHost = std::weak_ptr<FeatureHostControl>(context.host)] {
                if (const auto host = weakHost.lock())
                    (void)host->SendWorkAvailable();
            });
    }
    catch (...) {
        return false;
    }
    m_views = context.views;
    m_data = context.data;
    m_store.SetDataPort(m_data);
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
    m_completeActive = std::make_shared<std::atomic<bool>>(true);
    m_lastOperation = {};
    m_displayOperation = {};
    m_isAttached = true;m_isClosing=false;m_pickOwnerActive=false;
    HostInputBinding input;input.featureId=std::string(featureId);
    input.targetViews.viewRoles={HostRenderViewRole::Primary3D,HostRenderViewRole::Composite3D,
        HostRenderViewRole::TopDownSlice,HostRenderViewRole::FrontBackSlice,HostRenderViewRole::LeftRightSlice};
    InteractionActionInfo select;select.actionId="surface.object.select";select.ownerId=input.featureId;select.isCurrentToolCommand=true;
    InputBinding pointer{InputTriggerKind::PointerPress,InputMouseButton::Primary};pointer.requiredModifiers=2;pointer.forbiddenModifiers=5;
    select.bindings.push_back({std::string(SurfaceObjectBindingKeys::Select),pointer,true,{},input.featureId,{InputTriggerKind::PointerPress}});
    input.actions.push_back(std::move(select));
    const std::weak_ptr<std::atomic<bool>> active=m_completeActive;
    input.onAction=[this,active](std::string_view,const InteractionEvent& event,const std::optional<HostSemanticTarget>&){
        const auto token=active.lock();return token&&token->load()?OnObjectInput(event):InteractionResult{};};
    input.getIsActionAvailable=[this,active](std::string_view,const InteractionEvent&){
        const auto token=active.lock();if(!token||!token->load())return false;
        const auto state=GetState();return state.isObjectPickingEnabled&&state.isOverlayVisible&&state.stage==SurfaceDeterminationStage::Ready;};
    input.getIsCaptureActive=[](std::string_view){return false;};
    if(!m_host->AttachInput(std::move(input))) {
        m_completeActive->store(false);m_isAttached=false;m_service.reset();m_views.reset();m_data.reset();m_host.reset();m_store.SetDataPort({});return false;
    }
    m_isInputAttached=true;
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) return true;
    if (!GetIsOwnerThread() || !m_service || m_isDetaching) return false;
    m_isDetaching = true;
    m_isClosing=true;
    if (m_completeActive) m_completeActive->store(false);
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.stage = SurfaceDeterminationStage::Stopping;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // 停止已开始后保持 closing；失败只保留挂载身份供重试解绑，读队列不再接纳工作。
    const bool stopped=m_service->Stop(deadline);
    const bool inputStopped=SetObjectPicking(false);
    if (!stopped || !inputStopped || !ClearResult()) {
        m_isDetaching = false;
        return false;
    }
    if(m_isInputAttached&&!m_host->DetachInput(featureId)){m_isDetaching=false;return false;}
    m_isInputAttached=false;
    // 先完成解绑并取走回调，用户代码才可重入 Detach/Attach；不迭代可重入的成员 map。
    auto requests = std::move(m_requests);
    m_requests.clear();
    const auto resultRevision = m_resultRevision;
    m_latestRequestId = 0;
    m_activeSource.reset(); m_activeViews.clear();
    m_service.reset(); m_views.reset(); m_data.reset();
    m_store.SetDataPort({}); m_host.reset(); m_ownerThread = {};
    m_isAttached = false; m_isDetaching = false; m_isStaleCleanupPending = false;
    SurfaceDeterminationState idle;
    idle.isOverlayVisible = m_config.isOverlayVisible;
    SetState(std::move(idle));
    for (auto& item : requests) {
        auto& request = item.second;
        auto result = BuildResult(item.first, SurfaceResultStatus::Cancelled,
            SurfaceFailureReason::Cancelled, request.source && request.source->data
                ? request.source->data->self : DataRevisionRef{}, resultRevision, 0, 0,
            "Surface request was cancelled by detach.");
        result.purpose = SurfaceContract::GetPurpose(request.params);
        result.resultScope = request.params.resultScope;
        SendComplete(std::move(request.onComplete), std::move(result));
    }
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached || m_isDetaching || m_isClosing || !GetIsOwnerThread() || !m_service || !m_data) {
        return false;
    }
    if (m_isStaleCleanupPending && RemoveDisplay()) {
        m_isStaleCleanupPending = false;
    }
    if (m_previewSource && (!GetSourceSame(m_previewSource) ||
                            (m_preview && !SurfaceContract::GetInputsCurrent(*m_data, m_data->GetDataGraph(),
                                                                             m_preview->inputs))))
        (void)ClearPreview();
    const auto activeGeneration = m_store.GetGeneration();
    if (m_activeSource &&
        (!GetSourceSame(m_activeSource) ||
         (activeGeneration &&
          !SurfaceContract::GetInputsCurrent(*m_data, m_data->GetDataGraph(), activeGeneration->inputs))))
    {
        SetSourceStale();
    }
    SetBindingProjection();
    ContinueObjectSelection();
    if(!m_isAttached || !m_service)return false;
    for (auto& item : m_requests) {
        RequestEntry& request = item.second;
        if (!request.isSourceChanged && !GetRequestInputsSame(request))
        {
            request.isSourceChanged = true;
            m_service->StopRequest(item.first);
            // 完成时只更新该请求；不能清除另一个来源/作用域的正式结果。
        }
    }
    if (m_latestRequestId != 0) {
        const auto progress = m_service->GetProgress(m_latestRequestId);
        if (progress) SetRequestProgress(*progress);
    }
    for (std::size_t index = 0;
        index < completionBatchLimit; ++index) {
        if (!m_isAttached || !m_service) break;
        auto complete = m_service->GetComplete();
        if (!complete) break;
        SetRequestComplete(std::move(*complete));
    }
    bool changed=false,displayComplete=!m_isDisplayPending,displayFailed=m_isDisplayPending&&!m_displayPrepareError.empty();
    std::uint64_t requested=0,loaded=0;std::string displayMessage=m_displayPrepareError;
    for(auto& binding:m_bindings) {
        FeatureInternal::SpatialMeshView* view=nullptr;
        if(auto mesh=std::dynamic_pointer_cast<SurfaceOverlayStrategy>(binding.overlay))view=&mesh->GetDisplay();
        else if(auto slice=std::dynamic_pointer_cast<SurfaceSliceOverlayStrategy>(binding.overlay))view=&slice->GetDisplay();
        if(!view)continue;
        try {changed=view->Refresh()||changed;}catch(const std::exception& error){displayComplete=false;displayFailed=true;displayMessage=error.what();}
        const auto& progress=view->GetProgress();requested+=progress.requested;loaded+=progress.loaded;
        displayComplete=displayComplete&&progress.isComplete;displayFailed=displayFailed||progress.hasFailure;
        if(!progress.message.empty())displayMessage=progress.message;
    }
    {const std::lock_guard<std::mutex> lock(m_stateMutex);m_state.isDisplayComplete=displayComplete;m_state.hasDisplayFailure=displayFailed;
        m_state.displayRequestedBatches=requested;m_state.displayLoadedBatches=loaded;m_state.displayMessage=std::move(displayMessage);}
    for(auto& binding:m_objectBindings) {
        if(auto mesh=std::dynamic_pointer_cast<SurfaceOverlayStrategy>(binding.overlay))changed=mesh->GetDisplay().Refresh()||changed;
        else if(auto slice=std::dynamic_pointer_cast<SurfaceSliceOverlayStrategy>(binding.overlay))changed=slice->GetDisplay().Refresh()||changed;
    }
    if(changed)(void)SendDisplayDelta();
    return true;
}

SurfaceDeterminationAdmission
SurfaceDeterminationHostFeature::Impl::SendRequest(
    SurfaceDeterminationRequest request,
    SurfaceDeterminationCallback onComplete)
{
    SurfaceDeterminationAdmission admission;
    if (!m_isAttached || !GetIsOwnerThread() || !m_service) {
        admission.status = SurfaceAdmissionStatus::Unavailable;
        return admission;
    }
    if (m_isDetaching||m_isClosing) {
        admission.status = SurfaceAdmissionStatus::Stopping;
        return admission;
    }
    SetBindingProjection();

    if(const auto* select=std::get_if<SurfaceObjectSelectionRequest>(&request)) {
        const auto displayed=GetDisplayGeneration();
        if(!GetTargetsUsed(select->targetViews)&&displayed&&displayed->dataRevision==select->generation) {
            if(!ClearObjectSelection()||!SetObjectSelection(*select))return admission;
            const auto id=GetNextRequestId();const auto state=GetState();
            auto result=BuildResult(id,SurfaceResultStatus::Succeeded,SurfaceFailureReason::None,state.sourceRevision,
                state.resultRevision,state.pointCount,state.objectCount,"Surface object selection updated.");
            result.dataRevision=select->generation;SendComplete(std::move(onComplete),std::move(result));
            return {SurfaceAdmissionStatus::Accepted,id};
        }
        if(!select->objectIndex) {
            const auto id=GetNextRequestId();if(!ClearObjectSelection())return admission;
            admission={SurfaceAdmissionStatus::Accepted,id};
            SendComplete(std::move(onComplete),BuildResult(id,SurfaceResultStatus::Succeeded,SurfaceFailureReason::None,{},m_resultRevision,0,0,"Surface object highlight cleared."));
            return admission;
        }
        if(!GetTargetsUsed(select->targetViews))return admission;
        const auto generation=m_store.GetGeneration(select->generation);
        if(!generation || !generation->objects || *select->objectIndex>=generation->objects->size())return admission;
        const auto object=(*generation->objects)[*select->objectIndex];if(object.objectIndex!=*select->objectIndex)return admission;
        const auto source=m_data->GetImageGrid(m_data->GetDataGraph(),generation->sourceRevision);
        const auto primary=m_data->GetPrimaryImage();
        if(!primary || !primary->data || primary->data->self!=generation->sourceRevision || !source || !GetSourceSame(source)){admission.status=SurfaceAdmissionStatus::Unavailable;return admission;}
        auto targets=select->targetViews;if(!GetTargetsUsed(targets))targets.viewRoles={HostRenderViewRole::Primary3D};
        auto views=GetTargetViews(targets);if(views.empty())return admission;
        const auto intention=m_objectIntent+1;
        if(!ClearObjectSelection())return admission;
        // 清除旧高亮的回调可能解绑；重新核对，不能在重入后使用失效的 Host。
        if(m_objectIntent!=intention || !m_isAttached || m_isDetaching || !m_host)return admission;
        auto job=std::make_unique<ObjectJob>();job->id=GetNextRequestId();job->generation=generation;
        job->object=object;job->source=source;job->views=std::move(views);job->callback=std::move(onComplete);
        const auto cancelled=job->cancelled;
        try {job->future=std::async(std::launch::async,[generation,object,cancelled,weakHost=std::weak_ptr<FeatureHostControl>(m_host)]{
            struct Notify { std::weak_ptr<FeatureHostControl> host; ~Notify(){if(auto live=host.lock())(void)live->SendWorkAvailable();} } notify{weakHost};
            const auto stopped=[cancelled]{return cancelled->load();};
            const auto mesh=BuildSurfaceObjectMesh(*generation,object,stopped);
            return mesh?FeatureInternal::SpatialMeshSource::Build(mesh,stopped):nullptr;
        });}catch(...){admission.status=SurfaceAdmissionStatus::Unavailable;return admission;}
        admission={SurfaceAdmissionStatus::Accepted,job->id};
        m_objectOperation={};m_objectOperation.operation={std::string(featureId),m_host->GetAttachmentId(),job->id};
        m_objectOperation.status=FeatureRunStatus::Running;m_objectOperation.stateRevision=1;
        m_objectOperation.inputs={{"source-volume",generation->sourceRevision},{"generation",generation->dataRevision},{"mesh",generation->meshRevision}};
        m_objectJob=std::move(job);(void)m_host->SendWorkAvailable();return admission;
    }
    if (const auto* start = std::get_if<SurfaceStartRequest>(&request)) {
        auto params = start->start.value_or(m_config.defaultStart);
        if (params.businessMode != SurfaceBusinessMode::Production) {
            admission.status = SurfaceAdmissionStatus::UnsupportedMode;
            return admission;
        }
        if (!SurfaceContract::GetInputValid(params)) return admission;
        auto targetViews = GetTargetViews(params.targetViews);
        if (GetTargetsUsed(params.targetViews) && targetViews.empty()) return admission;
        if (params.sourcePolicy == DataPublishPolicy::AllowHistoricalResult
            && (!params.sourceVolume || GetTargetsUsed(params.targetViews)
                || SurfaceContract::GetPurpose(params) != SurfaceTaskPurpose::Determine)) return admission;
        auto source = m_data ? (params.sourceVolume
            ? m_data->GetImageGrid(m_data->GetDataGraph(), *params.sourceVolume)
            : m_data->GetPrimaryImage()) : nullptr;
        if (!source || !source->image || !source->data
            || !GetSourceSame(source, params.sourcePolicy)) {
            admission.status = SurfaceAdmissionStatus::Unavailable;
            return admission;
        }
        const auto graph = m_data->GetDataGraph();
        SurfaceAlgorithmInputs inputs;
        if (params.analysisRoi) {
            const auto resolved = m_data->GetRoi(graph, *params.analysisRoi, source->data->self);
            if (resolved.error != RoiError::None || !resolved.roi) return admission;
            inputs.roi = resolved.roi;
            if (!GetRoiCurrent(inputs.roi, params.sourcePolicy)) return admission;
        }
        if(params.parentGeneration){inputs.parent=m_store.GetGeneration(*params.parentGeneration);
            const auto data=m_data->GetData(graph,*params.parentGeneration);
            const auto* payload=data?dynamic_cast<const SurfaceGenerationPayload*>(data->payload.get()):nullptr;
            if(payload)inputs.parentStorage=payload->GetStorage();
            if(!inputs.parent || inputs.parent->sourceRevision!=source->data->self || inputs.parent->algorithmRevision!=surfaceAlgorithmRevision
                || SurfaceRecipeCodec::BuildText(params)!=SurfaceRecipeCodec::BuildText(inputs.parent->requestedParams))return admission;
            const auto parent=m_data->GetData(graph,inputs.parent->dataRevision);
            const auto mesh=m_data->GetData(graph,inputs.parent->meshRevision);
            const auto* parentPayload=parent?dynamic_cast<const SurfaceGenerationPayload*>(parent->payload.get()):nullptr;
            if(parentPayload)inputs.parentDisplay=parentPayload->GetDisplaySource();
            inputs.parentMesh=mesh?std::dynamic_pointer_cast<const SurfaceMeshPayload>(mesh->payload):nullptr;
            if(!inputs.parentMesh)return admission;
        }
        if (SurfaceDeterminationAlgorithm::GetInputFailure(source, params, inputs) !=
            SurfaceFailureReason::None)
            return admission;
        params.sourceVolume = source->data->self;
        params.purpose = SurfaceContract::GetPurpose(params);
        const std::uint64_t requestId = GetNextRequestId();
        const bool isFirstRequest = m_requests.empty();
        auto requestItem = m_requests.end();
        try {
            std::vector<DataRevisionRef> readRefs{source->data->self};
            if(inputs.parent){readRefs.push_back(inputs.parent->dataRevision);readRefs.push_back(inputs.parent->meshRevision);}
            if (inputs.roi) {
                readRefs.push_back(inputs.roi->GetRevision());
                const auto dependencies=inputs.roi->GetDependencies();
                readRefs.insert(readRefs.end(),dependencies.begin(),dependencies.end());
            }
            auto readers=FeatureInternal::StartInputReads(graph,readRefs,"surface-determination-reader");
            if (!readers) return admission;
            const auto inputReads=std::make_shared<const FeatureInternal::InputReads>(std::move(*readers));
            struct ReadSource { VtkImageGridSnapshot source; std::shared_ptr<const FeatureInternal::InputReads> readers; };
            const auto readSource=std::make_shared<const ReadSource>(ReadSource{source,inputReads});
            const VtkImageGridSnapshot workerSource(readSource,source.get());
            const auto inserted = m_requests.emplace(
                requestId,
                RequestEntry{
                    source,
                    targetViews,
                    std::move(onComplete),
                    false,
                    GetResultBinding(params.resultScope),
                    m_completeActive
                });
            if (!inserted.second) return admission;
            requestItem = inserted.first;
            requestItem->second.params = params;
            requestItem->second.inputs = inputs;
            requestItem->second.readers = inputReads;
            admission.status = m_service->Start(workerSource, params, WorkLimit(m_config.maxWorkingBytes), requestId, inputs);
        }
        catch (...) {
            if (requestItem != m_requests.end()) {
                m_requests.erase(requestItem);
            }
            admission.status = SurfaceAdmissionStatus::Unavailable;
            return admission;
        }
        if (admission.status != SurfaceAdmissionStatus::Accepted) {
            m_requests.erase(requestItem);
            return admission;
        }
        admission.requestId = requestId;
        for (auto& item : m_requests) if (item.first != requestId
            && item.second.params.resultScope == params.resultScope
            && item.second.params.purpose == params.purpose) item.second.isSuperseded = true;
        requestItem->second.operation.operation = {
            std::string(featureId), m_host->GetAttachmentId(), requestId };
        requestItem->second.operation.inputs = { { "source-volume", source->data->self } };
        if (inputs.roi) {
            auto& operationInputs = requestItem->second.operation.inputs;
            operationInputs.push_back({"analysis-roi", inputs.roi->GetRevision()});
            std::size_t index = 0;
            for (const auto& ref : inputs.roi->GetDependencies())
                if (ref != inputs.roi->GetRevision() && ref != source->data->self)
                    operationInputs.push_back({"analysis-roi.input-" + std::to_string(index++), ref});
        }
        if(inputs.parent){requestItem->second.operation.inputs.push_back({"measurement-parent",inputs.parent->dataRevision});requestItem->second.operation.inputs.push_back({"measurement-parent.mesh",inputs.parent->meshRevision});}
        requestItem->second.operation.status = FeatureRunStatus::Preparing;
        requestItem->second.operation.stateRevision = 1;
        m_latestRequestId = requestId;
        SetRequestRunning(
            requestId,
            requestItem->second.source->data->self,
            isFirstRequest);
        return admission;
    }

    if (const auto* stop = std::get_if<SurfaceStopRequest>(&request)) {
        const std::uint64_t targetRequestId = stop->targetRequestId == 0
            ? m_latestRequestId : stop->targetRequestId;
        if (targetRequestId == 0
            || m_requests.find(targetRequestId) == m_requests.end()) {
            return admission;
        }
        if (!m_service->StopRequest(targetRequestId)) return admission;
        const std::uint64_t controlRequestId = GetNextRequestId();
        admission = { SurfaceAdmissionStatus::Accepted, controlRequestId };
        if (targetRequestId == m_latestRequestId) {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            m_state.stage = SurfaceDeterminationStage::Stopping;
        }
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                controlRequestId,
                SurfaceResultStatus::Succeeded,
                SurfaceFailureReason::None,
                state.sourceRevision,
                m_resultRevision,
                state.pointCount,
                state.objectCount,
                "Surface stop was requested."));
        return admission;
    }

    if(const auto* mode=std::get_if<SurfaceObjectPickModeRequest>(&request)) {
        if(!SetObjectPicking(mode->isEnabled))return admission;
        const auto id=GetNextRequestId();const auto state=GetState();
        SendComplete(std::move(onComplete),BuildResult(id,SurfaceResultStatus::Succeeded,SurfaceFailureReason::None,
            state.sourceRevision,state.resultRevision,state.pointCount,state.objectCount,"Surface object picking mode updated."));return {SurfaceAdmissionStatus::Accepted,id};
    }
    if (m_service->GetIsBusy() || !m_requests.empty()) {
        admission.status = SurfaceAdmissionStatus::Busy;
        return admission;
    }
    const std::uint64_t requestId = GetNextRequestId();
    const auto* visibility = std::get_if<SurfaceVisibilityRequest>(&request);
    if (visibility && SetVisibility(visibility->isVisible)) {
        admission = { SurfaceAdmissionStatus::Accepted, requestId };
    }
    else if (std::holds_alternative<SurfaceClearPreviewRequest>(request) && ClearPreview()) {
        admission = { SurfaceAdmissionStatus::Accepted, requestId };
    }
    else if (std::holds_alternative<SurfaceClearRequest>(request) && ClearResult()) {
        admission = { SurfaceAdmissionStatus::Accepted, requestId };
    }
    if (admission.status == SurfaceAdmissionStatus::Accepted) {
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                requestId,
                SurfaceResultStatus::Succeeded,
                SurfaceFailureReason::None,
                state.sourceRevision,
                state.resultRevision,
                state.pointCount,
                state.objectCount,
                "Surface control request succeeded."));
    }
    return admission;
}

SurfaceDeterminationState
SurfaceDeterminationHostFeature::Impl::GetState() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state;
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::Impl::GetSurfaceSnapshot() const
{
    return m_store.GetCurrentGeneration();
}

bool SurfaceDeterminationHostFeature::Impl::GetIsOwnerThread() const noexcept
{
    return m_ownerThread == std::this_thread::get_id();
}

std::uint64_t
SurfaceDeterminationHostFeature::Impl::GetNextRequestId() noexcept
{
    const std::uint64_t requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    return requestId == 0 ? m_nextRequestId++ : requestId;
}

bool SurfaceDeterminationHostFeature::Impl::GetRoiCurrent(const RoiReadSnapshot& roi, const DataPublishPolicy policy) const
{
    if (!roi) return true;
    if (!m_data) return false;
    const auto graph=m_data->GetDataGraph();
    auto dependencies = roi->GetDependencies();
    dependencies.push_back(roi->GetRevision());
    for (const auto& ref:dependencies) {
        if (!m_data->GetData(graph, ref)) return false;

    }
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::GetSourceSame(
    const VtkImageGridSnapshot& source, const DataPublishPolicy policy) const
{
    if (!m_data || !source || !source->data) return false;
    const auto graph = m_data->GetDataGraph();
    if (policy == DataPublishPolicy::AllowHistoricalResult)
        return bool(m_data->GetData(graph, source->data->self));
    return SurfaceContract::GetSourceCurrent(*m_data, graph, source->data->self, source->binding);
}

std::size_t SurfaceDeterminationHostFeature::Impl::GetRetainedBytes() const
{
    std::size_t bytes = 0;
    const auto active = m_store.GetGeneration();
    for (const auto &generation : {active, m_preview})
        if (generation)
        {
            const auto add = [&](std::size_t count, std::size_t width) {
                if (count > (SIZE_MAX - bytes) / width)
                {
                    bytes = SIZE_MAX;
                    return;
                }
                bytes += count * width;
            };
            if (generation->points)
                add(generation->points->GetResidentBytes(),1);
            if (generation->triangleIndices)
                add(generation->triangleIndices->GetResidentBytes(),1);
            if (generation->triangleValidity)
                add(generation->triangleValidity->GetResidentBytes(),1);
            if (generation->objects)
                add(generation->objects->GetResidentBytes(),1);
            if(generation->selection)add(generation->selection->GetResidentBytes(),1);
            // VTK 连接数组另计固定映射窗口，属性与点共享正式文件来源。
            add(8*1024*1024,1);
        }
    return bytes;
}

bool SurfaceDeterminationHostFeature::Impl::GetRequestInputsSame(const RequestEntry &request) const
{
    if (!GetSourceSame(request.source, request.params.sourcePolicy)
        || !GetRoiCurrent(request.inputs.roi, request.params.sourcePolicy))
        return false;
    if(request.inputs.parent){const auto graph=m_data->GetDataGraph();if(!m_data->GetData(graph,request.inputs.parent->dataRevision) || !m_data->GetData(graph,request.inputs.parent->meshRevision))return false;}
    return true;
}

SurfacePointDiagnostic SurfaceDeterminationHostFeature::Impl::GetPointDiagnostic(
    const DataRevisionRef revision, const std::uint64_t pointIndex) const
{
    const auto generation = m_store.GetGeneration(revision);
    if (!m_data || !generation || !generation->points || pointIndex >= generation->points->size())
    {
        SurfacePointDiagnostic result;
        result.message = "Surface generation or point is unavailable.";
        return result;
    }
    if (generation->algorithmRevision != surfaceAlgorithmRevision)
    {
        SurfacePointDiagnostic result;
        result.message = "Surface algorithm revision is incompatible with diagnostic replay.";
        return result;
    }
    const auto graph = m_data->GetDataGraph();
    SurfaceAlgorithmInputs inputs;
    if (generation->resolvedParams.analysisRoi)
        inputs.roi = m_data->GetRoi(graph, *generation->resolvedParams.analysisRoi,
                                  generation->sourceRevision).roi;
    return SurfaceDeterminationAlgorithm::GetPointDiagnostic(
        m_data->GetImageGrid(graph, generation->sourceRevision), generation->resolvedParams,
        (*generation->points)[static_cast<std::size_t>(pointIndex)], inputs);
}

SurfaceRestoreState SurfaceDeterminationHostFeature::Impl::GetResultValidity(
    const DataRevisionRef revision) const
{
    SurfaceRestoreState state;
    const auto generation = m_store.GetGeneration(revision);
    if (!m_data || !generation)
    {
        state.message = "Surface result is unavailable.";
        return state;
    }
    state.inputs = generation->inputs;
    const auto graph = m_data->GetDataGraph();
    const auto mesh = m_data->GetData(graph, generation->meshRevision);
    const auto *payload = mesh ? dynamic_cast<const SurfaceMeshPayload *>(mesh->payload.get()) : nullptr;
    state.canDisplay = payload && payload->GetValid();
    if (!state.canDisplay)
    {
        state.message = "Surface mesh is unavailable.";
        return state;
    }
    if (generation->algorithmRevision != surfaceAlgorithmRevision ||
        !SurfaceRecipeCodec::GetError(generation->resolvedParams).empty())
    {
        state.status = SurfaceRestoreStatus::IncompatibleRecipe;
        state.message = "Surface recipe requires explicit version migration.";
        return state;
    }
    if (!m_data->GetData(graph, generation->sourceRevision))
    {
        state.status = SurfaceRestoreStatus::MissingInput;
        state.message = "The frozen source volume is unavailable.";
        return state;
    }
    for (const auto &input : state.inputs)
        if (!m_data->GetData(graph, input.source))
        {
            state.status = SurfaceRestoreStatus::MissingInput;
            state.message = "A frozen surface input is unavailable.";
            return state;
        }
    SurfaceAlgorithmInputs inputs;
    if (generation->resolvedParams.analysisRoi)
        inputs.roi = m_data->GetRoi(graph, *generation->resolvedParams.analysisRoi,
                                  generation->sourceRevision).roi;
    if (generation->resolvedParams.analysisRoi && !inputs.roi)
    {
        state.status = SurfaceRestoreStatus::MissingInput;
        state.message = "A recipe input is unavailable.";
        return state;
    }
    const auto source = m_data->GetImageGrid(graph, generation->sourceRevision);
    const auto *image = source && source->data
                            ? dynamic_cast<const ImageGrid3DPayload *>(source->data->payload.get())
                            : nullptr;
    if (!image || image->GetGeometry().coordinateFrame != generation->coordinateFrame ||
        payload->GetCoordinateFrame() != generation->coordinateFrame ||
        generation->resolvedParams.sourceVolume != generation->sourceRevision ||
        SurfaceDeterminationAlgorithm::GetInputFailure(source, generation->resolvedParams, inputs) !=
            SurfaceFailureReason::None)
    {
        state.status = SurfaceRestoreStatus::IncompatibleRecipe;
        state.message = "Surface recipe references, input geometry or coordinate frame are inconsistent.";
        return state;
    }
    state.canRecompute = true;
    const bool current = SurfaceContract::GetSourceCurrent(*m_data, graph, generation->sourceRevision,
                                                           generation->sourceBinding) &&
                         SurfaceContract::GetInputsCurrent(*m_data, graph, state.inputs);
    state.status = current ? SurfaceRestoreStatus::Current : SurfaceRestoreStatus::Historical;
    state.canMeasure =
        current && generation->points &&
        std::any_of(generation->points->begin(), generation->points->end(), [&](const auto &point) {
            return SurfaceContract::GetPointValid(point, generation->method);
        });
    state.message = current ? "Surface inputs are current; measurement requires valid point and face quality."
                            : "Surface inputs are historical; recomputation uses explicit frozen inputs.";
    return state;
}

std::vector<HostFeatureView>
SurfaceDeterminationHostFeature::Impl::GetTargetViews(
    const HostViewTargets& targets) const
{
    auto views = m_views
        ? m_views->GetViews(targets)
        : std::vector<HostFeatureView>{};
    if (views.empty()
        || !std::all_of(
            views.begin(), views.end(),
            [](const HostFeatureView& view) {
                return !view.id.empty() && GetRoleSupported(view.role);
            })) {
        return {};
    }
    std::sort(views.begin(), views.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    views.erase(
        std::unique(
            views.begin(), views.end(),
            [](const auto& left, const auto& right) {
                return left.id == right.id;
            }),
        views.end());
    return views;
}

std::vector<std::string>
SurfaceDeterminationHostFeature::Impl::GetViewIds(
    const std::vector<HostFeatureView>& views)
{
    std::vector<std::string> viewIds;
    viewIds.reserve(views.size());
    for (const HostFeatureView& view : views) viewIds.push_back(view.id);
    return viewIds;
}

void SurfaceDeterminationHostFeature::Impl::SetState(
    SurfaceDeterminationState state)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = std::move(state);
}

void SurfaceDeterminationHostFeature::Impl::SetRequestRunning(
    const std::uint64_t requestId,
    const DataRevisionRef sourceRevision,
    const bool isFirstRequest)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    (void)isFirstRequest; // 只在正式结果采用/绑定投影时更新恢复状态。
    m_state.stage = SurfaceDeterminationStage::Preparing;
    m_state.failureReason = SurfaceFailureReason::None;
    m_state.requestId = requestId;
    m_state.sourceRevision = sourceRevision;
    m_state.progress01 = 0.0;
    m_state.errorMessage.clear();
}

void SurfaceDeterminationHostFeature::Impl::SetRequestProgress(
    const SurfaceRequestProgress& progress)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_state.requestId != m_latestRequestId) return;
    m_state.stage = progress.stage;
    m_state.progress01 = std::min(progress.progress01, 0.999);
}

void SurfaceDeterminationHostFeature::Impl::SendComplete(
    SurfaceDeterminationCallback callback,
    SurfaceDeterminationResult result,
    const DataBindingRevision bindingRevision,
    std::shared_ptr<std::atomic<bool>> requestActive) const noexcept
{
    if (!callback) return;
    try {
        const auto active = requestActive ? std::move(requestActive) : m_completeActive;
        const std::weak_ptr<TrustedDataPort> data = m_data;
        const auto pending = std::make_shared<SurfaceDeterminationResult>(std::move(result));
        const auto once = std::make_shared<std::atomic<bool>>(false);
        const auto send = [active, data, pending, once, bindingRevision, callback = std::move(callback)]() mutable {
            if (once->exchange(true)) return;
            if (pending->status == SurfaceResultStatus::Succeeded) {
                if (!active || !active->load()) {
                    pending->status = SurfaceResultStatus::Cancelled;
                    pending->failureReason = SurfaceFailureReason::Cancelled;
                }
                else if (!pending->isPublished && GetDataRevisionRefValid(pending->sourceRevision)) {
                    const auto port = data.lock();
                    const auto graph = port ? port->GetDataGraph() : DataGraphSnapshot{};
                    const auto binding = port && bindingRevision != 0
                        ? port->GetDataBinding(graph, primaryVolumeBinding) : std::optional<DataBinding>{};
                    if (!port || !SurfaceContract::GetSourceCurrent(*port, graph, pending->sourceRevision)
                        || (bindingRevision != 0 && (!binding || binding->revision != bindingRevision
                            || binding->target != pending->sourceRevision))) {
                        pending->status = SurfaceResultStatus::Failed;
                        pending->failureReason = SurfaceFailureReason::SourceChanged;
                    }
                }
            }
            try { callback(std::move(*pending)); } catch (...) {}
        };
        if (!m_host || !m_host->SendOwnerComplete(send)) {
            if (pending->status == SurfaceResultStatus::Succeeded) {
                pending->status = SurfaceResultStatus::Cancelled;
                pending->failureReason = SurfaceFailureReason::Cancelled;
            }
            auto fallback = send;
            fallback();
        }
    }
    catch (...) {
    }
}

bool SurfaceDeterminationHostFeature::Impl::BuildBindings(
    std::shared_ptr<FeatureInternal::SpatialMeshCache> displayData,
    const std::vector<HostFeatureView>& views,
    const VtkImageGridSnapshot& source,
    std::vector<OverlayBinding>& bindings, bool isPreview)
{
    if (!displayData || !source || !source->image || !m_views) return false;
    bindings.reserve(views.size());
    for (const HostFeatureView& view : views) {
        auto service = m_views->GetOverlayPort(view.id);
        auto overlay = service
            ? CreateOverlay(view.role, isPreview) : nullptr;
        if (!service || !overlay) {
            RemoveBindings(bindings);
            return false;
        }
        const std::weak_ptr<FeatureHostControl> weakHost=m_host;
        auto notify=[weakHost]{if(const auto host=weakHost.lock())host->SendWorkAvailable();};
        if(auto mesh=std::dynamic_pointer_cast<SurfaceOverlayStrategy>(overlay))mesh->SetDisplayCache(displayData,128,notify);
        else if(auto slice=std::dynamic_pointer_cast<SurfaceSliceOverlayStrategy>(overlay))slice->SetDisplayCache(displayData,128,notify);
        else {RemoveBindings(bindings);return false;}
        if(!isPreview) {
            const auto state=GetState();const auto generation=GetDisplayGeneration();
            if(generation&&state.selectedObject&&state.selectionGeneration==generation->dataRevision) {
                const auto object=generation->GetObjectById(*state.selectedObject);
                if(object) {
                    const auto range=std::make_pair(object->firstTriangle,object->firstTriangle+object->triangleCount);
                    if(auto mesh=std::dynamic_pointer_cast<SurfaceOverlayStrategy>(overlay))mesh->GetDisplay().SetObjectRange(range,state.isSelectionIsolated);
                    else if(auto slice=std::dynamic_pointer_cast<SurfaceSliceOverlayStrategy>(overlay))slice->GetDisplay().SetObjectRange(range,state.isSelectionIsolated);
                }
            }
        }
        OverlayBinding binding{ service, overlay, view.id };
        bool isAttached = false;
        try { isAttached = service->AttachOverlay(overlay); }
        catch (...) {}
        if (!isAttached) {
            service->RemoveOverlay(overlay);
            RemoveBindings(bindings);
            return false;
        }
        bindings.push_back(std::move(binding));
    }
    return true;
}

void SurfaceDeterminationHostFeature::Impl::RemoveBindings(
    std::vector<OverlayBinding>& bindings) noexcept
{
    for (auto item = bindings.rbegin(); item != bindings.rend(); ++item) {
        if (item->service && item->overlay) {
            item->service->RemoveOverlay(item->overlay);
        }
    }
    bindings.clear();
}

bool SurfaceDeterminationHostFeature::Impl::ClearObjectSelection()
{
    ++m_objectIntent;
    auto job=std::move(m_objectJob);if(job)job->cancelled->store(true);
    const auto source=m_objectSource;std::vector<std::string> retiredViews;
    for(const auto& binding:m_objectBindings)retiredViews.push_back(binding.viewId);
    RemoveBindings(m_objectBindings);m_objectGeneration.reset();m_objectSource.reset();
    m_objectOperation.status=FeatureRunStatus::Cancelled;++m_objectOperation.stateRevision;
    bool updated=SendDisplayDelta();
    if(m_bindings.empty() && source && source->data && !retiredViews.empty() && m_host) {
        std::sort(retiredViews.begin(),retiredViews.end());retiredViews.erase(std::unique(retiredViews.begin(),retiredViews.end()),retiredViews.end());
        FeatureSceneDelta delta;delta.requestId=GetNextRequestId();delta.priority=FeatureScenePriority::Scene;delta.scope=FeatureSceneScope::RequiredAllViews;
        delta.viewIds=retiredViews;delta.inputStamp={source->data->self};delta.inputs={{"source-volume",source->data->self}};delta.hasDisplayUpdate=true;
        updated=m_host->SendSceneDelta(std::move(delta)) && m_host->SetActiveViews({});
    }

    if(job) {
        auto result=BuildResult(job->id,SurfaceResultStatus::Cancelled,SurfaceFailureReason::Cancelled,job->generation->sourceRevision,
            job->generation->resultRevision,0,0,"Surface object highlight was cancelled.");
        result.dataRevision=job->generation->dataRevision;result.meshRevision=job->generation->meshRevision;
        SendComplete(std::move(job->callback),std::move(result));
    }
    return updated;
}
void SurfaceDeterminationHostFeature::Impl::ContinueObjectSelection()
{
    const auto primary=m_data->GetPrimaryImage();
    const auto same=[&](const VtkImageGridSnapshot& source){return primary && primary->data && source && source->data && primary->data->self==source->data->self && GetSourceSame(source);};
    if(m_objectSource && !same(m_objectSource)){(void)ClearObjectSelection();return;}
    if(!m_objectJob)return;
    if(!same(m_objectJob->source)){(void)ClearObjectSelection();return;}
    if(m_objectJob->future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
    auto job=std::move(m_objectJob);
    auto result=BuildResult(job->id,SurfaceResultStatus::Failed,SurfaceFailureReason::DisplayFailed,job->generation->sourceRevision,
        job->generation->resultRevision,job->object.pointCount,1,"Surface object highlight preparation failed.");
    result.dataRevision=job->generation->dataRevision;result.meshRevision=job->generation->meshRevision;
    try {
        const auto source=job->future.get();
        const auto display=source?std::make_shared<FeatureInternal::SpatialMeshCache>(source,m_service->GetReadQueue(),128):nullptr;
        if(display && BuildBindings(display,job->views,job->source,m_objectBindings,true)) {
            m_objectGeneration=job->generation;m_objectSource=job->source;
            m_objectOperation.status=FeatureRunStatus::Succeeded;m_objectOperation.progress=1;++m_objectOperation.stateRevision;
            auto active=GetViewIds(job->views);for(const auto& binding:m_bindings)active.push_back(binding.viewId);
            std::sort(active.begin(),active.end());active.erase(std::unique(active.begin(),active.end()),active.end());
            if(m_host->SetActiveViews(active) && SendDisplayDelta()) {
                result.status=SurfaceResultStatus::Succeeded;result.failureReason=SurfaceFailureReason::None;result.message="Surface object range highlighted without re-extraction.";
            }
        }
    }catch(const std::exception& error){result.message=error.what();}
    if(result.status!=SurfaceResultStatus::Succeeded){RemoveBindings(m_objectBindings);m_objectGeneration.reset();m_objectSource.reset();m_objectOperation.status=FeatureRunStatus::Failed;++m_objectOperation.stateRevision;(void)SendDisplayDelta();}
    SendComplete(std::move(job->callback),std::move(result));
}
bool SurfaceDeterminationHostFeature::Impl::RemoveDisplay()
{
    (void)ClearObjectSelection();
    (void)SetObjectPicking(false);
    RemoveBindings(m_bindings);
    m_displayData = nullptr;
    return !m_host || m_host->SetActiveViews({});
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::Impl::GetDisplayGeneration() const
{
    const auto preview = GetPreviewSnapshot();
    return preview ? preview : m_store.GetGeneration();
}

bool SurfaceDeterminationHostFeature::Impl::SendDisplayDelta()
{
    if (m_bindings.empty() && m_objectBindings.empty()) return true;
    try {
        const auto generation = GetDisplayGeneration();
        const bool isPreview = generation && generation->purpose == SurfaceTaskPurpose::Preview;
        const auto source = m_bindings.empty()?m_objectSource:(isPreview?m_previewSource:m_activeSource);
        const auto& views = isPreview ? m_previewViews : m_activeViews;
        if (!m_host || !source || !source->data) return false;
        FeatureSceneDelta delta;
        delta.requestId = GetNextRequestId();
        delta.priority = FeatureScenePriority::Scene;
        delta.scope = FeatureSceneScope::RequiredAllViews;
        delta.inputStamp = { source->data->self };
        delta.viewIds = GetViewIds(views);
        for(const auto& binding:m_objectBindings)delta.viewIds.push_back(binding.viewId);
        std::sort(delta.viewIds.begin(),delta.viewIds.end());delta.viewIds.erase(std::unique(delta.viewIds.begin(),delta.viewIds.end()),delta.viewIds.end());
        delta.hasDisplayUpdate = true;
        delta.inputs = { { "source-volume", source->data->self } };
        if (!isPreview && generation && !m_bindings.empty()) {
            delta.inputs.push_back({ "mesh", generation->meshRevision });
            for (const auto& binding : m_bindings)
                delta.displays.push_back({ binding.viewId, std::string(featureId), "surface",
                    generation->meshRevision, m_displayOperation.operation });
        }
        if(m_objectGeneration && !m_objectBindings.empty()) {
            delta.inputs.push_back({"object-generation",m_objectGeneration->dataRevision});delta.inputs.push_back({"object-mesh",m_objectGeneration->meshRevision});
            for(const auto& binding:m_objectBindings)delta.displays.push_back({binding.viewId,std::string(featureId),"surface-object",m_objectGeneration->meshRevision,m_objectOperation.operation});
        }
        return m_host->SendSceneDelta(std::move(delta));
    }
    catch (...) { return false; }
}

std::vector<FeatureOperationState> SurfaceDeterminationHostFeature::Impl::GetOperationStates() const
{
    if (!m_isAttached || !GetIsOwnerThread() || !m_service) return {};
    std::vector<FeatureOperationState> states;
    for (const auto& [id, request] : m_requests) {
        auto state = request.operation;
        const auto execution = m_service->GetExecutionState(id);
        state.stateRevision += execution.stateRevision;
        state.status = execution.status;
        state.progress = execution.progress;
        states.push_back(std::move(state));
    }
    if (m_lastOperation.operation.requestId != 0
        && m_requests.find(m_lastOperation.operation.requestId) == m_requests.end())
        states.push_back(m_lastOperation);
    if (!m_bindings.empty() && m_displayOperation.operation.requestId != 0
        && m_displayOperation.operation.requestId != m_lastOperation.operation.requestId
        && m_requests.find(m_displayOperation.operation.requestId) == m_requests.end())
        states.push_back(m_displayOperation);
    if(m_objectJob || !m_objectBindings.empty())states.push_back(m_objectOperation);
    return states;
}

bool SurfaceDeterminationHostFeature::Impl::SetObjectSelection(const SurfaceObjectSelectionRequest& request)
{
    const auto generation=GetDisplayGeneration();
    if(!generation||generation->purpose!=SurfaceTaskPurpose::Determine||generation->dataRevision!=request.generation
        ||!GetDataRevisionRefValid(request.generation)||!generation->triangleIndices||!m_data)return false;
    const auto graph=m_data->GetDataGraph();
    if(!SurfaceContract::GetSourceCurrent(*m_data,graph,generation->sourceRevision,generation->sourceBinding)
        ||!SurfaceContract::GetInputsCurrent(*m_data,graph,generation->inputs))return false;
    std::optional<std::pair<std::uint64_t,std::uint64_t>> range;
    if(request.objectIndex) {
        const auto object=generation->GetObjectById(*request.objectIndex);
        const auto count=generation->triangleIndices->size()/3;
        if(!object||object->firstTriangle>count||object->triangleCount>count-object->firstTriangle)return false;
        range=std::make_pair(object->firstTriangle,object->firstTriangle+object->triangleCount);
    }else if(request.isIsolated)return false;
    const auto before=GetState();
    std::optional<std::pair<std::uint64_t,std::uint64_t>> previous;
    if(before.selectionGeneration==generation->dataRevision&&before.selectedObject){const auto old=generation->GetObjectById(*before.selectedObject);if(old)previous=std::make_pair(old->firstTriangle,old->firstTriangle+old->triangleCount);}
    const auto apply=[&](const auto& selection,bool isolated){
        for(auto& binding:m_bindings) {
            if(auto mesh=std::dynamic_pointer_cast<SurfaceOverlayStrategy>(binding.overlay))mesh->GetDisplay().SetObjectRange(selection,isolated);
            else if(auto slice=std::dynamic_pointer_cast<SurfaceSliceOverlayStrategy>(binding.overlay))slice->GetDisplay().SetObjectRange(selection,isolated);
        }
    };
    try {apply(range,request.isIsolated);if(!SendDisplayDelta())throw std::runtime_error("Surface object display change was rejected.");}
    catch(...) {
        try{apply(previous,before.isSelectionIsolated);}catch(...){const std::lock_guard<std::mutex> lock(m_stateMutex);m_state.hasDisplayFailure=true;m_state.displayMessage="Surface object display restoration failed.";}
        return false;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    for(auto* state:{&m_state,&m_stateBeforeRequest}){state->selectionGeneration=request.generation;state->selectedObject=request.objectIndex;state->isSelectionIsolated=request.isIsolated;}
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::SetObjectPicking(bool enabled)
{
    if(!m_host||!m_isInputAttached)return !enabled;
    if(enabled) {
        const auto generation=GetDisplayGeneration();const auto state=GetState();
        if(!generation||generation->purpose!=SurfaceTaskPurpose::Determine||state.stage!=SurfaceDeterminationStage::Ready
            ||!state.isOverlayVisible||m_bindings.empty())return false;
    }
    if(enabled||m_pickOwnerActive) {
        // 清除时不携带可能已经失效的旧视图列表；Host 仅清除本 Feature 当前拥有的路由。
        const bool applied=m_host->SetInputOwner(featureId,enabled,enabled?GetViewIds(m_activeViews):std::vector<std::string>{});
        if(!applied)return false;
        m_pickOwnerActive=enabled;
    }
    {const std::lock_guard<std::mutex> lock(m_stateMutex);m_state.isObjectPickingEnabled=enabled;m_stateBeforeRequest.isObjectPickingEnabled=enabled;}
    (void)m_host->SetViewStatus(GetViewIds(m_activeViews),enabled?u8"Ctrl + 点击选择表面对象":"");
    return true;
}

InteractionResult SurfaceDeterminationHostFeature::Impl::OnObjectInput(const InteractionEvent& event)
{
    if(event.eventKind!=InteractionEventKind::PrimaryPress&&event.eventKind!=InteractionEventKind::SecondaryPress
        &&event.eventKind!=InteractionEventKind::MiddlePress)return {true,true,true,InteractionFailureReason::None};
    if(!GetIsOwnerThread()||!m_isAttached||m_isDetaching||!GetState().isObjectPickingEnabled||!m_views||!m_host)return {};
    const auto generation=GetDisplayGeneration();
    const auto target=m_host->GetDisplayTarget(event.viewId,"surface");
    if(!generation||!target||target->display.data!=generation->meshRevision||!(target->display.operation==m_displayOperation.operation))return {true,true,true,InteractionFailureReason::None};
    const auto view=m_views->GetInputView({event.viewId,false,HostRenderViewRole::Auxiliary});
    const auto lease=view?view->lease.lock():nullptr;
    if(!view||!lease||!lease->GetIsActive()||!lease->GetIsOwnerThread()||!view->renderer)return {true,true,true,InteractionFailureReason::None};
    std::optional<std::uint64_t> face;
    for(const auto& binding:m_bindings)if(binding.viewId==event.viewId) {
        if(auto mesh=std::dynamic_pointer_cast<SurfaceOverlayStrategy>(binding.overlay))face=mesh->GetDisplay().GetPickedFace(event.x,event.y,view->renderer);
        else if(auto slice=std::dynamic_pointer_cast<SurfaceSliceOverlayStrategy>(binding.overlay))face=slice->GetDisplay().GetPickedFace(event.x,event.y,view->renderer);
        break;
    }
    if(!face||!generation->objects)return {true,true,true,InteractionFailureReason::None};
    const auto& objects=*generation->objects;
    std::size_t first=0,last=objects.size();
    while(first<last){const auto middle=first+(last-first)/2;const auto object=objects[middle];
        if(*face>=object.firstTriangle&&*face-object.firstTriangle>=object.triangleCount)first=middle+1;else last=middle;}
    if(first==objects.size())return {true,true,true,InteractionFailureReason::None};
    const auto object=objects[first];
    if(*face<object.firstTriangle||*face-object.firstTriangle>=object.triangleCount)return {true,true,true,InteractionFailureReason::None};
    const bool succeeded=SetObjectSelection({generation->dataRevision,object.objectIndex,false});
    return {true,true,succeeded,succeeded?InteractionFailureReason::None:InteractionFailureReason::StateRejected};
}

bool SurfaceDeterminationHostFeature::Impl::SetVisibility(const bool isVisible)
{
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.isOverlayVisible = isVisible;
        m_stateBeforeRequest.isOverlayVisible = isVisible;
    }
    if (!isVisible) { m_isDisplayPending = false; return RemoveDisplay(); }
    m_isDisplayPending = !SetDisplayProjection();
    return !m_isDisplayPending;
}

bool SurfaceDeterminationHostFeature::Impl::ClearPreview()
{
    { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview.reset(); }
    m_previewDisplay.reset();
    m_previewSource.reset(); m_previewViews.clear();
    if (!RemoveDisplay()) return false;
    m_isDisplayPending = !SetDisplayProjection();
    const bool visible = GetState().isOverlayVisible;
    auto restored = m_store.GetGeneration() ? m_stateBeforeRequest : SurfaceDeterminationState{};
    restored.isOverlayVisible = visible;
    SetState(std::move(restored));
    if (m_isDisplayPending) SetDisplayFailure();
    return !m_isDisplayPending;
}

bool SurfaceDeterminationHostFeature::Impl::ClearResult()
{
    if (!ClearResultBinding()) return false;
    if (!RemoveDisplay()) return false;
    { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview.reset(); }
    m_previewDisplay.reset();
    m_previewSource.reset(); m_previewViews.clear();
    m_store.ClearGeneration();
    m_activeSource.reset(); m_activeViews.clear();
    m_isDisplayPending = false;
    SurfaceDeterminationState idle;
    idle.isOverlayVisible = GetState().isOverlayVisible;
    m_stateBeforeRequest = idle;
    SetState(std::move(idle));
    return !m_data || m_resultScopes.Clear(*m_data);
}

bool SurfaceDeterminationHostFeature::Impl::ClearResultBinding()
{
    const auto generation = m_store.GetGeneration();
    if (!generation || !m_data) return true;
    const auto binding = m_data->GetDataBinding(m_data->GetDataGraph(), SurfaceContract::GetBindingName(m_activeScope));
    if (!binding || binding->revision != m_resultBinding.revision
        || binding->target != m_resultBinding.target) return true;
    DataTransaction transaction;
    transaction.bindings.push_back({ binding->name,
        binding->revision, true, binding->target, {} });
    return m_resultScopes.Commit(*m_data,std::move(transaction)).status == DataCommitStatus::Succeeded;
}

DataBinding SurfaceDeterminationHostFeature::Impl::GetResultBinding() const
{ return GetResultBinding(m_activeScope); }

DataBinding SurfaceDeterminationHostFeature::Impl::GetResultBinding(const std::string_view scope) const
{
    const auto name = SurfaceContract::GetBindingName(scope);
    return m_data ? m_data->GetDataBinding(m_data->GetDataGraph(), name)
        .value_or(DataBinding{ name, {}, 0 }) : DataBinding{};
}

void SurfaceDeterminationHostFeature::Impl::SetDisplayFailure(const std::uint64_t requestId)
{
    m_isDisplayPending = true;
    const auto generation = m_store.GetGeneration();
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_latestRequestId != 0 && m_state.stage != SurfaceDeterminationStage::Ready
        && (requestId == 0 || requestId != m_latestRequestId)) return;
    if (requestId != 0 && generation) {
        m_state.stage = SurfaceDeterminationStage::Ready;
        m_state.sourceRevision = generation->sourceRevision;
        m_state.resultRevision = generation->resultRevision;
        m_state.pointCount = generation->points->size();
        m_state.objectCount = static_cast<std::uint32_t>(generation->objects->size());
    }
    m_state.failureReason = SurfaceFailureReason::DisplayFailed;
    m_state.errorMessage = m_displayPrepareError.empty()?"Surface candidate is ready; display attachment is pending."
        :"Surface result is ready; display preparation failed: "+m_displayPrepareError;
}

bool SurfaceDeterminationHostFeature::Impl::SetDisplayProjection()
{
    const auto generation = GetDisplayGeneration();
    if (!generation || !m_host) return RemoveDisplay();
    const bool isPreview = generation->purpose == SurfaceTaskPurpose::Preview;
    const auto& views = isPreview ? m_previewViews : m_activeViews;
    const auto source = isPreview ? m_previewSource : m_activeSource;
    if (!GetState().isOverlayVisible || views.empty()) return RemoveDisplay();
    if (!m_displayData) {
        std::shared_ptr<const FeatureInternal::SpatialMeshSource> display=m_previewDisplay;
        m_displayPrepareError=m_previewDisplayError;
        if(!isPreview) {
            const auto snapshot=m_data->GetData(m_data->GetDataGraph(),generation->dataRevision);
            const auto* payload=snapshot?dynamic_cast<const SurfaceGenerationPayload*>(snapshot->payload.get()):nullptr;
            display=payload?payload->GetDisplaySource():nullptr;
            m_displayPrepareError=payload?payload->GetDisplayError():"Display source metadata is unavailable.";
        }
        if(!display)return false;
        m_displayPrepareError.clear();
        m_displayData=std::make_shared<FeatureInternal::SpatialMeshCache>(std::move(display),m_service->GetReadQueue(),
            128);
    }
    if (m_bindings.empty()) {
        std::vector<OverlayBinding> bindings;
        try {
            if (!BuildBindings(m_displayData, views, source, bindings, isPreview)
                || !m_host->SetActiveViews(GetViewIds(views))) {
                RemoveBindings(bindings); return false;
            }
        }
        catch (...) { RemoveBindings(bindings); return false; }
        m_bindings = std::move(bindings);
    }
    return SendDisplayDelta();
}

void SurfaceDeterminationHostFeature::Impl::SetBindingProjection()
{
    if (!m_isAttached || m_isDetaching || !m_data) return;
    try {
        const auto graph = m_data->GetDataGraph();
        const auto binding = GetResultBinding();
        if (binding.revision != m_resultBinding.revision
            || binding.target != m_resultBinding.target) {
            // 外部重新激活（包括 ABA）是新的业务意图；退休旧展示但不写图。
            m_resultBinding = binding;
            if (!RemoveDisplay()) m_isStaleCleanupPending = true;
            const auto snapshot = binding.target ? m_data->GetData(graph, *binding.target) : DataSnapshot{};
            const auto* payload = snapshot
                ? dynamic_cast<const SurfaceGenerationPayload*>(snapshot->payload.get()) : nullptr;
            const auto generation = payload ? payload->GetGeneration() : nullptr;
            const auto source = generation ? m_data->GetImageGrid(graph, generation->sourceRevision) : nullptr;
            if (!generation || !source || !source->data || generation->dataRevision != snapshot->self ||
                generation->resultScope != m_activeScope ||
                generation->purpose != SurfaceTaskPurpose::Determine ||
                !SurfaceContract::GetSourceCurrent(*m_data, graph, generation->sourceRevision,
                                                   generation->sourceBinding) ||
                !SurfaceContract::GetInputsCurrent(*m_data, graph, generation->inputs))
            {
                m_store.ClearGeneration();
                m_activeSource.reset();
                m_isDisplayPending = false;
                auto state = GetState();
                m_stateBeforeRequest = {};
                m_stateBeforeRequest.isOverlayVisible = state.isOverlayVisible;
                if (state.stage == SurfaceDeterminationStage::Ready) {
                    const bool isVisible = state.isOverlayVisible;
                    state = {};
                    state.isOverlayVisible = isVisible;
                    if (binding.target) {
                        state.stage = SurfaceDeterminationStage::Stale;
                        state.failureReason = SurfaceFailureReason::SourceChanged;
                    }
                    SetState(std::move(state));
                }
                return;
            }
            m_store.SetGeneration(snapshot);
            m_displayData.reset();
            // Historical activation is a new display attempt; it does not rewrite graph history.
            m_displayOperation = {};
            m_displayOperation.operation = { std::string(featureId), m_host->GetAttachmentId(), GetNextRequestId() };
            m_displayOperation.stateRevision = 1;
            m_displayOperation.status = FeatureRunStatus::Succeeded;
            m_displayOperation.progress = 1.0;
            m_displayOperation.inputs = generation->inputs;
            if (GetDataRevisionRefValid(generation->meshRevision))
                m_displayOperation.outputs.push_back(generation->meshRevision);
            m_displayOperation.outputs.push_back(snapshot->self);
            auto activeSource = std::make_shared<VtkImageGridView>(*source);
            activeSource->binding = generation->sourceBinding;
            m_activeSource = std::move(activeSource);
            if (m_activeViews.empty()) {
                auto targets = m_config.defaultStart.targetViews;
                if (!GetTargetsUsed(targets)) targets.viewRoles = { HostRenderViewRole::Primary3D };
                m_activeViews = GetTargetViews(targets);
            }
            m_resultRevision = std::max(m_resultRevision, generation->resultRevision);
            SurfaceDeterminationState ready;
            ready.stage = SurfaceDeterminationStage::Ready;
            ready.sourceRevision = generation->sourceRevision;
            ready.purpose = generation->purpose;
            ready.resultScope = generation->resultScope;
            ready.isoEstimate = generation->isoEstimate;
            ready.resultRevision = generation->resultRevision;
            ready.progress01 = 1.0;
            ready.pointCount = generation->points ? generation->points->size() : 0;
            ready.objectCount = generation->objects
                ? static_cast<std::uint32_t>(generation->objects->size()) : 0;
            const auto& statistics = payload->GetStatistics();
            ready.acceptedPointCount = statistics.acceptedPointCount;
            ready.lowContrastPointCount = statistics.lowContrastPointCount;
            ready.rejectedPointCount = statistics.rejectedPointCount;
            ready.truncatedPointCount = statistics.truncatedPointCount;
            ready.nonManifoldObjectCount = statistics.nonManifoldObjectCount;
            ready.isOverlayVisible = GetState().isOverlayVisible;
            if (!GetPreviewSnapshot() && (m_latestRequestId == 0
                || GetState().stage == SurfaceDeterminationStage::Ready)) SetState(ready);
            m_stateBeforeRequest = std::move(ready);
            m_isDisplayPending = true;
        }
        if (m_isDisplayPending) {
            if (SetDisplayProjection()) {
                m_isDisplayPending = false;
                const std::lock_guard<std::mutex> lock(m_stateMutex);
                if (m_state.failureReason == SurfaceFailureReason::DisplayFailed) {
                    m_state.failureReason = SurfaceFailureReason::None;
                    m_state.errorMessage.clear();
                }
            }
            else { SetDisplayFailure(); }
        }
    }
    catch (...) { SetDisplayFailure(); }
}

void SurfaceDeterminationHostFeature::Impl::SetSourceStale()
{
    if (!ClearResultBinding()) {
        m_isStaleCleanupPending = true;
        return;
    }
    const auto previousState = GetState();
    m_store.ClearGeneration();
    m_activeSource.reset();
    m_activeViews.clear();
    m_isDisplayPending = false;
    if (!RemoveDisplay()) m_isStaleCleanupPending = true;
    const bool isVisible = GetState().isOverlayVisible;
    SurfaceDeterminationState stale;
    stale.stage = SurfaceDeterminationStage::Stale;
    stale.failureReason = SurfaceFailureReason::SourceChanged;
    stale.requestId = m_latestRequestId != 0
        ? m_latestRequestId : previousState.requestId;
    stale.sourceRevision = previousState.sourceRevision;
    stale.resultRevision = m_resultRevision;
    stale.isOverlayVisible = isVisible;
    stale.errorMessage = "Surface source changed before commit.";
    SetState(std::move(stale));
}

void SurfaceDeterminationHostFeature::Impl::SetRequestComplete(SurfaceJobComplete complete)
{
    const auto item = m_requests.find(complete.requestId);
    if (item == m_requests.end()) return;
    RequestEntry request = std::move(item->second);
    m_requests.erase(item);
    request.operation.stateRevision += m_service->GetExecutionState(complete.requestId).stateRevision;
    auto operation = request.operation;
    const bool isLatest = complete.requestId == m_latestRequestId;
    auto& result = complete.result;
    const auto purpose = SurfaceContract::GetPurpose(request.params);
    if (request.isSourceChanged || !GetRequestInputsSame(request))
    {
        result.status = SurfaceResultStatus::Failed;
        result.failureReason = SurfaceFailureReason::SourceChanged;
        result.message = "Surface source changed before commit.";
    }
    else if (request.isSuperseded) {
        result.status = SurfaceResultStatus::Cancelled;
        result.failureReason = SurfaceFailureReason::Cancelled;
        result.message = "Surface request was superseded in its scope and purpose.";
    }
    else if (result.status == SurfaceResultStatus::Succeeded && purpose == SurfaceTaskPurpose::Determine
        && request.params.sourcePolicy == DataPublishPolicy::RequireCurrentInputs) {
        const auto current = GetResultBinding(request.params.resultScope);
        if (current.revision != request.resultBinding.revision || current.target != request.resultBinding.target) {
            result.status = SurfaceResultStatus::Cancelled;
            result.failureReason = SurfaceFailureReason::Cancelled;
            result.message = "Surface result binding changed before commit.";
        }
    }
    auto callbackResult = BuildResult(complete.requestId, result.status, result.failureReason,
        result.sourceRevision, 0, result.points.size(), static_cast<std::uint32_t>(result.objects.size()), result.message);
    callbackResult.purpose = purpose;
    callbackResult.resultScope = request.params.resultScope;
    callbackResult.isoEstimate = result.isoEstimate;
    DataSnapshot published;
    if (result.status == SurfaceResultStatus::Succeeded) {
        try {
            if (purpose == SurfaceTaskPurpose::Determine) {
                published = SetRequestSucceeded(request, std::move(result), complete.requestId,std::move(complete.meshes));
                if (!published) {
                    callbackResult.status = SurfaceResultStatus::Failed;
                    callbackResult.failureReason = SurfaceFailureReason::PublishFailed;
                    callbackResult.message = "Surface candidate could not be published.";
                }
            }
            else if (isLatest) SetTransientResult(request, result, complete.requestId,std::move(complete.meshes));
        }
        catch (...) {
            callbackResult.status = SurfaceResultStatus::Failed;
            callbackResult.failureReason = SurfaceFailureReason::BudgetExceeded;
            callbackResult.message = "Surface candidate allocation failed.";
        }
    }
    if (callbackResult.status != SurfaceResultStatus::Succeeded && isLatest && m_isAttached) {
        SurfaceAlgorithmResult failure;
        failure.status = callbackResult.status; failure.failureReason = callbackResult.failureReason;
        failure.message = callbackResult.message;
        SetRequestFailed(request, failure);
    }
    if (published) {
        const auto* payload = dynamic_cast<const SurfaceGenerationPayload*>(published->payload.get());
        const auto generation = payload->GetGeneration();
        callbackResult.isPublished = true;
        callbackResult.isActivated = request.params.sourcePolicy == DataPublishPolicy::RequireCurrentInputs;
        callbackResult.dataRevision = generation->dataRevision;
        callbackResult.meshRevision = generation->meshRevision;
        callbackResult.resultRevision = generation->resultRevision;
        callbackResult.pointCount = generation->points->size();
        callbackResult.objectCount = static_cast<std::uint32_t>(generation->objects->size());
        operation.outputs = { generation->meshRevision, generation->dataRevision };
    }
    if (isLatest && m_isAttached && callbackResult.status == SurfaceResultStatus::Succeeded
        && request.params.sourcePolicy != DataPublishPolicy::AllowHistoricalResult
        && (m_isDisplayPending
            || (published && GetResultBinding(request.params.resultScope).target != published->self))) {
        callbackResult.failureReason = SurfaceFailureReason::DisplayFailed;
        callbackResult.message = "Surface calculation completed; display has not followed the result.";
    }
    operation.status = callbackResult.status == SurfaceResultStatus::Succeeded ? FeatureRunStatus::Succeeded
        : callbackResult.status == SurfaceResultStatus::Cancelled ? FeatureRunStatus::Cancelled : FeatureRunStatus::Failed;
    operation.progress = callbackResult.status == SurfaceResultStatus::Succeeded ? 1.0 : 0.0;
    operation.stateRevision += 2;
    if (m_isAttached && operation.operation.requestId >= m_lastOperation.operation.requestId) m_lastOperation = operation;
    if (m_latestRequestId == complete.requestId) m_latestRequestId = 0;
    SendComplete(std::move(request.onComplete), std::move(callbackResult),
        request.source->binding ? request.source->binding->revision : 0, std::move(request.completeActive));
}

void SurfaceDeterminationHostFeature::Impl::SetRequestFailed(
    const RequestEntry& request,
    const SurfaceAlgorithmResult& result)
{
    if (result.failureReason == SurfaceFailureReason::SourceChanged || !GetRequestInputsSame(request))
    {
        if (m_activeSource && !GetSourceSame(m_activeSource)) SetSourceStale();
        else {
            auto state = m_stateBeforeRequest;
            state.stage = SurfaceDeterminationStage::Stale;
            state.failureReason = SurfaceFailureReason::SourceChanged;
            state.requestId = GetState().requestId;
            state.sourceRevision = request.source->data->self;
            SetState(std::move(state));
        }
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_stateBeforeRequest.stage == SurfaceDeterminationStage::Ready) {
        m_state = m_stateBeforeRequest;
        return;
    }
    const auto requestId = m_state.requestId;
    m_state = m_stateBeforeRequest;
    m_state.requestId = requestId;
    m_state.sourceRevision = request.source && request.source->data ? request.source->data->self : DataRevisionRef{};
    m_state.stage = result.status == SurfaceResultStatus::Cancelled
        ? SurfaceDeterminationStage::Cancelled
        : SurfaceDeterminationStage::Failed;
    m_state.failureReason = result.failureReason;
    m_state.progress01 = 0.0;
    m_state.errorMessage = result.message;
}

std::shared_ptr<SurfaceGenerationSnapshot> SurfaceDeterminationHostFeature::Impl::BuildGeneration(
    const RequestEntry &request, SurfaceAlgorithmResult &result, const std::uint64_t requestId,
    const bool isFormal)
{
    if (!result.prepared || result.points.empty() || result.triangleIndices.empty() || result.objects.empty()
        || result.triangleValidity.size() != result.triangleIndices.size() / 3) return {};
    const auto* image = dynamic_cast<const ImageGrid3DPayload*>(request.source->data->payload.get());
    if (!image || image->GetGeometry().coordinateFrame.empty()) return {};
    SurfaceGenerationSnapshot generation;
    generation.inputs = request.operation.inputs;
    if (request.inputs.roi) generation.selection=std::make_shared<const MeshReadArray<SurfaceSelectionPiece>>(result.prepared->selection);
    generation.selectedAreaMM2=result.selectedAreaMM2;generation.selectionToleranceMM=result.selectionToleranceMM;
    generation.execution = result.execution;
    generation.interfaces =
        std::make_shared<const std::vector<SurfaceInterfaceRecord>>(std::move(result.interfaces));
    generation.requestId = requestId;
    generation.purpose = SurfaceContract::GetPurpose(request.params);
    generation.resultScope = request.params.resultScope;
    generation.coordinateFrame = image->GetGeometry().coordinateFrame;
    generation.modelUnit = request.params.modelUnit;
    generation.requestedParams = request.params;
    generation.requestedParams.targetViews = {};
    generation.resolvedParams = result.resolvedParams;
    generation.canonicalParameters = SurfaceContract::BuildParameters(
        generation.requestedParams, generation.resolvedParams, generation.coordinateFrame, WorkLimit(m_config.maxWorkingBytes));
    generation.sourceBinding = request.source->binding;
    if (isFormal) {
        generation.meshRevision = request.inputs.parent?request.inputs.parent->meshRevision:DataRevisionRef{ m_data->CreateDataEntityId(), 1 };
        generation.dataRevision = { m_data->CreateDataEntityId(), 1 };
        generation.resultRevision = m_resultRevision + 1;
    }
    generation.sourceRevision = result.sourceRevision;
    generation.parameterFingerprint = result.parameterFingerprint;
    generation.algorithmRevision = result.algorithmRevision;
    generation.method = result.method;
    generation.isoEstimate = result.isoEstimate;
    generation.points = std::make_shared<const MeshReadArray<SurfacePointRecord>>(result.prepared->points);
    generation.triangleIndices = std::make_shared<const MeshReadArray<std::uint32_t>>(result.prepared->triangles);
    generation.triangleValidity = std::make_shared<const MeshReadArray<std::uint8_t>>(result.prepared->validity);
    generation.objects = std::make_shared<const MeshReadArray<SurfaceObjectRecord>>(result.prepared->objects);
    if(request.inputs.parent){generation.points=request.inputs.parent->points;generation.triangleIndices=request.inputs.parent->triangleIndices;generation.triangleValidity=request.inputs.parent->triangleValidity;generation.objects=request.inputs.parent->objects;generation.interfaces=request.inputs.parent->interfaces;}
    return std::make_shared<SurfaceGenerationSnapshot>(std::move(generation));
}

void SurfaceDeterminationHostFeature::Impl::SetTransientResult(const RequestEntry& request,
    SurfaceAlgorithmResult& result, const std::uint64_t requestId,
    SurfacePreparedMeshes display)
{
    const auto purpose = SurfaceContract::GetPurpose(request.params);
    SetBindingProjection(); // 先吸收其他通道刚发布的正式槽，供预览退出时恢复。
    SurfaceDeterminationState ready = m_stateBeforeRequest;
    ready.stage = SurfaceDeterminationStage::Ready;
    ready.failureReason = SurfaceFailureReason::None;
    ready.errorMessage.clear(); ready.requestId = requestId;
    ready.sourceRevision = result.sourceRevision; ready.progress01 = 1.0;
    ready.purpose = purpose; ready.resultScope = request.params.resultScope;
    ready.isoEstimate = result.isoEstimate;
    if (purpose == SurfaceTaskPurpose::Preview) {
        const auto preview = BuildGeneration(request, result, requestId, false);
        if (!preview) throw std::runtime_error("Invalid surface preview");
        ready.pointCount = preview->points->size(); ready.objectCount = static_cast<std::uint32_t>(preview->objects->size());
        ready.isObjectPickingEnabled=false;
        ready.acceptedPointCount = 0; ready.rejectedPointCount = ready.pointCount;
        { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview = preview; }
        m_previewSource = request.source; m_previewViews = request.views;m_previewDisplay=std::move(display.display);m_previewDisplayError=std::move(display.displayError);
        (void)SetObjectPicking(false);RemoveBindings(m_bindings); m_displayData = nullptr;
        SetState(ready);
        m_isDisplayPending = !SetDisplayProjection();
        if (m_isDisplayPending) SetDisplayFailure();
    }
}

DataSnapshot SurfaceDeterminationHostFeature::Impl::SetRequestSucceeded(
    const RequestEntry& request, SurfaceAlgorithmResult result, const std::uint64_t requestId,SurfacePreparedMeshes prepared)
{
    ResultArrayScope publicationStorage(WorkLimit(m_config.maxWorkingBytes).GetBound(8 * 1024 * 1024));
    if (!GetRequestInputsSame(request) || m_resultRevision == std::numeric_limits<std::uint64_t>::max())
        return {};
    const auto publicationStarted = std::chrono::steady_clock::now();
    std::size_t publicationBytes = GetRetainedBytes();
    const auto charge = [&](std::size_t count, std::size_t width) {
        if (count > (SIZE_MAX - publicationBytes) / width)
            return false;
        publicationBytes += count * width;
        return true; // 正式结果由后备存储保活，总逻辑字节数不作为驻留准入。
    };
    if(!result.prepared || !charge(result.points.GetResidentBytes(),1) || !charge(result.triangleIndices.GetResidentBytes(),1)
        || !charge(result.triangleValidity.GetResidentBytes(),1) || !charge(result.objects.GetResidentBytes(),1)
        || !charge(result.selection.GetResidentBytes(),1))throw std::bad_alloc{};
    result.execution.estimatedWorkingBytes =
        std::max(result.execution.estimatedWorkingBytes, publicationBytes);
    const auto stagedGeneration = BuildGeneration(request, result, requestId, true);
    if (!stagedGeneration) return {};
    const auto meshRef = stagedGeneration->meshRevision;
    const auto generationRef = stagedGeneration->dataRevision;
    auto mesh=std::move(prepared.measurement);
    if(request.inputs.parent) {
        const auto existing=m_data->GetData(m_data->GetDataGraph(),request.inputs.parent->meshRevision);
        mesh=existing?std::dynamic_pointer_cast<const SurfaceMeshPayload>(existing->payload):nullptr;
    }
    if(!mesh||!mesh->GetValid())return {};
    const auto& expected = request.resultBinding;
    DataTransaction transaction;
    transaction.policy = request.params.sourcePolicy;
    const bool isHistorical = request.params.sourcePolicy == DataPublishPolicy::AllowHistoricalResult;
    if (!isHistorical) {
        for (const auto& input : request.operation.inputs) {
            if (input.source == request.source->data->self) continue;
            DataExpectation expectation;
            expectation.kind = DataExpectationKind::EntityHead;
            expectation.entityId = input.source.entityId;
            expectation.expectedGeneration = input.source.generation;
            transaction.expectations.push_back(expectation);
        }
        DataExpectation sourceExpected;
        sourceExpected.kind = DataExpectationKind::EntityHead;
        sourceExpected.entityId = request.source->data->self.entityId;
        sourceExpected.expectedGeneration = request.source->data->self.generation;
        transaction.expectations.push_back(sourceExpected);
        if (request.source->binding) {
            sourceExpected = {};
            sourceExpected.kind = DataExpectationKind::Binding;
            sourceExpected.binding = request.source->binding->name;
            sourceExpected.expectedBindingRevision = request.source->binding->revision;
            sourceExpected.isTargetChecked = true;
            sourceExpected.expectedTarget = request.source->data->self;
            transaction.expectations.push_back(sourceExpected);
        }
    }
    const DataProvenance provenance{ std::string(featureId), "global-automatic-surface",
        std::to_string(stagedGeneration->algorithmRevision),
        stagedGeneration->canonicalParameters };
    std::vector<DataInputRef> generationInputs = request.operation.inputs;
    {
        if(!request.inputs.parent)transaction.outputs.push_back(
            {meshRef.entityId, 0, DataTypes::surfaceMesh, request.operation.inputs, mesh, provenance});
        generationInputs.push_back({"mesh", meshRef});
    }
    if (stagedGeneration->selection) {
        if(!prepared.selection||!prepared.selection->GetValid())return {};
        const DataRevisionRef selectionRef{m_data->CreateDataEntityId(),1};stagedGeneration->selectionMeshRevision=selectionRef;
        transaction.outputs.push_back({selectionRef.entityId,0,DataTypes::surfaceMesh,{{"measurement-parent",meshRef},{"analysis-roi",*request.params.analysisRoi}},std::move(prepared.selection),provenance});
        generationInputs.push_back({"selection-mesh",selectionRef});
    }
    stagedGeneration->execution.publicationPreparationMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - publicationStarted)
            .count();
    transaction.outputs.push_back({generationRef.entityId, 0, surfaceGenerationType,
        std::move(generationInputs),
        std::make_shared<const SurfaceGenerationPayload>(stagedGeneration,
            SurfaceGenerationPayload::Statistics{result.acceptedPointCount, result.lowContrastPointCount,
                result.rejectedPointCount, result.truncatedPointCount, result.nonManifoldObjectCount},result.prepared,std::move(prepared.display),std::move(prepared.displayError)), provenance});
    if (!isHistorical) transaction.bindings.push_back({ expected.name,
        expected.revision, true, expected.target, generationRef });

    const bool isVisible = GetState().isOverlayVisible;
    auto nextViews = request.views;
    const auto data = m_data;
    publicationStorage.Finish();
    auto committed = m_resultScopes.Commit(*data,std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded
        && committed.status != DataCommitStatus::SucceededHistorical) {
        return {};
    }
    // 从这里起正式结果已经有效；显示失败不回滚业务数据。
    const auto published = committed.published.back();
    m_resultRevision = std::max(m_resultRevision, stagedGeneration->resultRevision);
    if (isHistorical || requestId != m_latestRequestId) {
        if (isHistorical && m_isAttached && requestId == m_latestRequestId) {
            auto ready = GetState();
            ready.stage = SurfaceDeterminationStage::Ready; ready.progress01 = 1.0;
            ready.purpose = SurfaceTaskPurpose::Determine; ready.resultScope = request.params.resultScope;
            ready.resultRevision = stagedGeneration->resultRevision;
            ready.sourceRevision = stagedGeneration->sourceRevision;
            ready.isoEstimate = stagedGeneration->isoEstimate;
            ready.pointCount = stagedGeneration->points->size(); ready.objectCount = static_cast<std::uint32_t>(stagedGeneration->objects->size());
            ready.acceptedPointCount = result.acceptedPointCount; ready.rejectedPointCount = result.rejectedPointCount;
            SetState(std::move(ready));
        }
        return published;
    }
    if (!m_isAttached || !m_data || !request.completeActive
        || !request.completeActive->load()) return published;
    try {
        m_activeScope = request.params.resultScope;
        { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview.reset(); }
        m_previewDisplay.reset();
        m_previewSource.reset(); m_previewViews.clear();
        m_resultBinding = std::move(committed.bindings.back());
        m_store.SetGeneration(published);
        m_displayOperation = request.operation;
        m_displayOperation.status = FeatureRunStatus::Succeeded;
        m_displayOperation.progress = 1.0;
        m_displayOperation.stateRevision += 2;
        for (const auto& output : committed.published) m_displayOperation.outputs.push_back(output->self);
        (void)SetObjectPicking(false);RemoveBindings(m_bindings);
        m_displayData = nullptr;
        m_activeSource = request.source;
        m_activeViews = std::move(nextViews);
        m_resultRevision = std::max(m_resultRevision, stagedGeneration->resultRevision);
        SurfaceDeterminationState ready;
        ready.stage = SurfaceDeterminationStage::Ready;
        ready.requestId = requestId;
        ready.purpose = SurfaceTaskPurpose::Determine;
        ready.resultScope = request.params.resultScope;
        ready.sourceRevision = stagedGeneration->sourceRevision;
        ready.isoEstimate = stagedGeneration->isoEstimate;
        ready.resultRevision = stagedGeneration->resultRevision;
        ready.progress01 = 1.0;
        ready.pointCount = stagedGeneration->points->size();
        ready.acceptedPointCount = result.acceptedPointCount;
        ready.lowContrastPointCount = result.lowContrastPointCount;
        ready.rejectedPointCount = result.rejectedPointCount;
        ready.truncatedPointCount = result.truncatedPointCount;
        ready.objectCount = static_cast<std::uint32_t>(
            stagedGeneration->objects->size());
        ready.nonManifoldObjectCount = result.nonManifoldObjectCount;
        ready.isOverlayVisible = isVisible;
        // observer 可能已经接纳下一请求；旧提交不得覆盖新的 Preparing/requestId。
        if (m_latestRequestId == requestId) SetState(ready);
        m_stateBeforeRequest = std::move(ready);
        m_isDisplayPending = true;
        SetBindingProjection();
    }
    catch (...) { SetDisplayFailure(requestId); }
    return published;
}

FeatureDataContract SurfaceDeterminationHostFeature::GetDataContract() const
{
    return {{{"source-volume", DataFacets::scalarGrid3D, true},
             {"analysis-roi", DataFacets::roiGeometry, false},
             {"material-labels", DataFacets::labelMap3D, false},
             {"initial-surface", DataFacets::surfaceMesh, false}},
            {{"mesh", DataTypes::surfaceMesh, {DataFacets::surfaceMesh}},
             {"generation", surfaceGenerationType, {surfaceGenerationFacet}}}};
}

std::vector<FeatureOperationState> SurfaceDeterminationHostFeature::GetOperationStates() const
{
    return m_impl ? m_impl->GetOperationStates() : std::vector<FeatureOperationState>{};
}

SurfaceDeterminationHostFeature::SurfaceDeterminationHostFeature(
    SurfaceDeterminationConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

SurfaceDeterminationHostFeature::~SurfaceDeterminationHostFeature() noexcept =
    default;

std::string_view
SurfaceDeterminationHostFeature::GetFeatureId() const noexcept
{
    return featureId;
}

bool SurfaceDeterminationHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl && m_impl->AttachHost(context);
}

bool SurfaceDeterminationHostFeature::DetachHost()
{
    return !m_impl || m_impl->DetachHost();
}

bool SurfaceDeterminationHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

SurfaceDeterminationAdmission
SurfaceDeterminationHostFeature::SendRequest(
    SurfaceDeterminationRequest request,
    SurfaceDeterminationCallback onComplete)
{
    return m_impl
        ? m_impl->SendRequest(
            std::move(request), std::move(onComplete))
        : SurfaceDeterminationAdmission{
            SurfaceAdmissionStatus::Unavailable, 0 };
}

SurfaceDeterminationState
SurfaceDeterminationHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : SurfaceDeterminationState{};
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetSurfaceSnapshot() const
{
    return m_impl ? m_impl->GetSurfaceSnapshot() : nullptr;
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetSurfaceSnapshot(const std::string_view scope) const
{ return m_impl ? m_impl->GetSurfaceSnapshot(scope) : nullptr; }

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetSurfaceSnapshot(const DataRevisionRef revision) const
{ return m_impl ? m_impl->GetSurfaceSnapshot(revision) : nullptr; }

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetPreviewSnapshot() const
{ return m_impl ? m_impl->GetPreviewSnapshot() : nullptr; }

SurfacePointDiagnostic SurfaceDeterminationHostFeature::GetPointDiagnostic(
    const DataRevisionRef revision, const std::uint64_t pointIndex) const
{
    return m_impl ? m_impl->GetPointDiagnostic(revision, pointIndex) : SurfacePointDiagnostic{};
}

SurfaceRestoreState SurfaceDeterminationHostFeature::GetResultValidity(const DataRevisionRef revision) const
{
    return m_impl ? m_impl->GetResultValidity(revision) : SurfaceRestoreState{};
}
