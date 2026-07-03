#include "Host/VtkAppHostSession.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppState.h"
#include "AppTypes.h"
#include "DataManager.h"
#include "Interaction/OrthogonalCropInteractionBridgeService.h"
#include "Render/Strategies/GapAlgorithmOverlayStrategies.h"
#include "Services/GapAnalysisService.h"
#include "StdRenderContext.h"
#include "VolumeAnalysisService.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindowInteractor.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr double kHostCommandHotkeyObserverPriority = 1.0;

using HostHotkeyBindings = VtkAppHostSession::HostHotkeyBindings;
using InitialVolumeLoadConfig = VtkAppHostSession::InitialVolumeLoadConfig;

struct HostKeyInput {
    char keyCode = 0;
    std::string keySym;
    bool isControlPressed = false;
};

static HostKeyInput BuildHostKeyInput(vtkRenderWindowInteractor* interactor)
{
    HostKeyInput input;
    if (!interactor) {
        return input;
    }

    input.keyCode = interactor->GetKeyCode();
    input.keySym = interactor->GetKeySym() ? interactor->GetKeySym() : "";
    input.isControlPressed = interactor->GetControlKey() != 0;
    return input;
}

static char ToUpperAscii(char value)
{
    return (value >= 'a' && value <= 'z')
        ? static_cast<char>(value - 'a' + 'A')
        : value;
}

static bool MatchesCharacterKey(const HostKeyInput& input, char key)
{
    const char upperKey = ToUpperAscii(key);
    const std::string keySymbol(1, key);
    const std::string upperKeySymbol(1, upperKey);
    return input.keyCode == key
        || input.keyCode == upperKey
        || input.keySym == keySymbol
        || input.keySym == upperKeySymbol;
}

static bool MatchesKeySymbol(const HostKeyInput& input, const std::string& keySym)
{
    return !keySym.empty() && input.keySym == keySym;
}

static std::string BuildKeyLabel(char key)
{
    return std::string(1, ToUpperAscii(key));
}

static std::string BuildControlKeyLabel(char key)
{
    return "Ctrl+" + BuildKeyLabel(key);
}

static std::string BuildStartupControlsText(const HostHotkeyBindings& hotkeys)
{
    return "Controls: "
        + BuildKeyLabel(hotkeys.modelTransformToggleKey) + " = toggle model transform | "
        + BuildKeyLabel(hotkeys.saveTransformedDataKey) + " = save transformed data | "
        + BuildKeyLabel(hotkeys.saveSliceImagesKey) + " = save slice images | "
        + BuildKeyLabel(hotkeys.cropToggleKey) + " = toggle orthogonal crop box | "
        + BuildKeyLabel(hotkeys.planarCropToggleKey) + " = toggle planar crop | "
        + BuildKeyLabel(hotkeys.gapOverlayToggleKey) + " = enter/toggle gap overlays | "
        + hotkeys.exitKeySym + " = exit active crop/gap/model-transform mode | "
        + BuildKeyLabel(hotkeys.keepInsidePreviewKey) + " = keep inside/normal-side preview | "
        + BuildKeyLabel(hotkeys.removeInsidePreviewKey) + " = remove inside/normal-side preview | "
        + BuildControlKeyLabel(hotkeys.submitKey) + " = apply submit";
}

class IGapAnalysisDisplayController {
public:
    virtual ~IGapAnalysisDisplayController() = default;

    // 宿主命令层只表达“进入/切换显示”和“退出模式”这两个用户意图；
    // timer 只消费显式分析请求，避免应用启动后默认进入孔隙分析链路。
    virtual bool GetAnalysisDisplayActive() const = 0;
    virtual bool GetAnalysisRunRequested() const = 0;
    virtual void ClearAnalysisRunRequest() = 0;
    virtual bool ToggleAnalysisOverlayVisibility() = 0;
    virtual bool ExitAnalysisDisplay() = 0;
};

class HostCommandHotkeyObserver : public vtkCommand {
public:
    static HostCommandHotkeyObserver* New() { return new HostCommandHotkeyObserver; }

    std::shared_ptr<OrthogonalCropInteractionBridgeService> orthogonalCropBridge;
    std::shared_ptr<IGapAnalysisDisplayController> gapAnalysisOverlayController;
    std::function<bool()> getHostToolModeActive;
    std::function<bool()> exitHostToolMode;
    HostHotkeyBindings hotkeys;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        (void)callData;
        this->AbortFlagOff();
        auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
        if (!iren) {
            return;
        }

        const HostKeyInput keyInput = BuildHostKeyInput(iren);
        const bool isExitKey = MatchesKeySymbol(keyInput, hotkeys.exitKeySym);
        const bool isGapOverlayToggleKey = MatchesCharacterKey(keyInput, hotkeys.gapOverlayToggleKey);
        const bool isCropToggleKey = MatchesCharacterKey(keyInput, hotkeys.cropToggleKey);
        const bool isPlanarCropToggleKey = MatchesCharacterKey(keyInput, hotkeys.planarCropToggleKey);
        const bool isInsidePreviewKey = MatchesCharacterKey(keyInput, hotkeys.keepInsidePreviewKey);
        const bool isOutsidePreviewKey = MatchesCharacterKey(keyInput, hotkeys.removeInsidePreviewKey);
        const bool isSubmitKeyCode = MatchesCharacterKey(keyInput, hotkeys.submitKey);
        const bool isSubmitKey = isSubmitKeyCode && keyInput.isControlPressed;
        const bool shouldBlockVtkStereoKey = isSubmitKeyCode && !keyInput.isControlPressed;
        const bool canHandleCropCommand = orthogonalCropBridge != nullptr;
        const bool canHandleGapDisplay = gapAnalysisOverlayController != nullptr;
        const bool canHandleExitCommand = isExitKey && GetAnyExitTargetActive();
        const bool isManagedHotkey =
            (canHandleCropCommand
                && (isCropToggleKey
                    || isPlanarCropToggleKey
                    || isInsidePreviewKey
                    || isOutsidePreviewKey
                    || isSubmitKey
                    || shouldBlockVtkStereoKey))
            || (canHandleGapDisplay && isGapOverlayToggleKey)
            || canHandleExitCommand;

        if (eventId == vtkCommand::CharEvent && isManagedHotkey) {
            this->AbortFlagOn();
            return;
        }

        if (eventId == vtkCommand::KeyPressEvent) {
            if (canHandleCropCommand && isCropToggleKey && !m_cropToggleKeyDown) {
                m_cropToggleKeyDown = true;
                orthogonalCropBridge->ToggleInteractiveCrop();
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isPlanarCropToggleKey && !m_planarCropToggleKeyDown) {
                m_planarCropToggleKeyDown = true;
                orthogonalCropBridge->ToggleInteractivePlanarCrop();
                this->AbortFlagOn();
                return;
            }

            if (canHandleGapDisplay && isGapOverlayToggleKey && !m_gapOverlayToggleKeyDown) {
                m_gapOverlayToggleKeyDown = true;
                gapAnalysisOverlayController->ToggleAnalysisOverlayVisibility();
                this->AbortFlagOn();
                return;
            }

            if (isExitKey) {
                if (ExitActiveHostMode()) {
                    this->AbortFlagOn();
                    return;
                }
            }

            if (canHandleCropCommand && isInsidePreviewKey) {
                orthogonalCropBridge->TogglePreview(CropRemovalMode::KeepInside);
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isOutsidePreviewKey) {
                orthogonalCropBridge->TogglePreview(CropRemovalMode::RemoveInside);
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isSubmitKey && !m_submitKeyDown) {
                m_submitKeyDown = true;
                orthogonalCropBridge->ApplySubmit();
                this->AbortFlagOn();
                return;
            }

            if (isManagedHotkey) {
                this->AbortFlagOn();
                return;
            }
        }

        if (eventId == vtkCommand::KeyReleaseEvent) {
            if (canHandleCropCommand && isCropToggleKey) {
                m_cropToggleKeyDown = false;
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isPlanarCropToggleKey) {
                m_planarCropToggleKeyDown = false;
                this->AbortFlagOn();
                return;
            }

            if (canHandleGapDisplay && isGapOverlayToggleKey) {
                m_gapOverlayToggleKeyDown = false;
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isInsidePreviewKey) {
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isOutsidePreviewKey) {
                this->AbortFlagOn();
                return;
            }

            if (canHandleCropCommand && isSubmitKeyCode) {
                m_submitKeyDown = false;
                this->AbortFlagOn();
                return;
            }
        }
    }

private:
    bool m_cropToggleKeyDown = false;
    bool m_planarCropToggleKeyDown = false;
    bool m_gapOverlayToggleKeyDown = false;
    bool m_submitKeyDown = false;

    bool GetAnyExitTargetActive() const
    {
        if (orthogonalCropBridge && orthogonalCropBridge->GetInteractiveCropActive()) {
            return true;
        }
        if (gapAnalysisOverlayController && gapAnalysisOverlayController->GetAnalysisDisplayActive()) {
            return true;
        }
        return getHostToolModeActive && getHostToolModeActive();
    }

    bool ExitActiveHostMode()
    {
        if (orthogonalCropBridge && orthogonalCropBridge->GetInteractiveCropActive()) {
            orthogonalCropBridge->ExitInteractiveCrop();
            return true;
        }
        if (gapAnalysisOverlayController && gapAnalysisOverlayController->GetAnalysisDisplayActive()) {
            gapAnalysisOverlayController->ExitAnalysisDisplay();
            return true;
        }
        if (getHostToolModeActive && getHostToolModeActive() && exitHostToolMode) {
            exitHostToolMode();
            return true;
        }
        return false;
    }
};

class GapAnalysisOverlayController : public IGapAnalysisDisplayController {
public:
    void SetRenderServices(
        std::shared_ptr<MedicalVizService> primary3DService,
        std::shared_ptr<MedicalVizService> topDownSliceService,
        std::shared_ptr<MedicalVizService> frontBackSliceService,
        std::shared_ptr<MedicalVizService> leftRightSliceService,
        std::shared_ptr<MedicalVizService> composite3DService)
    {
        HideOverlays();
        m_primary3DService = std::move(primary3DService);
        m_topDownSliceService = std::move(topDownSliceService);
        m_frontBackSliceService = std::move(frontBackSliceService);
        m_leftRightSliceService = std::move(leftRightSliceService);
        m_composite3DService = std::move(composite3DService);
    }

    bool GetAnalysisDisplayActive() const override
    {
        return m_isAnalysisDisplayActive;
    }

    bool GetAnalysisRunRequested() const override
    {
        return m_hasPendingAnalysisRunRequest;
    }

    void ClearAnalysisRunRequest() override
    {
        m_hasPendingAnalysisRunRequest = false;
    }

    bool ApplyAnalysisResult(
        vtkSmartPointer<vtkPolyData> voidMesh,
        vtkSmartPointer<vtkImageData> labelImage)
    {
        if (!m_isAnalysisDisplayActive) {
            return false;
        }

        m_voidMesh = std::move(voidMesh);
        m_labelImage = std::move(labelImage);
        HideOverlays();
        if (!m_shouldShowOverlay) {
            std::cout << "[Host] Gap Analysis completed, but overlays are hidden. Use the host overlay toggle command to show them." << std::endl;
            return false;
        }

        return ShowStoredAnalysisResult();
    }

    bool ToggleAnalysisOverlayVisibility() override
    {
        if (!m_isAnalysisDisplayActive) {
            m_isAnalysisDisplayActive = true;
            m_shouldShowOverlay = true;
            m_hasPendingAnalysisRunRequest = true;
            m_voidMesh = nullptr;
            m_labelImage = nullptr;
            HideOverlays();
            std::cout << "[Host] Gap Analysis display mode requested. Analysis will start after volume data is ready." << std::endl;
            return true;
        }

        m_shouldShowOverlay = !m_shouldShowOverlay;
        if (!m_shouldShowOverlay) {
            const bool removedAnyOverlay = HideOverlays();
            std::cout << "[Host] Gap Analysis overlays hidden. Use the host overlay toggle command to show them again." << std::endl;
            return removedAnyOverlay;
        }

        if (!HasStoredAnalysisResult()) {
            std::cout << "[Host] Gap Analysis overlays enabled. They will appear after analysis completes." << std::endl;
            return true;
        }

        return ShowStoredAnalysisResult();
    }

    bool ExitAnalysisDisplay() override
    {
        const bool wasAnalysisDisplayActive = m_isAnalysisDisplayActive;
        const bool hadStoredAnalysisResult = HasStoredAnalysisResult();
        m_isAnalysisDisplayActive = false;
        m_shouldShowOverlay = false;
        m_hasPendingAnalysisRunRequest = false;
        m_voidMesh = nullptr;
        m_labelImage = nullptr;
        const bool removedAnyOverlay = HideOverlays();
        if (wasAnalysisDisplayActive || removedAnyOverlay || hadStoredAnalysisResult) {
            std::cout << "[Host] Gap Analysis display mode exited. Void overlays are hidden." << std::endl;
        }
        return wasAnalysisDisplayActive || removedAnyOverlay || hadStoredAnalysisResult;
    }

private:
    struct OverlayBinding {
        std::shared_ptr<MedicalVizService> service;
        std::shared_ptr<AbstractVisualStrategy> overlayStrategy;
    };

    bool HasStoredAnalysisResult() const
    {
        return m_voidMesh != nullptr || m_labelImage != nullptr;
    }

    bool ShowStoredAnalysisResult()
    {
        HideOverlays();

        const vtkIdType meshPoints = m_voidMesh ? m_voidMesh->GetNumberOfPoints() : 0;
        const vtkIdType meshCells = m_voidMesh ? m_voidMesh->GetNumberOfCells() : 0;
        if (m_voidMesh && meshPoints > 0 && meshCells > 0) {
            AddMeshOverlay(m_primary3DService, m_voidMesh);
            AddMeshOverlay(m_composite3DService, m_voidMesh);
        }
        else {
            std::cerr << "[Host] Gap Analysis produced no 3D void mesh overlay." << std::endl;
        }

        int labelDims[3] = { 0, 0, 0 };
        if (m_labelImage) {
            m_labelImage->GetDimensions(labelDims);
        }
        if (m_labelImage && labelDims[0] > 0 && labelDims[1] > 0 && labelDims[2] > 0) {
            AddSliceOverlay(m_topDownSliceService, m_labelImage, Orientation::Top_down);
            AddSliceOverlay(m_frontBackSliceService, m_labelImage, Orientation::Front_back);
            AddSliceOverlay(m_leftRightSliceService, m_labelImage, Orientation::Left_right);
        }
        else {
            std::cerr << "[Host] Gap Analysis produced no 2D label overlay." << std::endl;
        }

        if (!m_overlayBindings.empty()) {
            std::cout << "[Host] Gap Analysis overlays shown: mesh points = "
                << meshPoints << ", mesh cells = " << meshCells
                << ", label dims = " << labelDims[0] << "x" << labelDims[1] << "x" << labelDims[2]
                << std::endl;
        }
        return !m_overlayBindings.empty();
    }

    bool HideOverlays()
    {
        bool removedAnyOverlay = false;
        for (const auto& binding : m_overlayBindings) {
            if (!binding.service || !binding.overlayStrategy) {
                continue;
            }
            binding.service->RemoveOverlayStrategy(binding.overlayStrategy);
            binding.service->MarkDirty();
            removedAnyOverlay = true;
        }
        m_overlayBindings.clear();
        return removedAnyOverlay;
    }

    void AddMeshOverlay(
        const std::shared_ptr<MedicalVizService>& service,
        vtkSmartPointer<vtkPolyData> voidMesh)
    {
        if (!service || !voidMesh) {
            return;
        }

        auto overlay = std::make_shared<GapMeshOverlayStrategy>();
        overlay->SetInputData(voidMesh);
        service->AddOverlayStrategy(overlay);
        m_overlayBindings.push_back({ service, overlay });
    }

    void AddSliceOverlay(
        const std::shared_ptr<MedicalVizService>& service,
        vtkSmartPointer<vtkImageData> labelImage,
        Orientation orientation)
    {
        if (!service || !labelImage) {
            return;
        }

        auto overlay = std::make_shared<GapSliceOverlayStrategy>(orientation);
        overlay->SetInputData(labelImage);
        service->AddOverlayStrategy(overlay);
        m_overlayBindings.push_back({ service, overlay });
    }

    // 隐藏与退出故意分开：宿主显隐命令只移除当前 overlay 句柄并保留算法结果；
    // 宿主退出命令才清掉缓存并关闭显示模式，避免“临时隐藏”被误解释为放弃分析。
    std::vector<OverlayBinding> m_overlayBindings;
    vtkSmartPointer<vtkPolyData> m_voidMesh;
    vtkSmartPointer<vtkImageData> m_labelImage;
    std::shared_ptr<MedicalVizService> m_primary3DService;
    std::shared_ptr<MedicalVizService> m_topDownSliceService;
    std::shared_ptr<MedicalVizService> m_frontBackSliceService;
    std::shared_ptr<MedicalVizService> m_leftRightSliceService;
    std::shared_ptr<MedicalVizService> m_composite3DService;
    bool m_isAnalysisDisplayActive = false;
    bool m_shouldShowOverlay = false;
    bool m_hasPendingAnalysisRunRequest = false;
};

class GapAnalysisOverlayCommitObserver : public vtkCommand {
public:
    static GapAnalysisOverlayCommitObserver* New() { return new GapAnalysisOverlayCommitObserver; }

    std::shared_ptr<GapAnalysisService> gapAnalysis;
    std::shared_ptr<MedicalVizService> serviceA;
    std::shared_ptr<GapAnalysisOverlayController> gapAnalysisOverlayController;
    std::shared_ptr<AbstractDataManager> dataMgr;
    std::shared_ptr<SharedInteractionState> sharedState;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        (void)caller;
        (void)callData;
        if (eventId != vtkCommand::TimerEvent || !gapAnalysis || !gapAnalysisOverlayController) {
            return;
        }

        if (!gapAnalysisOverlayController->GetAnalysisDisplayActive()) {
            return;
        }

        if (gapAnalysisOverlayController->GetAnalysisRunRequested()) {
            if (gapAnalysis->GetAnalysisState() != GapAnalysisState::Running && TryStartAnalysis()) {
                gapAnalysisOverlayController->ClearAnalysisRunRequest();
                m_completionHandled = false;
                m_failureLogged = false;
            }
            return;
        }

        if (m_completionHandled) {
            return;
        }

        const GapAnalysisState state = gapAnalysis->GetAnalysisState();
        if (state == GapAnalysisState::Idle) {
            return;
        }

        if (state == GapAnalysisState::Running) {
            return;
        }

        if (gapAnalysis->ConsumePendingCompletionCallback()) {
            gapAnalysis->ExecutePendingCompletionCallback();
        }

        if (state == GapAnalysisState::Failed) {
            if (!m_failureLogged) {
                std::cerr << "[Host] Gap Analysis failed; overlay will not be attached." << std::endl;
                m_failureLogged = true;
            }
            return;
        }

        CommitOverlays();
        m_completionHandled = true;
    }

private:
    bool m_completionHandled = false;
    bool m_failureLogged = false;

    bool TryStartAnalysis()
    {
        if (!sharedState || sharedState->GetFileLoadState() != LoadState::Succeeded) {
            return false;
        }

        if (!dataMgr || !gapAnalysis->SetInputImage(dataMgr->GetVtkImage())) {
            return false;
        }

        const auto range = sharedState->GetDataRange();
        const double isoValue = range[0] + (range[1] - range[0]) * 0.55;
        if (serviceA) {
            serviceA->SetIsoThreshold(isoValue);
        }

        SurfaceParams surfP;
        surfP.isoValue = static_cast<float>(isoValue);
        gapAnalysis->SetSurfaceParams(surfP);

        VoidDetectionParams voidP;
        voidP.grayMin = -0.2262536138296127f;
        voidP.grayMax = 0.15f;
        voidP.minVolumeMM3 = 0.0001;
        voidP.angleThresholdDeg = 30.0f;
        voidP.tensorWindowSize = 1;
        voidP.erosionIterations = 2;
        gapAnalysis->SetVoidParams(voidP);

        gapAnalysis->RunAsync();
        return true;
    }

    void CommitOverlays()
    {
        if (!gapAnalysisOverlayController) {
            std::cout << "[Host] Gap Analysis completed, but overlay display controller is missing." << std::endl;
            return;
        }

        if (!gapAnalysisOverlayController->GetAnalysisDisplayActive()) {
            std::cout << "[Host] Gap Analysis completed, but display mode has exited." << std::endl;
            return;
        }

        auto voidMesh = gapAnalysis->BuildVoidMesh();
        auto labelImage = gapAnalysis->BuildLabelImage();

        // 算法服务只提供稳定结果；是否显示、显示到哪些窗口、如何隐藏都由显示控制器决定。
        // 这样临时隐藏和彻底退出不会相互污染。
        gapAnalysisOverlayController->ApplyAnalysisResult(voidMesh, labelImage);
    }
};

static std::pair<
    std::shared_ptr<MedicalVizService>,
    std::shared_ptr<StdRenderContext>>
    GetWindowPair(
        const WindowConfig& cfg,
        std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> sharedState,
        std::shared_ptr<IStateEventSource> stateEventSource)
{
    auto service = std::make_shared<MedicalVizService>(std::move(dataMgr), std::move(sharedState), std::move(stateEventSource));
    auto context = std::make_shared<StdRenderContext>();

    context->SetServiceBound(service);
    service->SetVisualConfig(cfg.preInitCfg);
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->ApplyCameraStyle(cfg.preInitCfg.vizMode);
    if (cfg.showAxes) {
        context->SetOrientationAxesVisible(true);
    }

    if (cfg.preInitCfg.hasBgColor) {
        service->SetBackground(cfg.preInitCfg.bgColor);
    }

    return { service, context };
}

struct HostCoreServices {
    std::shared_ptr<RawVolumeDataManager> sharedDataMgr;
    std::shared_ptr<SharedStateBroadcaster> sharedStateBroadcaster;
    std::shared_ptr<SharedInteractionState> sharedState;
    std::shared_ptr<VolumeAnalysisService> imageAnalysis;
    std::shared_ptr<GapAnalysisService> gapAnalysis;
    std::shared_ptr<GapAnalysisOverlayController> gapAnalysisOverlayController;
    std::shared_ptr<OrthogonalCropInteractionBridgeService> orthogonalCropBridge;
};

struct HostWindowConfigs {
    WindowConfig cfgA;
    WindowConfig cfgB;
    WindowConfig cfgC;
    WindowConfig cfgD;
    WindowConfig cfgE;
};

struct HostRenderWindows {
    std::shared_ptr<MedicalVizService> serviceA;
    std::shared_ptr<MedicalVizService> serviceB;
    std::shared_ptr<MedicalVizService> serviceC;
    std::shared_ptr<MedicalVizService> serviceD;
    std::shared_ptr<MedicalVizService> serviceE;
    std::shared_ptr<StdRenderContext> contextA;
    std::shared_ptr<StdRenderContext> contextB;
    std::shared_ptr<StdRenderContext> contextC;
    std::shared_ptr<StdRenderContext> contextD;
    std::shared_ptr<StdRenderContext> contextE;
};

static HostCoreServices BuildHostCoreServices()
{
    HostCoreServices core;
    core.sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    core.sharedState = std::make_shared<SharedInteractionState>(core.sharedStateBroadcaster);
    core.imageAnalysis = std::make_shared<VolumeAnalysisService>(core.sharedDataMgr);
    core.gapAnalysis = std::make_shared<GapAnalysisService>();
    core.gapAnalysisOverlayController = std::make_shared<GapAnalysisOverlayController>();
    core.orthogonalCropBridge = std::make_shared<OrthogonalCropInteractionBridgeService>();
    return core;
}

static std::vector<TFNode> BuildVolumeTransferFunction()
{
    return {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };
}

static HostWindowConfigs BuildWindowConfigs()
{
    HostWindowConfigs configs;
    const auto volTF = BuildVolumeTransferFunction();

    configs.cfgE.title = "Window E: Composite Volume";
    configs.cfgE.width = 600; configs.cfgE.height = 600;
    configs.cfgE.posX = 660; configs.cfgE.posY = 50;
    configs.cfgE.preInitCfg.vizMode = VizMode::CompositeVolume;
    configs.cfgE.preInitCfg.tfNodes = volTF;
    configs.cfgE.preInitCfg.hasTF = true;
    configs.cfgE.preInitCfg.bgColor = { 0.08, 0.08, 0.12 };
    configs.cfgE.preInitCfg.hasBgColor = true;

    configs.cfgB.title = "Window B: Top_down Slice";
    configs.cfgB.width = 400; configs.cfgB.height = 400;
    configs.cfgB.posX = 50;  configs.cfgB.posY = 660;
    configs.cfgB.preInitCfg.vizMode = VizMode::SliceTop_down;
    configs.cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    configs.cfgB.preInitCfg.hasBgColor = true;
    configs.cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };
    configs.cfgB.preInitCfg.hasWindowLevel = true;

    configs.cfgC.title = "Window C: Front_back Slice";
    configs.cfgC.width = 400; configs.cfgC.height = 400;
    configs.cfgC.posX = 460; configs.cfgC.posY = 660;
    configs.cfgC.preInitCfg.vizMode = VizMode::SliceFront_back;
    configs.cfgC.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    configs.cfgC.preInitCfg.hasBgColor = true;
    configs.cfgC.preInitCfg.windowLevel = { 400.0, 40.0 };
    configs.cfgC.preInitCfg.hasWindowLevel = true;

    configs.cfgD.title = "Window D: Left_right Slice";
    configs.cfgD.width = 400; configs.cfgD.height = 400;
    configs.cfgD.posX = 870; configs.cfgD.posY = 660;
    configs.cfgD.preInitCfg.vizMode = VizMode::SliceLeft_right;
    configs.cfgD.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    configs.cfgD.preInitCfg.hasBgColor = true;
    configs.cfgD.preInitCfg.windowLevel = { 400.0, 40.0 };
    configs.cfgD.preInitCfg.hasWindowLevel = true;

    configs.cfgA.title = "Window A: Composite IsoSurface";
    configs.cfgA.width = 600; configs.cfgA.height = 600;
    configs.cfgA.posX = 50;  configs.cfgA.posY = 50;
    configs.cfgA.showAxes = true;
    configs.cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    configs.cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    configs.cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    configs.cfgA.preInitCfg.hasBgColor = true;

    return configs;
}

static HostRenderWindows BuildRenderWindows(
    const HostCoreServices& core,
    const HostWindowConfigs& configs)
{
    HostRenderWindows windows;

    auto [serviceA, contextA] = GetWindowPair(configs.cfgA, core.sharedDataMgr, core.sharedState, core.sharedStateBroadcaster);
    auto [serviceE, contextE] = GetWindowPair(configs.cfgE, core.sharedDataMgr, core.sharedState, core.sharedStateBroadcaster);
    auto [serviceB, contextB] = GetWindowPair(configs.cfgB, core.sharedDataMgr, core.sharedState, core.sharedStateBroadcaster);
    auto [serviceC, contextC] = GetWindowPair(configs.cfgC, core.sharedDataMgr, core.sharedState, core.sharedStateBroadcaster);
    auto [serviceD, contextD] = GetWindowPair(configs.cfgD, core.sharedDataMgr, core.sharedState, core.sharedStateBroadcaster);

    windows.serviceA = std::move(serviceA);
    windows.serviceB = std::move(serviceB);
    windows.serviceC = std::move(serviceC);
    windows.serviceD = std::move(serviceD);
    windows.serviceE = std::move(serviceE);
    windows.contextA = std::move(contextA);
    windows.contextB = std::move(contextB);
    windows.contextC = std::move(contextC);
    windows.contextD = std::move(contextD);
    windows.contextE = std::move(contextE);
    return windows;
}

static void BindGapAnalysisDisplay(
    const HostCoreServices& core,
    const HostRenderWindows& windows)
{
    core.gapAnalysisOverlayController->SetRenderServices(
        windows.serviceA,
        windows.serviceB,
        windows.serviceC,
        windows.serviceD,
        windows.serviceE);
}

static void BindOrthogonalCropFeature(
    const HostCoreServices& core,
    const HostRenderWindows& windows)
{
    auto bridge = core.orthogonalCropBridge;
    bridge->SetDataManager(core.sharedDataMgr);
    bridge->SetReferenceRenderService(windows.serviceA);
    bridge->SetReferenceRenderer(windows.contextA->GetRenderer());
    bridge->SetPreviewRenderServices({
        windows.serviceA,
        windows.serviceB,
        windows.serviceC,
        windows.serviceD,
        windows.serviceE });

    // Submit reload 是裁切 feature 与宿主数据流的唯一写回能力；
    // 收在这里，后续上位机命令可复用 bridge，而不需要知道 reload 细节。
    bridge->SetSubmitReloadHandler(
        [
            sharedDataMgr = core.sharedDataMgr,
            sharedState = core.sharedState,
            serviceA = windows.serviceA
        ](
            vtkSmartPointer<vtkImageData> image,
            std::function<void(bool success)> onComplete) {
            if (!sharedDataMgr || !sharedState || !serviceA || !image) {
                return false;
            }

            if (sharedState->GetFileLoadState() == LoadState::Loading
                || sharedState->GetReloadLoadState() == LoadState::Loading)
            {
                std::cerr << "[Host] Orthogonal crop submit failed: reload is already in progress." << std::endl;
                return false;
            }

            sharedState->SetReloadLoadStarted();
            if (!sharedDataMgr->TakeImageSnapshot(std::move(image))
                || !sharedDataMgr->ConsumePendingImage()) {
                sharedState->SetReloadLoadFailed();
                return false;
            }

            const auto range = sharedDataMgr->GetScalarRange();
            const auto spacing = sharedDataMgr->GetSpacing();
            sharedState->SetReloadDataReady(range[0], range[1], spacing);
            serviceA->ProcessPendingUpdates();

            if (onComplete) {
                onComplete(true);
            }
            return true;
        });
}

static void ConfigureInitialWindowVisibility(const HostRenderWindows& windows)
{
    windows.serviceA->SetElementVisible(VisFlags::Planes3D, false);
    windows.serviceE->SetElementVisible(VisFlags::Planes3D, false);

    windows.serviceA->SetElementVisible(VisFlags::Ruler, false);
    windows.serviceE->SetElementVisible(VisFlags::Ruler, false);

    windows.serviceB->SetElementVisible(VisFlags::Crosshair, true);
    windows.serviceC->SetElementVisible(VisFlags::Crosshair, true);
    windows.serviceD->SetElementVisible(VisFlags::Crosshair, true);
}

static void StartInitialVolumeLoad(
    const HostCoreServices& core,
    const HostRenderWindows& windows,
    const InitialVolumeLoadConfig& initialVolume,
    const HostHotkeyBindings& hotkeys)
{
    windows.serviceA->LoadFileAsync(
        initialVolume.filePath,
        initialVolume.spacing,
        initialVolume.origin,
        [
            sharedState = core.sharedState,
            sharedDataMgr = core.sharedDataMgr,
            serviceA = windows.serviceA,
            imageAnalysis = core.imageAnalysis,
            orthogonalCropBridge = core.orthogonalCropBridge,
            initialVolume,
            hotkeys
        ](bool success)
        {
            if (!success) {
                std::cerr << "[Host] Volume data load failed.\n";
                return;
            }

            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.55;
            serviceA->SetIsoThreshold(isoVal);

            auto histogramTable = imageAnalysis->GetHistogramData(initialVolume.histogramBinCount);
            if (histogramTable && histogramTable->GetNumberOfRows() > 0) {
                imageAnalysis->SaveHistogramImage(initialVolume.histogramFilePath, initialVolume.histogramBinCount);
            }
            else {
                std::cerr << "[Host] Histogram generation failed.\n";
            }

            orthogonalCropBridge->SetInputImage(sharedDataMgr->GetVtkImage());
            if (!orthogonalCropBridge->GetInputImage()) {
                std::cerr << "[Host] Orthogonal crop init failed: input image missing." << std::endl;
            }
            else {
                std::cout << "[Host] Crop tools armed. " << BuildStartupControlsText(hotkeys) << std::endl;
            }

            // GapAnalysis 不随加载自动进入；宿主收到对应命令后，再由 Timer 消费显式分析请求。
        }
    );
}

static void RenderAllWindows(const HostRenderWindows& windows)
{
    windows.contextA->Render();
    windows.contextB->Render();
    windows.contextC->Render();
    windows.contextD->Render();
    windows.contextE->Render();
}

static void InitializeAllInteractors(const HostRenderWindows& windows)
{
    windows.contextA->InitializeInteractor();
    windows.contextB->InitializeInteractor();
    windows.contextC->InitializeInteractor();
    windows.contextD->InitializeInteractor();
    windows.contextE->InitializeInteractor();
}

static bool HandleHostRenderContextHotkey(
    const HostHotkeyBindings& hotkeys,
    const std::shared_ptr<MedicalVizService>& service,
    vtkRenderWindowInteractor* interactor,
    StdRenderContext& context)
{
    const HostKeyInput keyInput = BuildHostKeyInput(interactor);
    if (MatchesCharacterKey(keyInput, hotkeys.modelTransformToggleKey)) {
        // 工具模式切换属于宿主输入映射：RenderContext 提供能力，具体键位不写进 core。
        context.SetToolMode(context.GetToolMode() == ToolMode::ModelTransform
            ? ToolMode::Navigation
            : ToolMode::ModelTransform);
        return true;
    }

    if (MatchesCharacterKey(keyInput, hotkeys.saveTransformedDataKey)) {
        if (service) {
            service->SaveTransformedDataAsync({});
        }
        return true;
    }

    if (MatchesCharacterKey(keyInput, hotkeys.saveSliceImagesKey)) {
        if (service) {
            service->SaveSliceImagesAsync({}, context.GetAngle());
        }
        return true;
    }

    if (MatchesKeySymbol(keyInput, hotkeys.exitKeySym)) {
        if (context.GetToolMode() == ToolMode::ModelTransform) {
            context.SetToolMode(ToolMode::Navigation);
            return true;
        }
        return false;
    }

    return false;
}

static void AttachHostRenderContextHotkey(
    const std::shared_ptr<StdRenderContext>& context,
    const std::shared_ptr<MedicalVizService>& service,
    const HostHotkeyBindings& hotkeys)
{
    if (!context) {
        return;
    }

    context->SetHostKeyEventHandler(
        [service, hotkeys](vtkRenderWindowInteractor* interactor, StdRenderContext& renderContext) {
            return HandleHostRenderContextHotkey(hotkeys, service, interactor, renderContext);
        });
}

static void AttachHostRenderContextHotkeys(
    const HostRenderWindows& windows,
    const HostHotkeyBindings& hotkeys)
{
    AttachHostRenderContextHotkey(windows.contextA, windows.serviceA, hotkeys);
    AttachHostRenderContextHotkey(windows.contextB, windows.serviceB, hotkeys);
    AttachHostRenderContextHotkey(windows.contextC, windows.serviceC, hotkeys);
    AttachHostRenderContextHotkey(windows.contextD, windows.serviceD, hotkeys);
    AttachHostRenderContextHotkey(windows.contextE, windows.serviceE, hotkeys);
}

static void AttachHostCommandHotkeyObserverToContext(
    const std::shared_ptr<StdRenderContext>& context,
    const vtkSmartPointer<HostCommandHotkeyObserver>& observer)
{
    if (!context || !context->GetInteractor()) {
        return;
    }

    context->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, observer, kHostCommandHotkeyObserverPriority);
    context->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, observer, kHostCommandHotkeyObserverPriority);
    context->GetInteractor()->AddObserver(vtkCommand::CharEvent, observer, kHostCommandHotkeyObserverPriority);
}

static bool GetRenderContextToolModeActive(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode)
{
    return context && context->GetToolMode() == mode;
}

static bool ExitRenderContextToolMode(
    const std::shared_ptr<StdRenderContext>& context,
    ToolMode mode)
{
    if (!GetRenderContextToolModeActive(context, mode)) {
        return false;
    }

    context->SetToolMode(ToolMode::Navigation);
    return true;
}

static std::function<bool()> BuildHostToolModeActiveHandler(const HostRenderWindows& windows)
{
    std::weak_ptr<StdRenderContext> contextA = windows.contextA;
    std::weak_ptr<StdRenderContext> contextB = windows.contextB;
    std::weak_ptr<StdRenderContext> contextC = windows.contextC;
    std::weak_ptr<StdRenderContext> contextD = windows.contextD;
    std::weak_ptr<StdRenderContext> contextE = windows.contextE;

    // observer 被 VTK interactor 持有；这里捕获 weak_ptr，避免宿主 session 关闭时形成引用环。
    return [contextA, contextB, contextC, contextD, contextE]() {
        return GetRenderContextToolModeActive(contextA.lock(), ToolMode::ModelTransform)
            || GetRenderContextToolModeActive(contextB.lock(), ToolMode::ModelTransform)
            || GetRenderContextToolModeActive(contextC.lock(), ToolMode::ModelTransform)
            || GetRenderContextToolModeActive(contextD.lock(), ToolMode::ModelTransform)
            || GetRenderContextToolModeActive(contextE.lock(), ToolMode::ModelTransform);
    };
}

static std::function<bool()> BuildExitHostToolModeHandler(const HostRenderWindows& windows)
{
    std::weak_ptr<StdRenderContext> contextA = windows.contextA;
    std::weak_ptr<StdRenderContext> contextB = windows.contextB;
    std::weak_ptr<StdRenderContext> contextC = windows.contextC;
    std::weak_ptr<StdRenderContext> contextD = windows.contextD;
    std::weak_ptr<StdRenderContext> contextE = windows.contextE;

    // observer 被 VTK interactor 持有；这里捕获 weak_ptr，避免宿主 session 关闭时形成引用环。
    return [contextA, contextB, contextC, contextD, contextE]() {
        bool exitedAnyToolMode = false;
        exitedAnyToolMode = ExitRenderContextToolMode(contextA.lock(), ToolMode::ModelTransform) || exitedAnyToolMode;
        exitedAnyToolMode = ExitRenderContextToolMode(contextB.lock(), ToolMode::ModelTransform) || exitedAnyToolMode;
        exitedAnyToolMode = ExitRenderContextToolMode(contextC.lock(), ToolMode::ModelTransform) || exitedAnyToolMode;
        exitedAnyToolMode = ExitRenderContextToolMode(contextD.lock(), ToolMode::ModelTransform) || exitedAnyToolMode;
        exitedAnyToolMode = ExitRenderContextToolMode(contextE.lock(), ToolMode::ModelTransform) || exitedAnyToolMode;
        return exitedAnyToolMode;
    };
}

static void AttachHostCommandHotkeys(
    const HostCoreServices& core,
    const HostRenderWindows& windows,
    const HostHotkeyBindings& hotkeys)
{
    auto observer = vtkSmartPointer<HostCommandHotkeyObserver>::New();
    observer->orthogonalCropBridge = core.orthogonalCropBridge;
    observer->gapAnalysisOverlayController = core.gapAnalysisOverlayController;
    observer->getHostToolModeActive = BuildHostToolModeActiveHandler(windows);
    observer->exitHostToolMode = BuildExitHostToolModeHandler(windows);
    observer->hotkeys = hotkeys;

    AttachHostCommandHotkeyObserverToContext(windows.contextA, observer);
    AttachHostCommandHotkeyObserverToContext(windows.contextB, observer);
    AttachHostCommandHotkeyObserverToContext(windows.contextC, observer);
    AttachHostCommandHotkeyObserverToContext(windows.contextD, observer);
    AttachHostCommandHotkeyObserverToContext(windows.contextE, observer);
}

static void AttachGapAnalysisTimer(
    const HostCoreServices& core,
    const HostRenderWindows& windows)
{
    auto observer = vtkSmartPointer<GapAnalysisOverlayCommitObserver>::New();
    observer->gapAnalysis = core.gapAnalysis;
    observer->serviceA = windows.serviceA;
    observer->gapAnalysisOverlayController = core.gapAnalysisOverlayController;
    observer->dataMgr = core.sharedDataMgr;
    observer->sharedState = core.sharedState;
    windows.contextB->GetInteractor()->AddObserver(vtkCommand::TimerEvent, observer, 0.2f);
}

static void PrintStartupControls(const HostHotkeyBindings& hotkeys)
{
    std::cout << "Application started. Loading data in background...\n"
        << BuildStartupControlsText(hotkeys) << '\n';
}

} // namespace

struct VtkAppHostSession::Impl {
    Config config;
    HostCoreServices core;
    HostWindowConfigs windowConfigs;
    HostRenderWindows windows;
    bool isInitialized = false;

    explicit Impl(Config sessionConfig)
        : config(std::move(sessionConfig))
    {
    }
};

VtkAppHostSession::VtkAppHostSession()
    : VtkAppHostSession(Config{})
{
}

VtkAppHostSession::VtkAppHostSession(Config config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

VtkAppHostSession::~VtkAppHostSession() = default;

VtkAppHostSession::VtkAppHostSession(VtkAppHostSession&&) noexcept = default;

VtkAppHostSession& VtkAppHostSession::operator=(VtkAppHostSession&&) noexcept = default;

void VtkAppHostSession::Initialize()
{
    if (m_impl->isInitialized) {
        return;
    }

    m_impl->core = BuildHostCoreServices(); //service 
    m_impl->windowConfigs = BuildWindowConfigs(); //windows
    m_impl->windows = BuildRenderWindows(m_impl->core, m_impl->windowConfigs); //service <-> render

    BindGapAnalysisDisplay(m_impl->core, m_impl->windows); // 孔隙
	BindOrthogonalCropFeature(m_impl->core, m_impl->windows); // 裁切
    AttachHostRenderContextHotkeys(m_impl->windows, m_impl->config.hotkeys); //render -> windows
    ConfigureInitialWindowVisibility(m_impl->windows); // 功能配置
    StartInitialVolumeLoad(
        m_impl->core,
        m_impl->windows,
        m_impl->config.initialVolume,
        m_impl->config.hotkeys);

    RenderAllWindows(m_impl->windows);
    InitializeAllInteractors(m_impl->windows);
    m_impl->core.orthogonalCropBridge->SetPrimaryInteractor(m_impl->windows.contextA->GetInteractor());

    AttachHostCommandHotkeys(m_impl->core, m_impl->windows, m_impl->config.hotkeys);
    AttachGapAnalysisTimer(m_impl->core, m_impl->windows);
    PrintStartupControls(m_impl->config.hotkeys);

    m_impl->isInitialized = true;
}

void VtkAppHostSession::Start()
{
    Initialize();
    if (m_impl->windows.contextB) {
        // contextB 持有当前独立 VTK host 的主事件循环；Qt / 上位机 host 后续可替换这一层。
        m_impl->windows.contextB->Start();
    }
}
