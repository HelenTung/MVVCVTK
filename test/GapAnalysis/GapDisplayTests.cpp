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

class GapDisplayTestService final : public GapAnalysisService {
public:
    using GapAnalysisService::SetInputSnapshot;
};

namespace {

class OverlayStub final : public OverlayService {
public:
    void AttachOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) override
    {
        m_strategy = std::move(strategy);
        ++m_attachCount;
    }

    void RemoveOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) override
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

    GapAnalysisSurfaceRequest surfaceRequest;
    surfaceRequest.isoMode = GapAnalysisIsoValueMode::AbsoluteValue;
    surfaceRequest.absoluteIsoValue = 50.0;
    VoidDetectionParams voidParams;
    voidParams.grayMax = 10.0f;
    voidParams.minVolumeMM3 = 0.0;
    voidParams.erosionIterations = 0;

    auto overlay = std::make_shared<OverlayStub>();
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> sliceTargets;
    sliceTargets.emplace_back(Orientation::Top_down, overlay);
    GapDisplayTestService service;
    expect(service.StartView(
        surfaceRequest,
        voidParams,
        {},
        sliceTargets,
        nullptr), "Gap view should accept one slice target.");
    expect(service.SetInputSnapshot(image),
        "Gap view should accept one controlled read-only input snapshot.");
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
    expect(overlay->GetAttachCount() == 1,
        "Terminal tick should attach one overlay.");
    expect(service.SwitchOverlay() && overlay->GetRemoveCount() == 1,
        "Hide should detach without discarding the result.");
    expect(service.SwitchOverlay() && overlay->GetAttachCount() == 2,
        "Show should reuse the stored result.");

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

    GapDisplayTestService ownerService;
    expect(ownerService.StartView(
        surfaceRequest,
        voidParams,
        {},
        sliceTargets,
        nullptr), "Owner release view should accept one slice target.");
    auto ownerImage = vtkSmartPointer<vtkImageData>::New();
    ownerImage->DeepCopy(image);
    vtkWeakPointer<vtkImageData> weakOwner;
    weakOwner = ownerImage.GetPointer();
    expect(ownerService.SetInputSnapshot(ownerImage),
        "Owner release view should retain one controlled input snapshot.");
    ownerImage = nullptr;
    expect(weakOwner != nullptr,
        "Controlled input snapshot should retain the image during the display session.");
    expect(ownerService.ExitView() && weakOwner == nullptr,
        "Display exit should release its retained input image owner.");

    auto teardownService = std::make_shared<GapAnalysisService>();
    expect(teardownService->StartView(
        surfaceRequest,
        voidParams,
        {},
        sliceTargets,
        nullptr), "Teardown view should bind its owner thread.");
    expect(teardownService->ExitView(),
        "Owner thread must detach the teardown view before releasing it.");
    std::thread releaseThread([serviceOwner = std::move(teardownService)]() mutable {
        serviceOwner.reset();
    });
    releaseThread.join();
    return failureCount;
}
