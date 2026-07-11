#include "GapDisplayTests.h"

#include "Services/GapAnalysisService.h"
#include "AppInterfaces.h"

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

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
    GapAnalysisService service;
    expect(service.StartView(
        surfaceRequest,
        voidParams,
        {},
        sliceTargets,
        nullptr), "Gap view should accept one slice target.");
    service.OnDisplayTick(image);

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
    expect(service.ExitView() && !service.GetViewOn(),
        "Exit should end the display session.");
    return failureCount;
}
