#include "QtHostMethodCases.h"

#include "Host/HostFeature.h"
#include "Host/VtkAppHostSession.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <array>
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

class LabelMapProbeFeature final : public HostFeature {
public:
    explicit LabelMapProbeFeature(std::string id)
        : m_id(std::move(id))
    {
    }

    std::string_view GetFeatureId() const noexcept override
    {
        return m_id;
    }

    bool AttachHost(const HostFeatureContext& context) override
    {
        if (!context.data || !context.labelMaps) return false;
        m_data = context.data;
        m_labelMaps = context.labelMaps;
        return true;
    }

    bool DetachHost() override
    {
        m_lastInput = nullptr;
        m_labelMaps.reset();
        m_data.reset();
        return true;
    }

    bool OnHostTick() override
    {
        return true;
    }

    TrustedLabelMapStageResult StageUInt32(
        const std::optional<LabelMapVersion> expectedVersion = std::nullopt,
        const std::uint32_t baseValue = 0,
        const bool hasGeometryMismatch = false,
        const bool hasSourceMismatch = false)
    {
        const auto source = m_data
            ? m_data->GetImageSnapshot() : TrustedImageSnapshot{};
        if (!source || !source->image || !m_labelMaps) return {};
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->CopyStructure(source->image);
        if (hasGeometryMismatch) {
            image->SetSpacing(9.0, 9.0, 9.0);
        }
        image->AllocateScalars(VTK_UNSIGNED_INT, 1);
        auto* values = static_cast<std::uint32_t*>(
            image->GetScalarPointer());
        if (!values) return {};
        const auto voxelCount = static_cast<std::size_t>(
            image->GetNumberOfPoints());
        for (std::size_t index = 0; index < voxelCount; ++index) {
            values[index] = baseValue
                + static_cast<std::uint32_t>(index);
        }
        m_lastInput = image;
        TrustedLabelMapCandidate candidate;
        candidate.id = labelMapId;
        candidate.displayName = "Probe labels";
        candidate.datasetId = hasSourceMismatch
            ? "other-dataset"
            : source->metadata.identity.datasetId;
        candidate.sourceVersion = source->version;
        candidate.expectedVersion = expectedVersion;
        candidate.image = std::move(image);
        return m_labelMaps->StageLabelMap(std::move(candidate));
    }

    TrustedLabelMapStageResult StageFloat()
    {
        const auto source = m_data
            ? m_data->GetImageSnapshot() : TrustedImageSnapshot{};
        if (!source || !source->image || !m_labelMaps) return {};
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->CopyStructure(source->image);
        image->AllocateScalars(VTK_FLOAT, 1);
        TrustedLabelMapCandidate candidate;
        candidate.id = labelMapId;
        candidate.displayName = "Invalid float labels";
        candidate.datasetId = source->metadata.identity.datasetId;
        candidate.sourceVersion = source->version;
        candidate.image = std::move(image);
        return m_labelMaps->StageLabelMap(std::move(candidate));
    }

    TrustedLabelMapCommitResult Commit(
        const LabelMapStageToken token)
    {
        return m_labelMaps
            ? m_labelMaps->CommitLabelMap(token)
            : TrustedLabelMapCommitResult{};
    }

    bool Discard(const LabelMapStageToken token) noexcept
    {
        return m_labelMaps
            && m_labelMaps->DiscardLabelMapStage(token);
    }

    TrustedLabelMapRemoveResult Remove(
        const std::optional<LabelMapVersion> expectedVersion)
    {
        return m_labelMaps
            ? m_labelMaps->RemoveLabelMap(
                labelMapId, expectedVersion)
            : TrustedLabelMapRemoveResult{};
    }

    TrustedLabelMapCommitResult CommitFromWrongThread(
        const LabelMapStageToken token)
    {
        TrustedLabelMapCommitResult result;
        std::thread worker([this, token, &result]() {
            result = Commit(token);
        });
        worker.join();
        return result;
    }

    void OverwriteLastInput(const std::uint32_t value)
    {
        if (!m_lastInput) return;
        auto* values = static_cast<std::uint32_t*>(
            m_lastInput->GetScalarPointer());
        const auto voxelCount = static_cast<std::size_t>(
            m_lastInput->GetNumberOfPoints());
        std::fill(values, values + voxelCount, value);
        m_lastInput->Modified();
    }

private:
    std::string m_id;
    std::shared_ptr<TrustedFeatureDataPort> m_data;
    std::shared_ptr<TrustedLabelMapPort> m_labelMaps;
    vtkSmartPointer<vtkImageData> m_lastInput;
};

bool GetReadContractValid(
    VtkAppHostSession& session,
    const LabelMapVersion version)
{
    LabelMapReadRequest request;
    request.id = labelMapId;
    request.expectedVersion = version;
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
    request.expectedVersion = version + 1;
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

    const auto geometryMismatch = feature->StageUInt32(
        std::nullopt, 0, true, false);
    const auto sourceMismatch = feature->StageUInt32(
        std::nullopt, 0, false, true);
    const auto unsupported = feature->StageFloat();
    if (geometryMismatch.error != LabelMapError::GeometryMismatch
        || sourceMismatch.error != LabelMapError::SourceMismatch
        || unsupported.error != LabelMapError::UnsupportedType) {
        return false;
    }

    const auto staged = feature->StageUInt32();
    if (staged.error != LabelMapError::None
        || staged.token == 0
        || !staged.candidate
        || staged.candidate->descriptor.version == 0
        || !session.GetLabelMapDescriptors().empty()
        || feature->StageUInt32().error != LabelMapError::Busy
        || other->Commit(staged.token).error
            != LabelMapError::OwnerMismatch
        || feature->CommitFromWrongThread(staged.token).error
            != LabelMapError::WrongThread) {
        return false;
    }
    feature->OverwriteLastInput(99);
    const auto committed = feature->Commit(staged.token);
    if (committed.error != LabelMapError::None
        || committed.published != staged.candidate) {
        return false;
    }
    const LabelMapVersion firstVersion =
        committed.published->descriptor.version;
    const auto descriptors = session.GetLabelMapDescriptors();
    const auto descriptor = session.GetLabelMapDescriptor(
        std::string{ labelMapId });
    if (descriptors.size() != 1
        || !descriptor
        || descriptor->producerFeatureId != "label-map-probe"
        || descriptor->datasetId != "label-map-dataset-a"
        || descriptor->sourceVersion == 0
        || descriptor->version != firstVersion
        || descriptor->valueType != ImageValueType::UInt32
        || descriptor->componentCount != 1
        || descriptor->voxelCount != 8
        || !GetReadContractValid(session, firstVersion)) {
        return false;
    }

    const auto replacement = feature->StageUInt32(
        firstVersion, 100);
    if (replacement.error != LabelMapError::None
        || !replacement.candidate
        || replacement.candidate->descriptor.version <= firstVersion) {
        return false;
    }
    const auto replacementCommit = feature->Commit(replacement.token);
    if (replacementCommit.error != LabelMapError::None
        || !replacementCommit.published) {
        return false;
    }
    const LabelMapVersion secondVersion =
        replacementCommit.published->descriptor.version;
    LabelMapReadRequest oldRead;
    oldRead.id = labelMapId;
    oldRead.expectedVersion = firstVersion;
    if (session.GetLabelMapReadResult(oldRead).error
            != LabelMapError::VersionMismatch
        || feature->Remove(firstVersion).error
            != LabelMapError::VersionMismatch) {
        return false;
    }

    if (!Reload(
            session, endpoint->interactor,
            "label-map-dataset-b", 10.0f)
        || !session.GetLabelMapDescriptors().empty()
        || session.GetLabelMapReadResult(oldRead).error
            != LabelMapError::NotFound) {
        return false;
    }
    const auto nextDatasetStage = feature->StageUInt32(
        secondVersion, 200);
    const auto nextDatasetCommit = feature->Commit(
        nextDatasetStage.token);
    if (nextDatasetStage.error != LabelMapError::None
        || nextDatasetCommit.error != LabelMapError::None
        || !nextDatasetCommit.published
        || nextDatasetCommit.published->descriptor.datasetId
            != "label-map-dataset-b"
        || nextDatasetCommit.published->descriptor.version
            <= secondVersion) {
        return false;
    }

    if (!session.DetachFeature(*feature)
        || !session.GetLabelMapDescriptors().empty()) {
        return false;
    }
    auto replacementFeature =
        std::make_shared<LabelMapProbeFeature>("label-map-probe");
    if (!session.AttachFeature(replacementFeature)) return false;
    const auto abandoned = replacementFeature->StageUInt32();
    if (abandoned.error != LabelMapError::None
        || !session.DetachFeature(*replacementFeature)) {
        return false;
    }
    auto finalFeature =
        std::make_shared<LabelMapProbeFeature>("label-map-probe");
    if (!session.AttachFeature(finalFeature)) return false;
    const auto finalStage = finalFeature->StageUInt32();
    const auto finalCommit = finalFeature->Commit(finalStage.token);
    if (finalStage.error != LabelMapError::None
        || finalCommit.error != LabelMapError::None
        || !session.DetachFeature(*finalFeature)
        || !session.DetachFeature(*other)
        || !session.Stop()) {
        return false;
    }
    return session.GetLabelMapDescriptors().empty()
        && session.GetLabelMapReadResult(unavailableRequest).error
            == LabelMapError::Unavailable;
}

} // namespace

int GetLabelMapFailCount()
{
    return GetCaseResult(
        GetLabelMapLifecycleValid(),
        "Session LabelMap store stages, commits, reads, versions and retires atomically")
        ? 0 : 1;
}
