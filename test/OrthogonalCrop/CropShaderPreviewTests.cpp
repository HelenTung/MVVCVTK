#include "PlanarTestSuites.h"
#include "Algorithms/CropAlgorithm.h"
#include "Render/CropShaderController.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"
#include "Render/Strategies/SliceStrategy.h"
#include "Render/Strategies/VolumeStrategy.h"

#include <vtkAutoInit.h>
#include <vtkAppendPolyData.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkShaderProperty.h>
#include <vtkShaderProgram.h>
#include <vtkSmartPointer.h>
#include <vtkTextureObject.h>
#include <vtkType.h>
#include <vtkWeakPointer.h>
#include <vtkCubeSource.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkActor.h>
#include <vtkImageData.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkInformation.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkOpenGLRenderPass.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkProperty.h>
#include <vtkVolume.h>
#include <vtkWindowToImageFilter.h>
#ifndef GLAD_API_CALL_EXPORT
#define GLAD_API_CALL_EXPORT
#endif
#include <vtk_glad.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <vector>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

namespace {
constexpr unsigned int kLargeTableWidth = 2565;
int inputIdentity = 0;

class ProbePass final : public vtkOpenGLRenderPass {
public:
    static ProbePass* New()
    {
        return new ProbePass();
    }
    vtkTypeMacro(ProbePass, vtkOpenGLRenderPass);

    void Render(const vtkRenderState*) override {}

    bool SetShaderParameters(
        vtkShaderProgram* program,
        vtkAbstractMapper* mapper,
        vtkProp*,
        vtkOpenGLVertexArrayObject* = nullptr) override
    {
        ++m_setCount;
        return program && mapper;
    }

    int GetSetCount() const noexcept
    {
        return m_setCount;
    }

protected:
    ProbePass()
    {
        SetActiveDrawBuffers(1);
    }
    ~ProbePass() override = default;

private:
    int m_setCount = 0;
};

double GetRenderRate(const bool isInteracting) noexcept
{
    return isInteracting ? 15.0 : 0.001;
}

bool SetExpect(const bool isExpected, const char* message)
{
    if (!isExpected) {
        std::cerr << message << '\n';
    }
    return isExpected;
}

bool SetSliceCamera(
    vtkRenderer* renderer,
    vtkImageData* image,
    const Orientation orientation)
{
    if (!renderer || !image || !renderer->GetActiveCamera()) {
        return false;
    }

    double imageCenter[3] = { 0.0, 0.0, 0.0 };
    image->GetCenter(imageCenter);
    vtkCamera* camera = renderer->GetActiveCamera();
    const double distance = (std::max)(camera->GetDistance(), 1e-6);
    camera->ParallelProjectionOn();
    camera->SetFocalPoint(imageCenter);
    if (orientation == Orientation::Top_down) {
        camera->SetPosition(
            imageCenter[0], imageCenter[1], imageCenter[2] + distance);
        camera->SetViewUp(0.0, 1.0, 0.0);
    }
    else if (orientation == Orientation::Front_back) {
        camera->SetPosition(
            imageCenter[0], imageCenter[1] + distance, imageCenter[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
    }
    else {
        camera->SetPosition(
            imageCenter[0] + distance, imageCenter[1], imageCenter[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
    }
    renderer->ResetCamera();
    renderer->ResetCameraClippingRange();
    return true;
}

CropShaderPayload BuildPayload(
    const std::vector<CropOpItem>& operations,
    const std::size_t nodeCount,
    const std::uint64_t revision,
    const std::uint64_t inputVersion = 1)
{
    const auto tableResult = CropAlgorithm::BuildPredicateTable(
        operations,
        operations.size());
    CropShaderPayload payload;
    payload.revision = revision;
    payload.sourceStamp = { &inputIdentity, inputVersion };
    payload.nodeCount = nodeCount;
    payload.predicateTable = tableResult.predicateTable;
    return payload;
}

bool StartTextureCapabilityCase()
{
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(8, 8);
    renderWindow->AddRenderer(renderer);
    renderWindow->Render();

    auto* context = vtkOpenGLRenderWindow::SafeDownCast(renderWindow);
    bool isPassed = SetExpect(
        context != nullptr,
        "Offscreen rendering should provide an OpenGL render window.");
    if (!context) {
        return false;
    }

    context->MakeCurrent();
    const int maximumWidth = vtkTextureObject::GetMaximumTextureSize(context);
    isPassed = SetExpect(
        maximumWidth >= static_cast<int>(kLargeTableWidth),
        "The current context should support a 2565-texel crop table.") && isPassed;
    isPassed = SetExpect(
        vtkTextureObject::IsSupported(context, true, false, false),
        "The current context should support floating-point textures.") && isPassed;

    std::vector<float> table(kLargeTableWidth * 4, 0.0f);
    table.back() = 1.0f;
    auto texture = vtkSmartPointer<vtkTextureObject>::New();
    texture->SetContext(context);
    texture->SetInternalFormat(GL_RGBA32F);
    texture->SetFormat(GL_RGBA);
    texture->SetDataType(GL_FLOAT);
    const bool isCreated = texture->Create1DFromRaw(
        kLargeTableWidth,
        4,
        VTK_FLOAT,
        table.data());
    isPassed = SetExpect(
        isCreated && texture->GetInternalFormat(VTK_FLOAT, 4, false) == GL_RGBA32F,
        "The current context should allocate a real RGBA32F 1D crop table.") && isPassed;
    isPassed = SetExpect(
        glGetError() == GL_NO_ERROR,
        "RGBA32F 1D crop table allocation should not leave an OpenGL error.") && isPassed;

    texture->ReleaseGraphicsResources(context);
    renderWindow->Finalize();
    return isPassed;
}

bool StartCachedProgramZeroCase()
{
    vtkNew<vtkCubeSource> cube;
    vtkNew<vtkOpenGLPolyDataMapper> mapper;
    mapper->SetInputConnection(cube->GetOutputPort());
    vtkNew<vtkActor> actor;
    actor->SetMapper(mapper);
    vtkNew<vtkRenderer> renderer;
    renderer->AddActor(actor);
    vtkNew<vtkRenderWindow> renderWindow;
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(32, 32);
    renderWindow->AddRenderer(renderer);
    renderer->ResetCamera();
    renderWindow->Render();

    struct ShaderCapture final {
        vtkWeakPointer<vtkShaderProgram> m_program;
        int m_eventCount = 0;
    };
    ShaderCapture shaderCapture;
    vtkNew<vtkCallbackCommand> captureCommand;
    captureCommand->SetClientData(&shaderCapture);
    captureCommand->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void* callData) {
            auto* capture = static_cast<ShaderCapture*>(
                clientData);
            capture->m_program = static_cast<vtkShaderProgram*>(callData);
            ++capture->m_eventCount;
        });
    const unsigned long captureTag = mapper->AddObserver(
        vtkCommand::UpdateShaderEvent,
        captureCommand);

    CropShaderController controller(RenderTargetKind::PolyData);
    bool isPassed = SetExpect(
        controller.SetShaderTarget(mapper, actor->GetShaderProperty()),
        "The cached-program regression should attach its crop shader.");

    CropOpItem rejectAll;
    rejectAll.operationIndex = 1;
    rejectAll.geometryType = CropShape::Plane;
    rejectAll.planeCenterInInputModel = { 100.0, 0.0, 0.0 };
    rejectAll.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    const auto activePayload = BuildPayload({ rejectAll }, 1, 70);
    isPassed = SetExpect(
        controller.SetCropParams(activePayload)
            && controller.StartRender(renderer),
        "The cached-program regression should stage its active crop.")
        && isPassed;
    renderWindow->Render();
    isPassed = controller.StopRender() && isPassed;
    isPassed = SetExpect(
        controller.GetState().status == RenderEffectStatus::Ready
            && controller.SetCropCommit(activePayload.revision)
            && controller.SetCropComplete(activePayload.revision),
        "The cached-program regression should commit its active crop.")
        && isPassed;
    isPassed = controller.StartRender(renderer) && isPassed;
    renderWindow->Render();
    isPassed = controller.StopRender() && isPassed;

    auto* context = vtkOpenGLRenderWindow::SafeDownCast(renderWindow);
    auto* program = shaderCapture.m_program.GetPointer();
    const int activeEventCount = shaderCapture.m_eventCount;
    int activeNodeCount = -1;
    if (context && program) {
        context->MakeCurrent();
        const int location = glGetUniformLocation(
            program->GetHandle(),
            "mvvcvtk_cropNodeCount");
        if (location >= 0) {
            glGetUniformiv(
                program->GetHandle(),
                location,
                &activeNodeCount);
        }
    }
    isPassed = SetExpect(
        context && program && activeNodeCount == 1,
        "The cached shader program should first hold nodeCount=1.")
        && isPassed;

    auto zeroPayload = activePayload;
    zeroPayload.revision = 71;
    zeroPayload.nodeCount = 0;
    isPassed = SetExpect(
        controller.SetCropParams(zeroPayload)
            && controller.StartRender(renderer)
            && controller.StopRender()
            && controller.GetState().status == RenderEffectStatus::Ready
            && controller.SetCropCommit(zeroPayload.revision)
            && controller.SetCropComplete(zeroPayload.revision),
        "The shared crop table should commit nodeCount=0 without recompiling.")
        && isPassed;

    isPassed = controller.StartRender(renderer) && isPassed;
    int zeroNodeCount = -1;
    if (context && program) {
        context->MakeCurrent();
        const int location = glGetUniformLocation(
            program->GetHandle(),
            "mvvcvtk_cropNodeCount");
        if (location >= 0) {
            glGetUniformiv(
                program->GetHandle(),
                location,
                &zeroNodeCount);
        }
    }
    isPassed = SetExpect(
        shaderCapture.m_eventCount == activeEventCount
            && zeroNodeCount == 0,
        "nodeCount=0 should refresh a cached shader program without a new shader event.")
        && isPassed;
    isPassed = controller.StopRender() && isPassed;

    mapper->RemoveObserver(captureTag);
    // controller 必须先在有效 context 上释放 texture；renderWindow 由声明逆序随后析构。
    return isPassed;
}

bool StartStrategyCase(
    const std::shared_ptr<AbstractVisualStrategy>& strategy,
    vtkSmartPointer<vtkDataObject> input,
    const CropShaderPayload& payload,
    const char* strategyName)
{
    auto effect = std::make_shared<CropShaderEffect>();
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(32, 32);
    renderWindow->AddRenderer(renderer);

    strategy->SetInputData(std::move(input));
    bool isPassed = strategy->SetRenderInputStamp(
        payload.sourceStamp);
    strategy->AttachRenderer(renderer);
    isPassed = SetExpect(
        strategy->AttachRenderEffect(
            effect, RenderBindingUse::Current),
        strategyName) && isPassed;
    renderer->GetActiveCamera()->ParallelProjectionOff();
    renderer->ResetCamera();
    RenderParams params;
    params.scalarRange[0] = 0.0;
    params.scalarRange[1] = 1.0;
    params.volumeTransferFunction.colorNodes = {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 }
    };
    params.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 1.0, 1.0 }
    };
    strategy->SetVisualState(
        params,
        UpdateFlags::VolumeTransfer | UpdateFlags::Material | UpdateFlags::WindowLevel);
    isPassed = SetExpect(effect->SetCropParams(payload), strategyName)
        && isPassed;
    renderWindow->Render();
    auto staged = effect->GetState();
    if (staged.status == RenderEffectStatus::Staged) {
        renderWindow->Render();
        staged = effect->GetState();
    }
    if (staged.status != RenderEffectStatus::Ready) {
        std::cerr << strategyName << " state=" << static_cast<int>(staged.status)
                  << " reason=" << static_cast<int>(staged.failureReason)
                  << " message=" << staged.message << '\n';
    }
    isPassed = SetExpect(
        staged.status == RenderEffectStatus::Ready
            && staged.stagedRevision == payload.revision,
        strategyName) && isPassed;
    isPassed = SetExpect(
        effect->SetCropCommit(payload.revision),
        strategyName) && isPassed;
    renderWindow->Render();
    const auto committed = effect->GetState();
    isPassed = SetExpect(
        committed.status == RenderEffectStatus::Committed
            && committed.activeRevision == payload.revision
            && glGetError() == GL_NO_ERROR,
        strategyName) && isPassed;
    if (auto* volume = vtkVolume::SafeDownCast(
            strategy->GetMainProp())) {
        auto* volumeMapper =
            vtkGPUVolumeRayCastMapper::SafeDownCast(
                volume->GetMapper());
        renderWindow->SetDesiredUpdateRate(
            GetRenderRate(true));
        renderWindow->Render();
        const auto previewState = effect->GetState();
        // Crop 的契约是 preview/still Render 都保持同一 committed
        // shader payload；具体采样距离由 VolumeStrategy 专项测试负责。
        const bool isPreviewCommitted =
            volumeMapper
            && previewState.status
                == RenderEffectStatus::Committed
            && previewState.activeRevision
                == payload.revision;
        renderWindow->SetDesiredUpdateRate(
            GetRenderRate(false));
        renderWindow->Render();
        const auto stillState = effect->GetState();
        isPassed = SetExpect(
            isPreviewCommitted
                && volumeMapper
                && stillState.status
                    == RenderEffectStatus::Committed
                && stillState.activeRevision
                    == payload.revision,
            "Volume preview must preserve the committed crop effect.")
            && isPassed;
    }
    isPassed = SetExpect(effect->ClearCropParams(), strategyName)
        && isPassed;
    renderWindow->Render();
    isPassed = SetExpect(
        strategy->DetachRenderEffect(effect.get()),
        strategyName) && isPassed;
    strategy->DetachRenderer(renderer);
    renderWindow->Finalize();
    return isPassed;
}

bool SetCropCommitted(
    const std::shared_ptr<AbstractVisualStrategy>& strategy,
    const std::shared_ptr<CropShaderEffect>& effect,
    const CropShaderPayload& payload,
    vtkRenderWindow* renderWindow);

bool StartMapperCases()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(2, 9, -3, 4, 5, 12);
    image->SetOrigin(10.0, -20.0, 30.0);
    image->SetSpacing(0.5, 1.0, 1.5);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(),
        1.0f);

    vtkNew<vtkCubeSource> cube;
    cube->SetBounds(-2.0, 2.0, -2.0, 2.0, -2.0, 2.0);
    cube->Update();
    auto poly = vtkSmartPointer<vtkPolyData>::New();
    poly->DeepCopy(cube->GetOutput());

    CropOpItem operation;
    operation.operationIndex = 1;
    operation.geometryType = CropShape::Plane;
    operation.removalMode = CropRemovalMode::KeepInside;
    operation.planeCenterInInputModel = { -100.0, 0.0, 0.0 };
    operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    const auto payload = BuildPayload({ operation }, 1, 1);

    bool isPassed = StartStrategyCase(
        std::make_shared<VolumeStrategy>(), image, payload,
        "Volume crop shader should realize and commit the shared revision.");
    isPassed = StartStrategyCase(
        std::make_shared<IsoSurfaceStrategy>(), poly, payload,
        "Iso crop shader should realize and commit the shared revision.") && isPassed;
    isPassed = StartStrategyCase(
        std::make_shared<SliceStrategy>(Orientation::Top_down), image, payload,
        "Top-down Slice should realize and commit the shared revision.") && isPassed;
    isPassed = StartStrategyCase(
        std::make_shared<SliceStrategy>(Orientation::Front_back), image, payload,
        "Front-back Slice should realize and commit the shared revision.") && isPassed;
    isPassed = StartStrategyCase(
        std::make_shared<SliceStrategy>(Orientation::Left_right), image, payload,
        "Left-right Slice should realize and commit the shared revision.") && isPassed;
    return isPassed;
}

bool StartEffectLifeCase()
{
    vtkNew<vtkCubeSource> cube;
    cube->Update();
    auto polyData = vtkSmartPointer<vtkPolyData>::New();
    polyData->DeepCopy(cube->GetOutput());

    int mismatchedIdentity = 0;
    auto rollbackEffect = std::make_shared<CropShaderEffect>();
    auto firstStrategy = std::make_shared<IsoSurfaceStrategy>();
    auto secondStrategy = std::make_shared<IsoSurfaceStrategy>();
    auto firstRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto secondRenderer = vtkSmartPointer<vtkRenderer>::New();
    firstStrategy->SetInputData(polyData);
    secondStrategy->SetInputData(polyData);
    bool isPassed = firstStrategy->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && secondStrategy->SetRenderInputStamp(
            { &mismatchedIdentity, 1 })
        && firstStrategy->AttachRenderEffect(
            rollbackEffect, RenderBindingUse::Current)
        && secondStrategy->AttachRenderEffect(
            rollbackEffect, RenderBindingUse::Current);
    firstStrategy->AttachRenderer(firstRenderer);
    secondStrategy->AttachRenderer(secondRenderer);

    CropOpItem keep;
    keep.operationIndex = 1;
    keep.geometryType = CropShape::Plane;
    keep.planeCenterInInputModel = { -10.0, 0.0, 0.0 };
    keep.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    const auto rejectedPayload = BuildPayload({ keep }, 1, 1);
    isPassed = SetExpect(
        !rollbackEffect->SetCropParams(rejectedPayload)
            && rollbackEffect->GetState().status
                == RenderEffectStatus::Idle
            && firstStrategy->GetRenderEffectState().status
                == RenderEffectStatus::Idle,
        "A mismatched Current target should roll back every accepted stage.")
        && isPassed;
    isPassed = secondStrategy->SetRenderInputStamp(
        { &inputIdentity, 1 }) && isPassed;
    const auto detachedPayload = BuildPayload({ keep }, 1, 2);
    isPassed = rollbackEffect->SetCropParams(detachedPayload)
        && isPassed;
    secondStrategy->DetachRenderer(secondRenderer);
    const auto detachedState = rollbackEffect->GetState();
    isPassed = SetExpect(
        detachedState.status == RenderEffectStatus::Failed
            && detachedState.failureReason
                == RenderEffectFailure::ContextLost
            && !rollbackEffect->SetCropCommit(
                detachedPayload.revision),
        "A detached staged Current target must invalidate the whole transaction.")
        && isPassed;
    isPassed = rollbackEffect->ClearCropStage(
        detachedPayload.revision) && isPassed;
    firstStrategy->DetachRenderer(firstRenderer);
    (void)firstStrategy->DetachRenderEffect(rollbackEffect.get());
    (void)secondStrategy->DetachRenderEffect(rollbackEffect.get());

    auto effect = std::make_shared<CropShaderEffect>();
    auto current = std::make_shared<IsoSurfaceStrategy>();
    auto currentRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto currentWindow = vtkSmartPointer<vtkRenderWindow>::New();
    currentWindow->SetOffScreenRendering(1);
    currentWindow->SetSize(32, 32);
    currentWindow->AddRenderer(currentRenderer);
    current->SetInputData(polyData);
    isPassed = current->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && current->AttachRenderEffect(
            effect, RenderBindingUse::Current)
        && isPassed;
    current->AttachRenderer(currentRenderer);
    currentRenderer->ResetCamera();
    isPassed = SetCropCommitted(
        current,
        effect,
        BuildPayload({ keep }, 1, 10),
        currentWindow) && isPassed;

    auto candidate = std::make_shared<IsoSurfaceStrategy>();
    auto candidateRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto candidateWindow = vtkSmartPointer<vtkRenderWindow>::New();
    candidateWindow->SetOffScreenRendering(1);
    candidateWindow->SetSize(32, 32);
    candidateWindow->AddRenderer(candidateRenderer);
    candidate->SetInputData(polyData);
    isPassed = candidate->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && candidate->AttachRenderEffect(
            effect, RenderBindingUse::Candidate)
        && isPassed;
    candidate->AttachRenderer(candidateRenderer);
    candidateRenderer->ResetCamera();
    for (int renderCount = 0; renderCount < 2; ++renderCount) {
        candidateWindow->Render();
        if (candidate->GetRenderEffectState().status
            == RenderEffectStatus::Committed) {
            break;
        }
    }
    isPassed = SetExpect(
        candidate->GetRenderEffectState().status
                == RenderEffectStatus::Committed
            && candidate->GetRenderEffectState().activeRevision == 10,
        "A Candidate binding should replay only the committed revision.")
        && isPassed;

    const auto candidateState = candidate->GetRenderEffectState();
    const auto currentPayload = BuildPayload({ keep }, 1, 11);
    isPassed = SetExpect(
        effect->SetCropParams(currentPayload)
            && candidate->GetRenderEffectState().status
                == candidateState.status
            && candidate->GetRenderEffectState().activeRevision
                == candidateState.activeRevision,
        "A Candidate binding must not receive an in-flight Current stage.")
        && isPassed;
    isPassed = effect->ClearCropStage(currentPayload.revision)
        && isPassed;

    candidate->DetachRenderer(candidateRenderer);
    isPassed = candidate->DetachRenderEffect(effect.get())
        && candidate->GetRenderEffectState().failureReason
            == RenderEffectFailure::Unsupported
        && isPassed;
    candidateWindow->Finalize();

    auto replacementRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto replacementWindow = vtkSmartPointer<vtkRenderWindow>::New();
    replacementWindow->SetOffScreenRendering(1);
    replacementWindow->SetSize(32, 32);
    replacementWindow->AddRenderer(replacementRenderer);
    current->AttachRenderer(replacementRenderer);
    currentWindow->Finalize();
    replacementRenderer->ResetCamera();
    for (int renderCount = 0; renderCount < 2; ++renderCount) {
        replacementWindow->Render();
        if (current->GetRenderEffectState().status
            == RenderEffectStatus::Committed) {
            break;
        }
    }
    isPassed = SetExpect(
        current->GetRenderEffectState().status
                == RenderEffectStatus::Committed
            && current->GetRenderEffectState().activeRevision == 10,
        "Renderer replacement should build a fresh binding and replay committed state.")
        && isPassed;
    current->DetachRenderer(replacementRenderer);
    (void)current->DetachRenderEffect(effect.get());
    replacementWindow->Finalize();
    return isPassed;
}

bool StartEffectAtomicCase()
{
    vtkNew<vtkCubeSource> cube;
    cube->Update();
    auto polyData = vtkSmartPointer<vtkPolyData>::New();
    polyData->DeepCopy(cube->GetOutput());

    auto firstEffect = std::make_shared<CropShaderEffect>();
    auto secondEffect = std::make_shared<CropShaderEffect>();
    auto firstStrategy =
        std::make_shared<IsoSurfaceStrategy>();
    auto secondStrategy =
        std::make_shared<IsoSurfaceStrategy>();
    auto firstRenderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto secondRenderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto firstWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    auto secondWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    firstWindow->SetOffScreenRendering(1);
    secondWindow->SetOffScreenRendering(1);
    firstWindow->SetSize(32, 32);
    secondWindow->SetSize(32, 32);
    firstWindow->AddRenderer(firstRenderer);
    secondWindow->AddRenderer(secondRenderer);

    firstStrategy->SetInputData(polyData);
    secondStrategy->SetInputData(polyData);
    bool isPassed = firstStrategy->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && secondStrategy->SetRenderInputStamp(
            { &inputIdentity, 1 })
        && firstStrategy->AttachRenderEffect(
            firstEffect, RenderBindingUse::Current)
        && secondStrategy->AttachRenderEffect(
            secondEffect, RenderBindingUse::Current);
    firstStrategy->AttachRenderer(firstRenderer);
    secondStrategy->AttachRenderer(secondRenderer);
    firstRenderer->ResetCamera();
    secondRenderer->ResetCamera();

    CropOpItem keep;
    keep.operationIndex = 1;
    keep.geometryType = CropShape::Plane;
    keep.planeCenterInInputModel =
        { -10.0, 0.0, 0.0 };
    keep.planeNormalInInputModel =
        { 1.0, 0.0, 0.0 };
    isPassed = SetCropCommitted(
        firstStrategy,
        firstEffect,
        BuildPayload({ keep }, 1, 40),
        firstWindow) && isPassed;
    isPassed = SetCropCommitted(
        secondStrategy,
        secondEffect,
        BuildPayload({ keep }, 1, 40),
        secondWindow) && isPassed;

    const auto nextPayload =
        BuildPayload({ keep }, 0, 41);
    isPassed = firstEffect->SetCropParams(nextPayload)
        && secondEffect->SetCropParams(nextPayload)
        && isPassed;
    for (int renderCount = 0;
        renderCount < 2;
        ++renderCount) {
        firstWindow->Render();
        secondWindow->Render();
        if (firstEffect->GetState().status
                == RenderEffectStatus::Ready
            && secondEffect->GetState().status
                == RenderEffectStatus::Ready) {
            break;
        }
    }
    isPassed = SetExpect(
        firstEffect->GetState().status
                == RenderEffectStatus::Ready
            && secondEffect->GetState().status
                == RenderEffectStatus::Ready,
        "Two independent crop effects should stage the same revision before atomic commit.")
        && isPassed;

    secondStrategy->DetachRenderer(secondRenderer);
    isPassed = SetExpect(
        firstEffect->StartCropCommit(41)
            && !secondEffect->StartCropCommit(41)
            && firstEffect->GetCropCommitReady(41)
            && !secondEffect->GetCropCommitReady(41)
            && firstEffect->ClearCropCommit(41)
            && secondEffect->ClearCropStage(41),
        "Global commit readiness should reject a failed later effect before any target completes.")
        && isPassed;
    const auto firstState = firstEffect->GetState();
    const auto secondState = secondEffect->GetState();
    isPassed = SetExpect(
        firstState.status == RenderEffectStatus::Committed
            && firstState.activeRevision == 40
            && firstState.stagedRevision == 0
            && secondState.status
                == RenderEffectStatus::Committed
            && secondState.activeRevision == 40
            && secondState.stagedRevision == 0,
        "Cross-effect rollback should preserve the previous complete revision on every target.")
        && isPassed;

    firstStrategy->DetachRenderer(firstRenderer);
    (void)firstStrategy->DetachRenderEffect(
        firstEffect.get());
    (void)secondStrategy->DetachRenderEffect(
        secondEffect.get());
    firstWindow->Finalize();
    secondWindow->Finalize();
    return isPassed;
}

std::array<unsigned char, 3> GetCenterPixel(vtkRenderWindow* renderWindow)
{
    vtkNew<vtkWindowToImageFilter> capture;
    capture->SetInput(renderWindow);
    capture->SetInputBufferTypeToRGB();
    capture->ReadFrontBufferOff();
    // 只读取调用方刚完成的帧；默认重绘会悄悄补出第二帧，掩盖
    // Volume shader 在重建首帧未同步 uniform 的真实问题。
    capture->ShouldRerenderOff();
    capture->Update();
    int dims[3] = {};
    capture->GetOutput()->GetDimensions(dims);
    const auto* pixel = static_cast<const unsigned char*>(
        capture->GetOutput()->GetScalarPointer(dims[0] / 2, dims[1] / 2, 0));
    return pixel ? std::array<unsigned char, 3>{ pixel[0], pixel[1], pixel[2] }
                 : std::array<unsigned char, 3>{};
}

bool SetCropCommitted(
    const std::shared_ptr<AbstractVisualStrategy>& strategy,
    const std::shared_ptr<CropShaderEffect>& effect,
    const CropShaderPayload& payload,
    vtkRenderWindow* renderWindow)
{
    if (!strategy || !effect || !renderWindow
        || !effect->SetCropParams(payload)) {
        return false;
    }
    for (int renderCount = 0; renderCount < 2; ++renderCount) {
        renderWindow->Render();
        if (effect->GetState().status != RenderEffectStatus::Staged) {
            break;
        }
    }
    return effect->GetState().status == RenderEffectStatus::Ready
        && effect->SetCropCommit(payload.revision);
}

struct SliceTestView final {
    std::shared_ptr<SliceStrategy> m_strategy;
    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;
    CropMatrixDouble16Array m_inputToWorld = {};
    const char* m_name = "Slice";

    bool GetBaselineMatched(
        const std::vector<CropPointFloat3Array>& modelPoints) const
    {
        vtkNew<vtkWindowToImageFilter> capture;
        capture->SetInput(m_renderWindow);
        capture->SetInputBufferTypeToRGB();
        capture->ReadFrontBufferOff();
        capture->ShouldRerenderOff();
        capture->Update();
        for (const auto& modelPoint : modelPoints) {
            if (!GetPointBright(capture->GetOutput(), modelPoint)) {
                std::cerr << m_name << " baseline missed model point=("
                          << modelPoint[0] << ',' << modelPoint[1] << ','
                          << modelPoint[2] << ")\n";
                return false;
            }
        }
        return true;
    }

    bool GetActiveMatched(
        const CropPredicateTable& predicateTable,
        const std::size_t nodeCount,
        const std::vector<CropPointFloat3Array>& modelPoints) const
    {
        vtkNew<vtkWindowToImageFilter> capture;
        capture->SetInput(m_renderWindow);
        capture->SetInputBufferTypeToRGB();
        capture->ReadFrontBufferOff();
        capture->ShouldRerenderOff();
        capture->Update();
        for (const auto& modelPoint : modelPoints) {
            const bool isGpuKept = GetPointBright(capture->GetOutput(), modelPoint);
            const bool isCpuKept = CropAlgorithm::GetPointKept(
                predicateTable,
                nodeCount,
                modelPoint);
            if (isGpuKept != isCpuKept) {
                std::cerr << m_name << " crop mismatch model point=("
                          << modelPoint[0] << ','
                          << modelPoint[1] << ',' << modelPoint[2]
                          << ") cpu=" << isCpuKept
                          << " gpu=" << isGpuKept << '\n';
                return false;
            }
        }
        return true;
    }

private:
    bool GetPointBright(
        vtkImageData* pixels,
        const CropPointFloat3Array& modelPoint) const
    {
        if (!pixels || !m_renderer) {
            return false;
        }
        const double worldPoint[3] = {
            m_inputToWorld[0] * modelPoint[0]
                + m_inputToWorld[1] * modelPoint[1]
                + m_inputToWorld[2] * modelPoint[2]
                + m_inputToWorld[3],
            m_inputToWorld[4] * modelPoint[0]
                + m_inputToWorld[5] * modelPoint[1]
                + m_inputToWorld[6] * modelPoint[2]
                + m_inputToWorld[7],
            m_inputToWorld[8] * modelPoint[0]
                + m_inputToWorld[9] * modelPoint[1]
                + m_inputToWorld[10] * modelPoint[2]
                + m_inputToWorld[11]
        };
        m_renderer->SetWorldPoint(
            worldPoint[0], worldPoint[1], worldPoint[2], 1.0);
        m_renderer->WorldToDisplay();
        const auto* displayPoint = m_renderer->GetDisplayPoint();
        int pixelDims[3] = {};
        pixels->GetDimensions(pixelDims);
        const int x = std::clamp(
            static_cast<int>(displayPoint[0] + 0.5), 0, pixelDims[0] - 1);
        const int y = std::clamp(
            static_cast<int>(displayPoint[1] + 0.5), 0, pixelDims[1] - 1);
        const auto* pixel = static_cast<const unsigned char*>(
            pixels->GetScalarPointer(x, y, 0));
        return pixel && (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0);
    }
};

bool SetViewsCommitted(
    const std::vector<SliceTestView>& views,
    const std::shared_ptr<CropShaderEffect>& effect,
    const CropShaderPayload& payload)
{
    // 三视图事务必须保持同一 revision：
    // 1. 所有 context 先接收同一 immutable table；
    // 2. 各自 Render 到 Ready，期间任何窗口都不能提前显示 staged；
    // 3. 全部 Ready 后统一 Commit，再逐窗口验证 active revision。
    if (!effect || !effect->SetCropParams(payload)) {
        return false;
    }

    for (const auto& view : views) {
        for (int renderCount = 0; renderCount < 2; ++renderCount) {
            view.m_renderWindow->Render();
            if (effect->GetState().status
                != RenderEffectStatus::Staged) {
                break;
            }
        }
    }
    const auto readyState = effect->GetState();
    const bool isReady =
        readyState.status == RenderEffectStatus::Ready
        && readyState.stagedRevision == payload.revision;
    if (!isReady) {
        (void)effect->ClearCropStage(payload.revision);
        return false;
    }

    if (!effect->SetCropCommit(payload.revision)) {
        return false;
    }
    for (const auto& view : views) {
        view.m_renderWindow->Render();
        const auto state = view.m_strategy->GetRenderEffectState();
        if (state.status != RenderEffectStatus::Committed
            || state.activeRevision != payload.revision) {
            return false;
        }
    }
    return true;
}

bool StartSliceCoordinateCase()
{
    // 用真实加载形态覆盖非对称 extent、非零 origin、非单位 spacing、
    // direction 与 validity mask，再叠加非单位 model matrix。每个采样点先经
    // input-model -> world 投影到对应 MPR 像素，commit 后第一帧的 GPU truth
    // 必须与相同 float32 predicate table 的 CPU truth 完全一致。
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(2, 9, -3, 4, 5, 12);
    image->SetOrigin(10.0, -20.0, 30.0);
    image->SetSpacing(0.5, 1.0, 1.5);
    vtkNew<vtkMatrix3x3> direction;
    direction->Zero();
    direction->SetElement(0, 1, -1.0);
    direction->SetElement(1, 0, 1.0);
    direction->SetElement(2, 2, 1.0);
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(),
        1.0f);
    image->SetScalarComponentFromFloat(2, -3, 5, 0, 0.0f);

    auto validityMask = vtkSmartPointer<vtkImageData>::New();
    validityMask->CopyStructure(image);
    validityMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(validityMask->GetScalarPointer()),
        validityMask->GetNumberOfPoints(),
        static_cast<unsigned char>(1));
    auto samplingMask = vtkSmartPointer<vtkImageData>::New();
    samplingMask->CopyStructure(image);
    samplingMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    const int* samplingExtent = samplingMask->GetExtent();
    for (int z = samplingExtent[4]; z <= samplingExtent[5]; ++z) {
        for (int y = samplingExtent[2]; y <= samplingExtent[3]; ++y) {
            for (int x = samplingExtent[0]; x <= samplingExtent[1]; ++x) {
                const int relativeSum =
                    (x - samplingExtent[0])
                    + (y - samplingExtent[2])
                    + (z - samplingExtent[4]);
                samplingMask->SetScalarComponentFromFloat(
                    x, y, z, 0,
                    relativeSum % 3 == 0 ? 0.0f : 1.0f);
            }
        }
    }

    double inputBounds[6] = {};
    image->GetBounds(inputBounds);
    const CropPointFloat3Array inputCenter = {
        static_cast<float>((inputBounds[0] + inputBounds[1]) * 0.5),
        static_cast<float>((inputBounds[2] + inputBounds[3]) * 0.5),
        static_cast<float>((inputBounds[4] + inputBounds[5]) * 0.5)
    };
    const auto getAxisPoint = [
        &inputBounds,
        &inputCenter](const int axis, const double ratio) {
        CropPointFloat3Array point = inputCenter;
        point[axis] = static_cast<float>(
            inputBounds[axis * 2]
            + (inputBounds[axis * 2 + 1] - inputBounds[axis * 2]) * ratio);
        return point;
    };

    const CropMatrixDouble16Array inputToWorld = {
        1.5, 0.0, 0.0, 10.0,
        0.0, 0.75, 0.0, -6.0,
        0.0, 0.0, 2.0, 3.0,
        0.0, 0.0, 0.0, 1.0
    };
    struct SliceCase final {
        Orientation m_orientation;
        std::vector<CropPointFloat3Array> m_modelPoints;
        const char* m_name;
    };
    const std::vector<SliceCase> cases = {
        {
            Orientation::Top_down,
            { getAxisPoint(0, 0.15), getAxisPoint(0, 0.38),
              getAxisPoint(0, 0.62), getAxisPoint(0, 0.85) },
            "Top-down Slice"
        },
        {
            Orientation::Front_back,
            { getAxisPoint(0, 0.15), getAxisPoint(0, 0.38),
              getAxisPoint(0, 0.62), getAxisPoint(0, 0.85) },
            "Front-back Slice"
        },
        {
            Orientation::Left_right,
            { getAxisPoint(1, 0.15), getAxisPoint(1, 0.38),
              getAxisPoint(1, 0.62), getAxisPoint(1, 0.85) },
            "Left-right Slice"
        }
    };

    bool isPassed = true;
    auto effect = std::make_shared<CropShaderEffect>();
    std::vector<SliceTestView> views;
    views.reserve(cases.size());
    for (const auto& sliceCase : cases) {
        SliceTestView view;
        view.m_strategy = std::make_shared<SliceStrategy>(sliceCase.m_orientation);
        view.m_renderer = vtkSmartPointer<vtkRenderer>::New();
        view.m_renderer->SetBackground(0.0, 0.0, 0.0);
        view.m_renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
        view.m_renderWindow->SetOffScreenRendering(1);
        view.m_renderWindow->SetSize(180, 160);
        view.m_renderWindow->AddRenderer(view.m_renderer);
        view.m_inputToWorld = inputToWorld;
        view.m_name = sliceCase.m_name;
        view.m_strategy->SetInputData(image);
        view.m_strategy->SetInputMask(samplingMask);
        isPassed = view.m_strategy->SetRenderInputStamp(
            { &inputIdentity, 1 }) && isPassed;
        isPassed = view.m_strategy->AttachRenderEffect(
            effect, RenderBindingUse::Current) && isPassed;
        view.m_strategy->AttachRenderer(view.m_renderer);
        isPassed = SetSliceCamera(
            view.m_renderer,
            image,
            sliceCase.m_orientation) && isPassed;

        RenderParams params;
        params.modelMatrix = inputToWorld;
        params.cursor = {
            inputToWorld[0] * inputCenter[0] + inputToWorld[3],
            inputToWorld[5] * inputCenter[1] + inputToWorld[7],
            inputToWorld[10] * inputCenter[2] + inputToWorld[11]
        };
        params.windowLevel.windowWidth = 1.0;
        params.windowLevel.windowCenter = 0.5;
        params.visibilityMask = 0;
        view.m_strategy->SetVisualState(
            params,
            UpdateFlags::Transform | UpdateFlags::Cursor
                | UpdateFlags::WindowLevel | UpdateFlags::Visibility);
        view.m_renderer->ResetCamera();
        view.m_renderWindow->Render();
        const auto samplingDimensions =
            view.m_strategy->GetMaskWorkingDimensions();
        const bool isSamplingValid = samplingDimensions[0] > 0
            && samplingDimensions[1] > 0
            && samplingDimensions[2] == 1
            && view.m_strategy->GetMaskWorkingPixelsValid();
        if (!isSamplingValid) {
            std::cerr << sliceCase.m_name
                << " patterned mask diagnostics failed\n";
        }
        isPassed = isSamplingValid && isPassed;

        // 替换同几何 mask 后必须在下一帧重建二维输出；后续 effect oracle 使用全有效 mask。
        view.m_strategy->SetInputMask(validityMask);
        view.m_renderWindow->Render();
        const auto maskWorkingDimensions =
            view.m_strategy->GetMaskWorkingDimensions();
        const bool isReplacementValid = maskWorkingDimensions[0] > 0
            && maskWorkingDimensions[1] > 0
            && maskWorkingDimensions[2] == 1
            && view.m_strategy->GetMaskWorkingPixelsValid();
        if (!isReplacementValid) {
            std::cerr << sliceCase.m_name
                << " replacement mask diagnostics failed\n";
        }
        isPassed = isReplacementValid && isPassed;
        const bool isBaselineMatched =
            view.GetBaselineMatched(sliceCase.m_modelPoints);
        if (!isBaselineMatched) {
            std::cerr << sliceCase.m_name
                << " baseline pixels failed\n";
        }
        isPassed = isBaselineMatched && isPassed;
        views.push_back(std::move(view));
    }

    std::uint64_t revision = 20;
    const auto getSharedMatched = [
        &cases,
        &isPassed,
        &revision,
        &views,
        &effect](CropOpItem operation) {
        operation.operationIndex = revision;
        const auto table = CropAlgorithm::BuildPredicateTable({ operation }, 1);
        const auto payload = BuildPayload({ operation }, 1, revision++);
        bool isMatched = table.isSucceeded
            && table.predicateTable
            && SetViewsCommitted(views, effect, payload);
        if (isMatched) {
            for (std::size_t index = 0; index < views.size(); ++index) {
                isMatched = views[index].GetActiveMatched(
                    *table.predicateTable,
                    1,
                    cases[index].m_modelPoints) && isMatched;
            }
        }
        isPassed = isMatched && isPassed;
    };

    CropOpItem plane;
    plane.geometryType = CropShape::Plane;
    plane.planeCenterInInputModel = {
        inputCenter[0], inputCenter[1], inputCenter[2]
    };
    plane.planeNormalInInputModel = { 1.0, 1.0, 1.0 };
    getSharedMatched(plane);
    plane.removalMode = CropRemovalMode::RemoveInside;
    getSharedMatched(plane);

    CropOpItem box;
    box.geometryType = CropShape::Box;
    const double halfX = (inputBounds[1] - inputBounds[0]) * 0.25;
    const double halfY = (inputBounds[3] - inputBounds[2]) * 0.25;
    const double halfZ = (inputBounds[5] - inputBounds[4]) * 0.25;
    box.boxToInputModelMatrix = {
        halfX, 0.0, 0.0, inputCenter[0],
        0.0, halfY, 0.0, inputCenter[1],
        0.0, 0.0, halfZ, inputCenter[2],
        0.0, 0.0, 0.0, 1.0
    };
    getSharedMatched(box);
    box.removalMode = CropRemovalMode::RemoveInside;
    getSharedMatched(box);

    for (auto& view : views) {
        view.m_strategy->DetachRenderer(view.m_renderer);
        view.m_renderWindow->Finalize();
    }
    return SetExpect(
        isPassed,
        "All Slice orientations should match the CPU crop oracle for plane/box keep/remove under direction and model transforms.");
}

bool StartVolumeCoordinateCase()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(4, 11, -2, 5, 3, 8);
    image->SetOrigin(10.0, -20.0, 30.0);
    image->SetSpacing(0.5, 1.25, 2.0);
    vtkNew<vtkMatrix3x3> direction;
    direction->Zero();
    direction->SetElement(0, 1, -1.0);
    direction->SetElement(1, 0, 1.0);
    direction->SetElement(2, 2, 1.0);
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(),
        1.0f);

    auto strategy = std::make_shared<VolumeStrategy>();
    auto effect = std::make_shared<CropShaderEffect>();
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    renderer->SetBackground(0.0, 0.0, 0.0);
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(96, 96);
    renderWindow->AddRenderer(renderer);
    strategy->SetInputData(image);
    auto* volume = vtkVolume::SafeDownCast(
        strategy->GetMainProp());
    vtkNew<ProbePass> probePass;
    vtkNew<vtkInformation> propertyKeys;
    propertyKeys->Append(
        vtkOpenGLRenderPass::RenderPasses(), probePass);
    if (volume) {
        volume->SetPropertyKeys(propertyKeys);
    }
    bool isPassed = volume
        && strategy->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && strategy->AttachRenderEffect(
            effect, RenderBindingUse::Current);
    strategy->AttachRenderer(renderer);
    auto* passKey = vtkOpenGLRenderPass::RenderPasses();
    auto* attachedKeys = volume
        ? volume->GetPropertyKeys() : nullptr;
    isPassed = SetExpect(
        attachedKeys
            && attachedKeys->Length(passKey) == 2
            && attachedKeys->Get(passKey, 1) == probePass,
        "Volume crop must preserve the existing final render pass while installing its program hook.") && isPassed;
    renderer->RemoveAllViewProps();
    renderer->AddViewProp(strategy->GetMainProp());

    RenderParams params;
    params.scalarRange[0] = 0.0;
    params.scalarRange[1] = 1.0;
    params.volumeTransferFunction.colorNodes = {
        { 0.0, 0.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 }
    };
    params.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 1.0, 1.0 }
    };
    params.material.ambient = 1.0;
    params.material.diffuse = 0.0;
    params.material.specular = 0.0;
    params.material.isShadeOn = true;
    params.modelMatrix[3] = 50.0;
    params.modelMatrix[7] = -10.0;
    params.visibilityMask = 0;
    vtkCamera* camera = renderer->GetActiveCamera();
    camera->SetPosition(7.0, -2.0, 11.0);
    camera->SetFocalPoint(1.0, 2.0, 3.0);
    camera->SetViewUp(0.0, 1.0, 0.0);
    const std::array<double, 3> oldPosition = {
        camera->GetPosition()[0],
        camera->GetPosition()[1],
        camera->GetPosition()[2]
    };
    const std::array<double, 3> oldFocalPoint = {
        camera->GetFocalPoint()[0],
        camera->GetFocalPoint()[1],
        camera->GetFocalPoint()[2]
    };
    strategy->SetVisualState(
        params,
        UpdateFlags::VolumeTransfer | UpdateFlags::Material
            | UpdateFlags::Transform | UpdateFlags::Visibility);
    isPassed = SetExpect(
        std::equal(
            oldPosition.begin(),
            oldPosition.end(),
            camera->GetPosition())
            && std::equal(
                oldFocalPoint.begin(),
                oldFocalPoint.end(),
                camera->GetFocalPoint()),
        "Direct Strategy transform must not mutate the renderer camera.")
        && isPassed;
    camera->ParallelProjectionOff();
    renderer->ResetCamera();

    double bounds[6] = {};
    image->GetBounds(bounds);
    CropOpItem keepAll;
    keepAll.operationIndex = 1;
    keepAll.geometryType = CropShape::Plane;
    keepAll.planeCenterInInputModel = { bounds[0] - 1.0, 0.0, 0.0 };
    keepAll.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    isPassed = SetCropCommitted(
        strategy,
        effect,
        BuildPayload({ keepAll }, 1, 30),
        renderWindow) && isPassed;
    renderWindow->Render();
    const auto keptPixel = GetCenterPixel(renderWindow);

    auto rejectAll = keepAll;
    rejectAll.operationIndex = 2;
    rejectAll.planeCenterInInputModel = { bounds[1] + 1.0, 0.0, 0.0 };
    isPassed = SetCropCommitted(
        strategy,
        effect,
        BuildPayload({ rejectAll }, 1, 31),
        renderWindow) && isPassed;
    renderWindow->Render();
    const auto rejectedPixel = GetCenterPixel(renderWindow);
    isPassed = SetExpect(
        isPassed
            && keptPixel != std::array<unsigned char, 3>{ 0, 0, 0 }
            && rejectedPixel == std::array<unsigned char, 3>{ 0, 0, 0 },
        "Volume texture-to-dataset mapping should honor direction/extent/origin and exclude the prop model matrix.") && isPassed;
    const auto mapperInputCount = strategy->GetMapperInputCount();
    const auto resampleUpdateCount =
        strategy->GetResampleUpdateCount();

    // 交互期会临时关闭光照并切换 preview，退出后再恢复；两次都会
    // 重建 Volume shader。裁切 uniform 必须写入本帧实际绘制所用的
    // program，不能只刷新上一帧缓存。
    params.isInteracting = true;
    isPassed = strategy->SetVisualState(
        params, UpdateFlags::RenderRate) && isPassed;
    renderWindow->Render();
    const auto interactingPixel = GetCenterPixel(renderWindow);
    params.isInteracting = false;
    isPassed = strategy->SetVisualState(
        params, UpdateFlags::RenderRate) && isPassed;
    renderWindow->Render();
    const auto stillPixel = GetCenterPixel(renderWindow);
    isPassed = SetExpect(
        interactingPixel == std::array<unsigned char, 3>{ 0, 0, 0 }
            && stillPixel == std::array<unsigned char, 3>{ 0, 0, 0 },
        "Volume crop must survive the shader rebuilds caused by interaction quality transitions.") && isPassed;

    auto* shaderProperty = volume ? volume->GetShaderProperty() : nullptr;
    if (shaderProperty) {
        shaderProperty->AddFragmentShaderReplacement(
            "//VTK::RenderToImage::Dec",
            true,
            "//VTK::RenderToImage::Dec\nuniform float mvvcvtk_testProgram;\n",
            false);
    }
    renderWindow->Render();
    const auto rebuiltPixel = GetCenterPixel(renderWindow);
    isPassed = SetExpect(
        shaderProperty
            && rebuiltPixel == std::array<unsigned char, 3>{ 0, 0, 0 },
        "Volume crop must update the actual program on its shader rebuild frame.") && isPassed;
    isPassed = SetExpect(
        probePass->GetSetCount() > 0
            && strategy->GetMapperInputCount() == mapperInputCount
            && strategy->GetResampleUpdateCount() == resampleUpdateCount,
        "Volume crop program sync must not rebuild the LOD producer or replace the mapper input.") && isPassed;

    isPassed = SetCropCommitted(
        strategy,
        effect,
        BuildPayload({ rejectAll }, 0, 32),
        renderWindow) && isPassed;
    renderWindow->Render();
    const auto originalPixel = GetCenterPixel(renderWindow);
    isPassed = SetExpect(
        originalPixel != std::array<unsigned char, 3>{ 0, 0, 0 },
        "Volume history node zero should bypass cropping and restore the source pixels.") && isPassed;
    isPassed = strategy->DetachRenderEffect(effect.get())
        && isPassed;
    auto* detachedKeys = volume
        ? volume->GetPropertyKeys() : nullptr;
    isPassed = SetExpect(
        detachedKeys
            && detachedKeys->Length(passKey) == 1
            && detachedKeys->Get(passKey, 0) == probePass,
        "Detaching Volume crop must restore the pre-existing render-pass list exactly.") && isPassed;
    strategy->DetachRenderer(renderer);
    renderWindow->Finalize();
    return isPassed;
}

bool StartPixelTransactionCase()
{
    vtkNew<vtkCubeSource> cube;
    cube->SetBounds(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    cube->Update();

    auto strategy = std::make_shared<IsoSurfaceStrategy>();
    auto effect = std::make_shared<CropShaderEffect>();
    strategy->SetInputData(cube->GetOutput());
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    renderer->SetBackground(0.0, 0.0, 0.0);
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(64, 64);
    renderWindow->AddRenderer(renderer);
    bool isPassed = strategy->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && strategy->AttachRenderEffect(
            effect, RenderBindingUse::Current);
    strategy->AttachRenderer(renderer);
    if (auto* actor = vtkActor::SafeDownCast(strategy->GetMainProp())) {
        renderer->RemoveAllViewProps();
        renderer->AddActor(actor);
        actor->GetProperty()->SetColor(1.0, 1.0, 1.0);
        actor->GetProperty()->SetAmbient(1.0);
        actor->GetProperty()->LightingOff();
    }
    renderer->ResetCamera();

    CropOpItem keep;
    keep.operationIndex = 1;
    keep.geometryType = CropShape::Plane;
    keep.planeCenterInInputModel = { -10.0, 0.0, 0.0 };
    keep.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    isPassed = effect->SetCropParams(
        BuildPayload({ keep }, 1, 1)) && isPassed;
    renderWindow->Render();
    if (effect->GetState().status == RenderEffectStatus::Staged) {
        renderWindow->Render();
    }
    isPassed = effect->SetCropCommit(1) && isPassed;
    renderWindow->Render();
    const auto keptPixel = GetCenterPixel(renderWindow);

    auto remove = keep;
    remove.operationIndex = 2;
    remove.planeCenterInInputModel = { 10.0, 0.0, 0.0 };
    isPassed = effect->SetCropParams(
        BuildPayload({ remove }, 1, 2)) && isPassed;
    renderWindow->Render();
    const auto stagedPixel = GetCenterPixel(renderWindow);
    if (effect->GetState().status == RenderEffectStatus::Staged) {
        renderWindow->Render();
    }
    isPassed = SetExpect(
        stagedPixel == keptPixel && keptPixel[0] > 0,
        "A staged crop revision must not change visible pixels before commit.") && isPassed;
    isPassed = effect->SetCropCommit(2) && isPassed;
    renderWindow->Render();
    const auto removedPixel = GetCenterPixel(renderWindow);
    isPassed = SetExpect(
        removedPixel == std::array<unsigned char, 3>{ 0, 0, 0 },
        "A committed rejecting predicate should discard the center mesh pixel.") && isPassed;
    isPassed = effect->SetCropParams(
        BuildPayload({ remove }, 0, 3)) && isPassed;
    renderWindow->Render();
    if (effect->GetState().status == RenderEffectStatus::Staged) {
        renderWindow->Render();
    }
    isPassed = effect->SetCropCommit(3) && isPassed;
    renderWindow->Render();
    isPassed = SetExpect(
        GetCenterPixel(renderWindow) == keptPixel,
        "PolyData history node zero should bypass cropping and restore the source pixels.") && isPassed;
    renderWindow->Finalize();
    return isPassed;
}

bool StartLargeTableCase()
{
    vtkNew<vtkCubeSource> cube;
    cube->Update();
    auto strategy = std::make_shared<IsoSurfaceStrategy>();
    auto effect = std::make_shared<CropShaderEffect>();
    strategy->SetInputData(cube->GetOutput());
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(16, 16);
    renderWindow->AddRenderer(renderer);
    bool isPassed = strategy->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && strategy->AttachRenderEffect(
            effect, RenderBindingUse::Current);
    strategy->AttachRenderer(renderer);
    renderer->ResetCamera();

    std::vector<CropOpItem> operations(513);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        operations[index].operationIndex = index + 1;
        operations[index].geometryType = CropShape::Plane;
        operations[index].planeCenterInInputModel = { -100.0, 0.0, 0.0 };
        operations[index].planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    }
    isPassed = effect->SetCropParams(
        BuildPayload(operations, operations.size(), 1)) && isPassed;
    renderWindow->Render();
    if (effect->GetState().status == RenderEffectStatus::Staged) {
        renderWindow->Render();
    }
    isPassed = SetExpect(
        effect->GetState().status == RenderEffectStatus::Ready
            && effect->SetCropCommit(1),
        "A 513-item crop table should realize and commit without shader unrolling.") && isPassed;
    for (int frame = 0; frame < 100; ++frame) {
        renderWindow->Render();
    }
    isPassed = SetExpect(
        glGetError() == GL_NO_ERROR,
        "A committed 513-item table should survive 100 render frames without GL errors.") && isPassed;
    renderWindow->Finalize();
    return isPassed;
}

bool GetPointGridMatched(
    const std::vector<CropOpItem>& operations,
    const std::size_t nodeCount,
    const std::uint64_t revision,
    const std::shared_ptr<IsoSurfaceStrategy>& strategy,
    const std::shared_ptr<CropShaderEffect>& effect,
    vtkRenderer* renderer,
    vtkRenderWindow* renderWindow,
    const std::vector<CropPointFloat3Array>& modelPoints,
    const CropMatrixDouble16Array& modelToWorld)
{
    const auto table = CropAlgorithm::BuildPredicateTable(
        operations,
        operations.size());
    CropShaderPayload payload;
    payload.revision = revision;
    payload.sourceStamp = { &inputIdentity, 1 };
    payload.nodeCount = nodeCount;
    payload.predicateTable = table.predicateTable;
    const bool isStaged = table.isSucceeded
        && effect
        && effect->SetCropParams(std::move(payload));
    if (!isStaged) {
        std::cerr << "Point-grid stage failed revision=" << revision
                  << " table=" << table.isSucceeded << '\n';
        return false;
    }
    renderWindow->Render();
    if (effect->GetState().status == RenderEffectStatus::Staged) {
        renderWindow->Render();
    }
    const auto readyState = effect->GetState();
    if (readyState.status != RenderEffectStatus::Ready
        || !effect->SetCropCommit(revision)) {
        std::cerr << "Point-grid realization failed revision=" << revision
                  << " status=" << static_cast<int>(readyState.status)
                  << " reason=" << static_cast<int>(readyState.failureReason)
                  << " message=" << readyState.message << '\n';
        return false;
    }
    renderWindow->Render();

    vtkNew<vtkWindowToImageFilter> capture;
    capture->SetInput(renderWindow);
    capture->SetInputBufferTypeToRGB();
    capture->ReadFrontBufferOff();
    capture->Update();
    auto* pixels = capture->GetOutput();
    int pixelDims[3] = {};
    pixels->GetDimensions(pixelDims);
    for (const auto& modelPoint : modelPoints) {
        const double worldPoint[3] = {
            modelToWorld[0] * modelPoint[0] + modelToWorld[1] * modelPoint[1]
                + modelToWorld[2] * modelPoint[2] + modelToWorld[3],
            modelToWorld[4] * modelPoint[0] + modelToWorld[5] * modelPoint[1]
                + modelToWorld[6] * modelPoint[2] + modelToWorld[7],
            modelToWorld[8] * modelPoint[0] + modelToWorld[9] * modelPoint[1]
                + modelToWorld[10] * modelPoint[2] + modelToWorld[11]
        };
        renderer->SetWorldPoint(worldPoint[0], worldPoint[1], worldPoint[2], 1.0);
        renderer->WorldToDisplay();
        const auto* displayPoint = renderer->GetDisplayPoint();
        const int x = std::clamp(
            static_cast<int>(displayPoint[0] + 0.5), 0, pixelDims[0] - 1);
        const int y = std::clamp(
            static_cast<int>(displayPoint[1] + 0.5), 0, pixelDims[1] - 1);
        const auto* pixel = static_cast<const unsigned char*>(
            pixels->GetScalarPointer(x, y, 0));
        const bool isGpuKept = pixel
            && (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0);
        const bool isCpuKept = CropAlgorithm::GetPointKept(
            *table.predicateTable, nodeCount, modelPoint);
        if (isGpuKept != isCpuKept) {
            std::cerr << "Point-grid mismatch revision=" << revision
                      << " point=(" << modelPoint[0] << ',' << modelPoint[1]
                      << ',' << modelPoint[2] << ") display=(" << x << ',' << y
                      << ") pixel=(" << (pixel ? static_cast<int>(pixel[0]) : -1)
                      << ',' << (pixel ? static_cast<int>(pixel[1]) : -1)
                      << ',' << (pixel ? static_cast<int>(pixel[2]) : -1)
                      << ") cpu=" << isCpuKept << " gpu=" << isGpuKept << '\n';
            return false;
        }
    }
    return true;
}

bool StartPointGridTruthCase()
{
    const std::vector<CropPointFloat3Array> modelPoints = {
        { -1.5f, 0.0f, 0.0f }, { -0.75f, 0.0f, 0.0f },
        { -0.25f, 0.0f, 0.0f }, { 0.25f, 0.0f, 0.0f },
        { 0.75f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f }
    };
    vtkNew<vtkAppendPolyData> append;
    for (const auto& point : modelPoints) {
        vtkNew<vtkCubeSource> marker;
        marker->SetBounds(
            point[0] - 0.04f, point[0] + 0.04f,
            point[1] - 0.15f, point[1] + 0.15f,
            point[2] - 0.02f, point[2] + 0.02f);
        marker->Update();
        append->AddInputData(marker->GetOutput());
    }
    append->Update();
    auto polyData = vtkSmartPointer<vtkPolyData>::New();
    polyData->DeepCopy(append->GetOutput());

    auto strategy = std::make_shared<IsoSurfaceStrategy>();
    auto effect = std::make_shared<CropShaderEffect>();
    strategy->SetInputData(polyData);
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    renderer->SetBackground(0.0, 0.0, 0.0);
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(180, 80);
    renderWindow->AddRenderer(renderer);
    bool isPassed = strategy->SetRenderInputStamp(
        { &inputIdentity, 1 })
        && strategy->AttachRenderEffect(
            effect, RenderBindingUse::Current);
    strategy->AttachRenderer(renderer);

    const CropMatrixDouble16Array modelToWorld = {
        1.0, 0.0, 0.0, 0.35,
        0.0, 1.0, 0.0, -0.15,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    RenderParams params;
    params.modelMatrix = modelToWorld;
    strategy->SetVisualState(params, UpdateFlags::Transform);
    if (auto* actor = vtkActor::SafeDownCast(strategy->GetMainProp())) {
        renderer->RemoveAllViewProps();
        renderer->AddActor(actor);
        actor->GetProperty()->SetColor(1.0, 1.0, 1.0);
        actor->GetProperty()->SetAmbient(1.0);
        actor->GetProperty()->LightingOff();
    }
    auto* camera = renderer->GetActiveCamera();
    camera->ParallelProjectionOn();
    camera->SetPosition(0.35, -0.15, 10.0);
    camera->SetFocalPoint(0.35, -0.15, 0.0);
    camera->SetViewUp(0.0, 1.0, 0.0);
    camera->SetParallelScale(1.0);
    renderer->ResetCameraClippingRange();

    CropOpItem box;
    box.operationIndex = 1;
    box.geometryType = CropShape::Box;
    auto removeBox = box;
    removeBox.operationIndex = 2;
    removeBox.removalMode = CropRemovalMode::RemoveInside;
    CropOpItem plane;
    plane.operationIndex = 3;
    plane.geometryType = CropShape::Plane;
    plane.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    auto removePlane = plane;
    removePlane.operationIndex = 4;
    removePlane.removalMode = CropRemovalMode::RemoveInside;

    isPassed = GetPointGridMatched(
        { box }, 1, 1, strategy, effect, renderer, renderWindow,
        modelPoints, modelToWorld) && isPassed;
    isPassed = GetPointGridMatched(
        { removeBox }, 1, 2, strategy, effect, renderer, renderWindow,
        modelPoints, modelToWorld) && isPassed;
    isPassed = GetPointGridMatched(
        { plane }, 1, 3, strategy, effect, renderer, renderWindow,
        modelPoints, modelToWorld) && isPassed;
    isPassed = GetPointGridMatched(
        { removePlane }, 1, 4, strategy, effect, renderer, renderWindow,
        modelPoints, modelToWorld) && isPassed;
    isPassed = GetPointGridMatched(
        { box, plane }, 2, 5, strategy, effect, renderer, renderWindow,
        modelPoints, modelToWorld) && isPassed;

    std::vector<CropOpItem> largeOperations(513);
    for (std::size_t index = 0; index < largeOperations.size(); ++index) {
        auto& operation = largeOperations[index];
        operation.operationIndex = index + 10;
        operation.geometryType = CropShape::Plane;
        operation.planeCenterInInputModel = {
            index == 512 ? 0.0 : -100.0, 0.0, 0.0
        };
        operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    }
    isPassed = GetPointGridMatched(
        largeOperations,
        largeOperations.size(),
        6,
        strategy,
        effect,
        renderer,
        renderWindow,
        modelPoints,
        modelToWorld) && isPassed;
    renderWindow->Finalize();
    return SetExpect(
        isPassed,
        "PolyData crop shader pixels should exactly match the CPU oracle for box/plane/mixed/513 cases.");
}

}

int CropShaderPreviewSuite::GetFailCount() const
{
    int failureCount = StartTextureCapabilityCase() ? 0 : 1;
    failureCount += StartCachedProgramZeroCase() ? 0 : 1;
    failureCount += StartMapperCases() ? 0 : 1;
    failureCount += StartEffectLifeCase() ? 0 : 1;
    failureCount += StartEffectAtomicCase() ? 0 : 1;
    failureCount += StartSliceCoordinateCase() ? 0 : 1;
    failureCount += StartVolumeCoordinateCase() ? 0 : 1;
    failureCount += StartPixelTransactionCase() ? 0 : 1;
    failureCount += StartLargeTableCase() ? 0 : 1;
    failureCount += StartPointGridTruthCase() ? 0 : 1;
    return failureCount;
}
