#include "GapDisplayTests.h"

#include "Services/GapAnalysisService.h"
#include "Render/Contracts/FeatureOverlay.h"
#include "Render/Contracts/OverlayService.h"

#include <vtkActor.h>
#include <vtkImageData.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
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
#include <iostream>
#include <memory>
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
        if (!overlay) {
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
