#include "GapDisplayTests.h"

#include "Services/GapAnalysisService.h"
#include "AppInterfaces.h"

#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

static_assert(noexcept(
    std::declval<OverlayService&>().RemoveOverlayStrategy(
        std::declval<std::shared_ptr<AbstractVisualStrategy>>())),
    "Overlay removal must not reopen the StartView exception boundary.");

class OverlayStub final : public OverlayService {
public:
    void AttachOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) override
    {
        m_strategy = std::move(strategy);
        ++m_attachCount;
    }

    void RemoveOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) noexcept override
    {
        if (m_strategy != strategy) {
            return;
        }
        m_strategy.reset();
        ++m_removeCount;
    }

    void ClearOverlayStrategies() override
    {
        m_strategy.reset();
    }

    int GetAttachCount() const { return m_attachCount; }
    int GetRemoveCount() const { return m_removeCount; }

private:
    std::shared_ptr<AbstractVisualStrategy> m_strategy;
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

    GapSurfaceRequest surfaceRequest;
    surfaceRequest.isoMode = GapIsoMode::AbsoluteValue;
    surfaceRequest.absoluteIsoValue = 50.0;
    GapVoidParams voidParams;
    voidParams.grayMax = 10.0f;
    voidParams.minVolumeMM3 = 0.0;
    voidParams.erosionIterations = 0;

    auto overlay = std::make_shared<OverlayStub>();
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> sliceTargets;
    sliceTargets.emplace_back(Orientation::Top_down, overlay);
    GapAnalysisService service;
    GapViewRequest viewRequest;
    viewRequest.inputImage = image;
    viewRequest.surface = surfaceRequest;
    viewRequest.voidParams = voidParams;
    viewRequest.sliceTargets = sliceTargets;
    bool isCompleted = false;
    bool isCompletionOk = false;
    expect(service.StartView(std::move(viewRequest),
        [&](bool isSuccess) {
            isCompleted = true;
            isCompletionOk = isSuccess;
        }), "Gap view should accept one slice target.");
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
    expect(overlay->GetAttachCount() == 1 && isCompleted && isCompletionOk,
        "Terminal tick should attach one overlay before completing.");
    expect(service.SwitchOverlay() && overlay->GetRemoveCount() == 1,
        "Hide should detach without discarding the result.");
    expect(service.SwitchOverlay() && overlay->GetAttachCount() == 2,
        "Show should reuse the stored result.");

    auto invalidMask = vtkSmartPointer<vtkImageData>::New();
    invalidMask->SetDimensions(4, 5, 5);
    invalidMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    GapViewRequest invalidRequest;
    invalidRequest.inputImage = image;
    invalidRequest.validityMask = invalidMask;
    invalidRequest.surface = surfaceRequest;
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
    expect(service.ExitView() && !service.GetViewOn(),
        "Exit should end the display session.");
    service.OnDisplayTick(nullptr);

    GapAnalysisService ownerService;
    auto ownerImage = vtkSmartPointer<vtkImageData>::New();
    ownerImage->DeepCopy(image);
    vtkWeakPointer<vtkImageData> weakOwner;
    weakOwner = ownerImage.GetPointer();
    GapViewRequest ownerRequest;
    ownerRequest.inputImage = ownerImage;
    ownerRequest.surface = surfaceRequest;
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
    auto* maskValues = validityMask
        ? static_cast<unsigned char*>(
            validityMask->GetScalarPointer())
        : nullptr;
    expect(maskValues != nullptr,
        "Valid Gap mask should expose unsigned-char scalars.");
    if (maskValues) {
        maskValues[2 + 5 * (2 + 5 * 2)] = 0;
        validityMask->Modified();
    }
    GapViewRequest maskRequest;
    maskRequest.inputImage = image;
    maskRequest.validityMask = validityMask;
    maskRequest.surface = surfaceRequest;
    maskRequest.voidParams = voidParams;
    maskRequest.sliceTargets = sliceTargets;
    expect(maskService.StartView(std::move(maskRequest)),
        "Gap view should accept an image and matching validity mask.");
    if (maskValues) {
        maskValues[2 + 5 * (2 + 5 * 2)] = 255;
        validityMask->Modified();
    }
    const auto maskDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (maskService.GetAnalysisState()
            == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < maskDeadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }
    expect(maskService.GetAnalysisState()
            == GapAnalysisState::Succeeded
        && maskService.GetVoidRegions().empty(),
        "Frozen mask=0 voxels should not enter Gap statistics.");
    const auto maskedLabel = maskService.BuildLabelImage();
    const auto* maskedLabels = maskedLabel
        ? static_cast<const int*>(
            maskedLabel->GetScalarPointer())
        : nullptr;
    expect(maskedLabels
        && maskedLabels[2 + 5 * (2 + 5 * 2)] == 0,
        "Frozen mask=0 voxels should remain zero in the Gap label image.");
    const auto maskedMesh = maskService.BuildVoidMesh();
    expect(maskedMesh
        && maskedMesh->GetNumberOfPoints() == 0
        && maskedMesh->GetNumberOfCells() == 0,
        "Frozen mask=0 voxels should not enter the Gap void mesh.");
    maskService.ClearView();

    auto teardownService = std::make_shared<GapAnalysisService>();
    GapViewRequest teardownRequest;
    teardownRequest.inputImage = image;
    teardownRequest.surface = surfaceRequest;
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
    blockedRequest.surface = surfaceRequest;
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
    retryRequest.surface = surfaceRequest;
    retryRequest.voidParams = voidParams;
    retryRequest.sliceTargets = sliceTargets;
    expect(callbackService.StartView(std::move(retryRequest)),
        "ClearView should release worker and callback slots for a new view.");
    callbackService.ClearView();
    return failureCount;
}
