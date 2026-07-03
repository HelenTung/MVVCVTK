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
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr double kHostCommandHotkeyObserverPriority = 1.0;

using HostHotkeyBindings = VtkAppHostSession::HostHotkeyBindings;
using HostRenderViewConfig = VtkAppHostSession::HostRenderViewConfig;
using HostRenderViewEndpoint = VtkAppHostSession::HostRenderViewEndpoint;
using HostRenderViewRole = VtkAppHostSession::HostRenderViewRole;
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
            // VTK 的 CharEvent 仍会触发内建快捷键，例如裸数字键可能进入 stereo 行为。
            // 宿主管理的键先在这里截断，避免 feature 命令和 VTK 默认命令同时响应。
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
        // Escape 只退出当前活跃工具模式，不做全局关闭；顺序体现用户正在操作的显式模式优先级。
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
    struct RenderServiceBinding {
        HostRenderViewRole role = HostRenderViewRole::Auxiliary;
        std::shared_ptr<MedicalVizService> service;
    };

    void SetRenderViews(std::vector<RenderServiceBinding> renderViews)
    {
        // render view 拓扑可能由 Qt host 重新注入，替换目标前先移除旧 overlay，避免残留策略挂在旧窗口上。
        HideOverlays();
        m_renderViews = std::move(renderViews);
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
            // 第一次 toggle 是进入显示模式并显式请求分析；应用启动或数据加载完成本身不触发孔隙分析。
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

        // overlay 分发只看 role，不看窗口序号；这样 Qt host 可以增减窗口而不改孔隙分析链路。
        const vtkIdType meshPoints = m_voidMesh ? m_voidMesh->GetNumberOfPoints() : 0;
        const vtkIdType meshCells = m_voidMesh ? m_voidMesh->GetNumberOfCells() : 0;
        int labelDims[3] = { 0, 0, 0 };
        if (m_labelImage) {
            m_labelImage->GetDimensions(labelDims);
        }

        bool hasMeshOverlayInput = m_voidMesh && meshPoints > 0 && meshCells > 0;
        bool hasSliceOverlayInput = m_labelImage && labelDims[0] > 0 && labelDims[1] > 0 && labelDims[2] > 0;
        bool addedMeshOverlay = false;
        bool addedSliceOverlay = false;

        for (const auto& view : m_renderViews) {
            if (hasMeshOverlayInput && ShouldReceiveGapMeshOverlay(view.role)) {
                AddMeshOverlay(view.service, m_voidMesh);
                addedMeshOverlay = true;
            }

            const auto orientation = GetGapSliceOverlayOrientation(view.role);
            if (hasSliceOverlayInput && orientation) {
                AddSliceOverlay(view.service, m_labelImage, *orientation);
                addedSliceOverlay = true;
            }
        }

        if (!addedMeshOverlay) {
            std::cerr << "[Host] Gap Analysis produced no 3D void mesh overlay target." << std::endl;
        }
        if (!addedSliceOverlay) {
            std::cerr << "[Host] Gap Analysis produced no 2D label overlay target." << std::endl;
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

    static bool ShouldReceiveGapMeshOverlay(HostRenderViewRole role)
    {
        return role == HostRenderViewRole::Primary3D
            || role == HostRenderViewRole::Composite3D;
    }

    static std::optional<Orientation> GetGapSliceOverlayOrientation(HostRenderViewRole role)
    {
        switch (role) {
        case HostRenderViewRole::TopDownSlice:
            return Orientation::Top_down;
        case HostRenderViewRole::FrontBackSlice:
            return Orientation::Front_back;
        case HostRenderViewRole::LeftRightSlice:
            return Orientation::Left_right;
        default:
            return std::nullopt;
        }
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
    std::vector<RenderServiceBinding> m_renderViews;
    bool m_isAnalysisDisplayActive = false;
    bool m_shouldShowOverlay = false;
    bool m_hasPendingAnalysisRunRequest = false;
};

class GapAnalysisOverlayCommitObserver : public vtkCommand {
public:
    static GapAnalysisOverlayCommitObserver* New() { return new GapAnalysisOverlayCommitObserver; }

    std::shared_ptr<GapAnalysisService> gapAnalysis;
    std::shared_ptr<MedicalVizService> primaryService;
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
            // Timer 只响应宿主层留下的显式请求；这样分析启动条件和显示模式边界都集中在 host/session。
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
        if (primaryService) {
            primaryService->SetIsoThreshold(isoValue);
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
        vtkSmartPointer<vtkRenderWindow> renderWindow,
        std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> sharedState,
        std::shared_ptr<IStateEventSource> stateEventSource)
{
    auto service = std::make_shared<MedicalVizService>(std::move(dataMgr), std::move(sharedState), std::move(stateEventSource));
    auto context = std::make_shared<StdRenderContext>();

    if (renderWindow) {
        // 外部窗口必须先注入，再绑定 service；否则 service 会先拿到默认 window，后续 overlay 目标会短暂错位。
        context->SetRenderWindow(std::move(renderWindow));
    }
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
    // core services 不知道有几个窗口；窗口拓扑只在 host render view runtime 中建立。
    std::shared_ptr<RawVolumeDataManager> sharedDataMgr;
    std::shared_ptr<SharedStateBroadcaster> sharedStateBroadcaster;
    std::shared_ptr<SharedInteractionState> sharedState;
    std::shared_ptr<VolumeAnalysisService> imageAnalysis;
    std::shared_ptr<GapAnalysisService> gapAnalysis;
    std::shared_ptr<GapAnalysisOverlayController> gapAnalysisOverlayController;
    std::shared_ptr<OrthogonalCropInteractionBridgeService> orthogonalCropBridge;
};

struct HostRenderViewRuntime {
    // runtime 把一个宿主视图的配置、业务 service 和渲染 context 绑在一起，方便按 role 做后续装配。
    HostRenderViewConfig config;
    std::shared_ptr<MedicalVizService> service;
    std::shared_ptr<StdRenderContext> context;
};

struct HostRenderWindows {
    std::vector<HostRenderViewRuntime> views;
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

static std::string BuildFallbackRenderViewId(size_t index)
{
    return "view-" + std::to_string(index + 1);
}

static HostRenderViewConfig BuildRenderViewConfig(
    std::string id,
    HostRenderViewRole role,
    WindowConfig window,
    bool startStandaloneEventLoop = false)
{
    HostRenderViewConfig view;
    view.id = std::move(id);
    view.role = role;
    view.window = std::move(window);
    view.startStandaloneEventLoop = startStandaloneEventLoop;
    return view;
}

static std::vector<HostRenderViewConfig> BuildDefaultRenderViewConfigsImpl()
{
    std::vector<HostRenderViewConfig> views;
    const auto volTF = BuildVolumeTransferFunction();

    // 默认五视图保留原独立 VTK 调试入口的观察布局；真正的窗口数量仍由 Config::renderViews 决定。
    WindowConfig compositeVolume;
    compositeVolume.title = "Window E: Composite Volume";
    compositeVolume.width = 600; compositeVolume.height = 600;
    compositeVolume.posX = 660; compositeVolume.posY = 50;
    compositeVolume.preInitCfg.vizMode = VizMode::CompositeVolume;
    compositeVolume.preInitCfg.tfNodes = volTF;
    compositeVolume.preInitCfg.hasTF = true;
    compositeVolume.preInitCfg.bgColor = { 0.08, 0.08, 0.12 };
    compositeVolume.preInitCfg.hasBgColor = true;

    WindowConfig topDownSlice;
    topDownSlice.title = "Window B: Top_down Slice";
    topDownSlice.width = 400; topDownSlice.height = 400;
    topDownSlice.posX = 50;  topDownSlice.posY = 660;
    topDownSlice.preInitCfg.vizMode = VizMode::SliceTop_down;
    topDownSlice.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    topDownSlice.preInitCfg.hasBgColor = true;
    topDownSlice.preInitCfg.windowLevel = { 400.0, 40.0 };
    topDownSlice.preInitCfg.hasWindowLevel = true;

    WindowConfig frontBackSlice;
    frontBackSlice.title = "Window C: Front_back Slice";
    frontBackSlice.width = 400; frontBackSlice.height = 400;
    frontBackSlice.posX = 460; frontBackSlice.posY = 660;
    frontBackSlice.preInitCfg.vizMode = VizMode::SliceFront_back;
    frontBackSlice.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    frontBackSlice.preInitCfg.hasBgColor = true;
    frontBackSlice.preInitCfg.windowLevel = { 400.0, 40.0 };
    frontBackSlice.preInitCfg.hasWindowLevel = true;

    WindowConfig leftRightSlice;
    leftRightSlice.title = "Window D: Left_right Slice";
    leftRightSlice.width = 400; leftRightSlice.height = 400;
    leftRightSlice.posX = 870; leftRightSlice.posY = 660;
    leftRightSlice.preInitCfg.vizMode = VizMode::SliceLeft_right;
    leftRightSlice.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    leftRightSlice.preInitCfg.hasBgColor = true;
    leftRightSlice.preInitCfg.windowLevel = { 400.0, 40.0 };
    leftRightSlice.preInitCfg.hasWindowLevel = true;

    WindowConfig primary3D;
    primary3D.title = "Window A: Composite IsoSurface";
    primary3D.width = 600; primary3D.height = 600;
    primary3D.posX = 50;  primary3D.posY = 50;
    primary3D.showAxes = true;
    primary3D.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    primary3D.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    primary3D.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    primary3D.preInitCfg.hasBgColor = true;

    views.push_back(BuildRenderViewConfig("primary-3d", HostRenderViewRole::Primary3D, std::move(primary3D)));
    views.push_back(BuildRenderViewConfig("composite-volume", HostRenderViewRole::Composite3D, std::move(compositeVolume)));
    views.push_back(BuildRenderViewConfig("slice-top-down", HostRenderViewRole::TopDownSlice, std::move(topDownSlice), true));
    views.push_back(BuildRenderViewConfig("slice-front-back", HostRenderViewRole::FrontBackSlice, std::move(frontBackSlice)));
    views.push_back(BuildRenderViewConfig("slice-left-right", HostRenderViewRole::LeftRightSlice, std::move(leftRightSlice)));
    return views;
}

static HostRenderWindows BuildRenderWindows(
    const HostCoreServices& core,
    const std::vector<HostRenderViewConfig>& configs)
{
    HostRenderWindows windows;

    windows.views.reserve(configs.size());
    for (size_t index = 0; index < configs.size(); ++index) {
        HostRenderViewConfig config = configs[index];
        if (config.id.empty()) {
            // endpoint 需要稳定 id 供 Qt / 上位机回填 widget 映射；缺省 id 只用于本地调试兜底。
            config.id = BuildFallbackRenderViewId(index);
        }

        auto [service, context] = GetWindowPair(
            config.window,
            config.renderWindow,
            core.sharedDataMgr,
            core.sharedState,
            core.sharedStateBroadcaster);

        HostRenderViewRuntime view;
        view.config = std::move(config);
        view.service = std::move(service);
        view.context = std::move(context);
        windows.views.push_back(std::move(view));
    }
    return windows;
}

static bool GetRoleIs3DView(HostRenderViewRole role)
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D;
}

static bool GetRoleIsSliceView(HostRenderViewRole role)
{
    return role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

static const HostRenderViewRuntime* FindFirstRenderViewByRole(
    const HostRenderWindows& windows,
    HostRenderViewRole role)
{
    for (const auto& view : windows.views) {
        if (view.config.role == role) {
            return &view;
        }
    }
    return nullptr;
}

static const HostRenderViewRuntime* FindPrimaryRenderView(const HostRenderWindows& windows)
{
    // 先选明确 Primary3D，再退到任意 3D 视图；非标准窗口拓扑也要给裁切和初始加载一个可解释的参考视图。
    if (const auto* view = FindFirstRenderViewByRole(windows, HostRenderViewRole::Primary3D)) {
        return view;
    }

    for (const auto& view : windows.views) {
        if (GetRoleIs3DView(view.config.role)) {
            return &view;
        }
    }

    return windows.views.empty() ? nullptr : &windows.views.front();
}

static const HostRenderViewRuntime* FindStandaloneStartView(const HostRenderWindows& windows)
{
    // 独立 VTK host 只能由一个 interactor 进入阻塞主循环；Qt host 不调用 Start，因此不会被这里约束。
    for (const auto& view : windows.views) {
        if (view.config.startStandaloneEventLoop) {
            return &view;
        }
    }
    return windows.views.empty() ? nullptr : &windows.views.front();
}

static std::vector<std::shared_ptr<AbstractInteractiveService>> BuildCropPreviewRenderServices(
    const HostRenderWindows& windows)
{
    std::vector<std::shared_ptr<AbstractInteractiveService>> services;
    services.reserve(windows.views.size());
    for (const auto& view : windows.views) {
        if (view.config.includeInCropPreview && view.service) {
            services.push_back(view.service);
        }
    }
    return services;
}

static std::vector<GapAnalysisOverlayController::RenderServiceBinding> BuildGapAnalysisRenderBindings(
    const HostRenderWindows& windows)
{
    std::vector<GapAnalysisOverlayController::RenderServiceBinding> bindings;
    bindings.reserve(windows.views.size());
    for (const auto& view : windows.views) {
        if (view.service) {
            bindings.push_back({ view.config.role, view.service });
        }
    }
    return bindings;
}

static void BindGapAnalysisDisplay(
    const HostCoreServices& core,
    const HostRenderWindows& windows)
{
    // 孔隙分析只拿显示目标绑定，不拿窗口对象所有权；显隐和退出由 controller 自己维护缓存语义。
    core.gapAnalysisOverlayController->SetRenderViews(BuildGapAnalysisRenderBindings(windows));
}

static void BindOrthogonalCropFeature(
    const HostCoreServices& core,
    const HostRenderWindows& windows)
{
    auto bridge = core.orthogonalCropBridge;
    const auto* primaryView = FindPrimaryRenderView(windows);
    if (!primaryView || !primaryView->service || !primaryView->context) {
        std::cerr << "[Host] Orthogonal crop binding skipped: primary render view is missing." << std::endl;
        return;
    }

    // 裁切 reference view 按业务 role 选择，而不是按第几个窗口选择；后续 Qt 多/少窗口布局仍可复用同一 bridge。
    bridge->SetDataManager(core.sharedDataMgr);
    bridge->SetReferenceRenderService(primaryView->service);
    bridge->SetReferenceRenderer(primaryView->context->GetRenderer());
    bridge->SetPreviewRenderServices(BuildCropPreviewRenderServices(windows));

    // Submit reload 是裁切 feature 与宿主数据流的唯一写回能力；
    // 收在这里，后续上位机命令可复用 bridge，而不需要知道 reload 细节。
    bridge->SetSubmitReloadHandler(
        [
            sharedDataMgr = core.sharedDataMgr,
            sharedState = core.sharedState,
            primaryService = primaryView->service
        ](
            vtkSmartPointer<vtkImageData> image,
            std::function<void(bool success)> onComplete) {
            if (!sharedDataMgr || !sharedState || !primaryService || !image) {
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
            primaryService->ProcessPendingUpdates();

            if (onComplete) {
                onComplete(true);
            }
            return true;
        });
}

static void ConfigureInitialWindowVisibility(const HostRenderWindows& windows)
{
    // 初始可见性是视图角色策略，不是窗口编号策略；新增窗口只要声明 role 即可进入正确默认状态。
    for (const auto& view : windows.views) {
        if (!view.service) {
            continue;
        }

        if (GetRoleIs3DView(view.config.role)) {
            view.service->SetElementVisible(VisFlags::Planes3D, false);
            view.service->SetElementVisible(VisFlags::Ruler, false);
        }

        if (GetRoleIsSliceView(view.config.role)) {
            view.service->SetElementVisible(VisFlags::Crosshair, true);
        }
    }
}

static void StartInitialVolumeLoad(
    const HostCoreServices& core,
    const HostRenderWindows& windows,
    const InitialVolumeLoadConfig& initialVolume,
    const HostHotkeyBindings& hotkeys)
{
    const auto* primaryView = FindPrimaryRenderView(windows);
    if (!primaryView || !primaryView->service) {
        std::cerr << "[Host] Initial volume load skipped: primary render view is missing." << std::endl;
        return;
    }

    // 数据只加载一次到共享 DataManager；其他视图通过共享状态和 Timer 同步，不各自触发文件 I/O。
    primaryView->service->LoadFileAsync(
        initialVolume.filePath,
        initialVolume.spacing,
        initialVolume.origin,
        [
            sharedState = core.sharedState,
            sharedDataMgr = core.sharedDataMgr,
            primaryService = primaryView->service,
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
            primaryService->SetIsoThreshold(isoVal);

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
    for (const auto& view : windows.views) {
        if (view.context) {
            view.context->Render();
        }
    }
}

static void InitializeAllInteractors(const HostRenderWindows& windows)
{
    for (const auto& view : windows.views) {
        if (view.context) {
            view.context->InitializeInteractor();
        }
    }
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
    // RenderContext 只暴露按键转交点；当前独立 VTK host 在这里安装自己的功能键映射。
    for (const auto& view : windows.views) {
        AttachHostRenderContextHotkey(view.context, view.service, hotkeys);
    }
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
    std::vector<std::weak_ptr<StdRenderContext>> contexts;
    contexts.reserve(windows.views.size());
    for (const auto& view : windows.views) {
        contexts.push_back(view.context);
    }

    // observer 被 VTK interactor 持有；这里捕获 weak_ptr，避免宿主 session 关闭时形成引用环。
    return [contexts = std::move(contexts)]() {
        for (const auto& context : contexts) {
            if (GetRenderContextToolModeActive(context.lock(), ToolMode::ModelTransform)) {
                return true;
            }
        }
        return false;
    };
}

static std::function<bool()> BuildExitHostToolModeHandler(const HostRenderWindows& windows)
{
    std::vector<std::weak_ptr<StdRenderContext>> contexts;
    contexts.reserve(windows.views.size());
    for (const auto& view : windows.views) {
        contexts.push_back(view.context);
    }

    // observer 被 VTK interactor 持有；这里捕获 weak_ptr，避免宿主 session 关闭时形成引用环。
    return [contexts = std::move(contexts)]() {
        bool exitedAnyToolMode = false;
        for (const auto& context : contexts) {
            exitedAnyToolMode = ExitRenderContextToolMode(context.lock(), ToolMode::ModelTransform) || exitedAnyToolMode;
        }
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

    // 同一个 observer 挂到所有窗口，保证 Escape / 裁切 / 孔隙显示命令在任意当前焦点窗口都能生效。
    for (const auto& view : windows.views) {
        AttachHostCommandHotkeyObserverToContext(view.context, observer);
    }
}

static void AttachGapAnalysisTimer(
    const HostCoreServices& core,
    const HostRenderWindows& windows)
{
    const auto* primaryView = FindPrimaryRenderView(windows);
    const auto* timerView = FindStandaloneStartView(windows);
    if (!timerView || !timerView->context || !timerView->context->GetInteractor()) {
        std::cerr << "[Host] Gap Analysis timer skipped: timer render view is missing." << std::endl;
        return;
    }

    // Timer 挂在独立 VTK 主循环承载视图上；Qt host 后续应由自己的事件循环驱动同等宿主命令链路。
    auto observer = vtkSmartPointer<GapAnalysisOverlayCommitObserver>::New();
    observer->gapAnalysis = core.gapAnalysis;
    observer->primaryService = primaryView ? primaryView->service : nullptr;
    observer->gapAnalysisOverlayController = core.gapAnalysisOverlayController;
    observer->dataMgr = core.sharedDataMgr;
    observer->sharedState = core.sharedState;
    timerView->context->GetInteractor()->AddObserver(vtkCommand::TimerEvent, observer, 0.2f);
}

static std::vector<HostRenderViewEndpoint> BuildRenderViewEndpoints(const HostRenderWindows& windows)
{
    std::vector<HostRenderViewEndpoint> endpoints;
    endpoints.reserve(windows.views.size());
    for (const auto& view : windows.views) {
        // endpoint 是非拥有观察句柄，给外部 host 做 widget 映射；service 仍留在 session 内部维持边界。
        HostRenderViewEndpoint endpoint;
        endpoint.id = view.config.id;
        endpoint.role = view.config.role;
        endpoint.renderer = view.context ? view.context->GetRenderer() : nullptr;
        endpoint.renderWindow = view.context ? view.context->GetRenderWindow() : nullptr;
        endpoint.interactor = view.context ? view.context->GetInteractor() : nullptr;
        endpoints.push_back(endpoint);
    }
    return endpoints;
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
    HostRenderWindows windows;
    std::vector<HostRenderViewEndpoint> renderViewEndpoints;
    bool isInitialized = false;

    explicit Impl(Config sessionConfig)
        : config(std::move(sessionConfig))
    {
        if (config.renderViews.empty()) {
            // 空配置保持历史调试入口可直接运行；传入 renderViews 时则完全尊重外部宿主拓扑。
            config.renderViews = VtkAppHostSession::BuildDefaultRenderViewConfigs();
        }
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

std::vector<VtkAppHostSession::HostRenderViewConfig> VtkAppHostSession::BuildDefaultRenderViewConfigs()
{
    return BuildDefaultRenderViewConfigsImpl();
}

void VtkAppHostSession::Initialize()
{
    if (m_impl->isInitialized) {
        return;
    }

    // 1. 先创建与窗口无关的 core services，再按宿主配置创建 render view runtime。
    // 2. feature 绑定只拿稳定 service/context/role，不知道具体窗口数量或 Qt widget。
    // 3. endpoint 在 interactor 初始化后生成，保证外部 host 拿到的是最终窗口句柄。
    m_impl->core = BuildHostCoreServices();
    m_impl->windows = BuildRenderWindows(m_impl->core, m_impl->config.renderViews);

    BindGapAnalysisDisplay(m_impl->core, m_impl->windows);
    BindOrthogonalCropFeature(m_impl->core, m_impl->windows);
    AttachHostRenderContextHotkeys(m_impl->windows, m_impl->config.hotkeys);
    ConfigureInitialWindowVisibility(m_impl->windows);
    StartInitialVolumeLoad(
        m_impl->core,
        m_impl->windows,
        m_impl->config.initialVolume,
        m_impl->config.hotkeys);

    RenderAllWindows(m_impl->windows);
    InitializeAllInteractors(m_impl->windows);
    if (const auto* primaryView = FindPrimaryRenderView(m_impl->windows)) {
        if (primaryView->context) {
            m_impl->core.orthogonalCropBridge->SetPrimaryInteractor(primaryView->context->GetInteractor());
        }
    }
    m_impl->renderViewEndpoints = BuildRenderViewEndpoints(m_impl->windows);

    AttachHostCommandHotkeys(m_impl->core, m_impl->windows, m_impl->config.hotkeys);
    AttachGapAnalysisTimer(m_impl->core, m_impl->windows);
    PrintStartupControls(m_impl->config.hotkeys);

    m_impl->isInitialized = true;
}

void VtkAppHostSession::Start()
{
    Initialize();
    if (const auto* startView = FindStandaloneStartView(m_impl->windows)) {
        if (startView->context) {
            // 独立 VTK host 需要一个 interactor 承载主循环；Qt host 可只调用 Initialize 后接管 endpoints。
            startView->context->Start();
        }
    }
}

const std::vector<VtkAppHostSession::HostRenderViewEndpoint>& VtkAppHostSession::GetRenderViewEndpoints() const
{
    return m_impl->renderViewEndpoints;
}

const VtkAppHostSession::HostRenderViewEndpoint* VtkAppHostSession::GetRenderViewEndpoint(
    const std::string& id) const
{
    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.id == id) {
            return &endpoint;
        }
    }
    return nullptr;
}

const VtkAppHostSession::HostRenderViewEndpoint* VtkAppHostSession::GetPrimaryRenderViewEndpoint() const
{
    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.role == VtkAppHostSession::HostRenderViewRole::Primary3D) {
            return &endpoint;
        }
    }

    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.role == VtkAppHostSession::HostRenderViewRole::Composite3D) {
            return &endpoint;
        }
    }

    return m_impl->renderViewEndpoints.empty() ? nullptr : &m_impl->renderViewEndpoints.front();
}
