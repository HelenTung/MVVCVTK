#include "CropBridgeTests.h"

#include "Algorithms/CropAlgorithm.h"
#include "AppInterfaces.h"
#include "Interaction/CropBridge.h"
#include "Render/CropShaderController.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"

#include <vtkCubeSource.h>
#include <vtkImageData.h>
#include <vtkCommand.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
class CropServiceStub final : public InteractiveService {
public:
    CropServiceStub(
        const RenderInputStamp inputStamp,
        vtkSmartPointer<vtkRenderer> renderer)
        : m_inputStamp(inputStamp)
        , m_renderer(std::move(renderer))
        , m_strategy(std::make_shared<IsoSurfaceStrategy>())
    {
        auto cube = vtkSmartPointer<vtkCubeSource>::New();
        cube->Update();
        m_strategy->SetInputData(cube->GetOutput());
        (void)m_strategy->SetRenderInputStamp(m_inputStamp);
        m_strategy->AttachRenderer(m_renderer);
    }

    ~CropServiceStub() override
    {
        if (m_strategy && m_renderer) {
            m_strategy->DetachRenderer(m_renderer);
        }
    }

    void SetSliceScroll(int) override {}
    int GetPlaneAxis(vtkActor*) override { return -1; }
    void SetCursorWorldPosition(double[3], int) override {}
    std::array<double, 3> GetCursorWorld() override { return {}; }
    void SetInteracting(bool) override {}
    vtkProp3D* GetMainProp() override { return nullptr; }
    void SetModelMatrix(vtkMatrix4x4*) override {}
    std::array<double, 16> GetModelMatrix() override
    {
        return { 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0 };
    }
    int GetNavigationAxis() const override { return -1; }
    WindowLevelParams GetWindowLevel() const override { return {}; }
    void SetElementVisible(uint32_t, bool) override {}
    void SetWindowLevelDrag(int, int, int, int, double, double) override {}
    void GetModelPositionFromWorld(const double source[3], double target[3]) const override
    {
        std::copy_n(source, 3, target);
    }
    void GetWorldPositionFromModel(const double source[3], double target[3]) const override
    {
        std::copy_n(source, 3, target);
    }
    RenderInputStamp GetRenderInputStamp() const override
    {
        return m_inputStamp;
    }

    bool AttachRenderEffect(std::shared_ptr<RenderEffect> effect) override
    {
        if (!effect || !m_effect.expired() || !m_strategy) {
            return false;
        }
        if (!m_strategy->SetRenderInputStamp(m_inputStamp)
            || !m_strategy->AttachRenderEffect(
                effect, RenderBindingUse::Current)) {
            return false;
        }
        m_effect = std::dynamic_pointer_cast<CropShaderEffect>(effect);
        ++attachCount;
        return !m_effect.expired();
    }

    bool DetachRenderEffect(const RenderEffect* effect) override
    {
        auto current = m_effect.lock();
        if (!current || current.get() != effect || !m_strategy
            || !m_strategy->DetachRenderEffect(effect)) {
            return false;
        }
        m_effect.reset();
        ++detachCount;
        return true;
    }

    bool SetRenderInputStamp(const RenderInputStamp inputStamp)
    {
        m_inputStamp = inputStamp;
        return m_strategy
            && m_strategy->SetRenderInputStamp(inputStamp);
    }

    RenderEffectState GetEffectState() const
    {
        const auto effect = m_effect.lock();
        return effect ? effect->GetState() : RenderEffectState{};
    }
    void AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy>) override {}
    void RemoveOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy>) noexcept override {}
    void ClearOverlayStrategies() override {}
    void SetRenderContext(vtkSmartPointer<vtkRenderWindow>, vtkSmartPointer<vtkRenderer>) override {}
    void SendUpdates() override {}
    bool GetDirty() const override { return false; }
    void SetDirty() override { ++dirtyCount; }
    bool ResetDirty() override { return false; }
    void SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy>) override {}

    int attachCount = 0;
    int detachCount = 0;
    int dirtyCount = 0;

private:
    RenderInputStamp m_inputStamp;
    vtkSmartPointer<vtkRenderer> m_renderer;
    std::shared_ptr<IsoSurfaceStrategy> m_strategy;
    std::weak_ptr<CropShaderEffect> m_effect;
};

bool SendWidgetInput(
    vtkRenderer* renderer,
    vtkRenderWindowInteractor* interactor,
    const int moveOffset = 8,
    const bool isReleased = true,
    const bool isMoveSent = true)
{
    if (!renderer || !interactor) {
        return false;
    }
    renderer->SetWorldPoint(3.0, 1.5, 1.5, 1.0);
    renderer->WorldToDisplay();
    const auto* displayPoint = renderer->GetDisplayPoint();
    const int x = static_cast<int>(displayPoint[0]);
    const int y = static_cast<int>(displayPoint[1]);
    interactor->SetEventPosition(x, y);
    interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
    if (isMoveSent) {
        interactor->SetEventPosition(x + moveOffset, y);
        interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    }
    if (isReleased) {
        interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    }
    return true;
}

bool SendPlaneInput(
    vtkRenderer* renderer,
    vtkRenderWindowInteractor* interactor,
    const int moveOffset)
{
    if (!renderer || !interactor) {
        return false;
    }
    renderer->SetWorldPoint(1.5, 1.5, 1.5, 1.0);
    renderer->WorldToDisplay();
    const auto* displayPoint = renderer->GetDisplayPoint();
    const int x = static_cast<int>(displayPoint[0]);
    const int y = static_cast<int>(displayPoint[1]);
    interactor->SetEventPosition(x, y);
    interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
    interactor->SetEventPosition(x + moveOffset, y);
    interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    return true;
}

bool SendShaderCommit(
    CropBridge& bridge,
    vtkRenderWindow* renderWindow)
{
    if (!renderWindow) {
        return false;
    }
    renderWindow->Render();
    return bridge.SendShaderCommit();
}
}

int CropBridgeSuite::GetFailCount() const
{
    int failureCount = 0;
    const auto expect = [&failureCount](const bool isExpected, const char* message) {
        if (!isExpected) {
            std::cerr << message << '\n';
            ++failureCount;
        }
    };

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    auto interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(200, 200);
    renderWindow->AddRenderer(renderer);
    interactor->SetRenderWindow(renderWindow);

    const RenderInputStamp inputStamp = { image.GetPointer(), 1 };
    auto service = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    CropViewRequest view;
    view.renderer = renderer;
    view.interactor = interactor;
    view.referenceService = service;
    view.targetServices = { service };

    CropBridge bridge;
    expect(bridge.StartView(view), "Bridge should accept a complete view request.");

    CropInputSnapshot input;
    input.dataSource = OrthogonalCropDataSource::ImageData;
    input.inputVersion = 1;
    input.inputModelBounds = { 0.0, 3.0, 0.0, 3.0, 0.0, 3.0 };
    input.imageData = image;
    expect(bridge.SetCropInput(input), "Bridge should accept an immutable input snapshot.");
    expect(bridge.SwitchCropBox(), "Bridge should enter box editing.");
    const int switchDirtyCount = service->dirtyCount;
    expect(bridge.SwitchCropPlane()
            && service->dirtyCount
                == switchDirtyCount + 1,
        "Switching from box to plane should request a reference-view frame.");
    expect(bridge.SwitchCropBox()
            && service->dirtyCount
                == switchDirtyCount + 2,
        "Switching from plane to box should request a reference-view frame.");
    renderer->ResetCamera(input.inputModelBounds.data());
    renderWindow->Render();
    expect(bridge.GetCropHistory().editMode == CropRemovalMode::None,
        "A new crop bridge should start in the non-cropping edit mode.");
    expect(SendWidgetInput(renderer, interactor), "Bridge test should send one box release interaction.");
    expect(!bridge.GetShaderTickNeeded()
            && bridge.GetCropHistory().operationCount == 0,
        "Releasing a widget in the non-cropping mode must not create history.");
    expect(bridge.SetCropMode(CropRemovalMode::KeepInside),
        "KeepInside should arm the current widget for cropping.");
    expect(SendWidgetInput(renderer, interactor, 0),
        "Bridge test should send a zero-distance box interaction.");
    expect(!bridge.GetShaderTickNeeded()
            && bridge.GetCropHistory().nodeCount == 0
            && bridge.GetCropHistory().operationCount == 0
            && service->GetEffectState().status
                == RenderEffectStatus::Idle
            && service->GetEffectState().stagedRevision == 0
            && service->GetEffectState().activeRevision == 0,
        "A zero-distance interaction must not create a crop history operation.");
    expect(SendWidgetInput(renderer, interactor),
        "Bridge test should release the armed box once.");
    expect(bridge.GetShaderTickNeeded(), "Released widget geometry should stage one shader revision.");
    const auto firstStage = service->GetEffectState();
    expect((firstStage.status == RenderEffectStatus::Staged
                || firstStage.status == RenderEffectStatus::Ready)
            && firstStage.stagedRevision != 0,
        "Released geometry should stage one revision on the target.");
    expect(SendShaderCommit(bridge, renderWindow),
        "A ready single-target revision should commit.");
    const auto firstCommit = service->GetEffectState();
    expect(firstCommit.status == RenderEffectStatus::Committed
            && firstCommit.activeRevision == firstStage.stagedRevision,
        "The original target should expose the committed revision.");
    const auto firstHistory = bridge.GetCropHistory();
    expect(firstHistory.nodeCount == 1
            && firstHistory.operationCount == 1
            && firstHistory.hasEditableOp,
        "The first armed release should expose one current and one total operation.");
    auto nextImage = vtkSmartPointer<vtkImageData>::New();
    nextImage->ShallowCopy(image);
    auto nextInput = input;
    nextInput.inputVersion = 2;
    nextInput.imageData = nextImage;
    auto blockedService = std::make_shared<CropServiceStub>(
        RenderInputStamp{ nextImage.GetPointer(), 2 },
        renderer);
    auto blockedView = view;
    blockedView.referenceService = blockedService;
    blockedView.targetServices = { blockedService };
    CropBridge blockerBridge;
    expect(blockerBridge.StartView(blockedView),
        "A blocker bridge should occupy the replacement target.");
    expect(!bridge.StartView(blockedView, nextInput)
            && bridge.GetCropActive()
            && bridge.GetCropHistory().nodeCount
                == firstHistory.nodeCount
            && bridge.GetCropHistory().operationCount
                == firstHistory.operationCount
            && service->GetEffectState().status
                == RenderEffectStatus::Committed,
        "Rejected input and target replacement should preserve the current binding and history.");
    expect(blockerBridge.ClearBindings(),
        "The blocker bridge should release the replacement target.");
    expect(bridge.SetCropMode(CropRemovalMode::None)
            && !bridge.GetShaderTickNeeded()
            && bridge.GetCropHistory().editMode == CropRemovalMode::None,
        "The non-cropping mode should pause editing without changing the committed prefix.");
    expect(bridge.SetCropMode(CropRemovalMode::RemoveInside),
        "Changing mode should stage an updated current draft immediately.");
    const auto modeStage = service->GetEffectState();
    expect(modeStage.status == RenderEffectStatus::Staged
            && modeStage.stagedRevision > firstCommit.activeRevision,
        "Mode change should stage a newer revision for the current history node.");
    expect(SendShaderCommit(bridge, renderWindow),
        "The mode-only draft revision should commit without another widget release.");

    expect(bridge.SetCropNode(0)
            && bridge.GetShaderTickNeeded(),
        "An explicit history node should stage the requested zero-length prefix.");
    const int zeroNodeDirtyCount = service->dirtyCount;
    expect(SendShaderCommit(bridge, renderWindow),
        "Explicit history node zero should commit.");
    expect(service->dirtyCount == zeroNodeDirtyCount + 1,
        "Committing history node zero should request a frame that publishes the baseline.");
    expect(bridge.SetCropNode(1)
            && bridge.GetShaderTickNeeded(),
        "An explicit history node should restore the requested committed prefix.");
    expect(SendShaderCommit(bridge, renderWindow),
        "Explicit history node one should commit.");
    const auto nodeRevision = service->GetEffectState().activeRevision;
    expect(bridge.SetCropNode(1)
            && !bridge.GetShaderTickNeeded()
            && service->GetEffectState().activeRevision == nodeRevision,
        "Selecting the current history node should be a successful no-op.");
    expect(!bridge.SetCropNode(2),
        "Selecting a history node beyond the available operation count should fail.");

    expect(bridge.PreviousCrop() && bridge.GetShaderTickNeeded(),
        "Previous should reuse the committed immutable table with a shorter prefix.");
    expect(SendShaderCommit(bridge, renderWindow),
        "Previous prefix should commit.");
    expect(bridge.NextCrop() && bridge.GetShaderTickNeeded(),
        "Next should reuse the same immutable table with the restored prefix.");
    expect(SendShaderCommit(bridge, renderWindow),
        "Next prefix should commit.");

    auto reboundService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    const int retiredDetachCount = service->detachCount;
    auto reboundView = view;
    reboundView.referenceService = reboundService;
    reboundView.targetServices = { reboundService };
    expect(bridge.StartView(reboundView),
        "An active bridge should stage the committed prefix on a replacement target.");
    expect(bridge.GetShaderTickNeeded()
            && reboundService->GetEffectState().status
                == RenderEffectStatus::Staged
            && service->detachCount == retiredDetachCount,
        "Target rebind should keep the old target active until the replacement is ready.");
    expect(SendShaderCommit(bridge, renderWindow),
        "A ready replacement target should commit its replayed prefix.");
    expect(reboundService->GetEffectState().status
                == RenderEffectStatus::Committed
            && service->detachCount == retiredDetachCount + 1,
        "Successful target rebind should clear the retired target only after commit.");

    const auto reboundRevision =
        reboundService->GetEffectState().activeRevision;
    const int reboundDetachCount = reboundService->detachCount;
    expect(bridge.SwitchCropBox()
            && SendWidgetInput(renderer, interactor)
            && bridge.GetShaderTickNeeded(),
        "A geometry update should leave one staged revision before editing exits.");
    const auto exitHistory = bridge.GetCropHistory();
    const auto exitEffect = reboundService->GetEffectState();
    const int exitDirtyCount = reboundService->dirtyCount;
    expect(bridge.ExitCrop(), "Bridge should stop crop editing.");
    expect(!bridge.GetCropActive()
            && reboundService->detachCount == reboundDetachCount
            && reboundService->dirtyCount == exitDirtyCount + 1
            && bridge.GetCropHistory().nodeCount == exitHistory.nodeCount
            && bridge.GetCropHistory().operationCount
                == exitHistory.operationCount
            && reboundService->GetEffectState().status
                == RenderEffectStatus::Committed
            && reboundService->GetEffectState().activeRevision
                == exitEffect.activeRevision
            && exitEffect.activeRevision == reboundRevision,
        "Exit should hide crop widgets while preserving the current committed history node.");
    expect(!bridge.ExitCrop()
            && reboundService->dirtyCount == exitDirtyCount + 1,
        "Repeated exit should not publish another reference frame.");
    expect(bridge.GetCropBound(),
        "Exit should preserve the committed target binding.");
    expect(bridge.PreviousCrop() && bridge.GetShaderTickNeeded(),
        "Previous should remain available after editing exits.");
    expect(SendShaderCommit(bridge, renderWindow),
        "Post-exit Previous should commit the shorter prefix.");
    expect(bridge.NextCrop() && bridge.GetShaderTickNeeded(),
        "Next should remain available after editing exits.");
    expect(SendShaderCommit(bridge, renderWindow),
        "Post-exit Next should restore the committed prefix.");
    const auto postExitRevision =
        reboundService->GetEffectState().activeRevision;

    bool hasAsyncResult = false;
    expect(bridge.ExportCrop(input, [&hasAsyncResult](CropExportResult result) {
        hasAsyncResult = result.inputVersion == 1
            && result.nodeCount == 1
            && result.operations.size() == 1
            && result.operations[0].removalMode == CropRemovalMode::RemoveInside;
    }), "A committed prefix should start one asynchronous export.");
    for (int pollCount = 0; pollCount < 200 && !bridge.GetExportTickNeeded(); ++pollCount) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(bridge.SendExportResult(), "A ready export future should be delivered on the owner thread.");
    expect(hasAsyncResult,
        "Async export should preserve the captured mode, version, and committed prefix.");

    expect(bridge.StartView(reboundView)
            && bridge.GetCropActive()
            && !bridge.GetShaderTickNeeded()
            && reboundService->detachCount == reboundDetachCount
            && reboundService->GetEffectState().activeRevision
                == postExitRevision,
        "Restarting the same view should resume editing without clearing or replaying its committed result.");
    expect(bridge.ExitCrop(), "Bridge should stop resumed editing without clearing its result.");

    auto resumedService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    auto resumedView = view;
    resumedView.referenceService = resumedService;
    resumedView.targetServices = { resumedService };
    expect(bridge.StartView(resumedView)
            && bridge.GetCropActive()
            && bridge.GetShaderTickNeeded()
            && resumedService->GetEffectState().status
                == RenderEffectStatus::Staged
            && reboundService->detachCount == reboundDetachCount,
        "Restarting with a replacement target should stage the preserved committed prefix atomically.");
    const int resumedDetachCount = resumedService->detachCount;
    expect(bridge.ExitCrop()
            && resumedService->detachCount
                == resumedDetachCount + 1
            && reboundService->detachCount == reboundDetachCount
            && reboundService->GetEffectState().activeRevision
                == postExitRevision,
        "Canceling a pending target rebind should detach its temporary effect and preserve the old target.");
    expect(bridge.StartView(resumedView)
            && bridge.GetCropActive()
            && bridge.GetShaderTickNeeded(),
        "A canceled replacement target should be attachable again for a fresh replay transaction.");
    expect(SendShaderCommit(bridge, renderWindow),
        "A ready replacement target should commit the preserved prefix after restart.");
    expect(resumedService->GetEffectState().status
                == RenderEffectStatus::Committed
            && reboundService->detachCount == reboundDetachCount + 1,
        "Restart target commit should retire the previous target only after the replacement is committed.");
    expect(bridge.ExitCrop(), "Bridge should stop replacement-target editing while preserving its result.");

    auto changedInput = input;
    changedInput.inputVersion = 2;
    expect(resumedService->SetRenderInputStamp(
        { image.GetPointer(), changedInput.inputVersion }),
        "Target service should publish the replacement input stamp.");
    expect(bridge.SetCropInput(changedInput), "Input version change should publish a new snapshot.");
    expect(resumedService->GetEffectState().status
            == RenderEffectStatus::Idle,
        "Input invalidation should clear the active shader target.");
    expect(!bridge.PreviousCrop(), "An empty history should not move backward.");
    expect(!bridge.NextCrop(), "An empty history should not move forward.");

    bool hasExportResult = false;
    expect(!bridge.ExportCrop(changedInput, [&hasExportResult](CropExportResult result) {
        hasExportResult = result.failureReason == CropFailure::BadInput;
    }), "Export should reject an empty committed prefix.");
    expect(hasExportResult, "Rejected export should synchronously report its failure.");
    expect(!bridge.GetCropActive(), "Input invalidation should not reactivate crop editing.");
    expect(bridge.ClearBindings(), "Bridge should clear all view bindings.");

    auto repeatService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    auto repeatView = view;
    repeatView.referenceService = repeatService;
    repeatView.targetServices = { repeatService };
    CropBridge repeatBridge;
    expect(repeatBridge.StartView(repeatView)
            && repeatBridge.SetCropInput(input)
            && repeatBridge.SwitchCropBox()
            && repeatBridge.SetCropMode(CropRemovalMode::KeepInside),
        "A repeated-release bridge should enter armed box editing.");
    renderer->ResetCamera(input.inputModelBounds.data());
    renderWindow->Render();
    expect(SendWidgetInput(renderer, interactor)
            && repeatBridge.GetShaderTickNeeded()
            && repeatBridge.SetCropMode(
                CropRemovalMode::RemoveInside)
            && repeatBridge.GetCropHistory().editMode
                == CropRemovalMode::RemoveInside
            && SendShaderCommit(
                repeatBridge, renderWindow)
            && repeatBridge.GetShaderTickNeeded(),
        "A mode switch before the current revision commits should be retained.");
    expect(SendShaderCommit(repeatBridge, renderWindow),
        "The retained RemoveInside mode should update the current draft.");
    expect(SendWidgetInput(renderer, interactor)
            && SendShaderCommit(repeatBridge, renderWindow),
        "The second release of the same box widget should commit another operation.");
    const auto repeatHistory = repeatBridge.GetCropHistory();
    expect(repeatHistory.nodeCount == 2
            && repeatHistory.operationCount == 2
            && repeatHistory.hasEditableOp,
        "Two releases of one widget should expose two independent crop operations.");
    expect(repeatBridge.SetCropMode(CropRemovalMode::KeepInside)
            && SendShaderCommit(repeatBridge, renderWindow)
            && repeatBridge.GetCropHistory().operationCount == 2,
        "Changing the latest operation mode should not increase the release count.");

    for (std::size_t operationCount = 3;
        operationCount <= 5;
        ++operationCount) {
        expect(SendWidgetInput(renderer, interactor)
                && SendShaderCommit(repeatBridge, renderWindow)
                && repeatBridge.GetCropHistory().nodeCount
                    == operationCount
                && repeatBridge.GetCropHistory().operationCount
                    == operationCount,
            "The branch test should build five committed operations.");
    }
    expect(repeatBridge.SetCropNode(2)
            && repeatBridge.GetShaderTickNeeded(),
        "Returning from A-B-C-D-E to B should stage the B prefix.");
    const auto prefixRevision =
        repeatService->GetEffectState().stagedRevision;
    expect(repeatBridge.StartView(repeatView)
            && repeatBridge.SwitchCropBox()
            && repeatBridge.SetCropMode(
                CropRemovalMode::RemoveInside),
        "Reopening the same crop editor should remain accepted while the B prefix is pending.");
    expect(SendWidgetInput(renderer, interactor, 12)
            && repeatBridge.GetShaderTickNeeded()
            && repeatBridge.GetCropHistory().nodeCount == 5
            && repeatBridge.GetCropHistory().operationCount == 5,
        "A valid F interaction should be retained until the B prefix commits.");
    expect(SendShaderCommit(repeatBridge, renderWindow),
        "The B prefix should commit before its retained F operation.");
    const auto branchStage = repeatService->GetEffectState();
    expect(repeatBridge.GetShaderTickNeeded()
            && repeatBridge.GetCropHistory().nodeCount == 2
            && repeatBridge.GetCropHistory().operationCount == 5
            && (branchStage.status
                    == RenderEffectStatus::Staged
                || branchStage.status
                    == RenderEffectStatus::Ready)
            && branchStage.activeRevision == prefixRevision
            && branchStage.stagedRevision > prefixRevision,
        "Committing B should immediately stage retained F without exposing the old redo branch.");
    expect(SendShaderCommit(repeatBridge, renderWindow),
        "The retained F revision should commit.");
    const auto branchHistory = repeatBridge.GetCropHistory();
    const auto branchCommit = repeatService->GetEffectState();
    expect(branchHistory.nodeCount == 3
            && branchHistory.operationCount == 3
            && branchHistory.hasEditableOp
            && branchCommit.status
                == RenderEffectStatus::Committed
            && branchCommit.activeRevision
                == branchStage.stagedRevision
            && !repeatBridge.NextCrop(),
        "Branch commit should replace A-B-C-D-E with active A-B-F and remove redo.");

    bool hasBranchExport = false;
    expect(repeatBridge.ExportCrop(
        input,
        [&hasBranchExport](CropExportResult result) {
            hasBranchExport = result.nodeCount == 3
                && result.operations.size() == 3
                && result.operations[0].operationIndex == 1
                && result.operations[1].operationIndex == 2
                && result.operations[2].operationIndex == 6
                && result.operations[2].removalMode
                    == CropRemovalMode::RemoveInside;
        }), "A-B-F should expose a three-operation export snapshot.");
    expect(SendWidgetInput(renderer, interactor, 16)
            && !repeatBridge.GetShaderTickNeeded()
            && repeatBridge.GetCropHistory().nodeCount == 3
            && repeatBridge.GetCropHistory().operationCount == 3,
        "Materialization should freeze the active Box or Plane widget without creating another operation.");
    expect(!repeatBridge.PreviousCrop()
            && !repeatBridge.SetCropNode(2)
            && !repeatBridge.SetCropMode(
                CropRemovalMode::KeepInside),
        "CPU materialization should freeze the captured active prefix until its result is consumed.");
    for (int pollCount = 0;
        pollCount < 200
            && !repeatBridge.GetExportTickNeeded();
        ++pollCount) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    expect(repeatBridge.SendExportResult()
            && hasBranchExport,
        "A-B-F export should use the rebuilt three-operation predicate history.");
    expect(repeatBridge.SetCropNode(2)
            && SendShaderCommit(repeatBridge, renderWindow),
        "Returning from A-B-F to B should commit the current history cursor.");
    const auto middleHistory = repeatBridge.GetCropHistory();
    const auto middleEffect = repeatService->GetEffectState();
    const int middleDirty = repeatService->dirtyCount;
    expect(middleHistory.nodeCount == 2
            && middleHistory.operationCount == 3
            && repeatBridge.ExitCrop()
            && repeatService->dirtyCount == middleDirty + 1
            && repeatBridge.GetCropHistory().nodeCount == 2
            && repeatBridge.GetCropHistory().operationCount == 3
            && repeatService->GetEffectState().status
                == RenderEffectStatus::Committed
            && repeatService->GetEffectState().activeRevision
                == middleEffect.activeRevision,
        "Exit should preserve the committed effect at the current history cursor.");
    bool hasMiddleExport = false;
    expect(repeatBridge.ExportCrop(
        input,
        [&hasMiddleExport](CropExportResult result) {
            hasMiddleExport = result.nodeCount == 2
                && result.operations.size() == 2
                && result.operations[0].operationIndex == 1
                && result.operations[1].operationIndex == 2;
        }), "Exit at B should export the current A-B committed prefix.");
    for (int pollCount = 0;
        pollCount < 200
            && !repeatBridge.GetExportTickNeeded();
        ++pollCount) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    expect(repeatBridge.SendExportResult()
            && hasMiddleExport,
        "Exit should keep the current A-B prefix instead of restoring A-B-F.");

    auto materializedImage =
        vtkSmartPointer<vtkImageData>::New();
    materializedImage->ShallowCopy(image);
    auto materializedInput = input;
    materializedInput.inputVersion = 2;
    materializedInput.imageData = materializedImage;
    const auto visibleRevision =
        repeatService->GetEffectState().activeRevision;
    expect(repeatBridge.StartCropBaseline(
                materializedInput, 2)
            && repeatBridge.GetCropHistory().nodeCount == 2
            && repeatBridge.GetCropHistory().operationCount == 3
            && repeatBridge.SetCropBaselineComplete(),
        "CPU materialization should publish a new active baseline.");
    const auto materializedHistory =
        repeatBridge.GetCropHistory();
    expect(materializedHistory.nodeCount == 0
            && materializedHistory.operationCount == 1
            && materializedHistory.baseNodeCount == 2
            && materializedHistory.allOperationCount == 3,
        "Materialization should keep A-B-F in allHistory while exposing only F as active redo.");
    const auto visibleEffect =
        repeatService->GetEffectState();
    expect(repeatBridge.GetShaderTickNeeded()
            && visibleEffect.status
                == RenderEffectStatus::Committed
            && visibleEffect.activeRevision
                == visibleRevision,
        "The old Strategy should keep the current committed node visible until the new render input converges.");
    expect(repeatService->SetRenderInputStamp(
                { materializedImage.GetPointer(), 2 })
            && repeatBridge.SendShaderCommit()
            && !repeatBridge.GetShaderTickNeeded()
            && repeatService->GetEffectState().status
                == RenderEffectStatus::Idle,
        "The retired shader should clear only after the materialized render input converges.");
    const int materializedDirty =
        repeatService->dirtyCount;
    const auto materializedEffect =
        repeatService->GetEffectState();
    expect(!repeatBridge.PreviousCrop(),
        "Previous at activeHistory node zero must not enter allHistory.");
    const auto previousHistory =
        repeatBridge.GetCropHistory();
    const auto previousEffect =
        repeatService->GetEffectState();
    expect(previousHistory.nodeCount
                == materializedHistory.nodeCount
            && previousHistory.operationCount
                == materializedHistory.operationCount
            && previousHistory.baseNodeCount
                == materializedHistory.baseNodeCount
            && previousHistory.allOperationCount
                == materializedHistory.allOperationCount
            && repeatService->dirtyCount
                == materializedDirty
            && previousEffect.status
                == materializedEffect.status
            && previousEffect.stagedRevision
                == materializedEffect.stagedRevision
            && previousEffect.activeRevision
                == materializedEffect.activeRevision,
        "Rejected Previous must leave both history objects and render state unchanged.");
    expect(repeatBridge.NextCrop()
            && SendShaderCommit(
                repeatBridge, renderWindow),
        "The retained F redo should rebuild on the materialized input stamp.");
    const auto redoneHistory =
        repeatBridge.GetCropHistory();
    expect(redoneHistory.nodeCount == 1
            && redoneHistory.operationCount == 1
            && redoneHistory.baseNodeCount == 2
            && redoneHistory.allOperationCount == 3,
        "Redo after materialization should not duplicate or delete allHistory nodes.");
    bool hasRootExport = false;
    expect(repeatBridge.ExportCrop(
        input,
        [&hasRootExport](CropExportResult result) {
            hasRootExport = result.inputVersion == 1
                && result.nodeCount == 3
                && result.operations.size() == 3
                && result.operations[0].operationIndex == 1
                && result.operations[1].operationIndex == 2
                && result.operations[2].operationIndex == 6;
        }),
        "Materialized history should export the absolute prefix directly from the root input.");
    for (int pollCount = 0;
        pollCount < 200
            && !repeatBridge.GetExportTickNeeded();
        ++pollCount) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    expect(repeatBridge.SendExportResult()
            && hasRootExport,
        "Root export should fuse A-B-F without publishing or caching intermediate masks.");

    auto originalInput = input;
    originalInput.inputVersion = 3;
    expect(repeatBridge.StartCropBaseline(
                originalInput, 0)
            && repeatBridge.GetCropHistory().nodeCount == 1
            && repeatBridge.GetCropHistory().baseNodeCount == 2
            && repeatService->SetRenderInputStamp(
                { image.GetPointer(), 3 })
            && repeatBridge.SetCropBaselineComplete(),
        "Switching back to the root snapshot should reactivate the complete history.");
    const auto originalHistory =
        repeatBridge.GetCropHistory();
    expect(originalHistory.nodeCount == 0
            && originalHistory.operationCount == 3
            && originalHistory.baseNodeCount == 0
            && originalHistory.allOperationCount == 3
            && repeatService->GetEffectState().status
                == RenderEffectStatus::Idle,
        "The root baseline should render the original node and expose allHistory only as active redo.");
    expect(repeatBridge.NextCrop()
            && SendShaderCommit(
                repeatBridge, renderWindow)
            && repeatBridge.GetCropHistory().nodeCount == 1,
        "Explicit root restore should allow redo from allHistory node zero.");
    expect(repeatBridge.ClearBindings(),
        "The repeated-release bridge should clear its bindings.");

    auto lagService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    auto lagView = view;
    lagView.referenceService = lagService;
    lagView.targetServices = { lagService };
    auto lagImage = vtkSmartPointer<vtkImageData>::New();
    lagImage->ShallowCopy(image);
    auto lagInput = input;
    lagInput.inputVersion = 4;
    lagInput.imageData = lagImage;
    const RenderInputStamp lagStamp = {
        lagImage.GetPointer(), 4
    };
    CropBridge lagBridge;
    expect(lagBridge.StartView(lagView)
            && lagBridge.SetCropInput(lagInput)
            && lagBridge.SwitchCropBox()
            && lagBridge.SetCropMode(
                CropRemovalMode::KeepInside),
        "A render-convergence bridge should enter armed editing.");
    renderer->ResetCamera(
        lagInput.inputModelBounds.data());
    renderWindow->Render();
    expect(SendWidgetInput(renderer, interactor)
            && lagBridge.GetShaderTickNeeded()
            && lagBridge.GetCropHistory().nodeCount == 0
            && lagService->GetEffectState().status
                == RenderEffectStatus::Idle,
        "A release must wait instead of disappearing while the render input stamp lags.");
    expect(!lagBridge.SendShaderCommit()
            && lagBridge.GetShaderTickNeeded()
            && lagService->SetRenderInputStamp(
                lagStamp)
            && !lagBridge.SendShaderCommit()
            && lagBridge.GetShaderTickNeeded(),
        "The retained release should stage after the render input converges.");
    expect(lagBridge.SetCropMode(
                CropRemovalMode::RemoveInside)
            && SendShaderCommit(
                lagBridge, renderWindow)
            && lagBridge.GetShaderTickNeeded()
            && SendShaderCommit(
                lagBridge, renderWindow)
            && lagBridge.GetCropHistory().nodeCount == 1
            && lagBridge.GetCropHistory().operationCount == 1
            && lagBridge.GetCropHistory().editMode
                == CropRemovalMode::RemoveInside,
        "The latest removal mode should commit after the retained release.");
    expect(lagBridge.ClearBindings(),
        "The render-convergence bridge should clear its bindings.");

    auto zeroService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    auto zeroView = view;
    zeroView.referenceService = zeroService;
    zeroView.targetServices = { zeroService };
    CropBridge zeroBridge;
    expect(zeroBridge.StartView(zeroView)
            && zeroBridge.SetCropInput(input)
            && zeroBridge.SetCropMode(
                CropRemovalMode::KeepInside),
        "A zero-node reentry bridge should enter armed editing.");
    const std::array<bool, 8> isPlaneOp = {
        true, true, false, false, true, false, true, false
    };
    for (std::size_t index = 0;
        index < isPlaneOp.size();
        ++index) {
        const bool isSwitched = isPlaneOp[index]
            ? zeroBridge.SwitchCropPlane()
            : zeroBridge.SwitchCropBox();
        renderWindow->Render();
        expect(isSwitched
                && (isPlaneOp[index]
                    ? SendPlaneInput(
                        renderer,
                        interactor,
                        8 + static_cast<int>(index))
                    : SendWidgetInput(
                        renderer,
                        interactor,
                        8 + static_cast<int>(index)))
                && SendShaderCommit(
                    zeroBridge,
                    renderWindow),
            "AABBABAB should commit every effective crop operation.");
    }
    expect(zeroBridge.GetCropHistory().nodeCount == 8
            && zeroBridge.GetCropHistory().operationCount == 8,
        "AABBABAB should expose eight committed history operations.");
    bool hasShapeSeq = false;
    expect(zeroBridge.ExportCrop(
        input,
        [&hasShapeSeq, &isPlaneOp](CropExportResult result) {
            hasShapeSeq = result.nodeCount == isPlaneOp.size()
                && result.operations.size() == isPlaneOp.size();
            for (std::size_t index = 0;
                hasShapeSeq && index < isPlaneOp.size();
                ++index) {
                const CropShape expectedShape = isPlaneOp[index]
                    ? CropShape::Plane
                    : CropShape::Box;
                hasShapeSeq = result.operations[index].geometryType
                    == expectedShape;
            }
        }), "AABBABAB should start an export snapshot.");
    for (int pollCount = 0;
        pollCount < 200
            && !zeroBridge.GetExportTickNeeded();
        ++pollCount) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    expect(zeroBridge.SendExportResult()
            && hasShapeSeq,
        "AABBABAB should preserve the exact Plane/Box operation order.");
    for (std::size_t nodeCount = 8;
        nodeCount > 0;
        --nodeCount) {
        expect(zeroBridge.PreviousCrop()
                && SendShaderCommit(zeroBridge, renderWindow)
                && zeroBridge.GetCropHistory().nodeCount
                    == nodeCount - 1,
            "Previous should commit every prefix down to node zero.");
    }
    const auto zeroEffect = zeroService->GetEffectState();
    const int zeroDetach = zeroService->detachCount;
    expect(zeroBridge.ExitCrop()
            && zeroBridge.StartView(zeroView)
            && zeroService->detachCount == zeroDetach
            && zeroService->GetEffectState().status
                == RenderEffectStatus::Committed
            && zeroService->GetEffectState().activeRevision
                == zeroEffect.activeRevision,
        "Reentry at node zero should retain the committed baseline binding.");
    expect(zeroBridge.SwitchCropPlane()
            && zeroBridge.SetCropMode(
                CropRemovalMode::KeepInside),
        "Reentry at node zero should arm a new plane operation.");
    renderWindow->Render();
    expect(SendPlaneInput(renderer, interactor, 18)
            && zeroBridge.GetShaderTickNeeded()
            && SendShaderCommit(zeroBridge, renderWindow)
            && zeroBridge.GetCropHistory().nodeCount == 1
            && zeroBridge.GetCropHistory().operationCount == 1
            && !zeroBridge.SetCropNode(2)
            && zeroService->GetEffectState().status
                == RenderEffectStatus::Committed
            && zeroService->GetEffectState().activeRevision
                > zeroEffect.activeRevision,
        "A valid crop after zero-node reentry should replace redo and commit.");
    expect(zeroBridge.ClearBindings(),
        "The zero-node reentry bridge should clear its bindings.");

    auto planeService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    auto planeView = view;
    planeView.referenceService = planeService;
    planeView.targetServices = { planeService };
    CropBridge planeBridge;
    expect(planeBridge.StartView(planeView)
            && planeBridge.SetCropInput(input)
            && planeBridge.SwitchCropPlane()
            && planeBridge.SetCropMode(CropRemovalMode::KeepInside),
        "A plane bridge should enter armed editing.");
    renderWindow->Render();
    expect(SendWidgetInput(renderer, interactor, 0)
            && !planeBridge.GetShaderTickNeeded()
            && planeBridge.GetCropHistory().operationCount == 0
            && planeService->GetEffectState().status
                == RenderEffectStatus::Idle
            && planeService->GetEffectState().stagedRevision == 0
            && planeService->GetEffectState().activeRevision == 0,
        "A zero-distance plane interaction must not create crop history.");
    expect(planeBridge.ClearBindings(),
        "The empty plane bridge should clear its bindings.");

    auto exitDragService = std::make_shared<CropServiceStub>(
        inputStamp, renderer);
    auto exitDragView = view;
    exitDragView.referenceService = exitDragService;
    exitDragView.targetServices = { exitDragService };
    CropBridge exitDragBridge;
    expect(exitDragBridge.StartView(exitDragView)
            && exitDragBridge.SetCropInput(input)
            && exitDragBridge.SwitchCropBox()
            && exitDragBridge.SetCropMode(CropRemovalMode::KeepInside),
        "An exit-during-drag bridge should enter armed box editing.");
    renderWindow->Render();
    expect(SendWidgetInput(renderer, interactor, 8, false),
        "Bridge test should begin a box drag without releasing it.");
    expect(exitDragBridge.ExitCrop()
            && !exitDragBridge.GetShaderTickNeeded()
            && exitDragBridge.GetCropHistory().operationCount == 0,
        "Exiting during a drag must not create a staged or committed history operation.");
    interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    expect(exitDragBridge.ClearBindings(),
        "The exit-during-drag bridge should clear its bindings.");

    auto multiRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto multiWindow = vtkSmartPointer<vtkRenderWindow>::New();
    auto multiInteractor =
        vtkSmartPointer<vtkRenderWindowInteractor>::New();
    multiWindow->SetOffScreenRendering(1);
    multiWindow->SetSize(200, 200);
    multiWindow->AddRenderer(multiRenderer);
    multiInteractor->SetRenderWindow(multiWindow);
    auto firstTarget = std::make_shared<CropServiceStub>(
        inputStamp, multiRenderer);
    auto secondTarget = std::make_shared<CropServiceStub>(
        inputStamp, multiRenderer);
    CropViewRequest multiView;
    multiView.renderer = multiRenderer;
    multiView.interactor = multiInteractor;
    multiView.referenceService = firstTarget;
    multiView.targetServices = {
        firstTarget, secondTarget
    };
    CropBridge multiBridge;
    expect(multiBridge.StartView(multiView)
            && multiBridge.SetCropInput(input)
            && multiBridge.SwitchCropBox()
            && multiBridge.SetCropMode(
                CropRemovalMode::KeepInside),
        "A multi-target bridge should enter armed box editing.");
    multiRenderer->ResetCamera(
        input.inputModelBounds.data());
    multiWindow->Render();
    expect(SendWidgetInput(
            multiRenderer, multiInteractor)
            && SendShaderCommit(
                multiBridge, multiWindow),
        "A multi-target crop revision should commit.");
    expect(multiBridge.SetCropNode(0),
        "A multi-target bridge should stage history node zero.");
    multiWindow->Render();
    const int firstDirtyCount =
        firstTarget->dirtyCount;
    const int secondDirtyCount =
        secondTarget->dirtyCount;
    expect(multiBridge.SendShaderCommit()
            && multiBridge.GetCropHistory().nodeCount == 0
            && firstTarget->dirtyCount
                == firstDirtyCount + 1
            && secondTarget->dirtyCount
                == secondDirtyCount + 1,
        "Committing history node zero should request a baseline frame on every current target.");
    expect(multiBridge.ClearBindings(),
        "The multi-target bridge should clear its bindings.");
    multiWindow->Finalize();
    return failureCount;
}
