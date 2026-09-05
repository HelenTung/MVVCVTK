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
#include <array>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view kFeatureId = "OrthogonalCrop";
constexpr std::string_view cropResultBinding =
    "analysis.crop.active";
constexpr std::string_view cropInputBinding =
    "feature.crop.input";
constexpr std::size_t kCommandKeyCount = 10;
constexpr std::size_t kNodeKeyCount =
    std::tuple_size_v<decltype(CropHostKeys::nodes)>;

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

DataBytes CreateMaskBytes(vtkImageData* mask)
{
    auto* scalars = mask && mask->GetPointData()
        ? mask->GetPointData()->GetScalars() : nullptr;
    const auto count = mask ? mask->GetNumberOfPoints() : 0;
    if (!scalars || count <= 0
        || scalars->GetDataType() != VTK_UNSIGNED_CHAR
        || scalars->GetNumberOfComponents() != 1
        || !scalars->GetVoidPointer(0)) {
        return {};
    }
    const auto byteCount = static_cast<std::size_t>(count);
    auto values = std::make_shared<std::vector<std::uint8_t>>(byteCount);
    std::memcpy(values->data(), scalars->GetVoidPointer(0), byteCount);
    return values;
}

std::shared_ptr<const RoiGeometryPayload> CreateRecipePayload(
    const std::vector<CropOpItem>& operations)
{
    std::vector<RoiPrimitive> primitives;
    primitives.reserve(operations.size());
    for (const auto& operation : operations) {
        RoiPrimitive primitive;
        primitive.operation = operation.removalMode
            == CropRemovalMode::KeepInside
            ? "keep-inside" : "remove-inside";
        if (operation.geometryType == CropShape::Box) {
            primitive.shape = RoiShape::Box;
            primitive.localToSource = operation.boxToInputModelMatrix;
        }
        else if (operation.geometryType == CropShape::Plane) {
            primitive.shape = RoiShape::Plane;
            primitive.origin = operation.planeCenterInInputModel;
            primitive.normal = operation.planeNormalInInputModel;
        }
        else {
            return {};
        }
        primitives.push_back(std::move(primitive));
    }
    auto payload = std::make_shared<const RoiGeometryPayload>(
        std::move(primitives));
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

bool GetCharMatched(
    const InteractionEvent& event,
    const char keyCode)
{
    if (keyCode == 0) {
        return false;
    }
    const char upper = keyCode >= 'a' && keyCode <= 'z'
        ? static_cast<char>(keyCode - 'a' + 'A')
        : keyCode;
    return event.keyCode == keyCode
        || event.keyCode == upper
        || event.keySym == std::string(1, keyCode)
        || event.keySym == std::string(1, upper);
}

bool GetChordMatched(
    const InteractionEvent& event,
    const HostKeyChord& chord)
{
    const bool hasKey = GetCharMatched(event, chord.keyCode)
        || (!chord.keySym.empty()
            && event.keySym == chord.keySym);
    return hasKey
        && event.isCtrlDown == chord.isCtrlDown
        && event.isAltDown == chord.isAltDown
        && event.isShiftDown == chord.isShiftDown;
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

    explicit Impl(CropHostConfig config)
        : m_config(std::move(config))
        , m_bridge(std::make_unique<CropBridge>())
    {
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

private:
    bool StartCrop(const CropHostTarget& target);
    std::optional<CropInputSnapshot> GetCropInput(
        const CropHostTarget& target) const;
    bool GetTargetsValid(
        const HostViewTargets& targets) const;
    bool SetActiveViews(
        const std::vector<std::string>& viewIds) const;
    bool SetCropInput(const CropHostTarget& target);
    bool SetPolyData(vtkSmartPointer<vtkPolyData> polyData);
    bool ClearPolyData();
    bool ResetOriginal();
    bool SetPrimaryResult();
    CropBuildResult SetBuildResult(
        const CropInputSnapshot& source,
        const DataBinding& resultBinding,
        CropMaterializationCandidate candidate);
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
    InteractionResult OnInput(const InteractionEvent& event);
    std::optional<std::size_t> GetKeyIndex(
        const InteractionEvent& event) const;
    CropHostRequest GetKeyRequest(
        std::size_t keyIndex) const;
    bool SendActionLog(
        const char* action,
        bool isAccepted) const;
    static const char* GetActionText(
        CropHostAction action);
    static const char* GetModeText(
        CropRemovalMode removalMode);
    static std::string GetHistoryText(
        const CropHistoryState& state);
    void SendStatus();
    void ClearBorrowed();

    CropHostConfig m_config;
    std::unique_ptr<CropBridge> m_bridge;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    CropHostState m_dataState;
    std::optional<CropHostTarget> m_activeTarget;
    std::vector<std::string> m_activeViewIds;
    std::optional<CropHistoryState> m_status;
    std::shared_ptr<CompleteState> m_completeState;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextSceneRequestId = 1;
    std::array<bool,
        kCommandKeyCount + kNodeKeyCount> m_isDown{};
    bool m_isPublishing = false;
    bool m_isAttached = false;
};

bool CropHostFeature::Impl::AttachHost(
    CropHostFeature& owner,
    const HostFeatureContext& context)
{
    if (m_isAttached) {
        return m_views == context.views
            && m_data == context.data
            && m_host == context.host
            && m_ownerThread == std::this_thread::get_id();
    }
    if (owner.weak_from_this().expired()
        || !context.views
        || !context.data
        || !context.host) {
        return false;
    }

    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_completeState = std::make_shared<CompleteState>();
    m_ownerThread = std::this_thread::get_id();
    m_dataState = {};

    if (!m_config.inputViews.viewIds.empty()
        || !m_config.inputViews.viewRoles.empty()) {
        std::weak_ptr<CropHostFeature> weakOwner =
            owner.weak_from_this();
        HostInputBinding binding;
        binding.featureId = std::string(kFeatureId);
        binding.targetViews = m_config.inputViews;
        binding.onInput = [weakOwner](
            const InteractionEvent& event) {
            const auto feature = weakOwner.lock();
            return feature && feature->m_impl
                ? feature->m_impl->OnInput(event)
                : InteractionResult{};
        };
        if (!m_host->AttachInput(std::move(binding))) {
            ClearBorrowed();
            return false;
        }
    }
    m_isAttached = true;
    return true;
}

bool CropHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) {
        return true;
    }
    if (m_ownerThread != std::this_thread::get_id()) {
        return false;
    }

    bool isInputDetached =
        m_config.inputViews.viewIds.empty()
        && m_config.inputViews.viewRoles.empty();
    if (!isInputDetached && m_host) {
        try {
            isInputDetached =
                m_host->DetachInput(kFeatureId);
        }
        catch (...) {
            isInputDetached = false;
        }
    }

    // 输入解绑失败时 Feature 仍留在 Session 注册表等待重试，但可见状态和质量来源必须先退休，
    // 避免宿主关闭流程因一个输入端口异常而长期保持 Crop/Quality。
    CancelCompletes();
    if (m_bridge) {
        if (m_bridge->GetCropActive()) {
            (void)m_bridge->ExitCrop();
        }
        (void)m_bridge->ClearBindings();
    }
    (void)ClearResultBinding();
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_status.reset();
    m_dataState = {};
    m_isDown.fill(false);
    if (!isInputDetached) {
        return false;
    }
    m_isAttached = false;
    ClearBorrowed();
    return true;
}

void CropHostFeature::Impl::ClearBorrowed()
{
    if (m_host) {
        (void)m_host->SetActiveViews({});
    }
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
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
    (void)target;
    if (!m_data) return std::nullopt;
    const auto graph = m_data->GetDataGraph();
    if (!graph.view) return std::nullopt;

    auto binding = m_data->GetDataBinding(graph, cropInputBinding);
    if (!binding || !binding->target) {
        binding = m_data->GetDataBinding(graph, primaryVolumeBinding);
    }
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

    const auto& requestedTargets = target.isTargetViewsUsed
        ? target.targetViews
        : m_config.defaultTarget.targetViews;
    if (!GetTargetsValid(requestedTargets)) {
        return false;
    }
    const auto targetViews = m_views->GetViews(requestedTargets);
    auto input = GetCropInput(target);
    if (targetViews.empty() || !input) {
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
        SendStatus();
    }
    return isStarted;
}

bool CropHostFeature::Impl::SetPolyData(
    vtkSmartPointer<vtkPolyData> polyData)
{
    if (!m_data || !polyData || m_isPublishing) return false;
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
    m_status.reset();
    m_dataState = {};
    return true;
}

bool CropHostFeature::Impl::ClearPolyData()
{
    if (!m_data || m_isPublishing) return false;
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
    m_status.reset();
    m_dataState = {};
    return true;
}

CropBuildResult CropHostFeature::Impl::SetBuildResult(
    const CropInputSnapshot& source,
    const DataBinding& resultBinding,
    CropMaterializationCandidate candidate)
{
    CropBuildResult result;
    result.failureReason = candidate.failureReason;
    result.failureOperationIndex = candidate.failureOperationIndex;
    result.nodeCount = candidate.nodeCount;
    result.sourceRevision = candidate.sourceRevision;
    result.message = std::move(candidate.message);
    if (!candidate.isSucceeded || !m_data || m_isPublishing
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

    auto recipe = CreateRecipePayload(candidate.operations);
    std::shared_ptr<const IDataPayload> output;
    DataTypeId outputType;
    if (source.image && candidate.imageData && candidate.maskImage) {
        const auto* sourcePayload = dynamic_cast<const ImageGrid3DPayload*>(
            source.data->payload.get());
        const auto mask = CreateMaskBytes(candidate.maskImage);
        auto image = sourcePayload
            ? sourcePayload->CreateMaskSnapshot(mask) : nullptr;
        if (image && image->GetValid()) {
            output = std::move(image);
            outputType = DataTypes::imageGrid3D;
        }
    }
    else if (source.mesh && candidate.polyData) {
        auto mesh = CreateMeshPayload(candidate.polyData);
        if (mesh && mesh->GetValid()) {
            output = std::move(mesh);
            outputType = DataTypes::surfaceMesh;
        }
    }
    if (!recipe || !output || !GetDataTypeIdValid(outputType)) {
        result.failureReason = CropFailure::BadInput;
        result.message = "Crop formal payload construction failed.";
        return result;
    }

    const auto recipeEntity = m_data->CreateDataEntityId();
    const auto outputEntity = m_data->CreateDataEntityId();
    const DataRevisionRef recipeRef{ recipeEntity, 1 };
    const DataRevisionRef outputRef{ outputEntity, 1 };
    DataExpectation sourceExpectation;
    sourceExpectation.kind = DataExpectationKind::Binding;
    sourceExpectation.binding = source.binding->name;
    sourceExpectation.expectedBindingRevision = source.binding->revision;
    sourceExpectation.isTargetChecked = true;
    sourceExpectation.expectedTarget = source.data->self;

    DataTransaction transaction;
    transaction.expectations.push_back(std::move(sourceExpectation));
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

    const PublishGuard publishGuard(m_isPublishing);
    const auto commit = m_data->SetDataCommit(std::move(transaction));
    if (commit.status != DataCommitStatus::Succeeded) {
        result.failureReason = CropFailure::VersionMismatch;
        result.message =
            "Crop source or active result binding changed before publication.";
        return result;
    }
    result.isSucceeded = true;
    result.failureReason = CropFailure::None;
    result.commitId = commit.commitId;
    result.recipeRevision = recipeRef;
    result.outputRevision = outputRef;
    result.message = "Crop recipe and derived output committed.";
    m_dataState.commitId = commit.commitId;
    m_dataState.sourceRevision = source.data->self;
    m_dataState.recipeRevision = recipeRef;
    m_dataState.outputRevision = outputRef;
    SendStatus();
    return result;
}

bool CropHostFeature::Impl::ResetOriginal()
{
    if (!m_data || m_isPublishing
        || !GetDataRevisionRefValid(m_dataState.outputRevision)) {
        return false;
    }
    const auto graph = m_data->GetDataGraph();
    const auto output = m_data->GetData(
        graph, m_dataState.outputRevision);
    if (!output) return false;
    const auto source = std::find_if(
        output->inputs.begin(), output->inputs.end(),
        [](const DataInputRef& input) {
            return input.role == "source-data";
        });
    if (source == output->inputs.end()) return false;
    const auto sourceData = m_data->GetData(graph, source->source);
    const auto primary = m_data->GetDataBinding(
        graph, primaryVolumeBinding);
    if (!sourceData || !primary
        || !GetFacetUsed(
            graph, sourceData->type, DataFacets::scalarGrid3D)) {
        return false;
    }
    DataTransaction transaction;
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(primaryVolumeBinding),
        primary->revision,
        true,
        primary->target,
        source->source });
    const PublishGuard guard(m_isPublishing);
    const bool isCommitted = m_data->SetDataCommit(std::move(transaction)).status
        == DataCommitStatus::Succeeded;
    if (isCommitted) {
        (void)SendSceneDelta(FeatureScenePriority::Scene, RenderInputStamp{ source->source });
    }
    return isCommitted;
}

bool CropHostFeature::Impl::SetPrimaryResult()
{
    if (!m_data || m_isPublishing
        || !GetDataRevisionRefValid(m_dataState.outputRevision)) {
        return false;
    }
    const auto graph = m_data->GetDataGraph();
    const auto output = m_data->GetData(
        graph, m_dataState.outputRevision);
    const auto primary = m_data->GetDataBinding(
        graph, primaryVolumeBinding);
    const auto active = m_data->GetDataBinding(
        graph, cropResultBinding);
    if (!output || !primary || !active || !active->target
        || *active->target != m_dataState.outputRevision
        || !GetFacetUsed(
            graph, output->type, DataFacets::scalarGrid3D)) {
        return false;
    }
    DataExpectation activeExpectation;
    activeExpectation.kind = DataExpectationKind::Binding;
    activeExpectation.binding = std::string(cropResultBinding);
    activeExpectation.expectedBindingRevision = active->revision;
    activeExpectation.isTargetChecked = true;
    activeExpectation.expectedTarget = active->target;
    DataTransaction transaction;
    transaction.expectations.push_back(std::move(activeExpectation));
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(primaryVolumeBinding),
        primary->revision,
        true,
        primary->target,
        m_dataState.outputRevision });
    const PublishGuard guard(m_isPublishing);
    const bool isCommitted = m_data->SetDataCommit(std::move(transaction)).status
        == DataCommitStatus::Succeeded;
    if (isCommitted) {
        (void)SendSceneDelta(FeatureScenePriority::Scene, RenderInputStamp{ m_dataState.outputRevision });
    }
    return isCommitted;
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
    return m_host->SendSceneDelta(std::move(delta));
}

std::uint64_t CropHostFeature::Impl::GetNextSceneRequestId() noexcept
{
    const std::uint64_t requestId = m_nextSceneRequestId++;
    if (m_nextSceneRequestId == 0) m_nextSceneRequestId = 1;
    return requestId == 0 ? GetNextSceneRequestId() : requestId;
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
        || !m_data) {
        return false;
    }
    auto source = GetCropInput(target);
    if (!source || !source->binding || !source->data
        || !m_bridge->SetCropInput(*source)) {
        return false;
    }
    DataBinding resultBinding;
    resultBinding.name = std::string(cropResultBinding);
    if (const auto current = m_data->GetDataBinding(
            source->graph, cropResultBinding)) {
        resultBinding = *current;
    }
    const auto history = m_bridge->GetCropHistory();
    std::cout << "[Crop][Materialize] request"
        << " sourceGeneration="
        << source->data->self.generation
        << " | " << GetHistoryText(history)
        << '\n';
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
    auto onResult =
        [this, source = *source, resultBinding,
            weakState, weakItem](
            CropMaterializationCandidate candidate) mutable {
            const auto state = weakState.lock();
            const auto item = weakItem.lock();
            auto result = SetBuildResult(
                source, resultBinding, std::move(candidate));
            if (!Impl::SetCompleteResult(
                    state,
                    item,
                    std::move(result), {}, {})) {
                return;
            }
        };
    const bool isAccepted = m_bridge->BuildCropResult(
        std::move(*source),
        std::move(onResult));
    if (!isAccepted) {
        // Bridge 可以在同步校验失败时先回传内部结果；入口返回 false 时必须丢弃，
        // 保证未接纳请求的外部 callback 永远不会在后续 tick 泄漏。
        (void)RemoveComplete(state, item);
    }
    return isAccepted;
}

bool CropHostFeature::Impl::SendRequest(
    CropHostRequest request,
    CropBuildCallback onComplete)
{
    if (!m_isAttached
        || m_ownerThread != std::this_thread::get_id()
        || m_isPublishing
        || ((request.action == CropHostAction::BuildResult)
            != static_cast<bool>(onComplete))) {
        return false;
    }
    switch (request.action) {
    case CropHostAction::Start:
        if (request.target) {
            return SendActionLog(
                "Start", StartCrop(*request.target));
        }
        return false;
    case CropHostAction::Box:
        if (request.target) {
            const bool isAccepted =
                StartCrop(*request.target)
                && m_bridge->SwitchCropBox();
            return SendActionLog(
                "SwitchBox", isAccepted);
        }
        return false;
    case CropHostAction::Plane:
        if (request.target) {
            const bool isAccepted =
                StartCrop(*request.target)
                && m_bridge->SwitchCropPlane();
            return SendActionLog(
                "SwitchPlane", isAccepted);
        }
        return false;
    case CropHostAction::Mode:
        if (request.target && request.removalMode) {
            const bool isAccepted =
                StartCrop(*request.target)
                && m_bridge->SetCropMode(
                    *request.removalMode);
            return SendActionLog(
                GetModeText(*request.removalMode),
                isAccepted);
        }
        return false;
    case CropHostAction::Previous:
        return SendActionLog(
            "Previous",
            m_activeTarget.has_value()
            && m_bridge->GetCropBound()
            && m_bridge->PreviousCrop());
    case CropHostAction::Next:
        return SendActionLog(
            "Next",
            m_activeTarget.has_value()
            && m_bridge->GetCropBound()
            && m_bridge->NextCrop());
    case CropHostAction::Node:
        if (request.nodeCount) {
            const bool isAccepted =
                m_activeTarget.has_value()
                && m_bridge->GetCropBound()
                && m_bridge->SetCropNode(
                    *request.nodeCount);
            std::cout
                << "[Crop][Request] targetNode="
                << *request.nodeCount << '\n';
            return SendActionLog(
                "SetNode", isAccepted);
        }
        return false;
    case CropHostAction::BuildResult:
        if (request.target) {
            return BuildCropResult(
                *request.target, std::move(onComplete));
        }
        return false;
    case CropHostAction::SetPrimaryResult:
        return SendActionLog(
            "SetPrimaryResult", SetPrimaryResult());
    case CropHostAction::SetPolyData:
        if (request.polyData) {
            return SetPolyData(std::move(request.polyData));
        }
        return false;
    case CropHostAction::ClearPolyData:
        return ClearPolyData();
    case CropHostAction::RestoreOriginal:
        return SendActionLog(
            "RestoreOriginal",
            ResetOriginal());
    case CropHostAction::Exit:
        m_status.reset();
        {
            const bool isResultCleared = ClearResultBinding();
            const bool isExited = m_bridge->ExitCrop();
            const bool isQualityRestored =
                !isExited || SetActiveViews({});
            if (isResultCleared) m_dataState = {};
            return SendActionLog(
                "Exit",
                isResultCleared && isExited && isQualityRestored);
        }
    case CropHostAction::None:
        return false;
    }
    return false;
}

CropHostState CropHostFeature::Impl::GetState() const
{
    CropHostState state = m_dataState;
    if (!m_isAttached
        || !m_bridge
        || m_ownerThread != std::this_thread::get_id()) {
        return state;
    }

    // owner thread 上连续读取 history、active 和发布屏障，形成同一时刻的只读快照。
    state.history = m_bridge->GetCropHistory();
    state.isActive = m_activeTarget.has_value()
        && m_bridge->GetCropActive();
    state.isPublishing = m_isPublishing;
    return state;
}

std::optional<std::size_t>
CropHostFeature::Impl::GetKeyIndex(
    const InteractionEvent& event) const
{
    const std::array<const HostKeyChord*,
        kCommandKeyCount> keys = {
        &m_config.keys.box,
        &m_config.keys.plane,
        &m_config.keys.noMode,
        &m_config.keys.keepMode,
        &m_config.keys.removeMode,
        &m_config.keys.previous,
        &m_config.keys.next,
        &m_config.keys.buildResult,
        &m_config.keys.restoreOriginal,
        &m_config.keys.exit
    };
    for (std::size_t index = 0;
        index < keys.size(); ++index) {
        if (GetChordMatched(event, *keys[index])) {
            return index;
        }
    }
    for (std::size_t node = 0;
        node < m_config.keys.nodes.size(); ++node) {
        if (GetChordMatched(
                event, m_config.keys.nodes[node])) {
            return keys.size() + node;
        }
    }
    return std::nullopt;
}

CropHostRequest CropHostFeature::Impl::GetKeyRequest(
    const std::size_t keyIndex) const
{
    CropHostRequest request;
    if (keyIndex == 0) {
        request.action = CropHostAction::Box;
        request.target = m_config.defaultTarget;
    }
    else if (keyIndex == 1) {
        request.action = CropHostAction::Plane;
        request.target = m_config.defaultTarget;
    }
    else if (keyIndex >= 2 && keyIndex <= 4) {
        CropRemovalMode mode = CropRemovalMode::None;
        if (keyIndex == 3) {
            mode = CropRemovalMode::KeepInside;
        }
        else if (keyIndex == 4) {
            mode = CropRemovalMode::RemoveInside;
        }
        request.action = CropHostAction::Mode;
        request.target = m_config.defaultTarget;
        request.removalMode = mode;
    }
    else if (keyIndex == 5) {
        request.action = CropHostAction::Previous;
    }
    else if (keyIndex == 6) {
        request.action = CropHostAction::Next;
    }
    else if (keyIndex == 7) {
        request.action = CropHostAction::BuildResult;
        request.target = m_config.defaultTarget;
    }
    else if (keyIndex == 8) {
        request.action = CropHostAction::RestoreOriginal;
    }
    else if (keyIndex == 9) {
        request.action = CropHostAction::Exit;
    }
    else {
        request.action = CropHostAction::Node;
        request.nodeCount = keyIndex - kCommandKeyCount;
    }
    return request;
}

InteractionResult CropHostFeature::Impl::OnInput(
    const InteractionEvent& event)
{
    const auto keyIndex = GetKeyIndex(event);
    if (!keyIndex || *keyIndex >= m_isDown.size()) {
        return {};
    }
    if (event.eventKind
        == InteractionEventKind::KeyRelease) {
        m_isDown[*keyIndex] = false;
        return { true, true };
    }
    if (event.eventKind
        == InteractionEventKind::TextInput) {
        return { true, true };
    }
    if (event.eventKind
        != InteractionEventKind::KeyPress) {
        return {};
    }
    if (m_isDown[*keyIndex]) {
        return { true, true };
    }
    m_isDown[*keyIndex] = true;
    auto request = GetKeyRequest(*keyIndex);
    CropBuildCallback callback;
    if (request.action == CropHostAction::BuildResult) {
        callback = [](CropBuildResult result) {
            std::cout
                << "[Crop][Materialize] callback"
                << " succeeded=" << result.isSucceeded
                << " failure="
                << static_cast<int>(result.failureReason)
                << " sourceGeneration="
                << result.sourceRevision.generation
                << " materializedPrefix="
                << result.nodeCount
                << " outputGeneration="
                << result.outputRevision.generation
                << " message=\"" << result.message
                << "\"\n";
        };
    }
    const auto action = request.action;
    const bool isAccepted = SendRequest(
        std::move(request), std::move(callback));
    std::cout
        << "[Crop][Hotkey] action="
        << GetActionText(action)
        << " keyCode="
        << static_cast<int>(event.keyCode)
        << " keySym=\"" << event.keySym << '"'
        << " ctrl=" << event.isCtrlDown
        << " alt=" << event.isAltDown
        << " shift=" << event.isShiftDown
        << " accepted=" << isAccepted
        << '\n';
    return { true, true };
}

bool CropHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached
        || !m_bridge
        || m_ownerThread != std::this_thread::get_id()
        || m_isPublishing) {
        return false;
    }
    if (m_bridge->GetCropBound()
        && m_activeTarget
        && !SetCropInput(*m_activeTarget)) {
        (void)m_bridge->ClearBindings();
        (void)SetActiveViews({});
        m_activeTarget.reset();
        m_activeViewIds.clear();
        m_status.reset();
    }
    if (m_bridge->GetShaderTickNeeded()
        && m_bridge->SendShaderCommit()) {
        if (!SendSceneDelta(FeatureScenePriority::Scene)) {
            std::cerr
                << "[Crop][History] committed shader delta was rejected.\n";
        }
        std::cout
            << "[Crop][History] shader commit"
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        SendStatus();
    }
    if (m_bridge->GetBuildTickNeeded()) {
        (void)m_bridge->SendBuildResult();
    }
    (void)SendReadyCompletes();
    return true;
}

bool CropHostFeature::Impl::SendActionLog(
    const char* action,
    const bool isAccepted) const
{
    std::cout
        << "[Crop][Request] action="
        << (action ? action : "Unknown")
        << " accepted=" << isAccepted;
    if (m_bridge) {
        std::cout << " | " << GetHistoryText(
            m_bridge->GetCropHistory());
    }
    std::cout << '\n';
    return isAccepted;
}

const char* CropHostFeature::Impl::GetActionText(
    const CropHostAction action)
{
    switch (action) {
    case CropHostAction::Start: return "Start";
    case CropHostAction::Box: return "Box";
    case CropHostAction::Plane: return "Plane";
    case CropHostAction::Mode: return "Mode";
    case CropHostAction::Previous: return "Previous";
    case CropHostAction::Next: return "Next";
    case CropHostAction::Node: return "Node";
    case CropHostAction::BuildResult: return "BuildResult";
    case CropHostAction::SetPrimaryResult:
        return "SetPrimaryResult";
    case CropHostAction::SetPolyData: return "SetPolyData";
    case CropHostAction::ClearPolyData: return "ClearPolyData";
    case CropHostAction::RestoreOriginal:
        return "RestoreOriginal";
    case CropHostAction::Exit: return "Exit";
    case CropHostAction::None: return "None";
    }
    return "Unknown";
}

const char* CropHostFeature::Impl::GetModeText(
    const CropRemovalMode removalMode)
{
    switch (removalMode) {
    case CropRemovalMode::KeepInside:
        return "ModeKeepInside";
    case CropRemovalMode::RemoveInside:
        return "ModeRemoveInside";
    case CropRemovalMode::None:
        return "ModeNone";
    }
    return "ModeUnknown";
}

std::string CropHostFeature::Impl::GetHistoryText(
    const CropHistoryState& state)
{
    std::ostringstream text;
    text << "activeHistoryNode=" << state.nodeCount
        << " activeHistorySize=" << state.operationCount
        << " baseNode=" << state.baseNodeCount
        << " allHistorySize=" << state.allOperationCount;
    return text.str();
}

void CropHostFeature::Impl::SendStatus()
{
    if (!m_activeTarget
        || !m_activeTarget->isStatusVisible
        || !m_bridge
        || !m_host) {
        return;
    }
    const auto state = m_bridge->GetCropHistory();
    if (m_status
        && m_status->nodeCount == state.nodeCount
        && m_status->operationCount
            == state.operationCount
        && m_status->editMode == state.editMode
        && m_status->hasEditableOp
            == state.hasEditableOp
        && m_status->isEditing == state.isEditing
        && m_status->baseNodeCount
            == state.baseNodeCount
        && m_status->allOperationCount
            == state.allOperationCount) {
        return;
    }

    const char* stateText = "Idle";
    if (!state.isEditing && state.operationCount > 0) {
        stateText = "Frozen";
    }
    else if (state.editMode
        == CropRemovalMode::KeepInside) {
        stateText = "KeepInside";
    }
    else if (state.editMode
        == CropRemovalMode::RemoveInside) {
        stateText = "RemoveInside";
    }

    std::ostringstream status;
    status << "Crop active " << state.nodeCount << '/'
        << state.operationCount
        << " | base " << state.baseNodeCount
        << " | all " << state.allOperationCount
        << " | " << stateText;
    if (state.hasEditableOp) {
        status << " | Editable";
    }
    (void)m_host->SetViewStatus(m_activeViewIds, status.str());
    std::cout << "[Crop] " << status.str() << '\n';
    m_status = state;
}

CropHostFeature::CropHostFeature(CropHostConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
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
    return !m_impl || m_impl->DetachHost();
}

bool CropHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

bool CropHostFeature::SendRequest(
    CropHostRequest request,
    CropBuildCallback onComplete)
{
    return m_impl
        && m_impl->SendRequest(
            std::move(request), std::move(onComplete));
}

CropHostState CropHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : CropHostState{};
}
