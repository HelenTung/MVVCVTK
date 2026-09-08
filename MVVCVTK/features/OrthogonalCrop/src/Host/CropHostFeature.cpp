#include "Host/CropHostFeature.h"

#include "App/Services/FeatureViewService.h"
#include "Data/DataPayloads.h"
#include "Interaction/CropBridge.h"

#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkIdList.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <exception>
#include <mutex>
#include <limits>
#include <map>
#include <deque>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view kFeatureId = "OrthogonalCrop";
constexpr std::string_view cropResultBinding =
    "analysis.crop.active";
constexpr std::string_view cropInputBinding =
    "feature.crop.input";
CropFailure GetCropFailure(DataCommitFailure failure)
{
    if (failure==DataCommitFailure::ResultInUse) return CropFailure::ResultInUse;
    if (failure==DataCommitFailure::ResultRetired) return CropFailure::ResultRetired;
    if (failure==DataCommitFailure::OutOfMemory) return CropFailure::LowRam;
    return CropFailure::VersionMismatch;
}

bool GetFacetUsed(
    const DataGraphSnapshot& graph,
    const DataTypeId& type,
    const DataFacetId& facet)
{
    if (!graph.view) return false;
    const auto facets = graph.view->GetDataFacets(type);
    return std::find(facets.begin(), facets.end(), facet)
        != facets.end();
}

std::shared_ptr<const SurfaceMeshPayload> CreateMeshPayload(
    vtkPolyData* mesh)
{
    if (!mesh) return {};
    auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
    triangles->SetInputData(mesh);
    triangles->PassLinesOff();
    triangles->PassVertsOff();
    triangles->Update();
    auto* output = triangles->GetOutput();
    if (!output) return {};

    std::vector<double> vertices;
    if (auto* points = output->GetPoints()) {
        vertices.resize(
            static_cast<std::size_t>(points->GetNumberOfPoints()) * 3);
        for (vtkIdType index = 0;
            index < points->GetNumberOfPoints(); ++index) {
            points->GetPoint(
                index,
                vertices.data() + static_cast<std::size_t>(index) * 3);
        }
    }
    else if (output->GetNumberOfPoints() != 0) {
        return {};
    }
    std::vector<std::uint64_t> cells;
    if (auto* polys = output->GetPolys()) {
        auto ids = vtkSmartPointer<vtkIdList>::New();
        polys->InitTraversal();
        while (polys->GetNextCell(ids)) {
            if (ids->GetNumberOfIds() != 3) return {};
            for (vtkIdType index = 0; index < 3; ++index) {
                const auto value = ids->GetId(index);
                if (value < 0) return {};
                cells.push_back(static_cast<std::uint64_t>(value));
            }
        }
    }
    else if (output->GetNumberOfCells() != 0) {
        return {};
    }
    auto payload = std::make_shared<const SurfaceMeshPayload>(
        std::move(vertices), std::move(cells));
    return payload->GetValid() ? payload : nullptr;
}

bool GetImageReady(vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }
    int dimensions[3] = { 0, 0, 0 };
    image->GetDimensions(dimensions);
    return dimensions[0] > 0
        && dimensions[1] > 0
        && dimensions[2] > 0;
}

}

class CropHostFeature::Impl final {
public:
    struct CompleteItem final {
        CropBuildCallback onComplete;
        std::optional<CropBuildResult> result;
        std::optional<RenderInputStamp> waitInput;
        std::vector<std::string> waitViewIds;
        bool isQueued = false;
    };

    struct CompleteState final {
        std::mutex mutex;
        std::vector<std::shared_ptr<CompleteItem>> items;
        bool isActive = true;
    };

    struct PublishGuard final {
        explicit PublishGuard(bool& isPublishing)
            : m_isPublishing(isPublishing)
        {
            m_isPublishing = true;
        }

        ~PublishGuard()
        {
            m_isPublishing = false;
        }

    private:
        bool& m_isPublishing;
    };

    struct BuildPublication final {
        std::vector<CropResultRecord> records;
        std::shared_ptr<const VtkPreparedDataView> prepared;
        CropBuildResult result;
        CropHostState state;
        FeatureOperationState operation;
        FeatureOperationState resultOperation;
        std::shared_ptr<CompleteState> completeState;
        std::shared_ptr<CompleteItem> completeItem;
    };

    struct SourceTransition final : FeatureDataCommit {
        CropBridge* bridge;
        CropBridge::SourceCommit prepared;
        std::uint64_t requestId;
        bool isCommitted=false;
        std::shared_ptr<BuildPublication> publication;
        bool isDocument=false;
        std::function<void(const DataCommitResult&)> onCommit;
        SourceTransition(CropBridge* value,CropBridge::SourceCommit token,std::uint64_t id)
            : bridge(value),prepared(std::move(token)),requestId(id) {}
        void SetCommit() noexcept override { SetDataCommitted({}); }
        void SetDataCommitted(const DataCommitResult& committed) noexcept override {
            bridge->SetSourceCommit(std::move(prepared));
            if (onCommit) onCommit(committed);
            isCommitted=true;
        }
    };

    Impl() = default;
    bool GetOwnerReady() const noexcept {
        const auto owner=std::atomic_load(&m_ownerThread);
        return owner && *owner==std::this_thread::get_id();
    }

    bool AttachHost(
        CropHostFeature& owner,
        const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    bool SendRequest(
        CropHostRequest request,
        CropBuildCallback onComplete);
    CropHostState GetState() const;
    std::vector<FeatureOperationState> GetOperationStates() const;

private:
    bool StartCrop(const CropHostTarget& target);
    bool StartSourcePreview(bool isDocument=false);
    void SetBuildPublished(BuildPublication& publication, const DataCommitResult& commit) noexcept;
    void SetBuildFailed(const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item, CropBuildResult result);
    bool SendDocumentRequest();
    void SetDocumentComplete(CropFailure failure,std::vector<DataLifetimeBlocker> blockers={},std::uint64_t completedRevision=0);
    void SendDocumentCompletes();
    bool SendResultReleases();
    void ClearBuildingResult();
    bool SendSourcePreview();
    bool CancelSourcePreview();
    std::optional<CropInputSnapshot> GetCropInput(
        const CropHostTarget& target) const;
    bool GetTargetsValid(
        const HostViewTargets& targets) const;
    bool SetActiveViews(
        const std::vector<std::string>& viewIds) const;
    bool SetCropInput(const CropHostTarget& target);
    bool SetPolyData(vtkSmartPointer<vtkPolyData> polyData);
    bool ClearPolyData();
    std::optional<CropBuildResult> SetBuildResult(
        const CropInputSnapshot& source,
        const DataBinding& resultBinding,
        CropMaterializationCandidate candidate,
        const std::shared_ptr<CompleteState>& completeState,
        const std::shared_ptr<CompleteItem>& completeItem);
    bool ClearResultBinding();
    bool BuildCropResult(
        const CropHostTarget& target,
        CropBuildCallback onComplete);
    static bool RemoveComplete(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item);
    static bool SetCompleteResult(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item,
        CropBuildResult result,
        std::optional<RenderInputStamp> waitInput,
        std::vector<std::string> waitViewIds);
    static bool SendComplete(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item);
    void CancelCompletes();
    bool SendReadyCompletes();
    std::optional<RenderInputStamp> GetActiveInputStamp() const;
    bool SendSceneDelta(
        FeatureScenePriority priority,
        std::optional<RenderInputStamp> inputStamp = {});
    std::uint64_t GetNextSceneRequestId() noexcept;
    void ClearBorrowed();

public:
    CropDocumentAdmission SendRequest(CropDocumentRequest request,CropDocumentCallback onComplete);
    std::optional<CropDocumentOutcome> GetDocumentOutcome(CropDocumentId documentId,CropRequestId requestId) const;
    CropEditAdmission SendRequest(CropEditRequest request);
    CropHistorySnapshot GetHistory(CropDocumentId documentId,CropNodeId after,std::size_t limit) const;
    std::optional<CropEditOutcome> GetOutcome(CropDocumentId documentId,CropRequestId requestId) const;
    CropPruneImpact GetPruneImpact(CropDocumentId documentId,const CropPruneRequest& request) const;

private:
    std::unique_ptr<CropBridge> m_bridge;
    std::shared_ptr<SourceTransition> m_sourceTransition;
    struct DocumentCommand final {
        CropDocumentRequest request;
        CropDocumentOutcome outcome;
        CropDocumentCallback onComplete;
        bool isDelivered=false;
    };
    std::map<CropRequestId,DocumentCommand> m_documentCommands;
    std::deque<CropRequestId> m_documentAdmissionOrder;
    CropRequestId m_expiredDocumentRequest=0;
    CropRequestId m_pendingDocumentRequest=0;
    bool m_documentSourceCommitted=false;
    CropDocumentId m_lastClosedDocument=0;
    std::uint64_t m_lastClosedRevision=0;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    CropHostState m_dataState;
    std::shared_ptr<const VtkPreparedDataView> m_resultPrepared;
    FeatureOperationState m_operation;
    FeatureOperationState m_resultOperation;
    bool m_isBuildPending = false;
    std::optional<CropResultRecord> m_buildRecord;
    std::optional<CropHostTarget> m_activeTarget;
    std::vector<std::string> m_activeViewIds;
    std::shared_ptr<CompleteState> m_completeState;
    std::shared_ptr<const std::thread::id> m_ownerThread;
    std::uint64_t m_nextSceneRequestId = 1;
    bool m_isPublishing = false;
    bool m_isAttached = false;
};

bool CropHostFeature::Impl::AttachHost(
    CropHostFeature& owner,
    const HostFeatureContext& context)
{
    if (owner.weak_from_this().expired()
        || !context.views
        || !context.data
        || !context.host) {
        return false;
    }
    // 首次有效 Attach 固定 owner；无效请求不能占用线程身份。
    auto thread=std::atomic_load(&m_ownerThread);
    if (!thread) {
        auto candidate=std::make_shared<const std::thread::id>(std::this_thread::get_id());
        (void)std::atomic_compare_exchange_strong(&m_ownerThread,&thread,candidate);
    }
    if (!GetOwnerReady()) return false;
    if (m_isAttached) {
        return m_views == context.views
            && m_data == context.data
            && m_host == context.host
            && GetOwnerReady();
    }

    if (!m_bridge) m_bridge=std::make_unique<CropBridge>();
    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_bridge->SetWorkAvailable(
        [weakHost = std::weak_ptr<FeatureHostControl>(context.host)] {
            if (const auto host = weakHost.lock())
                (void)host->SendWorkAvailable();
        });
    m_completeState = std::make_shared<CompleteState>();
    m_dataState = {};

    m_operation = {};
    m_resultOperation = {};
    m_isBuildPending = false;
    m_isAttached = true;
    return true;
}

bool CropHostFeature::Impl::DetachHost()
{
    if (!GetOwnerReady()) return !std::atomic_load(&m_ownerThread);
    if (!m_isAttached) {
        return true;
    }
    if (!GetOwnerReady() || m_isPublishing) {
        return false;
    }

    if (m_bridge && m_bridge->GetCropHistory().documentId) {
        if (!m_pendingDocumentRequest) {
            const auto history=m_bridge->GetCropHistory();
            CropDocumentRequest closing;closing.action=CropDocumentAction::CloseDocument;
            closing.documentId=history.documentId;closing.requestId=CropHistory::CreateNodeId();
            closing.expectedRevision=history.stateRevision;
            if (!SendRequest(closing,{})) return false;
        }
        // Detach/Stop retries pump the same close transaction even when ordinary frames are stopped.
        (void)OnHostTick();
        if (m_bridge->GetCropHistory().documentId) return false;
    }
    const PublishGuard guard(m_isPublishing);
    if (!CancelSourcePreview()) return false;
    CancelCompletes();
    SendDocumentCompletes();
    bool isReleased = true;
    if (m_bridge && m_bridge->GetCropBound()) {
        if (m_bridge->GetCropActive()) {
            isReleased = m_bridge->ExitCrop();
        }
        isReleased = m_bridge->ClearBindings() && isReleased;
    }
    isReleased = ClearResultBinding() && isReleased;
    isReleased = SetActiveViews({}) && isReleased;
    if (!isReleased) return false;
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_dataState = {};m_resultPrepared.reset();
    m_isAttached = false;
    ClearBorrowed();
    return true;
}

void CropHostFeature::Impl::ClearBorrowed()
{
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_completeState.reset();
    m_dataState = {};
}

bool CropHostFeature::Impl::SetCropInput(
    const CropHostTarget& target)
{
    auto input = GetCropInput(target);
    if (!m_bridge || !input) {
        return false;
    }

    return m_bridge->SetCropInput(std::move(*input));
}

std::optional<CropInputSnapshot>
CropHostFeature::Impl::GetCropInput(
    const CropHostTarget& target) const
{
    if (!m_data || target.inputBinding.empty()) return std::nullopt;
    const auto graph = m_data->GetDataGraph();
    if (!graph.view) return std::nullopt;

    const auto binding = m_data->GetDataBinding(graph, target.inputBinding);
    if (!binding || !binding->target) return std::nullopt;
    const auto data = m_data->GetData(graph, *binding->target);
    if (!data) return std::nullopt;

    CropInputSnapshot input;
    input.graph = graph;
    input.binding = binding;
    input.data = data;
    if (GetFacetUsed(
            graph, data->type, DataFacets::scalarGrid3D)) {
        input.image = m_data->GetImageGrid(graph, data->self);
        if (!input.image || !GetImageReady(input.image->image)) {
            return std::nullopt;
        }
        input.image->image->GetBounds(
            input.inputModelBounds.data());
    }
    else if (GetFacetUsed(
            graph, data->type, DataFacets::surfaceMesh)) {
        input.mesh = m_data->GetSurfaceMesh(graph, data->self);
        if (!input.mesh || !input.mesh->mesh) {
            return std::nullopt;
        }
        input.mesh->mesh->GetBounds(
            input.inputModelBounds.data());
    }
    else {
        return std::nullopt;
    }
    return input;
}

bool CropHostFeature::Impl::GetTargetsValid(
    const HostViewTargets& targets) const
{
    if (!m_views
        || (targets.viewIds.empty()
            && targets.viewRoles.empty())) {
        return false;
    }
    const auto views = m_views->GetViews(targets);
    if (views.empty()) {
        return false;
    }
    for (const auto& viewId : targets.viewIds) {
        if (viewId.empty()
            || std::none_of(
                views.begin(), views.end(),
                [&viewId](const HostFeatureView& view) {
                    return view.id == viewId;
                })) {
            return false;
        }
    }
    for (const auto role : targets.viewRoles) {
        if (std::none_of(
                views.begin(), views.end(),
                [role](const HostFeatureView& view) {
                    return view.role == role;
                })) {
            return false;
        }
    }
    return true;
}

bool CropHostFeature::Impl::SetActiveViews(
    const std::vector<std::string>& viewIds) const
{
    return m_host
        && m_host->SetActiveViews(viewIds);
}

bool CropHostFeature::Impl::StartCrop(
    const CropHostTarget& target)
{
    if (!m_isAttached
        || !m_views
        || !m_bridge
        || (target.referenceView.viewId.empty()
            && !target.referenceView.isViewRoleUsed)) {
        return false;
    }
    const auto referenceView =
        m_views->GetInputView(target.referenceView);
    if (!referenceView) {
        return false;
    }
    const auto referencePort =
        m_views->GetFeaturePort(referenceView->view.id);
    if (!referencePort) return false;

    const auto& requestedTargets = target.targetViews;
    if (!GetTargetsValid(requestedTargets)) {
        return false;
    }
    const auto targetViews = m_views->GetViews(requestedTargets);
    auto input = m_bridge->GetSource().data ? std::optional<CropInputSnapshot>{m_bridge->GetSource()} : GetCropInput(target);
    if (targetViews.empty() || !input || !input->binding || input->binding->name!=target.inputBinding) {
        return false;
    }

    CropViewRequest request;
    request.interactor = referenceView->interactor;
    request.renderer = referenceView->renderer;
    request.lease = referenceView->lease;
    request.referenceService = referencePort;
    std::vector<std::string> activeViewIds;
    activeViewIds.reserve(targetViews.size() + 1);
    for (const auto& view : targetViews) {
        const auto port = m_views->GetFeaturePort(view.id);
        if (port) {
            request.targetServices.push_back(port);
            if (!view.id.empty()
                && std::find(
                    activeViewIds.begin(),
                    activeViewIds.end(),
                    view.id) == activeViewIds.end()) {
                activeViewIds.push_back(view.id);
            }
        }
    }
    if (request.targetServices.empty()) {
        return false;
    }
    if (std::find(
            activeViewIds.begin(),
            activeViewIds.end(),
            referenceView->view.id)
            == activeViewIds.end()) {
        activeViewIds.push_back(referenceView->view.id);
    }
    const bool isStarted = m_bridge->StartView(
        request, std::move(*input));
    if (isStarted) {
        if (!SetActiveViews(activeViewIds)) {
            (void)m_bridge->ExitCrop();
            (void)m_bridge->ClearBindings();
            return false;
        }
        m_activeTarget = target;
        m_activeViewIds = std::move(activeViewIds);
    }
    return isStarted;
}

bool CropHostFeature::Impl::SetPolyData(
    vtkSmartPointer<vtkPolyData> polyData)
{
    if (!m_data || !polyData || m_isPublishing || m_sourceTransition) return false;
    auto payload = CreateMeshPayload(polyData);
    if (!payload || payload->GetVertices().empty()) return false;

    const auto graph = m_data->GetDataGraph();
    DataBinding inputBinding;
    inputBinding.name = std::string(cropInputBinding);
    if (const auto current = m_data->GetDataBinding(
            graph, cropInputBinding)) {
        inputBinding = *current;
    }
    const auto entity = m_data->CreateDataEntityId();
    const DataRevisionRef inputRef{ entity, 1 };
    DataTransaction transaction;
    transaction.outputs.push_back(DataRevisionDraft{
        entity, 0, DataTypes::surfaceMesh, {}, std::move(payload),
        DataProvenance{
            std::string(kFeatureId), "register-mesh-input", "1", "{}" } });
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(cropInputBinding),
        inputBinding.revision,
        true,
        inputBinding.target,
        inputRef });
    if (const auto resultBinding = m_data->GetDataBinding(
            graph, cropResultBinding);
        resultBinding && resultBinding->target
        && GetDataRevisionRefValid(m_dataState.outputRevision)
        && *resultBinding->target == m_dataState.outputRevision) {
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(cropResultBinding),
            resultBinding->revision,
            true,
            resultBinding->target,
            {} });
    }
    const PublishGuard guard(m_isPublishing);
    const auto commit = m_data->SetDataCommit(std::move(transaction));
    if (commit.status != DataCommitStatus::Succeeded) return false;

    if (m_bridge) (void)m_bridge->ClearBindings();
    (void)SetActiveViews({});
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_dataState = {};
    return true;
}

bool CropHostFeature::Impl::ClearPolyData()
{
    if (!m_data || m_isPublishing || m_sourceTransition) return false;
    const auto graph = m_data->GetDataGraph();
    const auto inputBinding = m_data->GetDataBinding(
        graph, cropInputBinding);
    DataTransaction transaction;
    if (inputBinding && inputBinding->target) {
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(cropInputBinding),
            inputBinding->revision,
            true,
            inputBinding->target,
            {} });
    }
    if (const auto resultBinding = m_data->GetDataBinding(
            graph, cropResultBinding);
        resultBinding && resultBinding->target
        && GetDataRevisionRefValid(m_dataState.outputRevision)
        && *resultBinding->target == m_dataState.outputRevision) {
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(cropResultBinding),
            resultBinding->revision,
            true,
            resultBinding->target,
            {} });
    }
    if (!transaction.bindings.empty()) {
        const PublishGuard guard(m_isPublishing);
        if (m_data->SetDataCommit(std::move(transaction)).status
            != DataCommitStatus::Succeeded) {
            return false;
        }
    }
    if (m_bridge) (void)m_bridge->ClearBindings();
    (void)SetActiveViews({});
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_dataState = {};
    return true;
}

std::optional<CropBuildResult> CropHostFeature::Impl::SetBuildResult(
    const CropInputSnapshot& source,
    const DataBinding& resultBinding,
    CropMaterializationCandidate candidate,
    const std::shared_ptr<CompleteState>& completeState,
    const std::shared_ptr<CompleteItem>& completeItem)
{
    CropBuildResult result;
    if (m_buildRecord) {
        result.documentId=m_bridge->GetCropHistory().documentId;
        result.nodeId=m_buildRecord->nodeId;result.resultId=m_buildRecord->resultId;result.scopeId=m_buildRecord->scopeId;
    }
    result.failureReason = candidate.failureReason;
    result.failureOperationIndex = candidate.failureOperationIndex;
    result.nodeCount = candidate.nodeCount;
    result.sourceRevision = candidate.sourceRevision;
    result.message = std::move(candidate.message);
    if (!candidate.isSucceeded) return result;
    if (!m_buildRecord || !m_data || m_isPublishing
        || !source.data
        || candidate.sourceRevision != source.data->self
        || candidate.operations.size() != candidate.nodeCount
        || !source.binding) {
        if (result.failureReason == CropFailure::None) {
            result.failureReason = CropFailure::BadInput;
        }
        result.message = "Crop result candidate is invalid.";
        return result;
    }

    auto recipe = std::move(candidate.recipePayload);
    auto output = std::move(candidate.outputPayload);
    const auto outputType = output ? output->GetDataType() : DataTypeId{};
    if (!recipe || !output || !candidate.preparedView || !GetDataTypeIdValid(outputType)) {
        result.failureReason = CropFailure::BadInput;
        result.message = "Crop formal payload construction failed.";
        return result;
    }

    const auto recipeEntity = m_data->CreateDataEntityId();
    const auto outputEntity = m_data->CreateDataEntityId();
    const DataRevisionRef recipeRef{ recipeEntity, 1 };
    const DataRevisionRef outputRef{ outputEntity, 1 };
    auto prepared = m_data->SetPreparedDataView(outputRef, std::move(candidate.preparedView));
    if (!prepared) {
        result.failureReason = CropFailure::BadInput;
        result.message = "Crop prepared view registration failed.";
        return result;
    }
    DataTransaction transaction;
    transaction.outputs = {
        DataRevisionDraft{
            recipeEntity, 0, DataTypes::roiGeometry,
            { DataInputRef{ "source-data", source.data->self } },
            std::move(recipe),
            DataProvenance{
                std::string(kFeatureId), "capture-recipe", "1", "{}" } },
        DataRevisionDraft{
            outputEntity, 0, outputType,
            { DataInputRef{ "source-data", source.data->self },
              DataInputRef{ "crop-recipe", recipeRef } },
            std::move(output),
            DataProvenance{
                std::string(kFeatureId), "materialize-crop", "1", "{}" } }
    };
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(cropResultBinding),
        resultBinding.revision,
        true,
        resultBinding.target,
        outputRef });

    for (auto& outputDraft:transaction.outputs) outputDraft.lifetimeScope=m_buildRecord->scopeId;
    transaction.outputs.back().preparedResources.push_back(prepared->resourceUse);
    auto records=m_bridge->GetHistory(0,1).results;
    bool hasBuilding=false;
    for (auto& record:records) {
        if (record.resultId==m_buildRecord->resultId && record.status==CropResultStatus::Building) {
            hasBuilding=true;record.status=CropResultStatus::Published;
            record.recipeRevision=recipeRef;record.outputRevision=outputRef;
        } else if (record.status==CropResultStatus::Published) {
            transaction.retireScopes.push_back({record.scopeId,DataLifetimeStatus::Published,
                {record.recipeRevision,record.outputRevision},true});
            record.status=CropResultStatus::Releasing;
        }
    }
    if (!hasBuilding || !m_bridge->GetResultsValid(records)) {
        result.failureReason=CropFailure::ResourceLimit;return result;
    }
    auto publication = std::make_shared<BuildPublication>();
    publication->records = std::move(records);
    publication->prepared = std::move(prepared);
    result.message = "Crop recipe and derived output committed.";
    result.isSucceeded = true; result.failureReason = CropFailure::None;
    result.recipeRevision = recipeRef; result.outputRevision = outputRef;
    publication->result = std::move(result);
    publication->state = m_dataState;
    publication->state.documentStatus = CropDocumentStatus::Ready;
    publication->state.sourceRevision = source.data->self;
    publication->state.recipeRevision = recipeRef;
    publication->state.outputRevision = outputRef;
    publication->state.failureReason = CropFailure::None;
    publication->state.blockers.clear();
    publication->operation = m_operation;
    publication->operation.status = FeatureRunStatus::Succeeded;
    publication->operation.stateRevision = 4;
    publication->operation.progress = 1.0;
    publication->operation.outputs = {recipeRef, outputRef};
    publication->resultOperation = publication->operation;
    publication->completeState = completeState; publication->completeItem = completeItem;
    const auto failed = [&](CropFailure failure) -> std::optional<CropBuildResult> {
        auto value = std::move(publication->result);
        value.isSucceeded = false; value.failureReason = failure;
        value.recipeRevision = {}; value.outputRevision = {};
        value.message = "Crop publication could not prepare all Root views.";
        return value;
    };
    if (!transaction.retireScopes.empty()) {
        // Source input, outputs and retirement share one coordinator transaction.
        // No observable Root binding change may precede the retirement checks.
        if (!source.image || m_sourceTransition) return failed(CropFailure::PreviewNotReady);
        const auto graph = m_data->GetDataGraph();
        const auto binding = m_data->GetDataBinding(graph, primaryVolumeBinding)
            .value_or(DataBinding{std::string(primaryVolumeBinding)});
        const auto id = GetNextSceneRequestId();
        if (!id || binding.revision == std::numeric_limits<DataBindingRevision>::max())
            return failed(CropFailure::ResourceLimit);
        const auto history = m_bridge->GetCropHistory();
        if (history.stateRevision >= std::numeric_limits<std::uint64_t>::max()-1)
            return failed(CropFailure::ResourceLimit);
        auto token = m_bridge->BuildSourceCommit(history.appliedHead, false);
        if (!token) return failed(CropFailure::PreviewNotReady);
        auto participant = std::make_shared<SourceTransition>(m_bridge.get(), std::move(*token), id);
        participant->publication = publication;
        participant->onCommit = [this, publication](const DataCommitResult& committed) noexcept {
            SetBuildPublished(*publication, committed);
        };
        transaction.bindings.push_back({std::string(primaryVolumeBinding), binding.revision,
            true, binding.target, source.data->self});
        auto view = std::make_shared<VtkImageGridView>(*source.image);
        view->graph = graph; view->data = source.data;
        view->binding = DataBinding{std::string(primaryVolumeBinding),source.data->self,binding.revision+1};
        FeatureDataTransitionRequest request;
        request.requestId = id; request.commit = participant;
        request.transaction = std::move(transaction); request.input = std::move(view);
        m_sourceTransition = participant;
        const PublishGuard guard(m_isPublishing);
        const auto started = m_host->StartDataTransition(std::move(request));
        if (started.status != FeatureRunStatus::Preparing && started.status != FeatureRunStatus::Running) {
            m_bridge->SetSourceCommitFailed(std::move(participant->prepared), CropFailure::PreviewNotReady);
            m_sourceTransition.reset();
            auto value = failed(started.commitFailure == DataCommitFailure::None
                ? CropFailure::PreviewNotReady : GetCropFailure(started.commitFailure));
            value->blockers = started.blockers;
            return value;
        }
        return std::nullopt;
    }
    const PublishGuard publishGuard(m_isPublishing);
    auto batch = m_data->StartDataChanges();
    if (!batch) return failed(CropFailure::Busy);
    const auto commit = m_data->SetDataCommit(std::move(transaction));
    if (commit.status != DataCommitStatus::Succeeded) {
        auto value = failed(GetCropFailure(commit.failureReason));
        value->blockers = commit.blockers; value->message = commit.message;
        return value;
    }
    SetBuildPublished(*publication, commit);
    return std::nullopt;
}

void CropHostFeature::Impl::SetBuildPublished(BuildPublication& publication, const DataCommitResult& commit) noexcept
{
    // Compile-time evidence for the no-allocation state/completion handoff after graph swap.
    static_assert(std::is_nothrow_move_assignable_v<CropHostState>);
    static_assert(std::is_nothrow_move_assignable_v<FeatureOperationState>);
    static_assert(std::is_nothrow_move_constructible_v<CropBuildResult>);
    static_assert(std::is_nothrow_move_assignable_v<std::optional<CropBuildResult>>);
    static_assert(std::is_nothrow_move_assignable_v<std::vector<std::string>>);
    m_bridge->SetResults(std::move(publication.records));
    m_resultPrepared = std::move(publication.prepared);
    publication.result.commitId = commit.commitId;
    publication.state.commitId = commit.commitId;
    m_dataState = std::move(publication.state);
    m_operation = std::move(publication.operation);
    m_resultOperation = std::move(publication.resultOperation);
    m_buildRecord.reset(); m_isBuildPending = false;
    // Owner admission and the publication guard keep this accepted item registered.
    if (!SetCompleteResult(publication.completeState, publication.completeItem,
            std::move(publication.result), {}, {})) std::terminate();
}

void CropHostFeature::Impl::SetBuildFailed(const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item, CropBuildResult result)
{
    result.isSucceeded = false; result.commitId = 0;
    result.recipeRevision = {}; result.outputRevision = {};
    ClearBuildingResult();
    if (!m_pendingDocumentRequest) m_dataState.documentStatus = CropDocumentStatus::Ready;
    m_dataState.failureReason = result.failureReason; m_dataState.blockers = result.blockers;
    m_isBuildPending = false;
    m_operation.status = result.failureReason == CropFailure::Cancelled ? FeatureRunStatus::Cancelled : FeatureRunStatus::Failed;
    m_operation.stateRevision = 4; m_operation.progress = 0.0;
    (void)SetCompleteResult(state, item, std::move(result), {}, {});
}

bool CropHostFeature::Impl::ClearResultBinding()
{
    if (!m_data) return false;
    const auto graph = m_data->GetDataGraph();
    const auto binding = m_data->GetDataBinding(graph, cropResultBinding);
    if (!binding || !binding->target) return true;
    if (!GetDataRevisionRefValid(m_dataState.outputRevision)
        || *binding->target != m_dataState.outputRevision) {
        return true;
    }
    DataTransaction transaction;
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(cropResultBinding),
        binding->revision,
        true,
        binding->target,
        {} });
    return m_data->SetDataCommit(std::move(transaction)).status
        == DataCommitStatus::Succeeded;
}

bool CropHostFeature::Impl::RemoveComplete(
    const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item)
{
    if (!state || !item) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(state->mutex);
    const auto current = std::find(
        state->items.begin(), state->items.end(), item);
    if (current == state->items.end()) {
        return false;
    }
    state->items.erase(current);
    return true;
}

bool CropHostFeature::Impl::SetCompleteResult(
    const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item,
    CropBuildResult result,
    std::optional<RenderInputStamp> waitInput,
    std::vector<std::string> waitViewIds)
{
    if (!state || !item) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->isActive
        || std::find(
            state->items.begin(),
            state->items.end(),
            item) == state->items.end()) {
        return false;
    }
    item->result = std::move(result);
    item->waitInput = waitInput;
    item->waitViewIds = waitInput
        ? std::move(waitViewIds) : std::vector<std::string>{};
    return true;
}

bool CropHostFeature::Impl::SendComplete(
    const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item)
{
    if (!state || !item) {
        return false;
    }

    CropBuildCallback onComplete;
    CropBuildResult result;
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        const auto current = std::find(
            state->items.begin(), state->items.end(), item);
        if (!state->isActive
            || current == state->items.end()
            || !item->onComplete
            || !item->result) {
            return false;
        }
        onComplete = std::move(item->onComplete);
        result = std::move(item->result.value());
        state->items.erase(current);
    }
    try { onComplete(std::move(result)); }
    catch (...) {}
    return true;
}

void CropHostFeature::Impl::CancelCompletes()
{
    if (!m_completeState) return;
    const auto state = m_completeState;
    std::vector<std::shared_ptr<CompleteItem>> toQueue;
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& item : state->items) {
            if (!item || !item->onComplete) continue;
            if (!item->result) item->result = CropBuildResult{};
            item->result->isSucceeded = false;
            item->result->failureReason = CropFailure::VersionMismatch;
            item->result->message =
                "Crop request was cancelled while detaching.";
            item->waitInput.reset();
            item->waitViewIds.clear();
            if (!item->isQueued) {
                item->isQueued = true;
                toQueue.push_back(item);
            }
        }
    }
    for (const auto& item : toQueue) {
        const auto send = [state, item]() {
            (void)Impl::SendComplete(state, item);
        };
        if (!m_host || !m_host->SendOwnerComplete(send)) send();
    }
}

std::optional<RenderInputStamp>
CropHostFeature::Impl::GetActiveInputStamp() const
{
    if (!m_views || m_activeViewIds.empty()) return std::nullopt;
    std::optional<RenderInputStamp> common;
    for (const auto& viewId : m_activeViewIds) {
        const auto port = m_views->GetFeaturePort(viewId);
        const auto stamp = port
            ? port->GetRenderInputStamp()
            : std::optional<RenderInputStamp>{};
        if (!stamp || !GetDataRevisionRefValid(stamp->dataRevision)
            || (common && *common != *stamp)) {
            return std::nullopt;
        }
        common = *stamp;
    }
    return common;
}

bool CropHostFeature::Impl::SendSceneDelta(
    const FeatureScenePriority priority,
    std::optional<RenderInputStamp> inputStamp)
{
    if (!inputStamp) inputStamp = GetActiveInputStamp();
    if (!m_host || !inputStamp || m_activeViewIds.empty()) {
        return false;
    }
    FeatureSceneDelta delta;
    delta.viewIds = m_activeViewIds;
    delta.inputStamp = *inputStamp;
    delta.requestId = GetNextSceneRequestId();
    delta.priority = priority;
    delta.scope = FeatureSceneScope::RequiredAllViews;
    delta.hasDisplayUpdate = true;
    // shader 预览使用已采用主体；只有正式派生体真正成为主体时才报告结果展示。
    delta.inputs = { { "source-volume", inputStamp->dataRevision } };
    if (inputStamp->dataRevision == m_dataState.outputRevision
        && m_resultOperation.operation.requestId != 0) {
        delta.inputs = m_resultOperation.inputs;
        delta.inputs.push_back({ "crop-recipe", m_dataState.recipeRevision });
        delta.inputs.push_back({ "derived-image", m_dataState.outputRevision });
        for (const auto& viewId : m_activeViewIds)
            delta.displays.push_back({ viewId, std::string(kFeatureId), "crop-result",
                m_dataState.outputRevision, m_resultOperation.operation });
    }
    return m_host->SendSceneDelta(std::move(delta));
}

std::uint64_t CropHostFeature::Impl::GetNextSceneRequestId() noexcept
{
    const auto requestId=m_nextSceneRequestId;
    if (requestId) m_nextSceneRequestId=requestId==std::numeric_limits<std::uint64_t>::max()?0:requestId+1;
    return requestId;
}

bool CropHostFeature::Impl::SendReadyCompletes()
{
    if (!m_completeState
        || !m_host
        || !m_views) {
        return false;
    }

    std::vector<std::shared_ptr<CompleteItem>> readyItems;
    {
        const std::lock_guard<std::mutex> lock(
            m_completeState->mutex);
        if (!m_completeState->isActive) {
            return false;
        }
        for (const auto& item : m_completeState->items) {
            if (!item
                || !item->result
                || item->isQueued) {
                continue;
            }
            bool isReady = !item->waitInput.has_value();
            if (item->waitInput) {
                isReady = !item->waitViewIds.empty()
                    && std::all_of(
                        item->waitViewIds.begin(),
                        item->waitViewIds.end(),
                        [this, &item](const auto& viewId) {
                            const auto port =
                                m_views->GetFeaturePort(viewId);
                            if (!port) {
                                return false;
                            }
                            const auto stamp =
                                port->GetRenderInputStamp();
                            return stamp
                                && *stamp == *item->waitInput;
                        });
            }
            if (isReady) {
                item->isQueued = true;
                readyItems.push_back(item);
            }
        }
    }

    const auto state = m_completeState;
    bool isSent = false;
    for (const auto& item : readyItems) {
        const std::weak_ptr<FeatureViewDirectory> weakViews = m_views;
        const auto waitInput = item->waitInput;
        const auto waitViewIds = item->waitViewIds;
        if (m_host->SendOwnerComplete(
                [state, item, weakViews,
                    waitInput, waitViewIds]() {
                    if (waitInput) {
                        const auto views = weakViews.lock();
                        const bool isCurrent = views
                            && !waitViewIds.empty()
                            && std::all_of(
                                waitViewIds.begin(),
                                waitViewIds.end(),
                                [&views, &waitInput](const auto& viewId) {
                                    const auto port =
                                        views->GetFeaturePort(viewId);
                                    const auto stamp = port
                                        ? port->GetRenderInputStamp()
                                        : std::optional<RenderInputStamp>{};
                                    return stamp && *stamp == *waitInput;
                                });
                        if (!isCurrent) {
                            const std::lock_guard<std::mutex> lock(
                                state->mutex);
                            if (item->result) {
                                item->result->isSucceeded = false;
                                item->result->failureReason =
                                    CropFailure::VersionMismatch;
                                item->result->message =
                                    "Crop input changed before rendered completion.";
                            }
                        }
                    }
                    (void)Impl::SendComplete(state, item);
                })) {
            isSent = true;
        }
        else {
            {
                const std::lock_guard<std::mutex> lock(state->mutex);
                if (item->result) {
                    item->result->isSucceeded = false;
                    item->result->failureReason =
                        CropFailure::VersionMismatch;
                    item->result->message =
                        "Crop completion was cancelled while stopping.";
                }
            }
            (void)SendComplete(state, item);
        }
    }
    return isSent;
}

bool CropHostFeature::Impl::BuildCropResult(
    const CropHostTarget& target,
    CropBuildCallback onComplete)
{
    if (!onComplete
        || !m_bridge
        || !m_completeState
        || !m_host
        || !m_data || m_buildRecord || m_pendingDocumentRequest
        || m_dataState.documentStatus==CropDocumentStatus::Closing || m_dataState.documentStatus==CropDocumentStatus::Closed) {
        return false;
    }
    const auto history=m_bridge->GetHistory(0,1);
    if (!history.documentId || history.appliedHead==history.rootNodeId) return false;
    CropResultRecord building;
    building.resultId=CropHistory::CreateNodeId();building.nodeId=history.appliedHead;
    building.scopeId=m_data->CreateDataEntityId();building.sourceRevision=history.sourceRevision;
    building.publicationGeneration=building.resultId;
    auto buildingRecords=history.results;buildingRecords.push_back(building);
    if (!building.resultId || !GetDataEntityIdValid(building.scopeId) || !m_bridge->GetResultsValid(buildingRecords)) return false;
    auto source = std::optional<CropInputSnapshot>{m_bridge->GetSource()};
    if (!source->binding || !source->data || source->binding->name!=target.inputBinding) {
        return false;
    }
    DataBinding resultBinding;
    resultBinding.name = std::string(cropResultBinding);
    if (const auto current = m_data->GetDataBinding(
            m_data->GetDataGraph(), cropResultBinding)) {
        resultBinding = *current;
    }
    const auto state = m_completeState;
    const auto item = std::make_shared<CompleteItem>();
    item->onComplete = std::move(onComplete);
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->isActive) {
            return false;
        }
        state->items.push_back(item);
    }

    const std::weak_ptr<CompleteState> weakState = state;
    const std::weak_ptr<CompleteItem> weakItem = item;
    const auto previousOperation = m_operation;
    const bool wasBuildPending = m_isBuildPending;
    m_operation = {};
    m_operation.operation = { std::string(kFeatureId), m_host->GetAttachmentId(), GetNextSceneRequestId() };
    m_operation.stateRevision = 1;
    m_operation.status = FeatureRunStatus::Preparing;
    m_operation.inputs = { { source->image ? "source-volume" : "source-mesh", source->data->self } };
    m_isBuildPending = true;
    m_buildRecord=building;
    auto onResult =
        [this, source = *source, resultBinding, resultId=building.resultId,
            weakState, weakItem](
            CropMaterializationCandidate candidate) mutable {
            const auto state = weakState.lock();
            const auto item = weakItem.lock();
            if (!m_isAttached || !m_buildRecord || m_buildRecord->resultId!=resultId) return;
            auto batch=m_data->StartDataChanges();
            if (!state || !item || !state->isActive) {
                // Detach already scheduled the accepted request's cancellation terminal.
                ClearBuildingResult();m_isBuildPending=false;return;
            }
            if (batch) {
                if (auto result = SetBuildResult(source,resultBinding,std::move(candidate),state,item))
                    SetBuildFailed(state,item,std::move(*result));
            } else {
                CropBuildResult result;
                result.documentId=m_bridge->GetCropHistory().documentId;
                result.nodeId=m_buildRecord->nodeId;result.resultId=m_buildRecord->resultId;
                result.scopeId=m_buildRecord->scopeId;result.sourceRevision=source.data->self;
                result.nodeCount=candidate.nodeCount;result.failureReason=CropFailure::Busy;
                result.message="Crop publication notification batch is unavailable.";
                SetBuildFailed(state,item,std::move(result));
            }
        };
    const bool isAccepted = m_bridge->BuildCropResult(building.nodeId,std::move(onResult));
    if (isAccepted) {
        m_bridge->SetResults(std::move(buildingRecords));
        m_dataState.documentStatus=CropDocumentStatus::Building;
        m_dataState.failureReason=CropFailure::None;m_dataState.blockers.clear();
    }
    if (!isAccepted) {
        // Bridge 可以在同步校验失败时先回传内部结果；入口返回 false 时必须丢弃，
        // 保证未接纳请求的外部 callback 永远不会在后续 tick 泄漏。
        (void)RemoveComplete(state, item);
        m_operation = previousOperation;
        m_isBuildPending = wasBuildPending;
        m_buildRecord.reset();
    }
    return isAccepted;
}

bool CropHostFeature::Impl::SendRequest(
    CropHostRequest request,
    CropBuildCallback onComplete)
{
    if (!GetOwnerReady()
        || !m_isAttached
        || m_isPublishing || m_pendingDocumentRequest
        || m_dataState.documentStatus==CropDocumentStatus::Closing || m_dataState.documentStatus==CropDocumentStatus::Closed
        || ((request.action == CropHostAction::BuildResult)
            != static_cast<bool>(onComplete))) {
        return false;
    }
    // 先验证动作字段，拒绝有歧义的请求，不能在模式非法时先启动 Widget。
    const bool needsTarget = request.action == CropHostAction::Start
        || request.action == CropHostAction::Box
        || request.action == CropHostAction::Plane
        || request.action == CropHostAction::Mode
        || request.action == CropHostAction::BuildResult;
    if (needsTarget != request.target.has_value()
        || (request.action == CropHostAction::Mode) != request.removalMode.has_value()
        || (request.action == CropHostAction::SetPolyData) != static_cast<bool>(request.polyData)) {
        return false;
    }
    if (request.removalMode
        && *request.removalMode != CropRemovalMode::None
        && *request.removalMode != CropRemovalMode::KeepInside
        && *request.removalMode != CropRemovalMode::RemoveInside) return false;
    if (request.target
        && (request.target->inputBinding.empty()
            || (request.target->referenceView.viewId.empty()
                && !request.target->referenceView.isViewRoleUsed)
            || !GetTargetsValid(request.target->targetViews)
            || !m_views->GetInputView(request.target->referenceView))) return false;

    switch (request.action) {
    case CropHostAction::Start:
        return StartCrop(*request.target);
    case CropHostAction::Box:
        return StartCrop(*request.target) && m_bridge->SwitchCropBox();
    case CropHostAction::Plane:
        return StartCrop(*request.target) && m_bridge->SwitchCropPlane();
    case CropHostAction::Mode:
        return StartCrop(*request.target) && m_bridge->SetCropMode(*request.removalMode);
    case CropHostAction::Previous:
        {
            const auto history=m_bridge->GetHistory(0,1);
            const auto current=m_bridge->GetNode(history.requestedHead);
            if (current && current->parentNodeId==history.rootNodeId) {
                CropDocumentRequest returning;returning.documentId=history.documentId;
                returning.requestId=CropHistory::CreateNodeId();returning.expectedRevision=history.stateRevision;
                return SendRequest(returning,{}).isAccepted;
            }
            return m_bridge->GetCropBound() && m_bridge->PreviousCrop();
        }
    case CropHostAction::Next:
        return m_bridge->GetCropBound() && m_bridge->NextCrop();
    case CropHostAction::BuildResult:
        return BuildCropResult(*request.target, std::move(onComplete));
    case CropHostAction::SetPolyData:
        return SetPolyData(std::move(request.polyData));
    case CropHostAction::ClearPolyData:
        return ClearPolyData();
    case CropHostAction::Exit:
        {
            const PublishGuard guard(m_isPublishing);
            const bool isExited=m_bridge->ExitCrop();
            const bool isQualityRestored=!isExited || m_bridge->GetShaderTickNeeded() || SetActiveViews({});
            return isExited && isQualityRestored;
        }
    case CropHostAction::None:
        return false;
    }
    return false;
}

CropHostState CropHostFeature::Impl::GetState() const
{
    if (!GetOwnerReady() || !m_isAttached || !m_bridge) return {};
    CropHostState state = m_dataState;

    // owner thread 上连续读取 history、active 和发布屏障，形成同一时刻的只读快照。
    state.history = m_bridge->GetCropHistory();
    state.isActive = m_activeTarget.has_value()
        && m_bridge->GetCropActive();
    state.isPublishing = m_isPublishing;
    return state;
}

std::vector<FeatureOperationState> CropHostFeature::Impl::GetOperationStates() const
{
    if (!GetOwnerReady() || !m_isAttached) return {};
    std::vector<FeatureOperationState> states;
    if (m_operation.operation.requestId != 0) {
        auto state = m_operation;
        if (m_isBuildPending && m_bridge) {
            const auto execution = m_bridge->GetExecutionState();
            if (execution.status != FeatureRunStatus::Idle) {
                state.status = execution.status;
                state.progress = execution.progress;
                state.stateRevision = execution.stateRevision;
            }
        }
        states.push_back(std::move(state));
    }
    if (m_resultOperation.operation.requestId != 0
        && m_resultOperation.operation.requestId != m_operation.operation.requestId)
        states.push_back(m_resultOperation);
    return states;
}

bool CropHostFeature::Impl::StartSourcePreview(bool isDocument)
{
    if (m_sourceTransition || (!isDocument && !m_bridge->GetSourceTransitionNeeded())) return false;
    const auto source=m_bridge->GetSource();
    if (!source.image || !source.data) return false;
    const auto graph=m_data->GetDataGraph();
    auto binding=m_data->GetDataBinding(graph,primaryVolumeBinding).value_or(DataBinding{std::string(primaryVolumeBinding)});
    const auto id=GetNextSceneRequestId();
    if (!id || binding.revision==std::numeric_limits<DataBindingRevision>::max()) return false;
    const auto history=m_bridge->GetHistory(0,1);
    if (history.stateRevision >= std::numeric_limits<std::uint64_t>::max()-1) return false;
    auto prepared=m_bridge->BuildSourceCommit(isDocument?history.rootNodeId:history.appliedHead,!isDocument);
    if (!prepared) return false;
    auto participant=std::make_shared<SourceTransition>(m_bridge.get(),std::move(*prepared),id);
    participant->isDocument=isDocument;
    FeatureDataTransitionRequest request;
    request.requestId=id;request.commit=participant;
    request.transaction.bindings.push_back({std::string(primaryVolumeBinding),binding.revision,true,binding.target,source.data->self});
    if (isDocument) {
        auto records=history.results;
        const auto resultBinding=m_data->GetDataBinding(graph,cropResultBinding);
        for (auto& record:records) {
            if (record.status==CropResultStatus::Building) return false;
            if (record.status!=CropResultStatus::Published) continue;
            request.transaction.retireScopes.push_back({record.scopeId,DataLifetimeStatus::Published,
                {record.recipeRevision,record.outputRevision},true});
            if (resultBinding && resultBinding->target==std::optional<DataRevisionRef>{record.outputRevision})
                request.transaction.bindings.push_back({std::string(cropResultBinding),resultBinding->revision,true,resultBinding->target,{}});
            record.status=CropResultStatus::Releasing;
        }
        if (!m_bridge->GetResultsValid(records)) return false;
        const bool closing=m_documentCommands.at(m_pendingDocumentRequest).request.action==CropDocumentAction::CloseDocument;
        participant->onCommit=[this,records=std::move(records),closing](const DataCommitResult&) mutable {
            m_bridge->SetResults(std::move(records));
            m_resultPrepared.reset();m_resultOperation={};
            m_dataState.commitId=0;m_dataState.recipeRevision={};m_dataState.outputRevision={};
            m_dataState.documentStatus=closing?CropDocumentStatus::Closing:CropDocumentStatus::Releasing;
            m_dataState.failureReason=CropFailure::None;m_dataState.blockers.clear();
            m_documentSourceCommitted=true;
        };
    }
    auto view=std::make_shared<VtkImageGridView>(*source.image);
    view->graph=graph;view->data=source.data;
    view->binding=DataBinding{std::string(primaryVolumeBinding),source.data->self,binding.revision+1};
    request.input=std::move(view);
    m_sourceTransition=participant;
    const PublishGuard guard(m_isPublishing);
    const auto state=m_host->StartDataTransition(std::move(request));
    if (state.status!=FeatureRunStatus::Preparing && state.status!=FeatureRunStatus::Running) {
        m_bridge->SetSourceCommitFailed(std::move(participant->prepared),CropFailure::PreviewNotReady);
        m_sourceTransition.reset();
        return false;
    }
    return true;
}

bool CropHostFeature::Impl::SendSourcePreview()
{
    const auto participant=m_sourceTransition;
    if (!participant) return false;
    if (!m_bridge->GetSourceCommitReady(participant->prepared)) return CancelSourcePreview();
    const PublishGuard guard(m_isPublishing);
    const auto state=m_host->SetDataTransition(participant->requestId);
    if (state.status==FeatureRunStatus::Preparing || state.status==FeatureRunStatus::Running) return false;
    if (!participant->isCommitted)
        m_bridge->SetSourceCommitFailed(std::move(participant->prepared),
            state.status==FeatureRunStatus::Cancelled?CropFailure::Cancelled:CropFailure::PreviewNotReady);
    if (participant->isDocument && !participant->isCommitted)
        SetDocumentComplete(state.commitFailure==DataCommitFailure::None?CropFailure::PreviewNotReady:GetCropFailure(state.commitFailure),state.blockers);
    if (participant->publication && !participant->isCommitted) {
        auto& publication = *participant->publication;
        publication.result.failureReason = state.status==FeatureRunStatus::Cancelled ? CropFailure::Cancelled :
            state.commitFailure==DataCommitFailure::None ? CropFailure::PreviewNotReady : GetCropFailure(state.commitFailure);
        publication.result.blockers = state.blockers;
        publication.result.message = "Crop publication was rejected; previous binding and result are preserved.";
        SetBuildFailed(publication.completeState, publication.completeItem, std::move(publication.result));
    }
    m_sourceTransition.reset();
    if (participant->isCommitted) (void)SendSceneDelta(FeatureScenePriority::Scene);
    return true;
}

bool CropHostFeature::Impl::CancelSourcePreview()
{
    const auto participant=m_sourceTransition;
    if (!participant) return true;
    const auto state=m_host->StopDataTransition(participant->requestId);
    if (state.status==FeatureRunStatus::Preparing || state.status==FeatureRunStatus::Running) return false;
    if (!participant->isCommitted)
        m_bridge->SetSourceCommitFailed(std::move(participant->prepared),CropFailure::Cancelled);
    if (participant->publication && !participant->isCommitted) {
        auto& publication = *participant->publication;
        publication.result.failureReason = CropFailure::Cancelled;
        publication.result.message = "Crop publication was cancelled before commit.";
        SetBuildFailed(publication.completeState, publication.completeItem, std::move(publication.result));
    }
    m_sourceTransition.reset();
    return true;
}

void CropHostFeature::Impl::ClearBuildingResult()
{
    if (!m_buildRecord) return;
    auto records=m_bridge->GetHistory(0,1).results;
    const auto end=std::remove_if(records.begin(),records.end(),[&](const auto& record) {
        return record.resultId==m_buildRecord->resultId && record.status==CropResultStatus::Building;
    });
    if (end!=records.end()) {
        records.erase(end,records.end());m_bridge->SetResults(std::move(records));
    }
    m_buildRecord.reset();
}

bool CropHostFeature::Impl::SendResultReleases()
{
    if (m_sourceTransition) return false;
    auto records=m_bridge->GetHistory(0,1).results;
    auto batch=m_data->StartDataChanges();
    if (!batch) return false;
    bool changed=false;
    for (auto record=records.begin();record!=records.end();) {
        if (record->status==CropResultStatus::Releasing
            && m_data->SetDataRelease(record->scopeId).status==DataLifetimeStatus::Released) {
            record=records.erase(record);changed=true;
        } else ++record;
    }
    if (changed) m_bridge->SetResults(std::move(records));
    return changed;
}

CropDocumentAdmission CropHostFeature::Impl::SendRequest(CropDocumentRequest request,CropDocumentCallback onComplete)
{
    CropDocumentAdmission admission;admission.requestId=request.requestId;
    if (!GetOwnerReady() || !m_isAttached || !m_bridge || m_isPublishing) {
        admission.failureReason=CropFailure::PreviewNotReady;return admission;
    }
    const auto history=m_bridge->GetCropHistory();admission.stateRevision=history.stateRevision;
    const auto found=m_documentCommands.find(request.requestId);
    if (found!=m_documentCommands.end()) {
        const auto& original=found->second.request;
        if (original.documentId!=request.documentId || original.action!=request.action || original.expectedRevision!=request.expectedRevision) {
            admission.failureReason=CropFailure::InvalidRequest;return admission;
        }
        admission.isAccepted=true;admission.isReplay=true;
        if (!history.documentId) admission.stateRevision=found->second.outcome.stateRevision;
        return admission;
    }
    const bool isClosed=request.documentId==m_lastClosedDocument && request.documentId!=0
        && m_dataState.documentStatus==CropDocumentStatus::Closed && request.action==CropDocumentAction::CloseDocument;
    if (isClosed) admission.stateRevision=m_lastClosedRevision;
    if (!request.requestId || !request.documentId || (request.documentId!=history.documentId && !isClosed)
        || (request.action!=CropDocumentAction::ReturnToSource && request.action!=CropDocumentAction::CloseDocument)) {
        admission.failureReason=CropFailure::InvalidRequest;return admission;
    }
    if (request.requestId<=m_expiredDocumentRequest) {admission.failureReason=CropFailure::RequestExpired;return admission;}
    if (request.expectedRevision!=admission.stateRevision) {admission.failureReason=CropFailure::StateVersionMismatch;return admission;}
    if (const auto prior=m_bridge->GetOutcome(request.requestId);prior && prior->failureReason!=CropFailure::RequestExpired) {
        admission.failureReason=CropFailure::InvalidRequest;return admission;
    }
    if (m_pendingDocumentRequest) {admission.failureReason=CropFailure::Busy;return admission;}
    if (m_dataState.documentStatus==CropDocumentStatus::Closing && request.action!=CropDocumentAction::CloseDocument) {
        admission.failureReason=CropFailure::ResultReleasing;return admission;
    }
    while (m_documentCommands.size()>=1024 && !m_documentAdmissionOrder.empty()) {
        const auto id=m_documentAdmissionOrder.front();
        const auto old=m_documentCommands.find(id);
        if (old==m_documentCommands.end()) {m_documentAdmissionOrder.pop_front();continue;}
        if (!old->second.isDelivered) {admission.failureReason=CropFailure::ResourceLimit;return admission;}
        m_expiredDocumentRequest=std::max(m_expiredDocumentRequest,id);
        m_documentCommands.erase(old);m_documentAdmissionOrder.pop_front();
    }
    DocumentCommand command;command.request=request;command.onComplete=std::move(onComplete);
    command.outcome.documentId=request.documentId;command.outcome.requestId=request.requestId;
    command.outcome.rootNodeId=m_bridge->GetHistory(0,1).rootNodeId;
    command.outcome.stateRevision=admission.stateRevision;
    if (isClosed) {command.outcome.status=CropEditStatus::Succeeded;command.outcome.documentStatus=CropDocumentStatus::Closed;}
    m_documentCommands.emplace(request.requestId,std::move(command));
    m_documentAdmissionOrder.push_back(request.requestId);
    if (isClosed) {admission.isAccepted=true;(void)m_host->SendWorkAvailable();return admission;}
    m_pendingDocumentRequest=request.requestId;m_documentSourceCommitted=false;
    m_dataState.documentStatus=request.action==CropDocumentAction::CloseDocument?CropDocumentStatus::Closing:CropDocumentStatus::Returning;
    m_dataState.failureReason=CropFailure::None;m_dataState.blockers.clear();
    if (!CancelSourcePreview() || !m_bridge->CancelPending()) SetDocumentComplete(CropFailure::Busy);
    else (void)m_bridge->ExitCrop();
    admission.isAccepted=true;
    (void)m_host->SendWorkAvailable();
    return admission;
}

std::optional<CropDocumentOutcome> CropHostFeature::Impl::GetDocumentOutcome(CropDocumentId documentId,CropRequestId requestId) const
{
    if (!GetOwnerReady()) return {};
    const auto found=m_documentCommands.find(requestId);
    if (found==m_documentCommands.end()) {
        if (requestId && requestId<=m_expiredDocumentRequest
            && (documentId==m_lastClosedDocument || documentId==m_bridge->GetCropHistory().documentId)) {
            CropDocumentOutcome expired;expired.documentId=documentId;expired.requestId=requestId;
            expired.status=CropEditStatus::Failed;expired.failureReason=CropFailure::RequestExpired;return expired;
        }
        return {};
    }
    if (found->second.request.documentId!=documentId) return {};
    auto outcome=found->second.outcome;
    if (outcome.status==CropEditStatus::Queued) {
        outcome.stateRevision=m_bridge->GetCropHistory().stateRevision;
        outcome.documentStatus=m_dataState.documentStatus;outcome.blockers=m_dataState.blockers;
    }
    return outcome;
}

void CropHostFeature::Impl::SetDocumentComplete(CropFailure failure,std::vector<DataLifetimeBlocker> blockers,std::uint64_t completedRevision)
{
    if (!m_pendingDocumentRequest) return;
    auto& entry=m_documentCommands.at(m_pendingDocumentRequest);
    entry.outcome.failureReason=failure;entry.outcome.blockers=std::move(blockers);
    entry.outcome.stateRevision=completedRevision?completedRevision:m_bridge->GetCropHistory().stateRevision;
    entry.outcome.status=failure==CropFailure::None?CropEditStatus::Succeeded:
        failure==CropFailure::Cancelled?CropEditStatus::Cancelled:CropEditStatus::Failed;
    if (failure!=CropFailure::None && entry.request.action!=CropDocumentAction::CloseDocument)
        m_dataState.documentStatus=CropDocumentStatus::Ready;
    entry.outcome.documentStatus=m_dataState.documentStatus;
    m_dataState.failureReason=failure;m_dataState.blockers=entry.outcome.blockers;
    m_pendingDocumentRequest=0;
}

bool CropHostFeature::Impl::SendDocumentRequest()
{
    if (!m_pendingDocumentRequest || m_sourceTransition) return false;
    if (m_isBuildPending) {
        if (m_bridge->GetBuildTickNeeded()) (void)m_bridge->SendBuildResult();
        if (m_isBuildPending) return false;
    }
    if (!m_documentSourceCommitted) {
        if (!StartSourcePreview(true)) SetDocumentComplete(CropFailure::PreviewNotReady);
        return true;
    }
    (void)SendResultReleases();
    const auto history=m_bridge->GetHistory(0,1);
    m_dataState.blockers.clear();
    for (const auto& record:history.results) {
        const auto state=m_data->GetDataLifetime(record.scopeId);
        m_dataState.blockers.insert(m_dataState.blockers.end(),state.blockers.begin(),state.blockers.end());
    }
    if (!history.results.empty()) return false;
    const bool closing=m_documentCommands.at(m_pendingDocumentRequest).request.action==CropDocumentAction::CloseDocument;
    if (closing) {
        if (!m_bridge->ClearDocument() || !SetActiveViews({})) return false;
        m_lastClosedDocument=history.documentId;m_lastClosedRevision=history.stateRevision;
        m_activeTarget.reset();m_activeViewIds.clear();m_dataState.documentStatus=CropDocumentStatus::Closed;
    } else m_dataState.documentStatus=CropDocumentStatus::Ready;
    SetDocumentComplete(CropFailure::None,{},history.stateRevision);
    return true;
}

void CropHostFeature::Impl::SendDocumentCompletes()
{
    std::vector<std::function<void()>> callbacks;
    for (auto& pair:m_documentCommands) {
        auto& entry=pair.second;
        if (entry.isDelivered || entry.outcome.status==CropEditStatus::Queued) continue;
        if (entry.onComplete) {
            callbacks.push_back([callback=std::move(entry.onComplete),outcome=entry.outcome]() mutable {
                try {callback(std::move(outcome));} catch (...) {}
            });
        }
        entry.isDelivered=true;
    }
    const auto host=m_host;
    for (auto& callback:callbacks) if (!host || !host->SendOwnerComplete(callback)) callback();
}

bool CropHostFeature::Impl::OnHostTick()
{
    if (!GetOwnerReady()
        || !m_isAttached
        || !m_bridge
        || m_isPublishing) {
        return false;
    }
    if (m_sourceTransition) (void)SendSourcePreview();
    if (m_pendingDocumentRequest) {
        (void)SendDocumentRequest();
        (void)SendReadyCompletes();SendDocumentCompletes();
        return true;
    }
    if (!m_sourceTransition && m_bridge->GetShaderTickNeeded()
        && m_bridge->SendShaderCommit()) {
        (void)SendSceneDelta(FeatureScenePriority::Scene);
    }
    if (!m_sourceTransition && m_bridge->GetSourceTransitionNeeded()) (void)StartSourcePreview();
    // Drain accepted previews first, while the worker remains fixed to its captured node.
    if (!m_sourceTransition && !m_bridge->GetShaderTickNeeded() && m_bridge->GetBuildTickNeeded())
        (void)m_bridge->SendBuildResult();
    if (!m_sourceTransition) (void)SendResultReleases();
    if (!m_bridge->GetCropActive() && !m_bridge->GetShaderTickNeeded()) (void)SetActiveViews({});
    (void)SendReadyCompletes();SendDocumentCompletes();
    return true;
}

CropHostFeature::CropHostFeature()
    : m_impl(std::make_unique<Impl>())
{
}

CropHostFeature::~CropHostFeature() noexcept = default;

std::string_view CropHostFeature::GetFeatureId() const noexcept
{
    return kFeatureId;
}

FeatureDataContract CropHostFeature::GetDataContract() const
{
    return FeatureDataContract{
        {
            DataInputSpec{
                "source-image", DataFacets::scalarGrid3D, false },
            DataInputSpec{
                "source-mesh", DataFacets::surfaceMesh, false }
        },
        {
            DataOutputSpec{
                "crop-recipe", DataTypes::roiGeometry,
                { DataFacets::roiGeometry } },
            DataOutputSpec{
                "derived-image", DataTypes::imageGrid3D,
                { DataFacets::scalarGrid3D } },
            DataOutputSpec{
                "derived-mesh", DataTypes::surfaceMesh,
                { DataFacets::surfaceMesh } }
        } };
}

bool CropHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl && m_impl->AttachHost(*this, context);
}

bool CropHostFeature::DetachHost()
{
    const auto keepAlive=weak_from_this().lock();
    return !m_impl || m_impl->DetachHost();
}

bool CropHostFeature::OnHostTick()
{
    const auto keepAlive=weak_from_this().lock();
    return m_impl && m_impl->OnHostTick();
}

bool CropHostFeature::SendRequest(
    CropHostRequest request,
    CropBuildCallback onComplete)
{
    const auto keepAlive=weak_from_this().lock();
    return m_impl
        && m_impl->SendRequest(
            std::move(request), std::move(onComplete));
}

CropHostState CropHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : CropHostState{};
}

std::vector<FeatureOperationState> CropHostFeature::GetOperationStates() const
{
    return m_impl ? m_impl->GetOperationStates() : std::vector<FeatureOperationState>{};
}

CropDocumentAdmission CropHostFeature::SendRequest(CropDocumentRequest request,CropDocumentCallback onComplete)
{
    const auto keepAlive=weak_from_this().lock();
    return m_impl->SendRequest(std::move(request),std::move(onComplete));
}
std::optional<CropDocumentOutcome> CropHostFeature::GetDocumentOutcome(CropDocumentId documentId,CropRequestId requestId) const
{ return m_impl->GetDocumentOutcome(documentId,requestId); }

CropRequestId CropHostFeature::CreateRequestId() noexcept { return CropHistory::CreateNodeId(); }
CropEditAdmission CropHostFeature::Impl::SendRequest(CropEditRequest request)
{
    CropEditAdmission rejected;rejected.requestId=request.requestId;rejected.failureReason=CropFailure::PreviewNotReady;
    if(!GetOwnerReady() || !m_isAttached || m_isPublishing || !m_bridge) return rejected;
    const auto history=m_bridge->GetHistory(0,1);
    const auto document=m_documentCommands.find(request.requestId);
    const bool isRoot=request.kind==CropEditKind::Select && request.documentId==history.documentId && request.nodeId==history.rootNodeId;
    const bool isRootReplay=request.kind==CropEditKind::Select && document!=m_documentCommands.end()
        && document->second.request.action==CropDocumentAction::ReturnToSource
        && request.documentId==document->second.request.documentId && request.nodeId==document->second.outcome.rootNodeId;
    if (isRoot || isRootReplay) {
        CropDocumentRequest returning;returning.documentId=request.documentId;returning.requestId=request.requestId;
        returning.expectedRevision=request.expectedRevision;
        const auto admitted=SendRequest(returning,{});
        CropEditAdmission result;result.isAccepted=admitted.isAccepted;result.isReplay=admitted.isReplay;
        result.failureReason=admitted.failureReason;result.requestId=admitted.requestId;result.stateRevision=admitted.stateRevision;
        result.nodeId=request.nodeId;return result;
    }
    if (document!=m_documentCommands.end()) {rejected.failureReason=CropFailure::InvalidRequest;return rejected;}
    if (m_pendingDocumentRequest || m_dataState.documentStatus==CropDocumentStatus::Closing || m_dataState.documentStatus==CropDocumentStatus::Closed)
        return rejected;
    return m_bridge->SendRequest(std::move(request));
}
CropEditAdmission CropHostFeature::SendRequest(CropEditRequest request) { return m_impl->SendRequest(std::move(request)); }
CropHistorySnapshot CropHostFeature::Impl::GetHistory(CropDocumentId documentId,CropNodeId after,std::size_t limit) const
{
    if(!GetOwnerReady() || !m_isAttached || !m_bridge) return {};
    if(documentId && documentId!=m_bridge->GetCropHistory().documentId)return {};
    return m_bridge->GetHistory(after,limit);
}
CropHistorySnapshot CropHostFeature::GetHistory(CropDocumentId documentId,CropNodeId after,std::size_t limit) const
{ return m_impl->GetHistory(documentId,after,limit); }
std::optional<CropEditOutcome> CropHostFeature::Impl::GetOutcome(CropDocumentId documentId,CropRequestId requestId) const
{
    if(!GetOwnerReady() || !m_isAttached || !m_bridge)return std::nullopt;
    const auto edit=documentId==m_bridge->GetCropHistory().documentId?m_bridge->GetOutcome(requestId):std::optional<CropEditOutcome>{};
    if (edit && edit->failureReason!=CropFailure::RequestExpired) return edit;
    if (const auto document=GetDocumentOutcome(documentId,requestId)) {
        CropEditOutcome result;result.requestId=requestId;result.nodeId=document->rootNodeId;
        result.stateRevision=document->stateRevision;result.status=document->status;result.failureReason=document->failureReason;
        return result;
    }
    if(documentId!=m_bridge->GetCropHistory().documentId)return std::nullopt;
    return m_bridge->GetOutcome(requestId);
}
std::optional<CropEditOutcome> CropHostFeature::GetOutcome(CropDocumentId documentId,CropRequestId requestId) const
{ return m_impl->GetOutcome(documentId,requestId); }
CropPruneImpact CropHostFeature::Impl::GetPruneImpact(CropDocumentId documentId,const CropPruneRequest& request) const
{
    if(!GetOwnerReady() || !m_isAttached || !m_bridge
        || documentId!=m_bridge->GetCropHistory().documentId) {
        CropPruneImpact rejected;rejected.failureReason=CropFailure::InvalidRequest;return rejected;
    }
    return m_bridge->GetPruneImpact(request);
}
CropPruneImpact CropHostFeature::GetPruneImpact(CropDocumentId documentId,const CropPruneRequest& request) const
{ return m_impl->GetPruneImpact(documentId,request); }
