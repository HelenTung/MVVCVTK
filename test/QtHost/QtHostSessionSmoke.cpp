#include "Host/VtkAppHostSession.h"
#include "Host/HostCoreServices.h"
#include "Host/HostCommandRouter.h"
#include "Host/HostRenderViewSet.h"
#include "Host/Types/HostRequestTypes.h"
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "App/Services/AppService.h"
#include "Data/DataManager.h"
#include "Render/StdRenderContext.h"
#include "VolumeStrategy.h"

#include <QApplication>
#include <QCoreApplication>
#include <QOpenGLWidget>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkCallbackCommand.h>
#include <vtkAutoInit.h>
#include <vtkAlgorithm.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkWindowToImageFilter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

namespace {

// 默认 true 路径只验证 Host 是否调用 Render；probe 隔离真实 OpenGL context 和平台 Timer。
class RenderProbeWindow final : public vtkGenericOpenGLRenderWindow {
public:
    static RenderProbeWindow* New();
    vtkTypeMacro(RenderProbeWindow, vtkGenericOpenGLRenderWindow);

    void Render() override
    {
        ++m_renderCount;
    }

    std::size_t GetRenderCount() const
    {
        return m_renderCount;
    }

    void* GetGenericWindowId() override
    {
        return this;
    }

protected:
    RenderProbeWindow()
    {
        this->Mapped = 1;
    }
    ~RenderProbeWindow() override = default;

private:
    std::size_t m_renderCount{0};
};

vtkStandardNewMacro(RenderProbeWindow);

class RenderProbeInteractor final : public vtkRenderWindowInteractor {
public:
    static RenderProbeInteractor* New();
    vtkTypeMacro(RenderProbeInteractor, vtkRenderWindowInteractor);

    void Initialize() override
    {
        this->Initialized = 1;
        this->Enabled = 1;
    }

    void Start() override
    {
        ++m_startCount;
    }

    std::size_t GetStartCount() const
    {
        return m_startCount;
    }

protected:
    RenderProbeInteractor() = default;
    ~RenderProbeInteractor() override = default;

    int InternalCreateTimer(
        int timerId, int, unsigned long) override
    {
        return timerId;
    }

    int InternalDestroyTimer(int) override
    {
        return 1;
    }

private:
    std::size_t m_startCount{0};
};

vtkStandardNewMacro(RenderProbeInteractor);

class PresetRaceData final : public RawVolumeDataManager {
public:
    bool SetState(
        std::shared_ptr<SharedInteractionState> state)
    {
        m_state = std::move(state);
        return !m_state.expired();
    }

    bool SetPresetRace()
    {
        m_readCount = 0;
        m_isRaceReady = true;
        return true;
    }

    ImageSnapshot GetImageSnapshot() const override
    {
        auto snapshot =
            RawVolumeDataManager::GetImageSnapshot();
        if (!m_isRaceReady) {
            return snapshot;
        }
        ++m_readCount;
        if (m_readCount == 2) {
            if (const auto state = m_state.lock()) {
                state->SetTFNodes({
                    { 0.0, 0.0, 0.75, 0.75, 0.75 },
                    { 1.0, 1.0, 0.75, 0.75, 0.75 }
                });
            }
        }
        return snapshot;
    }

private:
    std::weak_ptr<SharedInteractionState> m_state;
    mutable std::size_t m_readCount{0};
    mutable bool m_isRaceReady{false};
};

bool BuildPresetReturnTest()
{
    auto dataManager =
        std::make_shared<PresetRaceData>();
    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    if (!dataManager->SetState(state)) {
        return false;
    }

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->AllocateScalars(VTK_FLOAT, 1);
    for (vtkIdType index = 0;
        index < image->GetNumberOfPoints();
        ++index) {
        static_cast<float*>(
            image->GetScalarPointer())[index] =
                static_cast<float>(index);
    }
    image->Modified();
    bool hasPending = false;
    if (!dataManager->SetImageSnapshot(image)
        || !dataManager->SetCurrentFromPending(hasPending)
        || !hasPending) {
        return false;
    }

    HostCoreServices core;
    core.sharedDataMgr = dataManager;
    core.sharedStateBroadcaster = broadcaster;
    core.sharedState = state;
    auto renderWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);
    HostRenderViewConfig view;
    view.id = "preset-router";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode =
        HostRenderMode::IsoSurface;
    view.renderWindow = renderWindow;
    HostRenderViewSet views;
    const std::vector<HostRenderViewConfig> configs{
        view
    };
    if (!views.Build(core, configs)
        || !dataManager->SetPresetRace()) {
        return false;
    }
    HostCommandRouter router(core, views);
    HostViewSetRequest request;
    request.targetView.viewId = "preset-router";
    request.transferPreset =
        HostTransferPreset::Percentile;
    const bool isPresetAccepted =
        router.Dispatch(std::move(request));
    std::cout
        << "DIAG_PRESET: accepted=" << isPresetAccepted
        << " final_preset="
        << static_cast<int>(state->GetTransferPreset())
        << '\n';
    return !isPresetAccepted
        && state->GetTransferPreset()
            == TransferPreset::Manual;
}

bool BuildRenderSourceTest()
{
    HostCoreServices core;
    core.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);

    auto setWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto setInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    setWindow->SetInteractor(setInteractor);
    setInteractor->SetRenderWindow(setWindow);
    HostRenderViewConfig setView;
    setView.id = "view-set-source";
    setView.role = HostRenderViewRole::Primary3D;
    setView.renderWindow = setWindow;
    HostRenderViewSet viewSet;
    const std::vector<HostRenderViewConfig> configs{
        setView
    };
    if (!viewSet.Build(core, configs)
        || setWindow->GetRenderCount() != 0) {
        return false;
    }
    viewSet.SendRenderAll();
    const bool hasViewSetRender =
        setWindow->GetRenderCount() == 1;

    auto startWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto startInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    startWindow->SetInteractor(startInteractor);
    startInteractor->SetRenderWindow(startWindow);
    StdRenderContext context;
    context.SetRenderWindow(startWindow);
    context.Start();
    const bool hasStartRender =
        startWindow->GetRenderCount() == 1
        && startInteractor->GetStartCount() == 1;

    auto sessionWindow =
        vtkSmartPointer<RenderProbeWindow>::New();
    auto sessionInteractor =
        vtkSmartPointer<RenderProbeInteractor>::New();
    sessionWindow->SetInteractor(sessionInteractor);
    sessionInteractor->SetRenderWindow(sessionWindow);
    HostRenderViewConfig sessionView;
    sessionView.id = "session-start-source";
    sessionView.role = HostRenderViewRole::Primary3D;
    sessionView.renderWindow = sessionWindow;
    HostSessionConfig sessionConfig;
    sessionConfig.renderViews.push_back(
        std::move(sessionView));
    VtkAppHostSession session(
        std::move(sessionConfig));
    const bool hasSessionStart =
        session.Start()
        && sessionWindow->GetRenderCount() == 2
        && sessionInteractor->GetStartCount() == 1;

    std::cout
        << "DIAG_RENDER_SOURCE: send_all="
        << setWindow->GetRenderCount()
        << " std_start=" << startWindow->GetRenderCount()
        << " session_start="
        << sessionWindow->GetRenderCount()
        << " interactor_start="
        << sessionInteractor->GetStartCount()
        << '\n';
    return hasViewSetRender
        && hasStartRender
        && hasSessionStart;
}

bool BuildDefaultRenderTest()
{
    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);
    const std::size_t renderCount = renderWindow->GetRenderCount();

    HostRenderViewConfig view;
    view.id = "render-probe";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));
    if (!session.BuildSession()
        || renderWindow->GetRenderCount() != renderCount) {
        return false;
    }
    interactor->Initialize();
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "render-probe", false, HostRenderViewRole::Primary3D
    };
    if (!session.AttachTimer(timer)) {
        return false;
    }

    const std::vector<HostTransferNode> nodes{
        { 0.0, 0.0, 0.75, 0.75, 0.75 },
        { 1.0, 1.0, 0.75, 0.75, 0.75 }
    };
    HostViewSetRequest firstRequest;
    firstRequest.targetView.viewId = "render-probe";
    firstRequest.mode = HostRenderMode::Volume;
    firstRequest.transferNodes = nodes;
    if (!session.SendRequest(std::move(firstRequest))) {
        return false;
    }
    interactor->InvokeEvent(vtkCommand::TimerEvent);
    if (renderWindow->GetRenderCount() != renderCount + 1) {
        return false;
    }

    HostViewSetRequest sameRequest;
    sameRequest.targetView.viewId = "render-probe";
    sameRequest.transferNodes = nodes;
    if (!session.SendRequest(std::move(sameRequest))) {
        return false;
    }
    const std::size_t changedRenderCount =
        renderWindow->GetRenderCount();
    interactor->InvokeEvent(vtkCommand::TimerEvent);
    interactor->InvokeEvent(vtkCommand::TimerEvent);
    return renderWindow->GetRenderCount() == changedRenderCount;
}

bool BuildZoomRenderTest()
{
    auto renderWindow = vtkSmartPointer<RenderProbeWindow>::New();
    auto interactor = vtkSmartPointer<RenderProbeInteractor>::New();
    renderWindow->SetInteractor(interactor);
    interactor->SetRenderWindow(renderWindow);

    HostRenderViewConfig view;
    view.id = "zoom-probe";
    view.role = HostRenderViewRole::TopDownSlice;
    view.window.viewInit.viewMode = HostRenderMode::SliceTopDown;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));
    if (!session.BuildSession()) {
        return false;
    }
    interactor->Initialize();
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = {
        "zoom-probe", false, HostRenderViewRole::TopDownSlice
    };
    if (!session.AttachTimer(timer)) {
        return false;
    }
    const auto* endpoint =
        session.GetRenderViewEndpoint("zoom-probe");
    if (!endpoint || !endpoint->renderer || !endpoint->interactor
        || !endpoint->renderer->GetActiveCamera()) {
        return false;
    }

    // 清空 BuildSession 留下的历史 dirty，再单独测右键缩放的 render 来源。
    endpoint->interactor->InvokeEvent(vtkCommand::TimerEvent);
    const std::size_t renderCount = renderWindow->GetRenderCount();
    auto* camera = endpoint->renderer->GetActiveCamera();
    const double startScale = camera->GetParallelScale();
    endpoint->interactor->SetEventPosition(100, 100);
    endpoint->interactor->InvokeEvent(
        vtkCommand::RightButtonPressEvent);
    constexpr int moveCount = 100;
    for (int index = 1; index <= moveCount; ++index) {
        endpoint->interactor->SetEventPosition(100, 100 + index);
        endpoint->interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    }
    endpoint->interactor->InvokeEvent(
        vtkCommand::RightButtonReleaseEvent);

    const double expectedScale =
        startScale * std::pow(1.01, moveCount);
    const bool isCameraUpdated =
        std::abs(camera->GetParallelScale() - expectedScale)
            < 1e-9;
    const std::size_t directRenderCount =
        renderWindow->GetRenderCount() - renderCount;
    const bool hasNoDirectRender =
        directRenderCount == 0;
    endpoint->interactor->InvokeEvent(vtkCommand::TimerEvent);
    const bool hasOneTimerRender =
        renderWindow->GetRenderCount() == renderCount + 1;
    endpoint->interactor->InvokeEvent(vtkCommand::TimerEvent);
    std::cout
        << "DIAG: zoom moves=" << moveCount
        << " direct_renders=" << directRenderCount
        << " camera_ok=" << isCameraUpdated
        << " timer_once=" << hasOneTimerRender
        << '\n';
    return isCameraUpdated
        && hasNoDirectRender
        && hasOneTimerRender
        && renderWindow->GetRenderCount() == renderCount + 1;
}

class SessionFixture final {
public:
    bool BuildWindow()
    {
        if (m_widget || m_renderWindow) {
            return false;
        }

        m_widget = std::make_unique<QVTKOpenGLNativeWidget>();
        m_widget->setWindowTitle(QStringLiteral("MVVCVTK Qt Host Session Smoke"));
        m_renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        if (!AttachRenderStats()) {
            return false;
        }
        m_errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
        m_errorCallback->SetClientData(this);
        m_errorCallback->SetCallback(&SessionFixture::OnVtkError);
        m_errorTag = m_renderWindow->AddObserver(
            vtkCommand::ErrorEvent, m_errorCallback);
        if (m_errorTag == 0) {
            DetachRenderStats();
            return false;
        }
        m_widget->setRenderWindow(m_renderWindow);
        m_widget->resize(640, 480);
        m_widget->show();
        return true;
    }

    bool StartHost()
    {
        if (!m_widget || !m_renderWindow || m_session) {
            return false;
        }

        HostRenderViewConfig view;
        view.id = "primary-3d";
        view.role = HostRenderViewRole::Primary3D;
        view.window.title = "Qt Host Session Smoke";
        view.window.width = 640;
        view.window.height = 480;
        view.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
        view.window.viewInit.background = { 0.08, 0.12, 0.16 };
        view.window.viewInit.hasBackground = true;
        view.renderWindow = m_renderWindow;

        HostSessionConfig config;
        config.renderViews.push_back(std::move(view));

        m_session = std::make_unique<VtkAppHostSession>(std::move(config));
        const std::size_t renderStartCount = m_renderStartCount;
        const bool isBuilt = m_session->BuildSession();
        HostTimerConfig timer;
        timer.isTimerEnabled = true;
        timer.targetView = { "primary-3d", false, HostRenderViewRole::Primary3D };
        const bool isTimerAttached = m_session->AttachTimer(timer);
        const bool hasInitialRender = m_renderStartCount != renderStartCount;

        const auto& endpoints = m_session->GetRenderViewEndpoints();
        const auto* endpoint = m_session->GetRenderViewEndpoint("primary-3d");
        const auto* primaryEndpoint = m_session->GetPrimaryEndpoint();
        if (m_vtkErrorCount > 0) {
            std::cerr
                << "FAIL: VTK ErrorEvent during BuildSession"
                << " count=" << m_vtkErrorCount;
            if (!m_vtkErrorText.empty()) {
                std::cerr << " message=" << m_vtkErrorText;
            }
            std::cerr << '\n';
        }
        const bool hasRenderStats =
            m_renderStartCount == m_renderEndCount
            && m_renderStarts.empty()
            && !m_renderTimesMs.empty();
        std::cout
            << "DIAG: render samples=" << m_renderTimesMs.size()
            << " p50_ms=" << GetRenderTimeMs(0.50)
            << " p95_ms=" << GetRenderTimeMs(0.95)
            << " max_ms=" << GetRenderTimeMs(1.00)
            << '\n';
        return isBuilt && isTimerAttached && !hasInitialRender
            && m_vtkErrorCount == 0
            && hasRenderStats
            && endpoints.size() == 1
            && endpoint != nullptr
            && primaryEndpoint == endpoint
            && endpoint->role == HostRenderViewRole::Primary3D
            && endpoint->renderer != nullptr
            && endpoint->interactor != nullptr
            && endpoint->renderWindow == m_renderWindow.Get()
            && m_widget->renderWindow() == m_renderWindow.Get();
    }

    bool GetBlendVisualValid()
    {
        if (!m_session) {
            return false;
        }
        const auto* endpoint =
            m_session->GetRenderViewEndpoint("primary-3d");
        if (!endpoint || !endpoint->renderer
            || !endpoint->renderWindow) {
            return false;
        }

        constexpr int sideLength = 32;
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(
            sideLength, sideLength, sideLength);
        image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* scalars =
            static_cast<unsigned char*>(image->GetScalarPointer());
        if (!scalars) {
            return false;
        }
        for (int z = 0; z < sideLength; ++z) {
            for (int y = 0; y < sideLength; ++y) {
                for (int x = 0; x < sideLength; ++x) {
                    const double dx =
                        x - (sideLength - 1) * 0.5;
                    const double dy =
                        y - (sideLength - 1) * 0.5;
                    const double dz =
                        z - (sideLength - 1) * 0.5;
                    const double radius =
                        std::sqrt(dx * dx + dy * dy + dz * dz);
                    const vtkIdType index =
                        x + sideLength
                            * (y + sideLength * z);
                    scalars[index] = radius < 13.0
                        ? static_cast<unsigned char>(
                            std::max(1.0, 255.0 - radius * 16.0))
                        : 0;
                }
            }
        }
        image->Modified();

        auto mapper =
            vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
        mapper->SetInputData(image);
        mapper->AutoAdjustSampleDistancesOff();
        mapper->SetSampleDistance(0.5);
        mapper->SetImageSampleDistance(1.0);
        mapper->UseJitteringOff();
        auto color =
            vtkSmartPointer<vtkColorTransferFunction>::New();
        color->AddRGBPoint(0.0, 0.0, 0.0, 0.0);
        color->AddRGBPoint(255.0, 0.75, 0.75, 0.75);
        auto opacity =
            vtkSmartPointer<vtkPiecewiseFunction>::New();
        opacity->AddPoint(0.0, 0.0);
        opacity->AddPoint(255.0, 0.85);
        auto property =
            vtkSmartPointer<vtkVolumeProperty>::New();
        property->SetColor(color);
        property->SetScalarOpacity(opacity);
        property->ShadeOff();
        property->SetInterpolationTypeToLinear();
        auto volume = vtkSmartPointer<vtkVolume>::New();
        volume->SetMapper(mapper);
        volume->SetProperty(property);

        vtkSmartPointer<vtkRenderer> renderer =
            endpoint->renderer;
        vtkNew<vtkCamera> cameraSnapshot;
        cameraSnapshot->DeepCopy(
            renderer->GetActiveCamera());
        const int multiSamples =
            endpoint->renderWindow->GetMultiSamples();
        renderer->AddVolume(volume);
        renderer->ResetCamera();
        endpoint->renderWindow->SetMultiSamples(0);
        constexpr int warmupCount = 3;
        for (int index = 0; index < warmupCount; ++index) {
            endpoint->renderWindow->Render();
        }

        const auto getPixels = [&]() {
            endpoint->renderWindow->Render();
            vtkNew<vtkWindowToImageFilter> capture;
            capture->SetInput(endpoint->renderWindow);
            capture->SetInputBufferTypeToRGB();
            capture->ReadFrontBufferOff();
            capture->ShouldRerenderOff();
            capture->Update();
            auto* output = capture->GetOutput();
            auto* pixels = output
                ? static_cast<unsigned char*>(
                    output->GetScalarPointer())
                : nullptr;
            const vtkIdType valueCount = output
                ? output->GetNumberOfPoints()
                    * output->GetNumberOfScalarComponents()
                : 0;
            return pixels && valueCount > 0
                ? std::vector<unsigned char>(
                    pixels, pixels + valueCount)
                : std::vector<unsigned char>{};
        };

        const int defaultBlend = mapper->GetBlendMode();
        const vtkMTimeType defaultMTime = mapper->GetMTime();
        const auto defaultPixels = getPixels();
        mapper->SetBlendModeToComposite();
        const vtkMTimeType explicitMTime = mapper->GetMTime();
        const auto explicitPixels = getPixels();
        mapper->SetBlendModeToComposite();
        const vtkMTimeType repeatMTime = mapper->GetMTime();
        const auto repeatPixels = getPixels();
        renderer->RemoveVolume(volume);
        renderer->GetActiveCamera()->DeepCopy(
            cameraSnapshot);
        endpoint->renderWindow->SetMultiSamples(
            multiSamples);

        const unsigned char maxSignal =
            defaultPixels.empty()
            ? 0
            : *std::max_element(
                defaultPixels.begin(),
                defaultPixels.end());
        const auto getMaxDiff = [](
            const std::vector<unsigned char>& left,
            const std::vector<unsigned char>& right) {
            if (left.size() != right.size()) {
                return 255;
            }
            int maxDiff = 0;
            for (std::size_t index = 0;
                index < left.size();
                ++index) {
                maxDiff = std::max(
                    maxDiff,
                    std::abs(
                        static_cast<int>(left[index])
                        - static_cast<int>(right[index])));
            }
            return maxDiff;
        };
        const int explicitDiff =
            getMaxDiff(defaultPixels, explicitPixels);
        const int repeatDiff =
            getMaxDiff(explicitPixels, repeatPixels);
        const bool hasSignal = maxSignal > 64;
        const bool isVisualEqual =
            !defaultPixels.empty()
            && explicitDiff <= 1
            && repeatDiff <= 1;
        const bool isBlendStable =
            defaultBlend == vtkVolumeMapper::COMPOSITE_BLEND
            && mapper->GetBlendMode()
                == vtkVolumeMapper::COMPOSITE_BLEND
            && defaultMTime == explicitMTime
            && explicitMTime == repeatMTime;
        std::cout
            << "DIAG_BLEND: default=" << defaultBlend
            << " pixels=" << defaultPixels.size()
            << " signal=" << hasSignal
            << " explicit_max_diff=" << explicitDiff
            << " repeat_max_diff=" << repeatDiff
            << " mtime_stable=" << isBlendStable
            << '\n';
        return hasSignal && isVisualEqual && isBlendStable;
    }

    bool StopHost()
    {
        m_session.reset();
        if (m_widget) {
            m_widget->setRenderWindow(
                static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        }
        if (m_renderWindow && m_errorTag != 0) {
            m_renderWindow->RemoveObserver(m_errorTag);
        }
        m_errorTag = 0;
        if (m_errorCallback) {
            m_errorCallback->SetClientData(nullptr);
        }
        DetachRenderStats();
        m_renderWindow = nullptr;
        return true;
    }

    bool StartVolumeBench(int sampleCount)
    {
        if (!m_session || sampleCount < 1) {
            return false;
        }
        const auto* endpoint =
            m_session->GetRenderViewEndpoint("primary-3d");
        if (!endpoint || !endpoint->renderer
            || !endpoint->renderWindow) {
            return false;
        }

        auto image = vtkSmartPointer<vtkImageData>::New();
        constexpr int sideLength = 64;
        image->SetDimensions(sideLength, sideLength, sideLength);
        image->SetSpacing(1.0, 1.0, 1.0);
        image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* scalars =
            static_cast<unsigned char*>(image->GetScalarPointer());
        if (!scalars) {
            return false;
        }
        const vtkIdType voxelCount = image->GetNumberOfPoints();
        for (vtkIdType index = 0; index < voxelCount; ++index) {
            scalars[index] = static_cast<unsigned char>(index % 256);
        }
        image->Modified();

        HostReloadRequest reload;
        reload.voxels.resize(
            static_cast<std::size_t>(voxelCount));
        for (vtkIdType index = 0;
            index < voxelCount;
            ++index) {
            reload.voxels[
                static_cast<std::size_t>(index)] =
                    static_cast<float>(scalars[index]);
        }
        reload.geometry.dimensions = {
            sideLength, sideLength, sideLength
        };
        reload.geometry.spacing = {
            1.0f, 1.0f, 1.0f
        };
        reload.geometry.origin = {
            0.0f, 0.0f, 0.0f
        };
        bool isReloadComplete = false;
        bool isReloadSucceeded = false;
        if (!m_session->SendRequest(
                std::move(reload),
                [&isReloadComplete,
                    &isReloadSucceeded](bool isSucceeded) {
                    isReloadSucceeded = isSucceeded;
                    isReloadComplete = true;
                })) {
            return false;
        }
        constexpr int reloadPollCount = 1000;
        for (int poll = 0;
            !isReloadComplete && poll < reloadPollCount;
            ++poll) {
            endpoint->interactor->InvokeEvent(
                vtkCommand::TimerEvent);
            endpoint->renderWindow->Render();
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        if (!isReloadComplete || !isReloadSucceeded) {
            return false;
        }

        VolumeStrategy strategy;
        strategy.SetInputData(image);
        RenderParams params;
        params.scalarRange[0] = 0.0;
        params.scalarRange[1] = 255.0;
        const std::vector<TFNode> currentNodes{
            { 0.00, 0.0, 0.00, 0.00, 0.00 },
            { 0.35, 0.0, 0.75, 0.75, 0.75 },
            { 0.60, 0.6, 0.85, 0.85, 0.85 },
            { 1.00, 1.0, 0.95, 0.95, 0.95 }
        };
        const std::vector<TFNode> grayNodes{
            { 0.0, 0.0, 0.75, 0.75, 0.75 },
            { 0.5, 0.1, 0.75, 0.75, 0.75 },
            { 1.0, 0.8, 0.75, 0.75, 0.75 }
        };
        params.tfNodes = grayNodes;
        strategy.SetVisualState(params, UpdateFlags::TF);
        vtkSmartPointer<vtkRenderer> renderer = endpoint->renderer;
        strategy.AttachRenderer(renderer);
        renderer->ResetCamera();
        auto* volume = vtkVolume::SafeDownCast(
            strategy.GetMainProp());
        auto* mapper = volume
            ? vtkGPUVolumeRayCastMapper::SafeDownCast(
                volume->GetMapper())
            : nullptr;
        if (!mapper || !volume->GetProperty()) {
            strategy.DetachRenderer(renderer);
            return false;
        }

        const auto startSamples =
            [&](const char* caseName,
                bool isMaskExpected = false,
                bool isDenoiseExpected = false) {
            if (!ResetRenderStats()) {
                return false;
            }
            constexpr int warmupCount = 3;
            for (int index = 0; index < warmupCount; ++index) {
                endpoint->renderWindow->Render();
            }
            const bool isWarmupValid =
                m_renderTimesMs.size()
                    == static_cast<std::size_t>(warmupCount)
                && m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty();
            const double warmupMaxMs = GetRenderTimeMs(1.00);
            if (!ResetRenderStats()) {
                return false;
            }
            for (int index = 0; index < sampleCount; ++index) {
                endpoint->renderWindow->Render();
            }

            const bool isSampleValid =
                m_renderTimesMs.size()
                    == static_cast<std::size_t>(sampleCount)
                && m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty();
            auto* property = volume->GetProperty();
            auto* color = property
                ? property->GetRGBTransferFunction()
                : nullptr;
            auto* opacity = property
                ? property->GetScalarOpacity()
                : nullptr;
            const bool hasCustomGradient =
                property
                && property->HasGradientOpacity() != 0;
            auto* gradient = property
                ? property->GetGradientOpacity()
                : nullptr;
            double colorRange[2] = {};
            double opacityRange[2] = {};
            double gradientRange[2] = {};
            if (color) {
                color->GetRange(colorRange);
            }
            if (opacity) {
                opacity->GetRange(opacityRange);
            }
            if (gradient) {
                gradient->GetRange(gradientRange);
            }
            const auto& material = params.material;
            const bool hasGradient =
                !params.gradientOpacity.empty();
            bool areTfNodesValid =
                color
                && opacity
                && color->GetSize()
                    == static_cast<int>(params.tfNodes.size())
                && opacity->GetSize()
                    == static_cast<int>(params.tfNodes.size());
            for (std::size_t index = 0;
                areTfNodesValid
                    && index < params.tfNodes.size();
                ++index) {
                double colorNode[6] = {};
                double opacityNode[4] = {};
                color->GetNodeValue(
                    static_cast<int>(index), colorNode);
                opacity->GetNodeValue(
                    static_cast<int>(index), opacityNode);
                const auto& expected = params.tfNodes[index];
                const double scalar =
                    params.scalarRange[0]
                    + expected.position
                        * (params.scalarRange[1]
                            - params.scalarRange[0]);
                areTfNodesValid =
                    std::abs(colorNode[0] - scalar) < 1e-12
                    && std::abs(colorNode[1] - expected.r) < 1e-12
                    && std::abs(colorNode[2] - expected.g) < 1e-12
                    && std::abs(colorNode[3] - expected.b) < 1e-12
                    && std::abs(opacityNode[0] - scalar) < 1e-12
                    && std::abs(
                        opacityNode[1]
                            - expected.opacity
                                * material.opacity) < 1e-12;
            }
            bool areGradientNodesValid =
                !hasGradient
                || (gradient
                    && gradient->GetSize()
                        == static_cast<int>(
                            params.gradientOpacity.size()));
            for (const auto& node : params.gradientOpacity) {
                areGradientNodesValid =
                    areGradientNodesValid
                    && gradient
                    && std::abs(
                        gradient->GetValue(node.gradient)
                            - node.opacity) < 1e-12;
            }
            vtkImageData* maskInput =
                mapper ? mapper->GetMaskInput() : nullptr;
            double maskRange[2] = {};
            if (maskInput) {
                maskInput->GetScalarRange(maskRange);
            }
            const auto inputConnection =
                mapper ? mapper->GetInputConnection(0, 0) : nullptr;
            vtkAlgorithm* inputProducer =
                inputConnection ? inputConnection->GetProducer()
                    : nullptr;
            const auto producerInputConnection =
                inputProducer
                ? inputProducer->GetInputConnection(0, 0)
                : nullptr;
            vtkAlgorithm* producerInput =
                producerInputConnection
                ? producerInputConnection->GetProducer()
                : nullptr;
            const bool isDenoiseApplied =
                producerInput
                && producerInput->IsA(
                    "vtkImageAnisotropicDiffusion3D");
            const bool isMaskApplied =
                maskInput
                && maskInput->GetScalarType()
                    == VTK_UNSIGNED_CHAR
                && maskInput->GetNumberOfScalarComponents() == 1
                && maskRange[0] <= 1.0
                && maskRange[1] >= 254.0;
            vtkNew<vtkWindowToImageFilter> visualCapture;
            visualCapture->SetInput(endpoint->renderWindow);
            visualCapture->SetInputBufferTypeToRGB();
            visualCapture->ReadFrontBufferOff();
            visualCapture->ShouldRerenderOff();
            visualCapture->Update();
            auto* visualOutput = visualCapture->GetOutput();
            auto* visualPixels = visualOutput
                ? static_cast<unsigned char*>(
                    visualOutput->GetScalarPointer())
                : nullptr;
            const vtkIdType visualValueCount = visualOutput
                ? visualOutput->GetNumberOfPoints()
                    * visualOutput->GetNumberOfScalarComponents()
                : 0;
            unsigned char visualSignal = 0;
            std::uint64_t visualHash = 1469598103934665603ULL;
            if (visualPixels && visualValueCount > 0) {
                for (vtkIdType index = 0;
                    index < visualValueCount;
                    ++index) {
                    visualSignal = std::max(
                        visualSignal, visualPixels[index]);
                    visualHash ^= visualPixels[index];
                    visualHash *= 1099511628211ULL;
                }
            }
            const bool isVisualValid =
                visualPixels && visualValueCount > 0
                && visualSignal > 0;
            const bool isStateValid =
                property
                && color
                && opacity
                && areTfNodesValid
                && areGradientNodesValid
                && std::abs(colorRange[0]
                    - params.scalarRange[0]) < 1e-12
                && std::abs(colorRange[1]
                    - params.scalarRange[1]) < 1e-12
                && std::abs(opacityRange[0]
                    - params.scalarRange[0]) < 1e-12
                && std::abs(opacityRange[1]
                    - params.scalarRange[1]) < 1e-12
                && property->GetShade()
                    == static_cast<int>(material.isShadeOn)
                && std::abs(
                    property->GetAmbient()
                        - material.ambient) < 1e-12
                && std::abs(
                    property->GetDiffuse()
                        - material.diffuse) < 1e-12
                && std::abs(
                    property->GetSpecular()
                        - material.specular) < 1e-12
                && std::abs(
                    property->GetSpecularPower()
                        - material.specularPower) < 1e-12
                && hasCustomGradient == hasGradient
                && (!hasGradient
                    || (gradient
                        && gradient->GetSize()
                            == static_cast<int>(
                                params.gradientOpacity.size())
                        && std::abs(
                            gradientRange[0]
                                - params.gradientOpacity.front().gradient)
                            < 1e-12
                        && std::abs(
                            gradientRange[1]
                                - params.gradientOpacity.back().gradient)
                            < 1e-12))
                && mapper->GetGradientOpacityRangeType()
                    == vtkGPUVolumeRayCastMapper::SCALAR
                && std::abs(image->GetScalarRange()[0]) < 1e-12
                && std::abs(
                    image->GetScalarRange()[1] - 255.0) < 1e-12
                && std::abs(image->GetSpacing()[0] - 1.0) < 1e-12
                && std::abs(image->GetSpacing()[1] - 1.0) < 1e-12
                && std::abs(image->GetSpacing()[2] - 1.0) < 1e-12
                && std::abs(
                    mapper->GetImageSampleDistance() - 1.0) < 1e-12
                && std::abs(
                    mapper->GetMinimumImageSampleDistance() - 1.0)
                    < 1e-12
                && std::abs(
                    mapper->GetMaximumImageSampleDistance() - 1.0)
                    < 1e-12
                && mapper->GetUseJittering()
                    == static_cast<int>(
                        params.volumeQuality.isJitterOn);
            const bool isAuxiliaryStateValid =
                isMaskApplied == isMaskExpected
                && isDenoiseApplied == isDenoiseExpected
                && (!isMaskExpected || mapper->GetMaskInput())
                && (!isDenoiseExpected || producerInput);
            std::cout
                << "BENCH: case=" << caseName
                << " volume_dims=" << sideLength
                << "^3 samples=" << m_renderTimesMs.size()
                << " warmup_max_ms=" << warmupMaxMs
                << " p50_ms=" << GetRenderTimeMs(0.50)
                << " p95_ms=" << GetRenderTimeMs(0.95)
                << " max_ms=" << GetRenderTimeMs(1.00)
                << " auto="
                << mapper->GetAutoAdjustSampleDistances()
                << " ray_step=" << mapper->GetSampleDistance()
                << " image_step="
                << mapper->GetImageSampleDistance()
                << " image_min="
                << mapper->GetMinimumImageSampleDistance()
                << " image_max="
                << mapper->GetMaximumImageSampleDistance()
                << " jitter=" << mapper->GetUseJittering()
                << " max_dim=" << params.volumeQuality.maxDimension
                << " shade=" << property->GetShade()
                << " ambient=" << property->GetAmbient()
                << " diffuse=" << property->GetDiffuse()
                << " specular=" << property->GetSpecular()
                << " specular_power="
                << property->GetSpecularPower()
                << " opacity=" << material.opacity
                << " gradient="
                << hasCustomGradient
                << " gradient_type="
                << mapper->GetGradientOpacityRangeType()
                << " gradient_size="
                << (gradient ? gradient->GetSize() : 0)
                << " gradient_range="
                << gradientRange[0] << ',' << gradientRange[1]
                << " mask_applied=" << isMaskApplied
                << " mask_range="
                << maskRange[0] << ',' << maskRange[1]
                << " denoise_applied=" << isDenoiseApplied
                << " visual_pixels=" << visualValueCount
                << " visual_signal="
                << static_cast<int>(visualSignal)
                << " visual_hash=" << visualHash
                << " scalar_range="
                << image->GetScalarRange()[0] << ','
                << image->GetScalarRange()[1]
                << " spacing="
                << image->GetSpacing()[0] << ','
                << image->GetSpacing()[1] << ','
                << image->GetSpacing()[2]
                << " ctf_size=" << color->GetSize()
                << " ctf_range="
                << colorRange[0] << ',' << colorRange[1]
                << " otf_size=" << opacity->GetSize()
                << " otf_range="
                << opacityRange[0] << ',' << opacityRange[1]
                << " props="
                << renderer->GetViewProps()->GetNumberOfItems()
                << " state_ok="
                << (isStateValid
                    && isAuxiliaryStateValid
                    && isVisualValid)
                << '\n';
            return isWarmupValid
                && isSampleValid
                && isStateValid
                && isAuxiliaryStateValid
                && isVisualValid;
        };

        // 所有场景共用同一真实 Qt/OpenGL 上下文、相机和体数据，以减少平台上下文噪声；
        // 每次计时前完整恢复策略基线，使 case 只保留名称所指的单一变量。
        const auto setBaseState = [&]() {
            strategy.SetInputMask(nullptr);
            params.tfNodes = grayNodes;
            params.material = {
                0.1, 0.7, 0.2, 10.0, 1.0, false
            };
            params.gradientOpacity.clear();
            params.volumeQuality = {
                VolumeQuality::Quality, 766, 1.0, true
            };
            params.isDenoiseOn = false;
            strategy.SetVisualState(
                params,
                UpdateFlags::TF
                    | UpdateFlags::Material
                    | UpdateFlags::GradientOpacity
                    | UpdateFlags::Quality
                    | UpdateFlags::Denoise);
            mapper->SetAutoAdjustSampleDistances(false);
        };

        setBaseState();
        params.tfNodes = currentNodes;
        strategy.SetVisualState(params, UpdateFlags::TF);
        bool areSamplesValid =
            startSamples("A_current_tf");
        setBaseState();
        areSamplesValid =
            startSamples("B_gray_tf") && areSamplesValid;

        setBaseState();
        params.material.isShadeOn = true;
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("C_gray_shade") && areSamplesValid;

        setBaseState();
        params.gradientOpacity = {
            { 0.0, 0.0 }, { 32.0, 0.3 }, { 255.0, 1.0 }
        };
        strategy.SetVisualState(
            params,
            UpdateFlags::GradientOpacity);
        areSamplesValid =
            startSamples("D_gray_gradient")
                && areSamplesValid;

        setBaseState();
        params.material.isShadeOn = true;
        params.gradientOpacity = {
            { 0.0, 0.0 }, { 32.0, 0.3 }, { 255.0, 1.0 }
        };
        strategy.SetVisualState(
            params,
            UpdateFlags::Material
                | UpdateFlags::GradientOpacity);
        areSamplesValid =
            startSamples("E_gray_shade_gradient")
                && areSamplesValid;

        setBaseState();
        mapper->SetAutoAdjustSampleDistances(true);
        areSamplesValid =
            startSamples("F_gray_auto") && areSamplesValid;

        setBaseState();
        params.volumeQuality = {
            VolumeQuality::Custom, 766, 0.5, true
        };
        strategy.SetVisualState(
            params, UpdateFlags::Quality);
        areSamplesValid =
            startSamples("G0_gray_ray_half")
                && areSamplesValid;

        setBaseState();
        params.volumeQuality = {
            VolumeQuality::Custom, 766, 1.0, true
        };
        strategy.SetVisualState(
            params, UpdateFlags::Quality);
        areSamplesValid =
            startSamples("G1_gray_ray_full")
                && areSamplesValid;

        setBaseState();
        params.material = {
            0.25, 0.65, 0.10, 8.0, 0.80, false
        };
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("M_soft") && areSamplesValid;

        setBaseState();
        params.material = {
            0.10, 0.85, 0.25, 20.0, 1.0, false
        };
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("M_dense") && areSamplesValid;

        setBaseState();
        params.material = {
            0.08, 0.65, 0.65, 40.0, 1.0, true
        };
        strategy.SetVisualState(
            params, UpdateFlags::Material);
        areSamplesValid =
            startSamples("M_glossy") && areSamplesValid;

        setBaseState();
        auto mask = vtkSmartPointer<vtkImageData>::New();
        mask->CopyStructure(image);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* maskScalars =
            static_cast<unsigned char*>(mask->GetScalarPointer());
        if (!maskScalars) {
            strategy.DetachRenderer(renderer);
            return false;
        }
        for (vtkIdType index = 0; index < voxelCount; ++index) {
            maskScalars[index] = index % 3 == 0 ? 0 : 255;
        }
        mask->Modified();
        const auto maskStart = std::chrono::steady_clock::now();
        strategy.SetInputMask(mask);
        const double maskBuildMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - maskStart).count();
        std::cout
            << "BENCH_BUILD: case=mask build_ms="
            << maskBuildMs << '\n';
        areSamplesValid =
            startSamples("mask", true, false) && areSamplesValid;

        setBaseState();
        params.isDenoiseOn = true;
        const auto denoiseStart =
            std::chrono::steady_clock::now();
        strategy.SetVisualState(
            params, UpdateFlags::Denoise);
        const double denoiseBuildMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now()
                    - denoiseStart).count();
        std::cout
            << "BENCH_BUILD: case=denoise build_ms="
            << denoiseBuildMs << '\n';
        areSamplesValid =
            startSamples("denoise", false, true) && areSamplesValid;

        const auto setSliceMode = [&]() {
            HostViewSetRequest request;
            request.targetView.viewId = "primary-3d";
            request.mode = HostRenderMode::SliceTopDown;
            if (!m_session->SendRequest(std::move(request))) {
                return false;
            }
            endpoint->interactor->InvokeEvent(
                vtkCommand::TimerEvent);
            return std::abs(
                endpoint->renderWindow->GetDesiredUpdateRate()
                    - 0.001) < 1e-12;
        };
        const auto samplePhase =
            [&](const char* caseName,
                const char* phaseName,
                double expectedRate,
                bool expectedAuto,
                double expectedRay,
                bool isWarmupNeeded) {
            if (!ResetRenderStats()) {
                return false;
            }
            if (isWarmupNeeded) {
                constexpr int warmupCount = 3;
                for (int index = 0;
                    index < warmupCount;
                    ++index) {
                    endpoint->renderWindow->Render();
                }
                const bool isWarmupValid =
                    m_renderTimesMs.size()
                        == static_cast<std::size_t>(
                            warmupCount)
                    && m_renderStartCount == m_renderEndCount
                    && m_renderStarts.empty();
                std::cout
                    << "BENCH_PHASE_WARMUP: case="
                    << caseName
                    << " phase=" << phaseName
                    << " samples=" << m_renderTimesMs.size()
                    << " max_ms=" << GetRenderTimeMs(1.00)
                    << '\n';
                if (!isWarmupValid
                    || !ResetRenderStats()) {
                    return false;
                }
            }
            for (int index = 0; index < sampleCount; ++index) {
                endpoint->renderWindow->Render();
                if (m_renderTimesMs.size()
                    != static_cast<std::size_t>(index + 1)) {
                    return false;
                }
                std::cout
                    << "BENCH_FRAME: case=" << caseName
                    << " phase=" << phaseName
                    << " frame=" << index
                    << " duration_ms="
                    << m_renderTimesMs.back()
                    << " desired_rate="
                    << endpoint->renderWindow
                        ->GetDesiredUpdateRate()
                    << " auto="
                    << mapper->GetAutoAdjustSampleDistances()
                    << " ray_step="
                    << mapper->GetSampleDistance()
                    << " image_step="
                    << mapper->GetImageSampleDistance()
                    << " image_min="
                    << mapper->GetMinimumImageSampleDistance()
                    << " image_max="
                    << mapper->GetMaximumImageSampleDistance()
                    << '\n';
            }
            const bool isPhaseValid =
                m_renderStartCount == m_renderEndCount
                && m_renderStarts.empty()
                && std::abs(
                    endpoint->renderWindow
                        ->GetDesiredUpdateRate()
                        - expectedRate) < 1e-12
                && mapper->GetAutoAdjustSampleDistances()
                    == static_cast<int>(expectedAuto)
                && std::abs(
                    mapper->GetSampleDistance() - expectedRay)
                    < 1e-12
                && std::abs(
                    mapper->GetImageSampleDistance() - 1.0)
                    < 1e-12
                && std::abs(
                    mapper->GetMinimumImageSampleDistance() - 1.0)
                    < 1e-12
                && std::abs(
                    mapper->GetMaximumImageSampleDistance() - 1.0)
                    < 1e-12;
            std::cout
                << "BENCH_PHASE: case=" << caseName
                << " phase=" << phaseName
                << " samples=" << m_renderTimesMs.size()
                << " p50_ms=" << GetRenderTimeMs(0.50)
                << " p95_ms=" << GetRenderTimeMs(0.95)
                << " max_ms=" << GetRenderTimeMs(1.00)
                << " desired_rate="
                << endpoint->renderWindow
                    ->GetDesiredUpdateRate()
                << " auto="
                << mapper->GetAutoAdjustSampleDistances()
                << " ray_step="
                << mapper->GetSampleDistance()
                << " image_step="
                << mapper->GetImageSampleDistance()
                << " image_min="
                << mapper->GetMinimumImageSampleDistance()
                << " image_max="
                << mapper->GetMaximumImageSampleDistance()
                << " state_ok=" << isPhaseValid
                << '\n';
            return isPhaseValid;
        };
        const auto setInteraction =
            [&](bool isInteracting) {
            if (isInteracting) {
                endpoint->interactor->SetEventPosition(
                    320, 240);
                endpoint->interactor->InvokeEvent(
                    vtkCommand::RightButtonPressEvent);
            }
            else {
                endpoint->interactor->InvokeEvent(
                    vtkCommand::RightButtonReleaseEvent);
            }
            endpoint->interactor->InvokeEvent(
                vtkCommand::TimerEvent);
            return std::abs(
                endpoint->renderWindow->GetDesiredUpdateRate()
                    - (isInteracting ? 15.0 : 0.001)) < 1e-12;
        };
        const auto runInteractionPhases =
            [&](const char* caseName,
                bool isAuto,
                double rayStep) {
            setBaseState();
            params.volumeQuality = {
                VolumeQuality::Custom, 766, rayStep, true
            };
            strategy.SetVisualState(
                params, UpdateFlags::Quality);
            mapper->SetAutoAdjustSampleDistances(isAuto);
            if (!setSliceMode()) {
                return false;
            }
            const bool isBefore = samplePhase(
                caseName, "before", 0.001,
                isAuto, rayStep, true);
            const bool isDuringBoundary = setInteraction(true);
            const bool isDuring = isDuringBoundary
                && samplePhase(
                    caseName, "during", 15.0,
                    isAuto, rayStep, false);
            const bool isAfterBoundary = setInteraction(false);
            const bool isAfter = isAfterBoundary
                && samplePhase(
                    caseName, "after", 0.001,
                    isAuto, rayStep, false);
            return isBefore && isDuring && isAfter;
        };
        areSamplesValid =
            runInteractionPhases(
                "I_fixed_interaction", false, 1.0)
            && areSamplesValid;
        areSamplesValid =
            runInteractionPhases(
                "J_auto_interaction", true, 1.0)
            && areSamplesValid;

        HostViewSetRequest restoreRequest;
        restoreRequest.targetView.viewId = "primary-3d";
        restoreRequest.mode =
            HostRenderMode::CompositeIsoSurface;
        const bool isRestoreSent =
            m_session->SendRequest(
                std::move(restoreRequest));
        endpoint->interactor->InvokeEvent(
            vtkCommand::TimerEvent);
        areSamplesValid =
            isRestoreSent
            && std::abs(
                endpoint->renderWindow->GetDesiredUpdateRate()
                    - 0.001) < 1e-12
            && areSamplesValid;
        strategy.DetachRenderer(renderer);
        return areSamplesValid;
    }

    QVTKOpenGLNativeWidget* GetWidget() const
    {
        return m_widget.get();
    }

private:
    bool AttachRenderStats()
    {
        if (!m_renderWindow || m_renderTimingCallback
            || m_renderStartTag != 0 || m_renderEndTag != 0) {
            return false;
        }
        m_renderTimingCallback =
            vtkSmartPointer<vtkCallbackCommand>::New();
        m_renderTimingCallback->SetClientData(this);
        m_renderTimingCallback->SetCallback(
            &SessionFixture::OnRenderTiming);
        m_renderStartTag = m_renderWindow->AddObserver(
            vtkCommand::StartEvent, m_renderTimingCallback);
        m_renderEndTag = m_renderWindow->AddObserver(
            vtkCommand::EndEvent, m_renderTimingCallback);
        if (m_renderStartTag != 0 && m_renderEndTag != 0) {
            return true;
        }
        DetachRenderStats();
        return false;
    }

    bool DetachRenderStats()
    {
        if (m_renderWindow && m_renderStartTag != 0) {
            m_renderWindow->RemoveObserver(m_renderStartTag);
        }
        if (m_renderWindow && m_renderEndTag != 0) {
            m_renderWindow->RemoveObserver(m_renderEndTag);
        }
        m_renderStartTag = 0;
        m_renderEndTag = 0;
        if (m_renderTimingCallback) {
            m_renderTimingCallback->SetClientData(nullptr);
        }
        m_renderTimingCallback = nullptr;
        m_renderStarts.clear();
        return true;
    }

    bool ResetRenderStats()
    {
        if (!m_renderStarts.empty()) {
            return false;
        }
        m_renderStartCount = 0;
        m_renderEndCount = 0;
        m_renderTimesMs.clear();
        return true;
    }

    double GetRenderTimeMs(double percentile) const
    {
        if (m_renderTimesMs.empty()) {
            return 0.0;
        }
        auto values = m_renderTimesMs;
        std::sort(values.begin(), values.end());
        const auto rank = static_cast<std::size_t>(
            std::ceil(percentile * static_cast<double>(values.size())));
        const std::size_t index = std::min(
            values.size() - 1,
            rank > 0 ? rank - 1 : std::size_t{0});
        return values[index];
    }

    static void OnRenderTiming(
        vtkObject*, unsigned long eventId, void* clientData, void*)
    {
        auto* fixture = static_cast<SessionFixture*>(clientData);
        if (!fixture) {
            return;
        }
        if (eventId == vtkCommand::StartEvent) {
            ++fixture->m_renderStartCount;
            fixture->m_renderStarts.push_back(
                std::chrono::steady_clock::now());
            return;
        }
        if (eventId != vtkCommand::EndEvent) {
            return;
        }

        ++fixture->m_renderEndCount;
        if (fixture->m_renderStarts.empty()) {
            return;
        }
        const auto startTime = fixture->m_renderStarts.back();
        fixture->m_renderStarts.pop_back();
        const auto duration = std::chrono::steady_clock::now()
            - startTime;
        fixture->m_renderTimesMs.push_back(
            std::chrono::duration<double, std::milli>(
                duration).count());
    }

    static void OnVtkError(
        vtkObject*, unsigned long, void* clientData, void* callData)
    {
        auto* fixture = static_cast<SessionFixture*>(clientData);
        if (!fixture) {
            return;
        }
        ++fixture->m_vtkErrorCount;
        if (callData) {
            fixture->m_vtkErrorText = static_cast<const char*>(callData);
        }
    }

    std::unique_ptr<QVTKOpenGLNativeWidget> m_widget;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> m_renderWindow;
    vtkSmartPointer<vtkCallbackCommand> m_renderTimingCallback;
    vtkSmartPointer<vtkCallbackCommand> m_errorCallback;
    std::unique_ptr<VtkAppHostSession> m_session;
    std::vector<std::chrono::steady_clock::time_point> m_renderStarts;
    std::vector<double> m_renderTimesMs;
    unsigned long m_renderStartTag{0};
    unsigned long m_renderEndTag{0};
    unsigned long m_errorTag{0};
    std::size_t m_renderStartCount{0};
    std::size_t m_renderEndCount{0};
    std::size_t m_vtkErrorCount{0};
    std::string m_vtkErrorText;
};

} // namespace

int main(int argc, char* argv[])
{
    // QVTK surface 格式必须早于 QApplication，和接入指南的构建链保持一致。
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    if (!BuildPresetReturnTest()) {
        std::cerr
            << "FAIL: Real preset conflict was not propagated\n";
        return 3;
    }
    if (!BuildRenderSourceTest()) {
        std::cerr
            << "FAIL: Render sources were not isolated\n";
        return 4;
    }
    if (!BuildDefaultRenderTest()) {
        std::cerr << "FAIL: BuildSession unexpectedly rendered the Qt-owned window\n";
        return 5;
    }
    if (!BuildZoomRenderTest()) {
        std::cerr << "FAIL: Viewer2D zoom did not converge through one Timer render\n";
        return 6;
    }

    SessionFixture smoke;
    if (!smoke.BuildWindow()) {
        std::cerr << "FAIL: Qt Host window build\n";
        return 1;
    }

    const bool isInteractive =
        QCoreApplication::arguments().contains(QStringLiteral("--interactive"));
    const bool isVolumeBench =
        QCoreApplication::arguments().contains(
            QStringLiteral("--volume-benchmark"));
    int result{2};
    bool hasStarted{false};
    QObject::connect(smoke.GetWidget(), &QOpenGLWidget::frameSwapped, &app, [&]() {
        if (hasStarted) {
            return;
        }
        hasStarted = true;
        const bool isHostPassed = smoke.StartHost();
        const bool isBlendPassed =
            isHostPassed && smoke.GetBlendVisualValid();
        const bool isBenchPassed =
            !isVolumeBench
            || (isHostPassed && smoke.StartVolumeBench(120));
        const bool isPassed =
            isHostPassed && isBlendPassed && isBenchPassed;
        result = isPassed ? 0 : 3;
        if (!isHostPassed) {
            std::cerr << "FAIL: Qt Host endpoint setup\n";
        }
        else if (!isBlendPassed) {
            std::cerr
                << "FAIL: Composite visual baseline changed\n";
        }
        else if (!isBenchPassed) {
            std::cerr << "FAIL: Volume benchmark sample collection\n";
        }
        std::cout << (isPassed
            ? "PASS: Qt Host session endpoint binding\n"
            : "FAIL: Qt Host session endpoint binding\n");

        if (!isInteractive || !isPassed) {
            QTimer::singleShot(0, &app, [&]() {
                smoke.StopHost();
                QApplication::exit(result);
            });
        }
    });
    QTimer::singleShot(5000, &app, [&]() {
        if (!hasStarted) {
            std::cerr << "FAIL: QVTK first frame timeout\n";
            result = 4;
            smoke.StopHost();
            QApplication::exit(result);
        }
    });

    const int appResult = app.exec();
    smoke.StopHost();
    return appResult;
}
