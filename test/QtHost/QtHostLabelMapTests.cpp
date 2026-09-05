#include "QtHostMethodCases.h"

#include "Host/HostFeature.h"
#include "Data/VtkDataBridge.h"
#include "Data/DataGraphStore.h"
#include "Data/LabelMapReader.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view labelMapId =
    "label-map-probe.labels";

bool SendTimer(vtkRenderWindowInteractor* interactor)
{
    if (!interactor) return false;
    int timerId = interactor->GetTimerEventId();
    if (timerId == 0) {
        for (int candidate = 1; candidate <= 64; ++candidate) {
            if (interactor->GetTimerDuration(candidate) != 0) {
                timerId = candidate;
                break;
            }
        }
    }
    if (timerId == 0) return false;
    interactor->InvokeEvent(vtkCommand::TimerEvent, &timerId);
    return true;
}

bool Reload(
    VtkAppHostSession& session,
    vtkRenderWindowInteractor* interactor,
    std::string datasetId,
    const float baseValue)
{
    HostReloadRequest request;
    request.geometry.dimensions = { 2, 2, 2 };
    request.geometry.spacing = { 0.5f, 0.75f, 1.25f };
    request.geometry.origin = { 2.0f, 3.0f, 4.0f };
    request.metadata.identity.datasetId = std::move(datasetId);
    request.metadata.source.kind = ImageSourceKind::Memory;
    request.metadata.source.uri =
        "memory://" + request.metadata.identity.datasetId;
    request.voxels.resize(8);
    for (std::size_t index = 0; index < request.voxels.size(); ++index) {
        request.voxels[index] = baseValue
            + static_cast<float>(index);
    }
    bool isComplete = false;
    bool isSucceeded = false;
    if (!session.SendRequest(
            std::move(request),
            [&isComplete, &isSucceeded](const bool value) {
                isSucceeded = value;
                isComplete = true;
            })) {
        return false;
    }
    constexpr int pollCount = 1000;
    for (int poll = 0; !isComplete && poll < pollCount; ++poll) {
        if (!SendTimer(interactor)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return isComplete && isSucceeded;
}

struct PreparedLabel final {
    DataTransaction transaction;
    DataRevisionRef ref;
};

class LabelMapProbeFeature final : public HostFeature {
public:
    explicit LabelMapProbeFeature(std::string id) : m_id(std::move(id)) {}
    std::string_view GetFeatureId() const noexcept override { return m_id; }
    FeatureDataContract GetDataContract() const override
    {
        return { { { "source-volume", DataFacets::scalarGrid3D, true } },
            { { "labels", DataTypes::labelMap3D, { DataFacets::labelMap3D } } } };
    }
    bool AttachHost(const HostFeatureContext& context) override
    { m_data = context.data; return m_data != nullptr; }
    bool DetachHost() override
    {
        if (m_data) {
            const auto binding = m_data->GetDataBinding(m_data->GetDataGraph(), GetBindingName());
            if (binding && binding->target && Remove(*binding->target).status != DataCommitStatus::Succeeded) return false;
        }
        m_data.reset();
        m_lastInput = nullptr;
        return true;
    }
    bool OnHostTick() override { return true; }
    DataGraphSnapshot GetGraph() const { return m_data ? m_data->GetDataGraph() : DataGraphSnapshot{}; }
    std::optional<PreparedLabel> Prepare(std::optional<DataRevisionRef> expected = {},
        std::uint32_t baseValue = 0, bool wrongGeometry = false)
    {
        const auto source = m_data ? m_data->GetPrimaryImage() : nullptr;
        if (!source || !source->data || !source->binding) return std::nullopt;
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->CopyStructure(source->image);
        if (wrongGeometry) image->SetSpacing(9, 9, 9);
        image->AllocateScalars(VTK_UNSIGNED_INT, 1);
        auto* values = static_cast<std::uint32_t*>(image->GetScalarPointer());
        for (vtkIdType index = 0; index < image->GetNumberOfPoints(); ++index)
            values[index] = baseValue + static_cast<std::uint32_t>(index);
        m_lastInput = image;
        VtkDataBridge bridge;
        const auto frozen = bridge.CreateLabelPayload(image);
        if (!frozen) return std::nullopt;
        auto payload = std::make_shared<const LabelMap3DPayload>(frozen->GetGeometry(),
            frozen->GetValues(), std::vector<LabelDefinition>{}, std::string(labelMapId), "Probe labels");
        const auto binding = m_data->GetDataBinding(source->graph, GetBindingName());
        const auto entity = expected ? expected->entityId : m_data->CreateDataEntityId();
        const auto generation = expected ? expected->generation : DataGeneration{0};
        PreparedLabel prepared;
        prepared.ref = { entity, generation + 1 };
        prepared.transaction.outputs.push_back({ entity, generation, DataTypes::labelMap3D,
            { { "source-volume", source->data->self } }, payload,
            DataProvenance{ m_id, "publish-labels", "1", "{}" } });
        DataExpectation sourceExpected;
        sourceExpected.kind = DataExpectationKind::Binding;
        sourceExpected.binding = std::string(primaryVolumeBinding);
        sourceExpected.expectedBindingRevision = source->binding->revision;
        sourceExpected.isTargetChecked = true;
        sourceExpected.expectedTarget = source->data->self;
        prepared.transaction.expectations.push_back(sourceExpected);
        prepared.transaction.bindings.push_back({ GetBindingName(), binding ? binding->revision : 0,
            true, expected, prepared.ref });
        return prepared;
    }
    DataCommitResult Commit(const PreparedLabel& prepared)
    { return m_data ? m_data->SetDataCommit(prepared.transaction) : DataCommitResult{}; }
    DataCommitResult Remove(DataRevisionRef expected)
    {
        if (!m_data) return {};
        const auto binding = m_data->GetDataBinding(m_data->GetDataGraph(), GetBindingName());
        if (!binding) return {};
        DataTransaction transaction;
        transaction.bindings.push_back({ GetBindingName(), binding->revision, true, expected, {} });
        return m_data->SetDataCommit(std::move(transaction));
    }
    void OverwriteLastInput(std::uint32_t value)
    {
        if (!m_lastInput) return;
        auto* values = static_cast<std::uint32_t*>(m_lastInput->GetScalarPointer());
        std::fill(values, values + m_lastInput->GetNumberOfPoints(), value);
        m_lastInput->Modified();
    }
private:
    std::string GetBindingName() const { return "labels." + m_id; }
    std::string m_id;
    std::shared_ptr<TrustedDataPort> m_data;
    vtkSmartPointer<vtkImageData> m_lastInput;
};

bool GetReadContractValid(
    VtkAppHostSession& session,
    const DataRevisionRef revision)
{
    LabelMapReadRequest request;
    request.id = labelMapId;
    request.expectedRevision = revision;
    request.maxBytes = 7 * sizeof(std::uint32_t);
    const auto tooLarge = session.GetLabelMapReadResult(request);
    request.maxBytes = 8 * sizeof(std::uint32_t);
    const auto whole = session.GetLabelMapReadResult(request);
    const std::array<std::uint32_t, 8> expected = {
        0, 1, 2, 3, 4, 5, 6, 7
    };
    if (tooLarge.error != LabelMapError::TooLarge
        || tooLarge.requiredBytes != sizeof(expected)
        || tooLarge.state
        || whole.error != LabelMapError::None
        || whole.requiredBytes != sizeof(expected)
        || !whole.state
        || !whole.state->values
        || whole.state->values->size() != sizeof(expected)
        || std::memcmp(
            whole.state->values->data(),
            expected.data(),
            sizeof(expected)) != 0) {
        return false;
    }

    request.region = ImageReadRegion{
        { 1, 0, 0 }, { 1, 2, 2 }
    };
    request.maxBytes = 4 * sizeof(std::uint32_t);
    const auto region = session.GetLabelMapReadResult(request);
    const std::array<std::uint32_t, 4> expectedRegion = {
        1, 3, 5, 7
    };
    if (region.error != LabelMapError::None
        || !region.state
        || region.state->dims != std::array<int, 3>{ 1, 2, 2 }
        || region.state->origin
            != std::array<double, 3>{ -2.0, -3.75, 4.0 }
        || !region.state->values
        || std::memcmp(
            region.state->values->data(),
            expectedRegion.data(),
            sizeof(expectedRegion)) != 0) {
        return false;
    }

    request.region.reset();
    request.maxBytes = 2 * sizeof(std::uint32_t);
    std::vector<std::uint32_t> chunks;
    std::size_t offset = 0;
    bool isDone = false;
    while (!isDone) {
        const auto chunk = session.GetLabelMapReadChunk(
            request, offset);
        if (chunk.error != LabelMapError::None
            || !chunk.state
            || !chunk.state->values
            || chunk.state->voxelCount != 2) {
            return false;
        }
        std::array<std::uint32_t, 2> values{};
        std::memcpy(
            values.data(),
            chunk.state->values->data(),
            sizeof(values));
        chunks.insert(chunks.end(), values.begin(), values.end());
        offset = chunk.nextVoxelOffset;
        isDone = chunk.isDone;
    }
    request.expectedRevision = DataRevisionRef{ revision.entityId, revision.generation + 1 };
    return chunks == std::vector<std::uint32_t>(
            expected.begin(), expected.end())
        && session.GetLabelMapReadResult(request).error
            == LabelMapError::VersionMismatch;
}

bool GetLabelMapLifecycleValid()
{
    VtkAppHostSession unavailable(HostSessionConfig{});
    LabelMapReadRequest unavailableRequest;
    unavailableRequest.id = labelMapId;
    if (unavailable.GetLabelMapReadResult(unavailableRequest).error
        != LabelMapError::Unavailable) {
        return false;
    }

    HostRenderViewConfig view;
    view.id = "label-map-primary";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    view.renderWindow->SetOffScreenRendering(1);
    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));
    if (!session.BuildSession()) return false;
    const auto* endpoint = session.GetPrimaryEndpoint();
    if (!endpoint || !endpoint->interactor) return false;
    endpoint->interactor->Initialize();
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView.viewId = "label-map-primary";
    if (!session.AttachTimer(timer)
        || !session.Start()
        || !Reload(
            session, endpoint->interactor,
            "label-map-dataset-a", 0.0f)) {
        return false;
    }

    auto feature = std::make_shared<LabelMapProbeFeature>(
        "label-map-probe");
    auto other = std::make_shared<LabelMapProbeFeature>(
        "label-map-other");
    if (!session.AttachFeature(feature)
        || !session.AttachFeature(other)
        || !session.GetLabelMapDescriptors().empty()
        || session.GetLabelMapDescriptor(std::string{ labelMapId })) {
        return false;
    }

    const auto before = feature->GetGraph();
    const auto invalid = feature->Prepare({}, 0, true);
    if (!invalid || feature->Commit(*invalid).failureReason != DataCommitFailure::PayloadInvalid
        || feature->GetGraph().commitId != before.commitId
        || feature->GetGraph().view->GetData(invalid->ref)) return false;
    const auto prepared = feature->Prepare();
    if (!prepared || !session.GetLabelMapDescriptors().empty()) return false;
    DataCommitResult wrongThread;
    std::thread worker([&] { wrongThread = feature->Commit(*prepared); });
    worker.join();
    if (wrongThread.status != DataCommitStatus::Rejected
        || feature->GetGraph().commitId != before.commitId) return false;
    feature->OverwriteLastInput(99);
    const auto first = feature->Commit(*prepared);
    if (first.status != DataCommitStatus::Succeeded || first.published.size() != 1
        || first.commitId != before.commitId + 1 || before.view->GetData(prepared->ref)) return false;
    const auto descriptor = session.GetLabelMapDescriptor(std::string(labelMapId));
    if (!descriptor || session.GetLabelMapDescriptors().size() != 1
        || descriptor->datasetId != "label-map-dataset-a"
        || descriptor->producerFeatureId != "label-map-probe"
        || descriptor->dataRevision != prepared->ref
        || !GetDataRevisionRefValid(descriptor->sourceRevision)
        || descriptor->valueType != ImageValueType::UInt32 || descriptor->componentCount != 1
        || descriptor->voxelCount != 8 || !GetReadContractValid(session, prepared->ref)) return false;

    // 并发候选使用同一预期绑定；只有第一笔能进入图，另一笔不能发布任何节点。
    const auto replacement = feature->Prepare(prepared->ref, 100);
    const auto competing = feature->Prepare(prepared->ref, 200);
    if (!replacement || !competing) return false;
    const auto second = feature->Commit(*replacement);
    const auto graphAfterSecond = feature->GetGraph();
    if (second.status != DataCommitStatus::Succeeded
        || replacement->ref.generation != prepared->ref.generation + 1
        || feature->Commit(*competing).status != DataCommitStatus::Rejected
        || feature->GetGraph().commitId != graphAfterSecond.commitId
        || !graphAfterSecond.view->GetData(prepared->ref)
        || !first.graph.view->GetData(prepared->ref)) return false;
    LabelMapReadRequest oldRead;
    oldRead.id = labelMapId;
    oldRead.expectedRevision = prepared->ref;
    if (session.GetLabelMapReadResult(oldRead).error != LabelMapError::VersionMismatch
        || feature->Remove(prepared->ref).status != DataCommitStatus::Rejected) return false;
    const auto stale = feature->Prepare(replacement->ref, 300);
    if (!stale || !Reload(session, endpoint->interactor, "label-map-dataset-b", 10)
        || !session.GetLabelMapDescriptors().empty()
        || session.GetLabelMapReadResult(oldRead).error != LabelMapError::NotFound) return false;
    const auto afterReload = feature->GetGraph();
    if (feature->Commit(*stale).status != DataCommitStatus::Rejected
        || feature->GetGraph().commitId != afterReload.commitId
        || afterReload.view->GetData(stale->ref)) return false;
    const auto next = feature->Prepare(replacement->ref, 400);
    if (!next || feature->Commit(*next).status != DataCommitStatus::Succeeded) return false;
    const auto nextDescriptor = session.GetLabelMapDescriptor(std::string(labelMapId));
    if (!nextDescriptor || nextDescriptor->datasetId != "label-map-dataset-b"
        || nextDescriptor->sourceRevision == descriptor->sourceRevision
        || !session.DetachFeature(*feature) || !session.GetLabelMapDescriptors().empty()) return false;
    auto reattached = std::make_shared<LabelMapProbeFeature>("label-map-probe");
    if (!session.AttachFeature(reattached)) return false;
    const auto abandoned = reattached->Prepare();
    const auto beforeDetach = reattached->GetGraph();
    if (!abandoned || !session.DetachFeature(*reattached)
        || reattached->Commit(*abandoned).status != DataCommitStatus::Rejected
        || beforeDetach.view->GetData(abandoned->ref)) return false;
    auto finalFeature = std::make_shared<LabelMapProbeFeature>("label-map-probe");
    if (!session.AttachFeature(finalFeature)) return false;
    const auto final = finalFeature->Prepare();
    if (!final || finalFeature->Commit(*final).status != DataCommitStatus::Succeeded
        || !session.DetachFeature(*finalFeature) || !session.DetachFeature(*other)
        || !session.Stop()) return false;
    return session.GetLabelMapDescriptors().empty()
        && session.GetLabelMapReadResult(unavailableRequest).error == LabelMapError::Unavailable;
}

bool GetIntegerLabelsValid()
{
    DataGraphStore store;
    VtkDataBridge bridge;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(-2, -1, 3, 4, 0, 1);
    image->SetOrigin(10, 20, 30);
    image->SetSpacing(2, 3, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(static_cast<float*>(image->GetScalarPointer()), 8, 0.0f);
    ImageMetadata metadata;
    metadata.identity.datasetId = "integer-label-dataset";
    metadata.source.uri = "memory://integer-label-dataset";
    metadata.source.byteSize = 8 * sizeof(float);
    const auto sourcePayload = bridge.CreateImagePayload(image, nullptr, metadata);
    const DataRevisionRef source{ store.CreateDataEntityId(), 1 };
    DataTransaction load;
    load.outputs.push_back({ source.entityId, 0, DataTypes::imageGrid3D, {}, sourcePayload, {} });
    load.bindings.push_back({ std::string(primaryVolumeBinding), 0, true, {}, source });
    if (store.SetDataCommit(std::move(load)).status != DataCommitStatus::Succeeded) return false;
    const auto check = [&](auto tag, int vtkType) {
        using Integer = decltype(tag);
        std::array<Integer, 8> values{};
        for (std::size_t index = 0; index < values.size(); ++index) values[index] = static_cast<Integer>(index);
        values[0] = std::numeric_limits<Integer>::min();
        values[7] = std::numeric_limits<Integer>::max();
        if constexpr (sizeof(Integer) == 8) {
            values[1] = static_cast<Integer>(9007199254740993ULL);
            values[2] = static_cast<Integer>(9007199254740995ULL);
        }
        if constexpr (std::is_signed_v<Integer>) values[3] = Integer{-1};
        auto labels = vtkSmartPointer<vtkImageData>::New();
        labels->CopyStructure(image);
        labels->AllocateScalars(vtkType, 1);
        std::memcpy(labels->GetScalarPointer(), values.data(), sizeof(values));
        const auto payload = bridge.CreateLabelPayload(labels);
        if (!payload) return false;
        std::memset(labels->GetScalarPointer(), 0, sizeof(values));
        const auto graph = store.GetDataGraph();
        const auto binding = graph.view->GetDataBinding("integer-labels");
        const DataRevisionRef ref{ store.CreateDataEntityId(), 1 };
        DataTransaction transaction;
        transaction.outputs.push_back({ ref.entityId, 0, DataTypes::labelMap3D,
            { { "source-volume", source } }, payload,
            DataProvenance{ "integer-probe", "label", "1", "{}" } });
        transaction.bindings.push_back({ "integer-labels", binding ? binding->revision : 0,
            true, binding ? binding->target : std::nullopt, ref });
        const auto commit = store.SetDataCommit(std::move(transaction));
        if (commit.status != DataCommitStatus::Succeeded) return false;
        const auto view = bridge.GetLabelMap(commit.published.front());
        if (!view || view->labels->GetScalarType() != vtkType
            || std::memcmp(view->labels->GetScalarPointer(), values.data(), sizeof(values)) != 0) return false;
        LabelMapReader reader(commit.graph);
        LabelMapReadRequest request;
        request.id = "integer-probe.labels";
        request.expectedRevision = ref;
        const auto whole = reader.GetReadResult(request);
        if (whole.error != LabelMapError::None || !whole.state || !whole.state->values
            || whole.state->origin != std::array<double, 3>{6, 29, 30}
            || whole.state->values->size() != sizeof(values)
            || std::memcmp(whole.state->values->data(), values.data(), sizeof(values)) != 0) return false;
        request.region = ImageReadRegion{ {1, 1, 1}, {1, 1, 1} };
        request.maxBytes = sizeof(Integer);
        const auto region = reader.GetReadChunk(request, 0);
        if (region.error != LabelMapError::None || !region.state || !region.state->values
            || region.state->origin != std::array<double, 3>{8, 32, 34}
            || !region.isDone || region.nextVoxelOffset != 1
            || std::memcmp(region.state->values->data(), &values[7], sizeof(Integer)) != 0) return false;
        const auto done = reader.GetReadChunk(request, 1);
        if (done.error != LabelMapError::None || !done.isDone || done.state) return false;
        request.region = ImageReadRegion{ {2, 0, 0}, {1, 1, 1} };
        return reader.GetReadResult(request).error == LabelMapError::InvalidRegion;
    };
    auto invalid = vtkSmartPointer<vtkImageData>::New();
    invalid->CopyStructure(image);
    invalid->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(static_cast<float*>(invalid->GetScalarPointer()), 8, 1.0f);
    return !bridge.CreateLabelPayload(invalid)
        && check(std::int8_t{}, VTK_SIGNED_CHAR) && check(std::uint8_t{}, VTK_UNSIGNED_CHAR)
        && check(std::int16_t{}, VTK_SHORT) && check(std::uint16_t{}, VTK_UNSIGNED_SHORT)
        && check(std::int32_t{}, VTK_INT) && check(std::uint32_t{}, VTK_UNSIGNED_INT)
        && check(std::int64_t{}, VTK_LONG_LONG) && check(std::uint64_t{}, VTK_UNSIGNED_LONG_LONG);
}


} // namespace

int GetLabelMapFailCount()
{
    int failures = GetCaseResult(GetLabelMapLifecycleValid(),
        "Session LabelMap reads immutable DataGraph revisions and atomically rejects stale transactions") ? 0 : 1;
    failures += GetCaseResult(GetIntegerLabelsValid(),
        "LabelMap preserves eight integer types, 64-bit values and nonzero-extent region geometry") ? 0 : 1;
    return failures;
}
