#include "../GapParentRecord.h"
#include "../../../common/FeatureInputReads.h"
#include "Host/GapHostFeature.h"
#include "Render/Support/AnalysisColorStyle.h"

#include "App/Services/FeatureViewService.h"
#include "Data/DataPayloads.h"
#include "Services/GapAnalysisService.h"


#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <iomanip>
#include <locale>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "GapAnalysis";
constexpr std::string_view gapResultBinding =
    "analysis.gaps.active";
const DataTypeId gapVoidTableType{
    "org.mvvcvtk.gap-analysis.void-table", 1 };
const DataTypeId gapStatisticsType{
    "org.mvvcvtk.gap-analysis.statistics", 1 };
const DataTypeId gapResultSetType{
    "org.mvvcvtk.gap-analysis.result-set", 1 };
const DataFacetId gapVoidRecordsFacet{ "tabular-gap-void-records" };
const DataFacetId gapStatisticsFacet{ "tabular-gap-statistics" };

GapHostResult BuildHostResult(
    const GapHostState& state,
    const GapResultStatus status,
    std::string message)
{
    GapHostResult result;
    result.status = status;
    result.commitId = state.commitId;
    result.sourceRevision = state.sourceRevision;
    result.labelMap = state.labelMap;
    result.voidTable = state.voidTable;
    result.voidMesh = state.voidMesh;
    result.statisticsData = state.statisticsData;
    result.resultSet = state.resultSet;
    result.statistics = state.statistics;
    result.message = std::move(message);
    return result;
}

} // namespace

class GapHostFeature::Impl final {
public:
    struct CompleteItem final {
        std::mutex mutex;
        GapHostCallback onComplete;
        std::uint64_t requestId = 0;
        std::optional<GapHostResult> result;
        bool isActive = true;
        bool isQueued = false;
        bool isSent = false;
    };

    struct ViewCandidate final {
        GapViewRequest request;
        std::vector<std::string> activeViewIds;
        VtkImageGridSnapshot source;
        DataBinding resultBinding;
        std::string canonicalParameters;
        std::vector<DataInputRef> inputs;
    };

    explicit Impl(GapHostConfig config)
        : m_config(std::move(config))
        , m_service(std::make_unique<GapAnalysisService>())
    {
    }

    bool AttachHost(
        GapHostFeature& owner,
        const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    GapAdmission SendRequest(
        GapHostRequest request,
        GapHostCallback onComplete);
    GapHostState GetState() const;
    std::optional<GapDisplayParams> GetDisplay(const DataRevisionRef& ref) const {
        if(!GetOwnerThread() || !m_data)return {};
        const auto graph=m_data->GetDataGraph();if(!graph.view || !graph.view->GetData(ref))return {};
        const auto i=m_resultDisplays.find(ref);return i==m_resultDisplays.end() ? std::nullopt:std::optional<GapDisplayParams>(i->second);
    }
    std::vector<FeatureOperationState> GetOperationStates() const;

    static constexpr std::string_view FeatureId =
        "GapAnalysis";

private:
    static bool GetImageReady(vtkImageData* image);
    static bool GetChordValid(const HostKeyChord& chord);
    static bool GetStartValid(
        const GapHostStartParams& params);
    static bool GetSnapshotValid(
        const VtkImageGridSnapshot& snapshot);
    static std::optional<Orientation> GetSliceOrient(
        HostRenderViewRole role);
    static bool SendComplete(
        const std::shared_ptr<CompleteItem>& item,
        GapHostResult result);
    bool QueueComplete(
        const std::shared_ptr<CompleteItem>& item);
    bool SendSceneDelta(FeatureScenePriority priority);
    std::uint64_t GetNextSceneRequestId() noexcept;

    std::optional<ViewCandidate> GetViewCandidate(
        const GapHostStartParams& start) const;
    bool StartView(
        const GapHostStartParams& start,
        GapHostCallback onComplete);
    bool SwitchOverlay();
    bool ExitView();
    bool SetDataTypes();
    bool GetSourceSame() const;
    bool SetCompletedResult(const GapAnalysisResult& candidate);
    bool ClearResultBinding();
    bool ClearResultScopes();
    bool GetResultCurrent() const;
    void SetBindingStale();
    void SetFailedResult(GapResultStatus status, std::string message);
    InteractionResult OnInput(
        const InteractionEvent& event);
    bool SetActiveViews(
        const std::vector<std::string>& viewIds) const;
    bool ClearComplete();
    bool ClearBorrowed();
    bool GetOwnerThread() const;

    GapHostConfig m_config;
    std::unique_ptr<GapAnalysisService> m_service;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::shared_ptr<CompleteItem> m_completeItem;
    std::shared_ptr<CompleteItem> m_exitComplete;
    std::optional<RenderInputStamp> m_activeInput;
    std::vector<std::string> m_activeViewIds;
    VtkImageGridSnapshot m_requestSource;
    DataBinding m_requestResultBinding;
    DataBinding m_resultBinding;
    std::vector<DataLifetimeRetirement> m_resultScopes;
    bool m_isClosing=false;
    GapHostState m_state;
    std::map<DataRevisionRef,GapDisplayParams> m_resultDisplays;
    std::optional<GapDisplayParams> m_inheritedDisplay;
    FeatureOperationState m_operation;
    std::string m_requestParameters;
    bool m_hasScenePending = false;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextSceneRequestId = 1;
    std::uint64_t m_activeRequestId = 0;
    bool m_isExitPending = false;
    bool m_isRequestPending = false;
    bool m_isInputAttached = false;
    bool m_isAttached = false;
};

bool GapHostFeature::Impl::GetImageReady(
    vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }
    int dimensions[3] = {};
    image->GetDimensions(dimensions);
    return dimensions[0] > 0
        && dimensions[1] > 0
        && dimensions[2] > 0;
}

bool GapHostFeature::Impl::GetChordValid(
    const HostKeyChord& chord)
{
    return chord.keyCode != 0 || !chord.keySym.empty();
}

bool GapHostFeature::Impl::GetStartValid(
    const GapHostStartParams& params)
{
    if (params.targetViews.viewIds.empty()
        && params.targetViews.viewRoles.empty()) {
        return false;
    }
    if (!std::isfinite(params.surface.dataRangeRatio)
        || !std::isfinite(params.surface.absoluteIsoValue)
        || !std::isfinite(params.surface.backgroundMean)
        || !std::isfinite(params.surface.materialMean)
        || !std::isfinite(params.voidParams.minVolumeMM3)) {
        return false;
    }

    switch (params.surface.isoMode) {
    case GapIsoMode::DataRangeRatio:
        if (params.surface.dataRangeRatio < 0.0
            || params.surface.dataRangeRatio > 1.0) {
            return false;
        }
        break;
    case GapIsoMode::AbsoluteValue:
        break;
    default:
        return false;
    }

    return params.surface.backgroundMean
            <= params.surface.materialMean
        && params.voidParams.minVolumeMM3 >= 0.0
        && params.voidParams.minVolumeMM3
            <= static_cast<double>(
                (std::numeric_limits<float>::max)())
        && (params.voidParams.minVolumeMM3 == 0.0
            || params.voidParams.minVolumeMM3
                >= static_cast<double>(
                    (std::numeric_limits<float>::denorm_min)()));
}

bool GapHostFeature::Impl::GetSnapshotValid(
    const VtkImageGridSnapshot& snapshot)
{
    const auto* payload = snapshot && snapshot->data
        ? dynamic_cast<const ImageGrid3DPayload*>(
            snapshot->data->payload.get())
        : nullptr;
    if (!snapshot || !snapshot->data || !snapshot->binding
        || !snapshot->binding->target
        || *snapshot->binding->target != snapshot->data->self
        || !payload || !payload->GetValid()
        || !GetImageReady(snapshot->image)
        || payload->GetComponentCount() != 1) {
        return false;
    }

    const auto& geometry = payload->GetGeometry();
    int dimensions[3] = {};
    snapshot->image->GetDimensions(dimensions);
    for (int axis = 0; axis < 3; ++axis) {
        if (dimensions[axis] != geometry.dimensions[axis]) {
            return false;
        }
    }
    return true;
}

std::optional<Orientation>
GapHostFeature::Impl::GetSliceOrient(
    const HostRenderViewRole role)
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

bool GapHostFeature::Impl::SendComplete(
    const std::shared_ptr<CompleteItem>& item,
    GapHostResult result)
{
    if (!item) {
        return false;
    }

    GapHostCallback callback;
    {
        const std::lock_guard<std::mutex> lock(item->mutex);
        if (!item->isActive || item->isSent) {
            return false;
        }
        item->isSent = true;
        callback = std::move(item->onComplete);
    }
    if (callback) {
        result.requestId = item->requestId;
        try {
            callback(std::move(result));
        }
        catch (...) {
        }
    }
    return true;
}

bool GapHostFeature::Impl::QueueComplete(
    const std::shared_ptr<CompleteItem>& item)
{
    if (!item) return false;
    {
        const std::lock_guard<std::mutex> lock(item->mutex);
        if (!item->isActive || item->isQueued || item->isSent || !item->result) return false;
        item->isQueued = true;
    }
    const auto expectedInput = m_activeInput;
    const auto expectedViews = m_activeViewIds;
    const std::weak_ptr<TrustedDataPort> weakData = m_data;
    const std::weak_ptr<FeatureViewDirectory> weakViews = m_views;
    const auto send = [item, expectedInput, expectedViews, weakData, weakViews]() noexcept {
        GapHostResult result;
        {
            const std::lock_guard<std::mutex> lock(item->mutex);
            if (!item->result) return;
            result = *item->result;
        }
        if (result.status == GapResultStatus::Succeeded && expectedInput) {
            const auto data = weakData.lock();
            const auto source = data ? data->GetPrimaryImage() : nullptr;
            const auto views = weakViews.lock();
            if (!source || !source->data
                || source->data->self != expectedInput->dataRevision) {
                result.status = GapResultStatus::SourceChanged;
                result.message = "Gap source changed before rendered completion.";
            }
            else if (!views || expectedViews.empty()
                || !std::all_of(expectedViews.begin(), expectedViews.end(),
                    [&views, &expectedInput](const auto& viewId) {
                        const auto port = views->GetFeaturePort(viewId);
                        const auto stamp = port ? port->GetRenderInputStamp()
                            : std::optional<RenderInputStamp>{};
                        return stamp && *stamp == *expectedInput;
                    })) {
                result.status = GapResultStatus::SucceededWithDisplayFailure;
                result.message = "Gap target changed before rendered completion.";
            }
        }
        (void)SendComplete(item, std::move(result));
    };
    if (!m_host || !m_host->SendOwnerComplete(send)) {
        {
            const std::lock_guard<std::mutex> lock(item->mutex);
            item->result->status = GapResultStatus::Failed;
            item->result->message = "Gap completion cancelled while stopping.";
        }
        send();
        return false;
    }
    return true;
}

bool GapHostFeature::Impl::SendSceneDelta(
    const FeatureScenePriority priority)
{
    if (!m_host || !m_activeInput || m_activeRequestId == 0
        || m_activeViewIds.empty()) {
        return false;
    }
    FeatureSceneDelta delta;
    delta.viewIds = m_activeViewIds;
    delta.inputStamp = *m_activeInput;
    delta.requestId = GetNextSceneRequestId();
    delta.priority = priority;
    delta.scope = FeatureSceneScope::RequiredAllViews;
    delta.hasDisplayUpdate = true;
    if (GetDataRevisionRefValid(m_state.sourceRevision))
        delta.inputs = { { "source-volume", m_state.sourceRevision } };
    if (m_service->GetDisplayOn() && GetDataRevisionRefValid(m_state.resultSet)) {
        delta.inputs.push_back({ "labels", m_state.labelMap });
        delta.inputs.push_back({ "void-surface", m_state.voidMesh });
        for (const auto& viewId : m_activeViewIds)
            delta.displays.push_back({ viewId, std::string(featureId), "gaps",
                m_state.resultSet, m_operation.operation });
    }
    m_hasScenePending = !m_host->SendSceneDelta(std::move(delta));
    return !m_hasScenePending;
}

std::uint64_t GapHostFeature::Impl::GetNextSceneRequestId() noexcept
{
    const std::uint64_t requestId = m_nextSceneRequestId++;
    if (m_nextSceneRequestId == 0) m_nextSceneRequestId = 1;
    return requestId == 0 ? GetNextSceneRequestId() : requestId;
}

std::optional<GapHostFeature::Impl::ViewCandidate>
GapHostFeature::Impl::GetViewCandidate(
    const GapHostStartParams& start) const
{
    if (!m_views
        || !m_data
        || !GetStartValid(start)) {
        return std::nullopt;
    }

    auto snapshot = m_data->GetPrimaryImage();
    if (!GetSnapshotValid(snapshot)) {
        return std::nullopt;
    }
    if (GetDataEntityIdValid(snapshot->data->lifetimeScope)) {
        const auto lifetime=snapshot->data->lifetime.lock();
        auto lease=lifetime?lifetime->StartResourceUse(snapshot->data->self,"gap-analysis-reader"):nullptr;
        if (!lease) return std::nullopt;
        auto retained=std::make_shared<std::pair<VtkImageGridSnapshot,std::shared_ptr<const DataResourceLease>>>(snapshot,std::move(lease));
        auto input=std::make_shared<VtkImageGridView>(*snapshot);
        input->data=DataSnapshot(retained,retained->first->data.get());
        snapshot=std::move(input);
    }
    const auto views = m_views->GetViews(start.targetViews);
    if (views.empty()) {
        return std::nullopt;
    }

    ViewCandidate candidate;
    candidate.source = snapshot;
    candidate.request.graphInput = snapshot;
    candidate.resultBinding.name = std::string(gapResultBinding);
    if (const auto binding = m_data->GetDataBinding(
            snapshot->graph, gapResultBinding)) {
        candidate.resultBinding = *binding;
    }
    candidate.request.surface = start.surface;
    candidate.request.voidParams = start.voidParams;
    candidate.request.roiPolicy=start.roiPolicy;candidate.request.isEvaluationOnly=start.isEvaluationOnly;
    candidate.inputs={{"source-volume",snapshot->data->self}};
    for (const auto& role:std::array<std::pair<const char*,std::optional<DataRevisionRef>>,2>{{{"evaluation-roi",start.evaluationRoi},{"detection-roi",start.detectionRoi}}}) {
        if (!role.second) continue;const auto resolved=m_data->GetRoi(snapshot->graph,*role.second,snapshot->data->self);
        if (resolved.error!=RoiError::None || !resolved.roi) return std::nullopt;
        if (std::string_view(role.first)=="evaluation-roi") candidate.request.evaluationRoi=resolved.roi;else candidate.request.detectionRoi=resolved.roi;
        candidate.inputs.push_back({role.first,*role.second});
        for (const auto& dependency:resolved.roi->GetDependencies()) if (dependency!=*role.second && dependency!=snapshot->data->self)
            candidate.inputs.push_back({std::string(role.first)+"-dependency-"+std::to_string(candidate.inputs.size()),dependency});
    }
    if(start.parentResult){
        if(!start.isEvaluationOnly)return std::nullopt;
        const auto node=m_data->GetData(snapshot->graph,*start.parentResult);const auto* collection=node?dynamic_cast<const DataCollectionPayload*>(node->payload.get()):nullptr;if(!collection || collection->GetDataType()!=gapResultSetType)return std::nullopt;
        const auto get=[&](std::string_view role)->DataSnapshot{for(const auto& entry:collection->GetItems())if(entry.role==role)return m_data->GetData(snapshot->graph,entry.data);return {};};
        const auto recordNode=get("parent-detection"),labelNode=get("parent-labels");const auto record=recordNode?std::dynamic_pointer_cast<const GapParentRecord>(recordNode->payload):nullptr;
        if(!record || !record->GetValid() || record->source!=snapshot->data->self || !labelNode || !std::dynamic_pointer_cast<const LabelMap3DPayload>(labelNode->payload) || std::dynamic_pointer_cast<const LabelMap3DPayload>(labelNode->payload)->GetValues()!=record->labels->GetValues())return std::nullopt;
        const auto labels=m_data->GetLabelMap(snapshot->graph,labelNode->self);if(!labels || !labels->labels || labels->labels->GetScalarType()!=VTK_INT)return std::nullopt;
        auto parent=std::make_shared<GapAnalysisResult>();parent->sourceRevision=record->source;parent->detectionParameters=record->parameters;parent->statistics=record->statistics;parent->voids=*record->voids;parent->labelImage=labels->labels;parent->isSucceeded=true;candidate.request.savedParent=std::move(parent);
        candidate.inputs.push_back({"parent-result",*start.parentResult});candidate.inputs.push_back({"measurement-parent-detection",recordNode->self});candidate.inputs.push_back({"measurement-parent-labels",labelNode->self});
    }
    std::vector<DataRevisionRef> refs;for(const auto& input:candidate.inputs)refs.push_back(input.source);auto readers=FeatureInternal::StartInputReads(snapshot->graph,refs,"gap-roi-parent-reader");if(!readers)return std::nullopt;
    auto retained=std::make_shared<std::pair<VtkImageGridSnapshot,FeatureInternal::InputReads>>(snapshot,std::move(*readers));auto scoped=std::make_shared<VtkImageGridView>(*snapshot);scoped->data=DataSnapshot(retained,retained->first->data.get());candidate.source=scoped;candidate.request.graphInput=scoped;
    std::ostringstream parameters;
    parameters.imbue(std::locale::classic());
    parameters << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "isoMode=" << static_cast<int>(start.surface.isoMode)
        << ";ratio=" << start.surface.dataRangeRatio << ";absoluteIso=" << start.surface.absoluteIsoValue
        << ";background=" << start.surface.backgroundMean << ";material=" << start.surface.materialMean
        << ";filter=" << start.voidParams.isFilterEnabled << ";minVolumeMM3=" << start.voidParams.minVolumeMM3
        << ";roiPolicy=" << int(start.roiPolicy) << ";evaluationOnly=" << start.isEvaluationOnly;
    candidate.canonicalParameters = parameters.str();

    for (const auto& view : views) {
        const auto port = m_views->GetOverlayPort(view.id);
        if (view.id.empty() || !port) {
            continue;
        }
        candidate.activeViewIds.push_back(view.id);
        candidate.request.viewIds[port.get()]=view.id;
        if (view.role
                == HostRenderViewRole::Primary3D
            || view.role
                == HostRenderViewRole::Composite3D) {
            candidate.request.meshTargets.push_back(
                port);
            continue;
        }
        if (const auto orientation =
                GetSliceOrient(view.role)) {
            candidate.request.sliceTargets.push_back(
                { *orientation, port });
        }
    }

    if (candidate.request.meshTargets.empty()
        && candidate.request.sliceTargets.empty()) {
        return std::nullopt;
    }
    return candidate;
}

bool GapHostFeature::Impl::AttachHost(
    GapHostFeature& owner,
    const HostFeatureContext& context)
{
    if (m_isAttached) {
        return false;
    }
    if (!context.views
        || !context.data
        || !context.host
        || (m_config.inputViews.viewIds.empty()
            && m_config.inputViews.viewRoles.empty())
        || !GetChordValid(m_config.keys.switchOverlay)
        || !GetChordValid(m_config.keys.exit)
        || !GetStartValid(m_config.defaultStart)) {
        return false;
    }

    const auto weakOwner = owner.weak_from_this();
    if (weakOwner.expired()) {
        return false;
    }

    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_service->SetWorkAvailable(
        [weakHost = std::weak_ptr<FeatureHostControl>(context.host)] {
            if (const auto host = weakHost.lock())
                (void)host->SendWorkAvailable();
        });
    m_ownerThread = std::this_thread::get_id();
    if (!SetDataTypes()) {
        ClearBorrowed();
        return false;
    }

    HostInputBinding binding;
    binding.featureId = std::string(FeatureId);
    binding.targetViews = m_config.inputViews;
    const auto toBinding=[](const HostKeyChord& chord) {
        InteractionEvent e;e.keyCode=chord.keyCode;e.keySym=chord.keySym;
        InputBinding input{InputTriggerKind::KeyPress,InputMouseButton::None};input.key=e.keyCode?static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(e.keyCode))):e.keySym=="Escape"?27:e.keySym=="F9"?static_cast<std::uint32_t>(InputKey::F9):e.keySym.size()==1?static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(e.keySym[0]))):0;
        input.requiredModifiers=(chord.isShiftDown?1:0)|(chord.isCtrlDown?2:0)|(chord.isAltDown?4:0);
        input.forbiddenModifiers=static_cast<std::uint8_t>(7&~input.requiredModifiers);return input;
    };
    InteractionActionInfo toggle;toggle.actionId="gap.toggleOverlay";toggle.isTriggerable=true;toggle.ownerId=binding.featureId;
    toggle.bindings.push_back({"gap.toggleOverlay.key",toBinding(m_config.keys.switchOverlay),true,{},"",{InputTriggerKind::KeyPress}});
    InteractionActionInfo exit;exit.actionId="gap.exit";exit.isTriggerable=true;exit.ownerId=binding.featureId;
    exit.bindings.push_back({"gap.exit.key",toBinding(m_config.keys.exit),true,{},"",{InputTriggerKind::KeyPress}});
    binding.actions={std::move(toggle),std::move(exit)};
    binding.onAction=[weakOwner](std::string_view id,const InteractionEvent& e,const std::optional<HostSemanticTarget>&) {
        const auto f=weakOwner.lock();if(!f)return InteractionResult{};auto event=e;event.actionId=std::string(id);return f->m_impl->OnInput(event);
    };
    binding.onTrigger=[weakOwner](std::string_view id,const ActionContext&) {
        const auto f=weakOwner.lock();if(!f)return ActionTriggerResult{};auto& p=*f->m_impl;GapHostRequest request;
        if(id=="gap.exit"){if(!p.m_service->GetViewOn()||p.m_isExitPending)return ActionTriggerResult{};request.action=GapHostAction::Exit;}
        else if(id=="gap.toggleOverlay") {if(p.m_service->GetViewOn()&&!p.m_isExitPending)request.action=GapHostAction::Overlay;else{request.action=GapHostAction::Start;request.start=p.m_config.defaultStart;}}
        else return ActionTriggerResult{};
        const auto admission=p.SendRequest(std::move(request),nullptr);
        return ActionTriggerResult{admission.status==GapAdmissionStatus::Accepted?ActionTriggerStatus::Accepted:ActionTriggerStatus::Failed,admission.requestId};
    };
    if (!m_host->AttachInput(std::move(binding))) {
        ClearBorrowed();
        return false;
    }

    m_isInputAttached = true;
    m_isAttached = true;m_isClosing=false;
    m_isExitPending = false;
    m_isRequestPending = false;
    m_state = {};m_resultDisplays.clear();m_inheritedDisplay.reset();
    m_operation = {};
    m_hasScenePending = false;
    return true;
}

bool GapHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) {
        return true;
    }
    if (!GetOwnerThread()) {
        return false;
    }

    bool isInputDetached = !m_isInputAttached;
    if (m_isInputAttached && m_host) {
        try {
            isInputDetached =
                m_host->DetachInput(FeatureId);
        }
        catch (...) {
            isInputDetached = false;
        }
        if (isInputDetached) {
            m_isInputAttached = false;
        }
    }

    m_isClosing=true;
    ClearComplete();
    if (m_service) {
        m_service->StopAsync();
        // DefX has no in-call cancellation. Retain the Feature and retry instead of blocking owner join.
        if (m_service->GetAnalysisState()==GapAnalysisState::Running) return false;
        m_service->ClearView();
    }
    m_requestSource.reset();
    if (!SetActiveViews({}) || !ClearResultScopes()) return false;
    m_requestResultBinding={};m_state={};m_isRequestPending=false;
    m_isExitPending=false;
    if (!isInputDetached) return false;

    m_isAttached = false;
    ClearBorrowed();
    if (auto exit = std::move(m_exitComplete))
        SendComplete(exit, BuildHostResult(m_state, GapResultStatus::Failed, "Gap exit cancelled by detach."));
    return true;
}

bool GapHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return false;
    }

    if (m_isClosing) {
        m_service->StopAsync();
        if (m_service->GetAnalysisState()!=GapAnalysisState::Running) {
            m_service->ClearView();m_requestSource.reset();
            if (SetActiveViews({}) && ClearResultScopes()) {
                m_state={};m_requestResultBinding={};m_isRequestPending=false;
                if (auto exit = std::move(m_exitComplete))
                    SendComplete(exit, BuildHostResult(m_state, GapResultStatus::Failed, "Gap exit cancelled while stopping."));
            }
        }
        // Only the registry's Detach retry revokes Host ports and ends the attachment.
        return true;
    }
    SetBindingStale();
    if (m_service->GetDisplayTickNeeded()) {
        m_service->OnDisplayTick(nullptr);
    }
    if (m_hasScenePending) (void)SendSceneDelta(FeatureScenePriority::Overlay);

    if (m_isRequestPending) {
        const auto analysisState = m_service->GetAnalysisState();
        if (analysisState == GapAnalysisState::Succeeded) {
            GapAnalysisResult candidate;
            if (m_service->GetCompletedResult(candidate)) {
                (void)SetCompletedResult(candidate);
            }
            if(m_isRequestPending) {
                // 成功终态只能发布或明确失败，不能消耗完成唤醒后仍无限等待。
                SetFailedResult(GapResultStatus::Failed,"Gap completed result could not be published.");
            }
        }
        else if (analysisState == GapAnalysisState::Failed
            || analysisState == GapAnalysisState::Idle) {
            SetFailedResult(
                GapResultStatus::Failed,
                "Gap analysis failed before data publication.");
        }
    }

    if (!m_isRequestPending
        && !m_isExitPending
        && GetDataRevisionRefValid(m_state.resultSet)
        && m_data) {
        const auto graph = m_data->GetDataGraph();
        if (m_data->GetDataRelation(
                graph,
                m_state.resultSet,
                "source-volume",
                primaryVolumeBinding)
            == DataRelationStatus::OutOfDateRelativeToCurrentBinding) {
            (void)ClearResultBinding();
            m_state.analysisState = GapAnalysisState::Stale;
            ClearComplete();
            if (m_service->ExitView()) {
                m_isExitPending = true;
            }
        }
    }

    if (m_isExitPending
        && !m_service->GetDisplayTickNeeded()) {
        if (SetActiveViews({})) {
            m_isExitPending = false;
            ClearComplete();
            if (auto exit = std::move(m_exitComplete))
                SendComplete(exit, BuildHostResult(m_state, GapResultStatus::Succeeded, {}));
        }
    }
    return true;
}

GapAdmission GapHostFeature::Impl::SendRequest(
    GapHostRequest request,
    GapHostCallback onComplete)
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return {GapAdmissionStatus::Unavailable, 0};
    }

    if (m_isClosing) return {GapAdmissionStatus::Stopping, 0};
    if (request.display && request.action != GapHostAction::SetDisplay) return {};
    SetBindingStale();
    std::shared_ptr<CompleteItem> control;
    if (request.action != GapHostAction::Start && request.action!=GapHostAction::Evaluate && onComplete) {
        try { control = std::make_shared<CompleteItem>(); }
        catch (...) { return {GapAdmissionStatus::Unavailable, 0}; }
        control->onComplete = std::move(onComplete);
    }
    bool succeeded = false;
    switch (request.action) {
    case GapHostAction::Start:
    case GapHostAction::Evaluate:
        if (!request.start) return {};
        if (request.action==GapHostAction::Evaluate) request.start->isEvaluationOnly=true;
        if (m_isRequestPending || m_isExitPending) return {GapAdmissionStatus::Busy, 0};
        {
            const auto parent=request.start->parentResult.value_or(m_state.resultSet);
            const auto inherited=request.action==GapHostAction::Evaluate ? GetDisplay(parent):std::nullopt;
            if (!StartView(*request.start, std::move(onComplete))) return {};
            m_inheritedDisplay=inherited;
        }
        return {GapAdmissionStatus::Accepted, m_activeRequestId};
    case GapHostAction::Overlay:
        if (request.start) return {};
        succeeded = SwitchOverlay();
        break;
    case GapHostAction::Exit:
        if (request.start) return {};
        succeeded = ExitView();
        break;
    case GapHostAction::SetDisplay:
        if (m_isRequestPending || m_isExitPending) return {GapAdmissionStatus::Busy, 0};
        if (!request.display || request.start || !GetResultCurrent() || request.displayResult!=m_state.resultSet) return {};
        {
            try {
                auto saved=*request.display;AnalysisColorStyle::SetTailColors(saved);
                std::map<DataRevisionRef,GapDisplayParams> candidate;candidate.emplace(request.displayResult,std::move(saved));
                succeeded=m_service->SetDisplay(*request.display,[this]{return SendSceneDelta(FeatureScenePriority::Overlay);});
                if(succeeded){m_resultDisplays.erase(request.displayResult);m_resultDisplays.insert(candidate.extract(candidate.begin()));}
            } catch(...) { return {GapAdmissionStatus::Unavailable,0}; }
        }
        break;
    case GapHostAction::None:
        return {};
    }
    if (!succeeded) return {};
    const GapAdmission admission{GapAdmissionStatus::Accepted, GetNextSceneRequestId()};
    if (control) {
        control->requestId = admission.requestId;
        if (!m_isAttached || m_isClosing || !GetOwnerThread())
            SendComplete(control, BuildHostResult(m_state, GapResultStatus::Failed, "Gap control cancelled during dispatch."));
        else if (request.action == GapHostAction::Exit) m_exitComplete = std::move(control);
        else SendComplete(control, BuildHostResult(m_state, GapResultStatus::Succeeded, {}));
    }
    return admission;
}

GapHostState GapHostFeature::Impl::GetState() const
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return {};
    }

    auto state = m_state;
    if (GetDataRevisionRefValid(state.resultSet) && !GetResultCurrent()) {
        state.analysisState = GapAnalysisState::Stale;
    }
    state.isViewActive = m_service->GetViewOn();
    state.isExitPending = m_isExitPending;
    state.isOverlayVisible = m_service->GetDisplayOn();
    state.display = m_service->GetDisplayParams();
    state.displayRange=m_service->GetDisplayRange();
    return state;
}

std::vector<FeatureOperationState> GapHostFeature::Impl::GetOperationStates() const
{
    if (!m_isAttached || !GetOwnerThread() || !m_service
        || m_operation.operation.requestId == 0) return {};
    auto state = m_operation;
    if (m_isRequestPending) {
        const auto execution = m_service->GetExecutionState();
        state.status = execution.status;
        state.progress = execution.progress;
        state.stateRevision = execution.stateRevision + 1;
    }
    return { std::move(state) };
}

bool GapHostFeature::Impl::StartView(
    const GapHostStartParams& start,
    GapHostCallback onComplete)
{
    auto candidate = GetViewCandidate(start);
    if (!candidate) {
        return false;
    }

    auto completeItem =
        std::make_shared<CompleteItem>();
    completeItem->onComplete = std::move(onComplete);
    const bool isAccepted = m_service->StartView(
        std::move(candidate->request));
    if (!isAccepted) {
        return false;
    }
    if (!SetActiveViews(candidate->activeViewIds)) {
        m_service->ClearView();
        ClearComplete();
        m_requestSource.reset();
        m_requestResultBinding = {};
        m_isRequestPending = false;
        m_isExitPending = false;
        return false;
    }

    ClearComplete();
    m_completeItem = std::move(completeItem);
    m_activeInput = RenderInputStamp{ candidate->source->data->self };
    m_activeViewIds = std::move(candidate->activeViewIds);
    m_activeRequestId = GetNextSceneRequestId();
    m_completeItem->requestId = m_activeRequestId;
    m_requestSource = std::move(candidate->source);
    m_requestResultBinding = std::move(candidate->resultBinding);
    m_requestParameters = std::move(candidate->canonicalParameters);
    m_state = {};
    m_state.analysisState = GapAnalysisState::Running;
    m_state.sourceRevision = m_requestSource->data->self;
    m_operation = {};
    m_operation.operation = { std::string(featureId), m_host->GetAttachmentId(), m_activeRequestId };
    m_operation.inputs = std::move(candidate->inputs);
    m_operation.status = FeatureRunStatus::Preparing;
    m_operation.stateRevision = m_service->GetExecutionState().stateRevision + 1;
    m_isRequestPending = true;
    m_isExitPending = false;
    // StartView 已卸载上一轮资源；本次尚未发布结果，提交空展示描述。
    (void)SendSceneDelta(FeatureScenePriority::Overlay);
    return true;
}

bool GapHostFeature::Impl::SwitchOverlay()
{
    const bool isSwitched = !m_isExitPending
        && m_service
        && m_service->GetViewOn()
        && m_service->SwitchOverlay();
    return isSwitched
        && SendSceneDelta(FeatureScenePriority::Overlay);
}

bool GapHostFeature::Impl::ExitView()
{
    if (m_isExitPending
        || !m_service
        || !m_service->GetViewOn()) {
        return false;
    }
    if (!ClearResultBinding()) return false;
    const bool isExited = m_service->ExitView();
    if (!isExited) return false;
    if (m_isRequestPending) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap analysis was cancelled before publication.");
        m_operation.status = FeatureRunStatus::Cancelled;
        ++m_operation.stateRevision;
    }
    const bool isDeltaSent = SendSceneDelta(
        FeatureScenePriority::Overlay);
    ClearComplete();
    m_isExitPending = true;
    return isDeltaSent;
}

bool GapHostFeature::Impl::SetDataTypes()
{
    if (!m_data) return false;
    const auto setType = [this](
        const DataTypeId& type,
        DataTypeDescriptor descriptor) {
        const auto graph = m_data->GetDataGraph();
        return (graph.view
                && !graph.view->GetDataFacets(type).empty())
            || m_data->SetDataType(std::move(descriptor));
    };
    return setType(gapParentRecordType,DataTypeDescriptor{gapParentRecordType,{gapParentRecordFacet},[](const IDataPayload& payload,std::string& message){const auto* parent=dynamic_cast<const GapParentRecord*>(&payload);if(parent && parent->GetValid())return true;message="Invalid full parent detection record.";return false;}})
        && setType(
            gapVoidTableType,
            GetRecordTableDescriptor(
                gapVoidTableType,
                { DataFacets::tabularRecords, gapVoidRecordsFacet }))
        && setType(
            gapStatisticsType,
            GetRecordTableDescriptor(
                gapStatisticsType,
                { DataFacets::tabularRecords, gapStatisticsFacet }))
        && setType(
            gapResultSetType,
            GetDataCollectionDescriptor(gapResultSetType));
}

bool GapHostFeature::Impl::GetSourceSame() const
{
    if (!m_data || !m_requestSource || !m_requestSource->data
        || !m_requestSource->binding) {
        return false;
    }
    const auto current = m_data->GetPrimaryImage();
    return current && current->data && current->binding
        && current->data->self == m_requestSource->data->self
        && current->binding->revision
            == m_requestSource->binding->revision;
}

bool GapHostFeature::Impl::SetCompletedResult(
    const GapAnalysisResult& candidate)
{
    if (!m_isRequestPending || !m_data || !m_requestSource
        || !m_requestSource->data || !m_requestSource->binding) {
        return false;
    }
    const auto* sourcePayload = dynamic_cast<const ImageGrid3DPayload*>(
        m_requestSource->data->payload.get());
    if (!candidate.isSucceeded || !sourcePayload
        || !sourcePayload->GetValid()) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap result candidate is invalid.");
        return false;
    }

    if (!GetSourceSame()) {
        SetFailedResult(GapResultStatus::SourceChanged,
            "Gap source changed before publication.");
        return false;
    }
    const auto& prepared = candidate.payloads;
    auto labels = prepared ? prepared->labels : nullptr;
    auto mesh = prepared ? prepared->mesh : nullptr;
    auto voids = prepared ? prepared->voids : nullptr;
    auto statistics = prepared ? prepared->statistics : nullptr;
    if (!labels || !mesh || !voids || !statistics) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap result payload construction failed.");
        return false;
    }

    const auto labelEntity = m_data->CreateDataEntityId();
    const auto voidEntity = m_data->CreateDataEntityId();
    const auto meshEntity = m_data->CreateDataEntityId();
    const auto statisticsEntity = m_data->CreateDataEntityId();
    const auto resultEntity = m_data->CreateDataEntityId();
    const DataRevisionRef labelRef{ labelEntity, 1 };
    const DataRevisionRef voidRef{ voidEntity, 1 };
    const DataRevisionRef meshRef{ meshEntity, 1 };
    const DataRevisionRef statisticsRef{ statisticsEntity, 1 };
    const DataRevisionRef resultRef{ resultEntity, 1 };
    const DataRevisionRef parentLabels{m_data->CreateDataEntityId(),1},parentVoids{m_data->CreateDataEntityId(),1},parentRecord{m_data->CreateDataEntityId(),1};
    auto resultSet = std::make_shared<const DataCollectionPayload>(
        gapResultSetType,
        std::vector<DataCollectionEntry>{
            { "labels", labelRef },
            { "void-regions", voidRef },
            { "void-surface", meshRef },
            { "statistics", statisticsRef },{"parent-labels",parentLabels},{"parent-void-regions",parentVoids},{"parent-detection",parentRecord} });
    if (!resultSet->GetValid()) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap result-set construction failed.");
        return false;
    }

    const auto sourceRef = m_requestSource->data->self;
    DataExpectation sourceExpectation;
    sourceExpectation.kind = DataExpectationKind::Binding;
    sourceExpectation.binding = std::string(primaryVolumeBinding);
    sourceExpectation.expectedBindingRevision =
        m_requestSource->binding->revision;
    sourceExpectation.isTargetChecked = true;
    sourceExpectation.expectedTarget = sourceRef;

    DataTransaction transaction;
    transaction.expectations.push_back(std::move(sourceExpectation));
    transaction.outputs = {
        DataRevisionDraft{
            labelEntity, 0, DataTypes::labelMap3D,
            { DataInputRef{ "source-volume", sourceRef } },
            std::move(labels),
            DataProvenance{
                std::string(featureId), "analyze-labels", "1", m_requestParameters } },
        DataRevisionDraft{
            voidEntity, 0, gapVoidTableType,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef } },
            std::move(voids),
            DataProvenance{
                std::string(featureId), "project-void-regions", "1", m_requestParameters } },
        DataRevisionDraft{
            meshEntity, 0, DataTypes::surfaceMesh,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef } },
            std::move(mesh),
            DataProvenance{
                std::string(featureId), "extract-void-surface", "1", m_requestParameters } },
        DataRevisionDraft{
            statisticsEntity, 0, gapStatisticsType,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef },
              DataInputRef{ "void-regions", voidRef } },
            std::move(statistics),
            DataProvenance{
                std::string(featureId), "project-statistics", "1", m_requestParameters } },
        DataRevisionDraft{
            resultEntity, 0, gapResultSetType,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef },
              DataInputRef{ "void-regions", voidRef },
              DataInputRef{ "void-surface", meshRef },
              DataInputRef{ "statistics", statisticsRef } },
            std::move(resultSet),
            DataProvenance{
                std::string(featureId), "collect-results", "1", m_requestParameters } }
    };
    for (auto& output:transaction.outputs)
        for (const auto& input:m_operation.inputs) if (input.role!="source-volume") output.inputs.push_back(input);
    if (!prepared || !prepared->parentLabels || !prepared->parentVoids || !prepared->parentRecord) {
        SetFailedResult(GapResultStatus::Failed,"Gap parent payload construction failed.");
        return false;
    }
    transaction.outputs.insert(transaction.outputs.begin(),{parentLabels.entityId,0,DataTypes::labelMap3D,{{"source-volume",sourceRef}},prepared->parentLabels,DataProvenance{std::string(featureId),"retain-parent-labels","1",m_requestParameters}});
    transaction.outputs.insert(transaction.outputs.begin()+1,{parentVoids.entityId,0,gapVoidTableType,{{"source-volume",sourceRef},{"labels",parentLabels}},prepared->parentVoids,DataProvenance{std::string(featureId),"retain-parent-defects","1",m_requestParameters}});
    transaction.outputs.insert(transaction.outputs.begin()+2,{parentRecord.entityId,0,gapParentRecordType,{{"source-volume",sourceRef},{"parent-labels",parentLabels},{"parent-void-regions",parentVoids}},prepared->parentRecord,DataProvenance{std::string(featureId),"retain-parent-detection","1",m_requestParameters}});
    transaction.outputs.back().inputs.push_back({"parent-detection",parentRecord});
    transaction.outputs.back().inputs.push_back({"parent-labels",parentLabels});transaction.outputs.back().inputs.push_back({"parent-void-regions",parentVoids});
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(gapResultBinding),
        m_requestResultBinding.revision,
        true,
        m_requestResultBinding.target,
        resultRef });

    auto scopes=m_resultScopes;
    if (GetDataEntityIdValid(m_requestSource->data->lifetimeScope)) {
        const auto policy = m_requestSource->data->lifetimePolicy;
        if (policy == DataLifetimePolicy::OwnerRetirement && scopes.size()>=1024) {
            SetFailedResult(GapResultStatus::Failed,"Controlled Gap result history limit reached.");return false;
        }
        const auto scope=m_data->CreateDataEntityId();
        for (auto& draft:transaction.outputs) { draft.lifetimeScope=scope; draft.lifetimePolicy=policy; }
        if (policy == DataLifetimePolicy::OwnerRetirement)
            scopes.push_back({scope,DataLifetimeStatus::Published,{labelRef,voidRef,meshRef,statisticsRef,resultRef,parentLabels,parentVoids,parentRecord},true});
    }
    auto batch=m_data->StartDataChanges();
    if (!batch) {SetFailedResult(GapResultStatus::Failed,"Gap publication batch is unavailable.");return false;}
    const auto commit = m_data->SetDataCommit(std::move(transaction));
    if (commit.status != DataCommitStatus::Succeeded) {
        SetFailedResult(
            GetSourceSame()
                ? GapResultStatus::Failed
                : GapResultStatus::SourceChanged,
            GetSourceSame()
                ? "Gap data transaction was rejected: "+commit.message
                : "Gap source changed before publication.");
        return false;
    }

    m_resultScopes.swap(scopes);
    m_resultBinding = commit.bindings.back();
    GapHostState state;
    state.analysisState = GapAnalysisState::Succeeded;
    state.statistics = candidate.statistics;
    state.commitId = commit.commitId;
    state.sourceRevision = sourceRef;
    state.labelMap = labelRef;
    state.voidTable = voidRef;
    state.voidMesh = meshRef;
    state.statisticsData = statisticsRef;
    state.resultSet = resultRef;
    m_state = state;
    m_operation.status = FeatureRunStatus::Succeeded;
    m_operation.progress = 1.0;
    m_operation.stateRevision = m_service->GetExecutionState().stateRevision + 2;
    m_operation.outputs = { labelRef, voidRef, meshRef, statisticsRef, resultRef,parentLabels,parentVoids,parentRecord };

    const auto graph = m_data->GetDataGraph();
    auto labelView = m_data->GetLabelMap(graph, labelRef);
    auto meshView = m_data->GetSurfaceMesh(graph, meshRef);
    bool isDisplayed = m_service->SetCommittedView(
        std::move(labelView), std::move(meshView),resultRef,m_inheritedDisplay);
    m_inheritedDisplay.reset();
    if(isDisplayed) m_resultDisplays[resultRef]=m_service->GetDisplayParams();
    const bool isDeltaSent = SendSceneDelta(FeatureScenePriority::Overlay);
    isDisplayed = isDisplayed && isDeltaSent;
    const auto callback = m_completeItem;
    m_isRequestPending = false;
    m_requestSource.reset();
    m_requestResultBinding = {};
    if (callback) {
        {
            const std::lock_guard<std::mutex> lock(callback->mutex);
            callback->result = BuildHostResult(m_state,
                isDisplayed ? GapResultStatus::Succeeded
                    : GapResultStatus::SucceededWithDisplayFailure,
                isDisplayed ? "Gap data committed and displayed."
                    : "Gap data committed; display attach failed.");
        }
        (void)QueueComplete(callback);
    }
    return true;
}

bool GapHostFeature::Impl::ClearResultScopes()
{
    if (!m_data) return m_resultScopes.empty();
    if (m_resultScopes.empty()) return ClearResultBinding();
    auto batch=m_data->StartDataChanges();if(!batch)return false;
    DataTransaction transaction;
    for (const auto& scope:m_resultScopes) {
        const auto state=m_data->GetDataLifetime(scope.scopeId);
        if (state.status==DataLifetimeStatus::Published) transaction.retireScopes.push_back(scope);
        else if (state.status!=DataLifetimeStatus::Releasing && state.status!=DataLifetimeStatus::Released) return false;
    }
    const auto binding=m_data->GetDataBinding(m_data->GetDataGraph(),gapResultBinding);
    if (binding && binding->target
        && ((binding->revision==m_resultBinding.revision && binding->target==m_resultBinding.target)
            || (binding->revision==m_requestResultBinding.revision && binding->target==m_requestResultBinding.target)))
        transaction.bindings.push_back({std::string(gapResultBinding),binding->revision,true,binding->target,{}});
    if (!transaction.retireScopes.empty() || !transaction.bindings.empty()) {
        const auto committed=m_data->SetDataCommit(std::move(transaction));
        if (committed.status!=DataCommitStatus::Succeeded)return false;
        if (!committed.bindings.empty())m_resultBinding=committed.bindings.back();
    }
    for (auto scope=m_resultScopes.begin();scope!=m_resultScopes.end();) {
        if (m_data->SetDataRelease(scope->scopeId).status==DataLifetimeStatus::Released) scope=m_resultScopes.erase(scope);
        else ++scope;
    }
    return m_resultScopes.empty();
}

bool GapHostFeature::Impl::ClearResultBinding()
{
    if (!m_data) return false;
    const auto graph = m_data->GetDataGraph();
    const auto binding = m_data->GetDataBinding(graph, gapResultBinding);
    if (!binding || !binding->target) return true;
    const bool ownsState = GetDataRevisionRefValid(m_state.resultSet)
        && *binding->target == m_state.resultSet
        && binding->revision == m_resultBinding.revision;
    const bool ownsRequest = m_requestResultBinding.target
        && *binding->target == *m_requestResultBinding.target
        && binding->revision == m_requestResultBinding.revision;
    if (!ownsState && !ownsRequest) return true;
    DataTransaction transaction;
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(gapResultBinding),
        binding->revision,
        true,
        binding->target,
        {} });
    const auto committed = m_data->SetDataCommit(std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded) return false;
    // 主动退出清空的是自己观察到的绑定；不要在下一 tick 将自己的清空
    // 误认成外部换绑，从而改写已经成功的固定分析结果。
    m_resultBinding = committed.bindings.back();
    return true;
}

bool GapHostFeature::Impl::GetResultCurrent() const
{
    const auto binding = m_data
        ? m_data->GetDataBinding(m_data->GetDataGraph(), gapResultBinding) : std::nullopt;
    return binding && binding->revision == m_resultBinding.revision
        && binding->target == m_resultBinding.target;
}

void GapHostFeature::Impl::SetBindingStale()
{
    if (!GetDataRevisionRefValid(m_state.resultSet) || GetResultCurrent()) return;
    // 正式历史仍然可读；外部换绑后不能继续显示旧结果，也不能清空外部新绑定。
    m_state.analysisState = GapAnalysisState::Stale;
    if (m_service && m_service->GetViewOn() && m_service->ExitView()) {
        m_isExitPending = true;
    }
    (void)ClearComplete();
}

void GapHostFeature::Impl::SetFailedResult(
    const GapResultStatus status,
    std::string message)
{
    if (!m_isRequestPending) return;
    const auto execution = m_service->GetExecutionState();
    m_operation.status = execution.status == FeatureRunStatus::Cancelled
        ? FeatureRunStatus::Cancelled : FeatureRunStatus::Failed;
    m_operation.stateRevision = execution.stateRevision + 2;
    m_operation.outputs.clear();
    m_state.analysisState = status == GapResultStatus::SourceChanged
        ? GapAnalysisState::Stale
        : GapAnalysisState::Failed;
    const auto callback = m_completeItem;
    m_isRequestPending = false;
    m_service->SetViewCommitFailed();
    if (status == GapResultStatus::SourceChanged
        && m_service->GetViewOn()
        && m_service->ExitView()) {
        m_isExitPending = true;
    }
    m_requestSource.reset();
    m_requestResultBinding = {};
    if (callback) {
        {
            const std::lock_guard<std::mutex> lock(callback->mutex);
            callback->result = BuildHostResult(m_state, status, std::move(message));
        }
        (void)QueueComplete(callback);
    }
}

InteractionResult GapHostFeature::Impl::OnInput(const InteractionEvent& event)
{
    GapHostRequest request;
    if(event.actionId=="gap.exit") {
        if(!m_service->GetViewOn()||m_isExitPending)return {};
        request.action=GapHostAction::Exit;
    } else if(event.actionId=="gap.toggleOverlay") {
        if(m_service->GetViewOn()&&!m_isExitPending)request.action=GapHostAction::Overlay;
        else {request.action=GapHostAction::Start;request.start=m_config.defaultStart;}
    } else return {};
    const bool accepted=SendRequest(std::move(request),nullptr).status==GapAdmissionStatus::Accepted;
    return {true,true,accepted,accepted?InteractionFailureReason::None:InteractionFailureReason::StateRejected};
}

bool GapHostFeature::Impl::SetActiveViews(
    const std::vector<std::string>& viewIds) const
{
    return m_host
        && m_host->SetActiveViews(viewIds);
}

bool GapHostFeature::Impl::ClearComplete()
{
    if (!m_completeItem) {
        return true;
    }
    const auto item = std::move(m_completeItem);
    {
        const std::lock_guard<std::mutex> lock(item->mutex);
        if (!item->isSent) item->result = BuildHostResult(
            m_state, GapResultStatus::Failed, "Gap completion was cancelled before presentation.");
    }
    (void)QueueComplete(item);
    return true;
}

bool GapHostFeature::Impl::ClearBorrowed()
{
    ClearComplete();
    if (m_host) {
        (void)m_host->SetActiveViews({});
    }
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
    m_isInputAttached = false;
    m_activeInput.reset();
    m_activeViewIds.clear();
    m_activeRequestId = 0;
    m_requestSource.reset();
    m_requestResultBinding = {};
    m_isRequestPending = false;
    ClearComplete();
    return true;
}

bool GapHostFeature::Impl::GetOwnerThread() const
{
    return m_ownerThread == std::this_thread::get_id();
}

GapHostFeature::GapHostFeature(GapHostConfig config)
    : m_impl(std::make_unique<Impl>(
        std::move(config)))
{
}

GapHostFeature::~GapHostFeature() noexcept = default;

std::string_view GapHostFeature::GetFeatureId() const noexcept
{
    return Impl::FeatureId;
}

FeatureDataContract GapHostFeature::GetDataContract() const
{
    return FeatureDataContract{
        { DataInputSpec{
            "source-volume", DataFacets::scalarGrid3D, true } },
        {
            DataOutputSpec{
                "labels", DataTypes::labelMap3D,
                { DataFacets::labelMap3D } },
            DataOutputSpec{
                "void-regions", gapVoidTableType,
                { DataFacets::tabularRecords, gapVoidRecordsFacet } },
            DataOutputSpec{
                "void-surface", DataTypes::surfaceMesh,
                { DataFacets::surfaceMesh } },
            DataOutputSpec{
                "statistics", gapStatisticsType,
                { DataFacets::tabularRecords, gapStatisticsFacet } },
            DataOutputSpec{"parent-detection",gapParentRecordType,{gapParentRecordFacet}},
            DataOutputSpec{
                "result-set", gapResultSetType,
                { DataFacets::dataCollection } }
        } };
}

bool GapHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl
        && m_impl->AttachHost(*this, context);
}

bool GapHostFeature::DetachHost()
{
    const auto keepAlive=weak_from_this().lock();
    return !m_impl || m_impl->DetachHost();
}

bool GapHostFeature::OnHostTick()
{
    const auto keepAlive=weak_from_this().lock();
    return m_impl && m_impl->OnHostTick();
}

GapAdmission GapHostFeature::SendRequest(
    GapHostRequest request,
    GapHostCallback onComplete)
{
    const auto keepAlive=weak_from_this().lock();
    return m_impl ? m_impl->SendRequest(std::move(request), std::move(onComplete))
        : GapAdmission{GapAdmissionStatus::Unavailable, 0};
}

GapHostState GapHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : GapHostState{};
}

std::vector<FeatureOperationState> GapHostFeature::GetOperationStates() const
{
    return m_impl ? m_impl->GetOperationStates() : std::vector<FeatureOperationState>{};
}

std::optional<GapDisplayParams> GapHostFeature::GetDisplay(const DataRevisionRef& result) const {
    return m_impl ? m_impl->GetDisplay(result):std::nullopt;
}
