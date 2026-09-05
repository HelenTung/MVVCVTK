#include "GapDisplayTests.h"

#include "Host/HostFeature.h"
#include "Services/GapAnalysisService.h"
#include "Render/Contracts/FeatureOverlay.h"
#include "Render/Contracts/OverlayService.h"

#include <vtkActor.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

static_assert(noexcept(
    std::declval<OverlayService&>().RemoveOverlay(
        std::declval<std::shared_ptr<FeatureOverlay>>())),
    "Overlay removal must not reopen the StartView exception boundary.");

class OverlayStub final : public OverlayService {
public:
    bool AttachOverlay(
        std::shared_ptr<FeatureOverlay> overlay) override
    {
        if (m_isAttachThrowing) {
            throw std::runtime_error("injected overlay attach failure");
        }
        if (!overlay || !m_isAttachAllowed) {
            return false;
        }
        m_overlay = std::move(overlay);
        m_overlay->AttachRenderer(m_renderer);
        m_inputs.push_back(GetAttachedInput());
        ++m_attachCount;
        return true;
    }

    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        if (m_overlay != overlay) {
            return;
        }
        m_overlay->DetachRenderer(m_renderer);
        m_overlay.reset();
        ++m_removeCount;
    }

    void ClearOverlays() noexcept override
    {
        if (m_overlay) {
            m_overlay->DetachRenderer(m_renderer);
        }
        m_overlay.reset();
    }

    int GetAttachCount() const { return m_attachCount; }
    int GetRemoveCount() const { return m_removeCount; }
    void SetAttachAllowed(const bool value) noexcept
    {
        m_isAttachAllowed = value;
    }
    void SetAttachThrowing(const bool value) noexcept
    {
        m_isAttachThrowing = value;
    }
    std::shared_ptr<FeatureOverlay> GetOverlay() const { return m_overlay; }
    vtkDataObject* GetInput(const std::size_t index) const
    {
        return index < m_inputs.size() ? m_inputs[index] : nullptr;
    }

private:
    vtkDataObject* GetAttachedInput() const
    {
        auto* props = m_renderer->GetViewProps();
        if (!props) {
            return nullptr;
        }
        props->InitTraversal();
        vtkProp* prop = nullptr;
        while (auto* nextProp = props->GetNextProp()) {
            prop = nextProp;
        }
        if (auto* actor = vtkActor::SafeDownCast(prop)) {
            auto* mapper = vtkPolyDataMapper::SafeDownCast(
                actor->GetMapper());
            return mapper ? mapper->GetInput() : nullptr;
        }
        if (auto* slice = vtkImageSlice::SafeDownCast(prop)) {
            auto* mapper = vtkImageResliceMapper::SafeDownCast(
                slice->GetMapper());
            return mapper ? mapper->GetInput() : nullptr;
        }
        return nullptr;
    }

    std::shared_ptr<FeatureOverlay> m_overlay;
    vtkSmartPointer<vtkRenderer> m_renderer =
        vtkSmartPointer<vtkRenderer>::New();
    std::vector<vtkDataObject*> m_inputs;
    int m_attachCount = 0;
    int m_removeCount = 0;
    bool m_isAttachAllowed = true;
    bool m_isAttachThrowing = false;
};

class LabelMapPortStub final : public TrustedLabelMapPort {
public:
    std::vector<LabelMapDescriptor>
        GetLabelMapDescriptors() const override
    {
        return m_published
            ? std::vector<LabelMapDescriptor>{
                m_published->descriptor }
            : std::vector<LabelMapDescriptor>{};
    }

    std::optional<LabelMapDescriptor> GetLabelMapDescriptor(
        const std::string_view id) const override
    {
        return m_published && m_published->descriptor.id == id
            ? std::optional<LabelMapDescriptor>{
                m_published->descriptor }
            : std::optional<LabelMapDescriptor>{};
    }

    LabelMapReadResult GetLabelMapReadResult(
        const LabelMapReadRequest&) const override
    {
        return {};
    }

    LabelMapReadChunkResult GetLabelMapReadChunk(
        const LabelMapReadRequest&,
        std::size_t) const override
    {
        return {};
    }

    TrustedLabelMapSnapshot GetLabelMapSnapshot(
        const std::string_view id) const override
    {
        return m_published && m_published->descriptor.id == id
            ? m_published : TrustedLabelMapSnapshot{};
    }

    TrustedLabelMapStageResult StageLabelMap(
        TrustedLabelMapCandidate candidate) override
    {
        TrustedLabelMapStageResult result;
        if (!candidate.image || m_staged) {
            result.error = m_staged
                ? LabelMapError::Busy : LabelMapError::CopyFailed;
            return result;
        }
        if (m_published
            && (!candidate.expectedVersion
                || *candidate.expectedVersion
                    != m_published->descriptor.version)) {
            result.error = LabelMapError::VersionMismatch;
            return result;
        }

        auto image = vtkSmartPointer<vtkImageData>::New();
        image->DeepCopy(candidate.image);
        auto state = std::make_shared<TrustedLabelMapState>();
        state->descriptor.id = std::move(candidate.id);
        state->descriptor.displayName =
            std::move(candidate.displayName);
        state->descriptor.producerFeatureId = "GapAnalysis";
        state->descriptor.datasetId = std::move(candidate.datasetId);
        state->descriptor.sourceVersion = candidate.sourceVersion;
        state->descriptor.version = m_nextVersion++;
        image->GetExtent(state->descriptor.extent.data());
        image->GetDimensions(state->descriptor.dims.data());
        image->GetSpacing(state->descriptor.spacing.data());
        image->GetOrigin(state->descriptor.origin.data());
        image->GetScalarRange(state->descriptor.scalarRange.data());
        state->descriptor.valueType = ImageValueType::Int32;
        state->descriptor.componentBytes = sizeof(std::int32_t);
        state->descriptor.componentCount = 1;
        state->descriptor.voxelCount = static_cast<std::size_t>(
            image->GetNumberOfPoints());
        state->image = std::move(image);
        m_staged = state;
        m_stageToken = m_nextToken++;
        result.error = LabelMapError::None;
        result.token = m_stageToken;
        result.candidate = std::move(state);
        return result;
    }

    TrustedLabelMapCommitResult CommitLabelMap(
        const LabelMapStageToken token) override
    {
        TrustedLabelMapCommitResult result;
        ++m_commitAttemptCount;
        if (m_failCommit) {
            result.error = LabelMapError::CopyFailed;
            return result;
        }
        if (!m_staged || token != m_stageToken) {
            result.error = LabelMapError::NotFound;
            return result;
        }
        m_published = std::move(m_staged);
        m_stageToken = 0;
        result.error = LabelMapError::None;
        result.published = m_published;
        ++m_commitCount;
        return result;
    }

    bool DiscardLabelMapStage(
        const LabelMapStageToken token) noexcept override
    {
        if (!m_staged || token != m_stageToken) return false;
        m_staged.reset();
        m_stageToken = 0;
        ++m_discardCount;
        return true;
    }

    TrustedLabelMapRemoveResult RemoveLabelMap(
        const std::string_view id,
        const std::optional<LabelMapVersion> expectedVersion) override
    {
        TrustedLabelMapRemoveResult result;
        if (!m_published || m_published->descriptor.id != id) {
            result.error = LabelMapError::NotFound;
            return result;
        }
        if (expectedVersion
            && *expectedVersion != m_published->descriptor.version) {
            result.error = LabelMapError::VersionMismatch;
            return result;
        }
        result.error = LabelMapError::None;
        result.isRemoved = true;
        result.removedVersion = m_published->descriptor.version;
        m_published.reset();
        ++m_removeCount;
        return result;
    }

    TrustedLabelMapSnapshot GetPublished() const noexcept
    {
        return m_published;
    }

    int GetCommitAttemptCount() const noexcept
    {
        return m_commitAttemptCount;
    }
    int GetCommitCount() const noexcept { return m_commitCount; }
    int GetDiscardCount() const noexcept { return m_discardCount; }
    int GetRemoveCount() const noexcept { return m_removeCount; }
    void SetFailCommit(const bool value) noexcept
    {
        m_failCommit = value;
    }

private:
    TrustedLabelMapSnapshot m_staged;
    TrustedLabelMapSnapshot m_published;
    LabelMapVersion m_nextVersion = 1;
    LabelMapStageToken m_nextToken = 1;
    LabelMapStageToken m_stageToken = 0;
    int m_commitAttemptCount = 0;
    int m_commitCount = 0;
    int m_discardCount = 0;
    int m_removeCount = 0;
    bool m_failCommit = false;
};

vtkSmartPointer<vtkImageData> GetMask(
    vtkImageData* image,
    unsigned char value = 255)
{
    if (!image) {
        return nullptr;
    }
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* values = static_cast<unsigned char*>(
        mask->GetScalarPointer());
    if (!values) {
        return nullptr;
    }
    std::fill_n(values, mask->GetNumberOfPoints(), value);
    mask->Modified();
    return mask;
}

TrustedImageSnapshot BuildTrustedInput(
    const vtkSmartPointer<vtkImageData>& image)
{
    if (!image || !image->GetPointData()
        || !image->GetPointData()->GetScalars()) {
        return {};
    }

    auto snapshot = std::make_shared<TrustedImageState>();
    snapshot->image = image;
    int dims[3] = {};
    double spacing[3] = {};
    double origin[3] = {};
    double scalarRange[2] = {};
    image->GetDimensions(dims);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);
    image->GetScalarRange(scalarRange);
    snapshot->dims = { dims[0], dims[1], dims[2] };
    snapshot->spacing = { spacing[0], spacing[1], spacing[2] };
    snapshot->origin = { origin[0], origin[1], origin[2] };
    snapshot->scalarRange = {
        scalarRange[0], scalarRange[1] };
    snapshot->metadata.identity.datasetId =
        "gap-display-dataset";
    snapshot->metadata.source.kind = ImageSourceKind::Memory;
    snapshot->metadata.source.uri =
        "memory://gap-display-dataset";
    snapshot->metadata.source.byteSize =
        static_cast<std::uint64_t>(image->GetNumberOfPoints())
        * static_cast<std::uint64_t>(image->GetScalarSize())
        * static_cast<std::uint64_t>(
            image->GetNumberOfScalarComponents());
    snapshot->version = 1;
    return snapshot;
}

bool StartDisplay(
    GapAnalysisService& service,
    GapViewRequest request,
    vtkImageData* image)
{
    bool isCompleted = false;
    bool isSucceeded = false;
    if (!service.StartView(
            std::move(request),
            [&](const bool isSuccess) {
                isCompleted = true;
                isSucceeded = isSuccess;
            })) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (service.GetAnalysisState() == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    service.OnDisplayTick(image);
    return service.GetAnalysisState() == GapAnalysisState::Succeeded
        && isCompleted && isSucceeded;
}

}

int GapDisplaySuite::GetFailCount() const
{
    int failureCount = 0;
    const auto expect = [&failureCount](bool isExpected, const char* message) {
        if (!isExpected) {
            std::cerr << message << '\n';
            ++failureCount;
        }
    };

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(5, 5, 5);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* voxels = static_cast<float*>(image->GetScalarPointer());
    std::fill_n(voxels, 125, 100.0f);
    voxels[2 + 5 * (2 + 5 * 2)] = 0.0f;

    GapSurfaceConfig surfaceConfig;
    surfaceConfig.isoMode = GapIsoMode::AbsoluteValue;
    surfaceConfig.absoluteIsoValue = 50.0;
    surfaceConfig.backgroundMean = 0.0f;
    surfaceConfig.materialMean = 100.0f;
    GapVoidParams voidParams;
    voidParams.isFilterEnabled = false;
    voidParams.minVolumeMM3 = 0.0;

    auto overlay = std::make_shared<OverlayStub>();
    auto meshOverlay = std::make_shared<OverlayStub>();
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> sliceTargets;
    sliceTargets.emplace_back(Orientation::Top_down, overlay);
    GapAnalysisService service;
    GapViewRequest viewRequest;
    viewRequest.inputImage = image;
    viewRequest.surface = surfaceConfig;
    viewRequest.voidParams = voidParams;
    viewRequest.meshTargets.push_back(meshOverlay);
    viewRequest.sliceTargets = sliceTargets;
    bool isCompleted = false;
    bool isCompletionOk = false;
    expect(service.StartView(std::move(viewRequest),
        [&](bool isSuccess) {
            isCompleted = true;
            isCompletionOk = isSuccess;
        }), "Gap view should accept mesh and slice targets.");
    expect(service.GetAnalysisState() != GapAnalysisState::Idle,
        "Accepted Gap view should reserve and start its worker before returning.");
    service.OnDisplayTick(nullptr);

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (service.GetAnalysisState() == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(service.GetAnalysisState() == GapAnalysisState::Succeeded,
        "Gap display worker should succeed.");

    service.OnDisplayTick(image);
    auto* firstSliceInput = overlay->GetInput(0);
    auto* firstMeshInput = meshOverlay->GetInput(0);
    expect(overlay->GetAttachCount() == 1
            && meshOverlay->GetAttachCount() == 1
            && overlay->GetOverlay()
            && meshOverlay->GetOverlay()
            && vtkImageData::SafeDownCast(firstSliceInput)
            && vtkPolyData::SafeDownCast(firstMeshInput)
            && isCompleted && isCompletionOk,
        "Terminal tick should attach one mesh and one slice overlay before completing.");
    expect(service.SwitchOverlay()
            && overlay->GetRemoveCount() == 1
            && meshOverlay->GetRemoveCount() == 1,
        "Hide should detach both overlays without discarding the result.");
    expect(service.SwitchOverlay()
            && overlay->GetAttachCount() == 2
            && meshOverlay->GetAttachCount() == 2
            && overlay->GetInput(1) == firstSliceInput
            && meshOverlay->GetInput(1) == firstMeshInput,
        "Show should reuse both stored display artifacts.");

    auto invalidMask = vtkSmartPointer<vtkImageData>::New();
    invalidMask->SetDimensions(4, 5, 5);
    invalidMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    GapViewRequest invalidRequest;
    invalidRequest.inputImage = image;
    invalidRequest.validityMask = invalidMask;
    invalidRequest.surface = surfaceConfig;
    invalidRequest.voidParams = voidParams;
    invalidRequest.sliceTargets = sliceTargets;
    expect(!service.StartView(std::move(invalidRequest)),
        "Gap view should reject a mask with mismatched geometry.");
    expect(service.GetViewOn()
        && overlay->GetAttachCount() == 2
        && overlay->GetRemoveCount() == 1,
        "Rejected Gap view should preserve the prior overlay session.");

    bool isWrongSwitchAccepted = true;
    bool isWrongExitAccepted = true;
    std::thread wrongThread([&]() {
        isWrongSwitchAccepted = service.SwitchOverlay();
        service.OnDisplayTick(image);
        isWrongExitAccepted = service.ExitView();
    });
    wrongThread.join();
    bool isWrongThreadViewOn = false;
    std::thread stateThread([&]() {
        isWrongThreadViewOn = service.GetViewOn();
    });
    stateThread.join();
    expect(!isWrongSwitchAccepted && !isWrongExitAccepted
        && isWrongThreadViewOn && service.GetViewOn() && overlay->GetAttachCount() == 2,
        "Non-owner commands must be rejected while state queries still report the active session.");

    auto firstLabelImage = service.BuildLabelImage();
    expect(firstLabelImage && firstLabelImage->GetScalarPointer(),
        "Gap result should expose one label image copy.");
    if (firstLabelImage && firstLabelImage->GetScalarPointer()) {
        static_cast<int*>(firstLabelImage->GetScalarPointer())[0] = 99;
        auto secondLabelImage = service.BuildLabelImage();
        expect(secondLabelImage
            && secondLabelImage != firstLabelImage
            && secondLabelImage->GetScalarPointer() != firstLabelImage->GetScalarPointer()
            && static_cast<int*>(secondLabelImage->GetScalarPointer())[0] != 99,
            "Mutating one public label image must not pollute later reads.");
    }
    auto firstVoidMesh = service.BuildVoidMesh();
    expect(firstVoidMesh && firstVoidMesh->GetNumberOfPoints() > 0,
        "Gap result should expose one non-empty void mesh copy.");
    if (firstVoidMesh && firstVoidMesh->GetNumberOfPoints() > 0
        && firstVoidMesh->GetPoints()) {
        double firstPoint[3] = {};
        firstVoidMesh->GetPoint(0, firstPoint);
        const double changedX = firstPoint[0] + 123.0;
        firstVoidMesh->GetPoints()->SetPoint(
            0, changedX, firstPoint[1], firstPoint[2]);
        firstVoidMesh->Modified();
        auto secondVoidMesh = service.BuildVoidMesh();
        double secondPoint[3] = {};
        if (secondVoidMesh && secondVoidMesh->GetNumberOfPoints() > 0) {
            secondVoidMesh->GetPoint(0, secondPoint);
        }
        expect(secondVoidMesh
                && secondVoidMesh != firstVoidMesh
                && secondVoidMesh->GetPoints()
                    != firstVoidMesh->GetPoints()
                && secondPoint[0] != changedX,
            "Mutating one public void mesh must not pollute later reads.");
    }
    expect(service.ExitView() && !service.GetViewOn(),
        "Exit should end the display session.");
    expect(overlay->GetRemoveCount() == 2
            && meshOverlay->GetRemoveCount() == 2,
        "Exit should remove every attached Gap overlay.");
    service.OnDisplayTick(nullptr);

    auto sliceOnlyOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService sliceOnlyService;
    GapViewRequest sliceOnlyRequest;
    sliceOnlyRequest.inputImage = image;
    sliceOnlyRequest.surface = surfaceConfig;
    sliceOnlyRequest.voidParams = voidParams;
    sliceOnlyRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        sliceOnlyOverlay);
    expect(StartDisplay(
            sliceOnlyService,
            std::move(sliceOnlyRequest),
            image)
            && sliceOnlyOverlay->GetAttachCount() == 1
            && vtkImageData::SafeDownCast(
                sliceOnlyOverlay->GetInput(0)),
        "Slice-only Gap view should display the shared label result.");
    expect(sliceOnlyService.ExitView(),
        "Slice-only Gap view should exit cleanly.");
    sliceOnlyService.OnDisplayTick(nullptr);

    auto trustedOverlay = std::make_shared<OverlayStub>();
    auto trustedInput = BuildTrustedInput(image);
    auto* trustedScalars = image->GetPointData()->GetScalars();
    const auto trustedVoxelCount = static_cast<std::size_t>(
        image->GetNumberOfPoints());
    const auto* trustedVoxels = static_cast<const float*>(
        image->GetScalarPointer());
    const std::vector<float> trustedValues(
        trustedVoxels,
        trustedVoxels + trustedVoxelCount);
    const int scalarOwners = trustedScalars->GetReferenceCount();
    auto trustedLabelMaps = std::make_shared<LabelMapPortStub>();
    GapAnalysisService trustedService;
    GapViewRequest trustedRequest;
    trustedRequest.trustedInput = trustedInput;
    trustedRequest.labelMaps = trustedLabelMaps;
    trustedRequest.surface = surfaceConfig;
    trustedRequest.voidParams = voidParams;
    trustedRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        trustedOverlay);
    const bool isTrustedDisplayed = StartDisplay(
        trustedService,
        std::move(trustedRequest),
        image);
    const auto publishedLabelMap = trustedLabelMaps->GetPublished();
    expect(isTrustedDisplayed
            && trustedScalars->GetReferenceCount() > scalarOwners,
        "Trusted Gap input should share the Host scalar array.");
    expect(publishedLabelMap
            && publishedLabelMap->descriptor.id
                == "GapAnalysis.labels"
            && publishedLabelMap->descriptor.producerFeatureId
                == "GapAnalysis"
            && publishedLabelMap->descriptor.datasetId
                == "gap-display-dataset"
            && publishedLabelMap->descriptor.sourceVersion == 1
            && publishedLabelMap->descriptor.valueType
                == ImageValueType::Int32
            && trustedLabelMaps->GetCommitAttemptCount() == 1
            && trustedLabelMaps->GetCommitCount() == 1
            && trustedOverlay->GetInput(0)
                == publishedLabelMap->image.GetPointer(),
        "Trusted Gap display should consume the committed Store image.");
    expect(trustedService.ExitView(),
        "Trusted Gap view should exit cleanly.");
    trustedService.OnDisplayTick(nullptr);
    expect(!trustedLabelMaps->GetPublished()
            && trustedLabelMaps->GetRemoveCount() == 1,
        "Trusted Gap exit should retire the published LabelMap.");
    expect(trustedScalars->GetReferenceCount() == scalarOwners,
        "Trusted Gap input should release its scalar owner after exit.");
    const auto* releasedVoxels = static_cast<const float*>(
        image->GetScalarPointer());
    expect(releasedVoxels
            && std::equal(
                trustedValues.begin(),
                trustedValues.end(),
                releasedVoxels),
        "Trusted Gap analysis must not modify the shared Host scalars.");

    GapAnalysisService mixedInputService;
    GapViewRequest mixedInputRequest;
    mixedInputRequest.trustedInput = trustedInput;
    mixedInputRequest.labelMaps = trustedLabelMaps;
    mixedInputRequest.inputImage = image;
    mixedInputRequest.surface = surfaceConfig;
    mixedInputRequest.voidParams = voidParams;
    mixedInputRequest.sliceTargets = sliceTargets;
    expect(!mixedInputService.StartView(
            std::move(mixedInputRequest)),
        "Gap view should reject ambiguous trusted and mutable inputs.");

    auto failingLabelMaps = std::make_shared<LabelMapPortStub>();
    failingLabelMaps->SetFailCommit(true);
    auto failingOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService failingStoreService;
    GapViewRequest failingStoreRequest;
    failingStoreRequest.trustedInput = trustedInput;
    failingStoreRequest.labelMaps = failingLabelMaps;
    failingStoreRequest.surface = surfaceConfig;
    failingStoreRequest.voidParams = voidParams;
    failingStoreRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        failingOverlay);
    bool isFailingStoreCompleted = false;
    bool isFailingStoreSucceeded = true;
    const bool isFailingStoreAccepted = failingStoreService.StartView(
        std::move(failingStoreRequest),
        [&](const bool isSuccess) {
            isFailingStoreCompleted = true;
            isFailingStoreSucceeded = isSuccess;
        });
    const auto failingStoreDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (failingStoreService.GetAnalysisState()
            == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < failingStoreDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    failingStoreService.OnDisplayTick(image);
    expect(isFailingStoreAccepted
            && isFailingStoreCompleted
            && !isFailingStoreSucceeded
            && failingStoreService.GetAnalysisState()
                == GapAnalysisState::Failed
            && !failingStoreService.BuildLabelImage()
            && !failingStoreService.BuildVoidMesh()
            && !failingLabelMaps->GetPublished()
            && failingLabelMaps->GetCommitAttemptCount() == 1
            && failingLabelMaps->GetCommitCount() == 0
            && failingLabelMaps->GetDiscardCount() == 1
            && failingOverlay->GetAttachCount() == 1
            && failingOverlay->GetRemoveCount() == 1
            && !failingOverlay->GetOverlay(),
        "LabelMap commit failure should retire the candidate and fail display.");
    expect(failingStoreService.ExitView(),
        "Failed trusted Gap view should still exit cleanly.");
    failingStoreService.OnDisplayTick(nullptr);

    auto rejectedDisplayMaps = std::make_shared<LabelMapPortStub>();
    auto rejectedDisplayOverlay = std::make_shared<OverlayStub>();
    rejectedDisplayOverlay->SetAttachAllowed(false);
    GapAnalysisService rejectedDisplayService;
    GapViewRequest rejectedDisplayRequest;
    rejectedDisplayRequest.trustedInput = trustedInput;
    rejectedDisplayRequest.labelMaps = rejectedDisplayMaps;
    rejectedDisplayRequest.surface = surfaceConfig;
    rejectedDisplayRequest.voidParams = voidParams;
    rejectedDisplayRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        rejectedDisplayOverlay);
    bool isRejectedDisplayCompleted = false;
    bool isRejectedDisplaySucceeded = true;
    const bool isRejectedDisplayAccepted = rejectedDisplayService.StartView(
        std::move(rejectedDisplayRequest),
        [&](const bool isSuccess) {
            isRejectedDisplayCompleted = true;
            isRejectedDisplaySucceeded = isSuccess;
        });
    const auto rejectedDisplayDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (rejectedDisplayService.GetAnalysisState()
            == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < rejectedDisplayDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    rejectedDisplayService.OnDisplayTick(image);
    expect(isRejectedDisplayAccepted
            && isRejectedDisplayCompleted
            && !isRejectedDisplaySucceeded
            && rejectedDisplayService.GetAnalysisState()
                == GapAnalysisState::Failed
            && !rejectedDisplayService.BuildLabelImage()
            && !rejectedDisplayMaps->GetPublished()
            && rejectedDisplayMaps->GetCommitAttemptCount() == 0
            && rejectedDisplayMaps->GetDiscardCount() == 1
            && rejectedDisplayOverlay->GetAttachCount() == 0,
        "Overlay rejection should discard the staged LabelMap before commit.");
    expect(rejectedDisplayService.ExitView(),
        "Rejected trusted Gap display should still exit cleanly.");
    rejectedDisplayService.OnDisplayTick(nullptr);

    auto throwingOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService throwingOverlayService;
    GapViewRequest throwingOverlayRequest;
    throwingOverlayRequest.inputImage = image;
    throwingOverlayRequest.surface = surfaceConfig;
    throwingOverlayRequest.voidParams = voidParams;
    throwingOverlayRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        throwingOverlay);
    const bool isThrowingOverlayDisplayed = StartDisplay(
        throwingOverlayService,
        std::move(throwingOverlayRequest),
        image);
    const bool isThrowingOverlayHidden =
        throwingOverlayService.SwitchOverlay();
    throwingOverlay->SetAttachThrowing(true);
    const bool isThrowingOverlayRejected =
        throwingOverlayService.SwitchOverlay();
    throwingOverlay->SetAttachThrowing(false);
    const bool isThrowingOverlayRestored =
        throwingOverlayService.SwitchOverlay();
    expect(isThrowingOverlayDisplayed
            && isThrowingOverlayHidden
            && !isThrowingOverlayRejected
            && isThrowingOverlayRestored
            && throwingOverlayService.GetViewOn()
            && throwingOverlay->GetAttachCount() == 2
            && throwingOverlay->GetRemoveCount() == 1
            && throwingOverlay->GetOverlay(),
        "Overlay reattach exception should roll back to a retryable hidden state.");
    expect(throwingOverlayService.ExitView(),
        "Recovered throwing overlay view should exit cleanly.");
    throwingOverlayService.OnDisplayTick(nullptr);

    auto meshOnlyOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService meshOnlyService;
    GapViewRequest meshOnlyRequest;
    meshOnlyRequest.inputImage = image;
    meshOnlyRequest.surface = surfaceConfig;
    meshOnlyRequest.voidParams = voidParams;
    meshOnlyRequest.meshTargets.push_back(meshOnlyOverlay);
    expect(StartDisplay(
            meshOnlyService,
            std::move(meshOnlyRequest),
            image)
            && meshOnlyOverlay->GetAttachCount() == 1
            && vtkPolyData::SafeDownCast(
                meshOnlyOverlay->GetInput(0)),
        "Mesh-only Gap view should display the worker-built mesh result.");
    expect(meshOnlyService.ExitView(),
        "Mesh-only Gap view should exit cleanly.");
    meshOnlyService.OnDisplayTick(nullptr);

    GapAnalysisService ownerService;
    auto ownerImage = vtkSmartPointer<vtkImageData>::New();
    ownerImage->DeepCopy(image);
    vtkWeakPointer<vtkImageData> weakOwner;
    weakOwner = ownerImage.GetPointer();
    GapViewRequest ownerRequest;
    ownerRequest.inputImage = ownerImage;
    ownerRequest.surface = surfaceConfig;
    ownerRequest.voidParams = voidParams;
    ownerRequest.sliceTargets = sliceTargets;
    expect(ownerService.StartView(std::move(ownerRequest)),
        "Owner release view should isolate one controlled input snapshot.");
    ownerImage = nullptr;
    expect(weakOwner == nullptr,
        "Gap view isolation should not retain the caller's mutable image.");
    expect(ownerService.ExitView(), "Display exit should be accepted.");
    ownerService.OnDisplayTick(nullptr);
    expect(weakOwner == nullptr,
        "Exit completion should keep the caller image released.");

    GapAnalysisService maskService;
    auto validityMask = GetMask(image);
    const int oldAttachCount = overlay->GetAttachCount();
    const int oldRemoveCount = overlay->GetRemoveCount();
    bool hasMaskCallback = false;
    GapViewRequest maskRequest;
    maskRequest.inputImage = image;
    maskRequest.validityMask = validityMask;
    maskRequest.surface = surfaceConfig;
    maskRequest.voidParams = voidParams;
    maskRequest.sliceTargets = sliceTargets;
    expect(!maskService.StartView(
            std::move(maskRequest),
            [&](bool) { hasMaskCallback = true; }),
        "Gap view must reject every non-null validity mask.");
    expect(maskService.GetAnalysisState() == GapAnalysisState::Idle
            && !maskService.GetViewOn()
            && maskService.GetVoidRegions().empty()
            && maskService.GetStatistics().voidVoxelCount == 0
            && overlay->GetAttachCount() == oldAttachCount
            && overlay->GetRemoveCount() == oldRemoveCount
            && !hasMaskCallback,
        "Rejected mask input must not start DefX or mutate overlay/result state.");
    auto teardownService = std::make_shared<GapAnalysisService>();
    GapViewRequest teardownRequest;
    teardownRequest.inputImage = image;
    teardownRequest.surface = surfaceConfig;
    teardownRequest.voidParams = voidParams;
    teardownRequest.sliceTargets = sliceTargets;
    expect(teardownService->StartView(std::move(teardownRequest)),
        "Teardown view should bind its owner thread.");
    teardownService->ClearView();
    std::thread releaseThread([serviceOwner = std::move(teardownService)]() mutable {
        serviceOwner.reset();
    });
    releaseThread.join();

    GapAnalysisService callbackService;
    expect(callbackService.SetGapInput(image),
        "Callback cleanup service should accept isolated input.");
    GapSurfaceParams surfaceParams;
    surfaceParams.isoValue = 50.0f;
    surfaceParams.background = 0.0f;
    surfaceParams.material = 100.0f;
    callbackService.SetSurface(surfaceParams);
    callbackService.SetVoid(voidParams);
    bool hasLowCallback = false;
    expect(callbackService.StartAsync(
        [&](bool) { hasLowCallback = true; }),
        "Low-level callback task should be accepted.");
    const auto callbackDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (callbackService.GetAnalysisState()
            == GapAnalysisState::Running
        && std::chrono::steady_clock::now()
            < callbackDeadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }
    GapViewRequest blockedRequest;
    blockedRequest.inputImage = image;
    blockedRequest.surface = surfaceConfig;
    blockedRequest.voidParams = voidParams;
    blockedRequest.sliceTargets = sliceTargets;
    expect(!callbackService.StartView(std::move(blockedRequest)),
        "Pending service callback should reject a new view before state changes.");
    callbackService.ClearView();
    expect(!callbackService.GetDoneEvent(),
        "ClearView should clear the service-level callback doorbell.");
    callbackService.SendCallback();
    expect(!hasLowCallback,
        "ClearView should make the pending service callback unreachable.");

    GapViewRequest retryRequest;
    retryRequest.inputImage = image;
    retryRequest.surface = surfaceConfig;
    retryRequest.voidParams = voidParams;
    retryRequest.sliceTargets = sliceTargets;
    expect(callbackService.StartView(std::move(retryRequest)),
        "ClearView should release worker and callback slots for a new view.");
    callbackService.ClearView();
    return failureCount;
}
