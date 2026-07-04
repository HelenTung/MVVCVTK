#include "Host/HostFeatureBindings.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppState.h"
#include "DataManager.h"
#include "Interaction/OrthogonalCropInteractionBridgeService.h"
#include "Render/Strategies/GapAlgorithmOverlayStrategies.h"
#include "Services/GapAnalysisService.h"
#include "StdRenderContext.h"
#include "OrthogonalCropTypes.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// 孔隙显示控制器接口是 host 内部状态机边界：
// public 命令负责进入/退出/显隐，Timer observer 负责提交异步算法结果，二者通过这个接口共享同一组状态。
class IGapAnalysisDisplayController {
public:
    virtual ~IGapAnalysisDisplayController() = default;

    virtual bool GetAnalysisDisplayActive() const = 0;
    virtual bool GetAnalysisRunRequested() const = 0;
    virtual void ClearAnalysisRunRequest() = 0;
    virtual bool ActivateAnalysisDisplay(const HostGapAnalysisActivationRequest& request) = 0;
    virtual bool ToggleAnalysisOverlayVisibility() = 0;
    virtual bool ExitAnalysisDisplay() = 0;
};

namespace {
constexpr double kHostCommandHotkeyObserverPriority = 1.0;

// standalone observer 的按键快照。VTK 的 KeyCode、KeySym 和 ControlKey 分散在 interactor 上，
// 先归一化成 host 输入结构，后面才能把“裸数字键拦截”和“Ctrl+提交”写清楚。
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
    if (key == 0) {
        // 0 表示 host 没有分配这个调试键；这里直接不匹配，避免未配置键位被 VTK 空 KeyCode 误触发。
        return false;
    }

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

static std::function<bool()> BuildHostToolModeActiveHandler(
    const std::vector<const HostRenderViewRuntime*>& targetViews)
{
    std::vector<std::weak_ptr<StdRenderContext>> contexts;
    contexts.reserve(targetViews.size());
    for (const auto* view : targetViews) {
        if (view) {
            contexts.push_back(view->context);
        }
    }

    // observer 被 VTK interactor 持有；这里捕获 weak_ptr，避免宿主 session 关闭时形成引用环。
    // 为什么不捕获 HostRenderViewSet 指针：observer 只需要知道目标窗口中是否仍有 host tool mode 活跃。
    return [contexts = std::move(contexts)]() {
        for (const auto& context : contexts) {
            if (GetRenderContextToolModeActive(context.lock(), ToolMode::ModelTransform)) {
                return true;
            }
        }
        return false;
    };
}

static std::function<bool()> BuildExitHostToolModeHandler(
    const std::vector<const HostRenderViewRuntime*>& targetViews)
{
    std::vector<std::weak_ptr<StdRenderContext>> contexts;
    contexts.reserve(targetViews.size());
    for (const auto* view : targetViews) {
        if (view) {
            contexts.push_back(view->context);
        }
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

static bool ShouldReceiveGapMeshOverlay(HostRenderViewRole role)
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D;
}

static CropRemovalMode GetCropRemovalMode(HostCropPreviewMode previewMode)
{
    switch (previewMode) {
    case HostCropPreviewMode::KeepInside:
        return CropRemovalMode::KeepInside;
    case HostCropPreviewMode::RemoveInside:
        return CropRemovalMode::RemoveInside;
    default:
        return CropRemovalMode::KeepInside;
    }
}

class GapAnalysisOverlayController final : public IGapAnalysisDisplayController {
public:
    // RenderServiceBinding 是孔隙 overlay 的投递目标快照：
    // role 决定采用 3D mesh 还是 2D slice 策略，service 是实际接收 overlay 的窗口服务。
    struct RenderServiceBinding {
        HostRenderViewRole role = HostRenderViewRole::Auxiliary;
        std::shared_ptr<MedicalVizService> service;
    };

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

    bool ActivateAnalysisDisplay(const HostGapAnalysisActivationRequest& request) override
    {
        if (request.targetViewIds.empty()
            && request.targetViewRoles.empty()
            && !request.useDefaultOverlayRoles) {
            std::cerr << "[Host] Gap Analysis display activation skipped: no render view target was requested." << std::endl;
            return false;
        }

        // 进入显示模式和选择窗口目标是同一个宿主命令边界；初始化阶段不预绑窗口，避免默认进入孔隙分析链路。
        // 这里先隐藏旧 overlay，是因为同一模式可以被上位机重新指定目标窗口，旧窗口残留会让“当前命令作用域”变得含糊。
        m_isAnalysisDisplayActive = true;
        m_shouldShowOverlay = true;
        m_hasPendingAnalysisRunRequest = true;
        m_voidMesh = nullptr;
        m_labelImage = nullptr;
        HideOverlays();
        return true;
    }

    void SetRenderViews(std::vector<RenderServiceBinding> renderViews)
    {
        // render view 拓扑可能由 Qt host 重新注入，替换目标前先移除旧 overlay，避免残留策略挂在旧窗口上。
        HideOverlays();
        m_renderViews = std::move(renderViews);
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
            std::cerr << "[Host] Gap Analysis overlay toggle ignored: display mode is not active." << std::endl;
            return false;
        }

        m_shouldShowOverlay = !m_shouldShowOverlay;
        // J 再次按下只是显隐 overlay，不退出 display mode；这样用户可以临时看原图，再恢复同一次分析结果。
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
        // Escape 语义是彻底退出孔隙显示模式，所以 pending 请求、缓存结果和 overlay 策略都一起清掉。
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
    // OverlayBinding 保存“哪个 service 加了哪个 strategy”，这是 HideOverlays 能精确移除的最小单位。
    // 不反向扫描 service，是为了避免 host 层依赖渲染服务内部 overlay 容器结构。
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
        // A. 3D role 接收 void mesh，用于整体孔隙空间观察。
        // B. slice role 接收 label image，并按 role 映射到对应切片方向。
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
    // m_overlayBindings 是当前显示出来的 overlay 句柄集合，HideOverlays 会清空它。
    std::vector<OverlayBinding> m_overlayBindings;
    // m_voidMesh 是最近一次孔隙分析生成的 3D mesh 缓存；隐藏 overlay 时保留，退出 display mode 时清除。
    vtkSmartPointer<vtkPolyData> m_voidMesh;
    // m_labelImage 是最近一次孔隙分析生成的 slice label 缓存，供切片 overlay 恢复显示。
    vtkSmartPointer<vtkImageData> m_labelImage;
    // m_renderViews 是本次显示命令解析出的目标窗口快照；重新激活时会替换并先清理旧 overlay。
    std::vector<RenderServiceBinding> m_renderViews;
    // m_isAnalysisDisplayActive 表示“孔隙显示模式”是否存在；只有 active 时 Timer 才能启动/提交算法。
    bool m_isAnalysisDisplayActive = false;
    // m_shouldShowOverlay 表示 active 模式下当前是否可见；false 仍保留结果以支持再次显示。
    bool m_shouldShowOverlay = false;
    // m_hasPendingAnalysisRunRequest 是 host 命令交给 Timer 的一次性启动信号，避免激活时直接依赖数据加载线程。
    bool m_hasPendingAnalysisRunRequest = false;
};

// Timer observer 是 VTK 事件循环到孔隙算法结果的提交桥。
// 它不决定进入模式，也不决定显示目标；这些都由 GapAnalysisOverlayController 和激活请求控制。
class GapAnalysisOverlayCommitObserver final : public vtkCommand {
public:
    static GapAnalysisOverlayCommitObserver* New() { return new GapAnalysisOverlayCommitObserver; }

    // 以下依赖由 HostFeatureBindings 注入，observer 本身不创建服务，避免事件循环对象拥有业务生命周期。
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
        if (state == GapAnalysisState::Idle || state == GapAnalysisState::Running) {
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
    // 每次分析完成只提交一次 overlay；重新激活或重新开始分析时重置。
    bool m_completionHandled = false;
    // 失败日志只输出一次，避免 TimerEvent 持续刷屏掩盖真实错误。
    bool m_failureLogged = false;

    bool TryStartAnalysis()
    {
        if (!sharedState || sharedState->GetFileLoadState() != LoadState::Succeeded) {
            // 孔隙分析消费的是已经进入共享 DataManager 的体数据；等待 DataReady 可避免算法直接依赖文件加载线程。
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
        // 这些阈值仍是当前孔隙算法的经验默认值，放在 host 绑定层只是为了先保留独立调试行为。
        // 后续接上位机时应改成请求参数或算法配置，避免 host 长期承担算法策略职责。
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

// HostCommandHotkeyObserver 只属于 standalone 调试输入层。
// 它把固定键位转换成 VtkAppHostSession 暴露的稳定命令；真实上位机不需要安装这个 observer。
class HostCommandHotkeyObserver final : public vtkCommand {
public:
    static HostCommandHotkeyObserver* New() { return new HostCommandHotkeyObserver; }

    // weak_ptr 防止 VTK interactor 比 session 活得更久时回调悬挂对象。
    std::weak_ptr<HostFeatureBindings> featureBindings;
    // 用函数注入 RenderContext 状态，避免 feature observer 直接依赖 HostRenderViewSet 或遍历窗口集合。
    std::function<bool()> getHostToolModeActive;
    std::function<bool()> exitHostToolMode;
    // hotkey 被触发后使用的 feature 目标请求；监听窗口和作用窗口由 HostCommandInputConfig 分开提供。
    HostOrthogonalCropActivationRequest orthogonalCropRequest;
    HostGapAnalysisActivationRequest gapAnalysisRequest;
    // standalone 键位配置。这里保存的是宿主输入协议，不是 feature 行为默认值。
    HostHotkeyBindings hotkeys;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        (void)callData;
        this->AbortFlagOff();
        auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
        auto bindings = featureBindings.lock();
        if (!iren || !bindings) {
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
        const bool canHandleExitCommand = isExitKey && GetAnyExitTargetActive();
        const bool isManagedHotkey =
            isCropToggleKey
            || isPlanarCropToggleKey
            || isInsidePreviewKey
            || isOutsidePreviewKey
            || isSubmitKey
            || shouldBlockVtkStereoKey
            || isGapOverlayToggleKey
            || canHandleExitCommand;

        if (eventId == vtkCommand::CharEvent && isManagedHotkey) {
            // VTK 的 CharEvent 仍会触发内建快捷键，例如裸数字键可能进入 stereo 行为。
            // 宿主管理的键先在这里截断，避免 feature 命令和 VTK 默认命令同时响应。
            this->AbortFlagOn();
            return;
        }

        if (eventId == vtkCommand::KeyPressEvent) {
            if (isCropToggleKey && !m_cropToggleKeyDown) {
                m_cropToggleKeyDown = true;
                bindings->ToggleInteractiveCrop(orthogonalCropRequest);
                this->AbortFlagOn();
                return;
            }

            if (isPlanarCropToggleKey && !m_planarCropToggleKeyDown) {
                m_planarCropToggleKeyDown = true;
                bindings->ToggleInteractivePlanarCrop(orthogonalCropRequest);
                this->AbortFlagOn();
                return;
            }

            if (isGapOverlayToggleKey && !m_gapOverlayToggleKeyDown) {
                m_gapOverlayToggleKeyDown = true;
                // J 的第一次按下进入 display mode，后续按下只切换 overlay 显隐；Escape 才彻底退出 display mode。
                if (!bindings->GetGapAnalysisDisplayActive()) {
                    bindings->ActivateGapAnalysisDisplay(gapAnalysisRequest);
                }
                else {
                    bindings->ToggleGapAnalysisOverlayVisibility();
                }
                this->AbortFlagOn();
                return;
            }

            if (isExitKey && ExitActiveHostMode()) {
                this->AbortFlagOn();
                return;
            }

            if (isInsidePreviewKey) {
                bindings->ToggleCropPreview(orthogonalCropRequest, HostCropPreviewMode::KeepInside);
                this->AbortFlagOn();
                return;
            }

            if (isOutsidePreviewKey) {
                bindings->ToggleCropPreview(orthogonalCropRequest, HostCropPreviewMode::RemoveInside);
                this->AbortFlagOn();
                return;
            }

            if (isSubmitKey && !m_submitKeyDown) {
                m_submitKeyDown = true;
                bindings->ApplyCropSubmit(orthogonalCropRequest);
                this->AbortFlagOn();
                return;
            }

            if (isManagedHotkey) {
                this->AbortFlagOn();
                return;
            }
        }

        if (eventId == vtkCommand::KeyReleaseEvent) {
            if (isCropToggleKey) {
                m_cropToggleKeyDown = false;
                this->AbortFlagOn();
                return;
            }

            if (isPlanarCropToggleKey) {
                m_planarCropToggleKeyDown = false;
                this->AbortFlagOn();
                return;
            }

            if (isGapOverlayToggleKey) {
                m_gapOverlayToggleKeyDown = false;
                this->AbortFlagOn();
                return;
            }

            if (isInsidePreviewKey || isOutsidePreviewKey) {
                this->AbortFlagOn();
                return;
            }

            if (isSubmitKeyCode) {
                m_submitKeyDown = false;
                this->AbortFlagOn();
                return;
            }
        }
    }

private:
    // VTK 可能对同一物理按键连续发送 KeyPressEvent；这些状态位让 Toggle/Submit 只在按下边沿触发一次。
    bool m_cropToggleKeyDown = false;
    bool m_planarCropToggleKeyDown = false;
    bool m_gapOverlayToggleKeyDown = false;
    bool m_submitKeyDown = false;

    bool GetAnyExitTargetActive() const
    {
        auto bindings = featureBindings.lock();
        if (bindings && bindings->GetInteractiveCropActive()) {
            return true;
        }
        if (bindings && bindings->GetGapAnalysisDisplayActive()) {
            return true;
        }
        return getHostToolModeActive && getHostToolModeActive();
    }

    bool ExitActiveHostMode()
    {
        auto bindings = featureBindings.lock();
        // Escape 只退出当前活跃工具模式，不做全局关闭；顺序体现用户正在操作的显式模式优先级。
        if (bindings && bindings->GetInteractiveCropActive()) {
            bindings->ExitInteractiveCrop();
            return true;
        }
        if (bindings && bindings->GetGapAnalysisDisplayActive()) {
            bindings->ExitGapAnalysisDisplay();
            return true;
        }
        if (getHostToolModeActive && getHostToolModeActive() && exitHostToolMode) {
            exitHostToolMode();
            return true;
        }
        return false;
    }
};

static std::vector<GapAnalysisOverlayController::RenderServiceBinding> BuildGapAnalysisRenderBindings(
    const std::vector<const HostRenderViewRuntime*>& views)
{
    // 只把 overlay 需要的 role + service 投给控制器，避免控制器看到 id、context、interactor 等 host 拓扑细节。
    std::vector<GapAnalysisOverlayController::RenderServiceBinding> bindings;
    bindings.reserve(views.size());
    for (const auto* view : views) {
        if (view && view->service) {
            bindings.push_back({ view->config.role, view->service });
        }
    }
    return bindings;
}

static void AttachHostCommandHotkeyObserverToContext(
    const std::shared_ptr<StdRenderContext>& context,
    const vtkSmartPointer<HostCommandHotkeyObserver>& observer)
{
    if (!context || !context->GetInteractor()) {
        return;
    }

    // 同一个 observer 同时监听 press/release/char：
    // 1. press 负责触发命令。
    // 2. release 负责复位边沿状态。
    // 3. char 负责阻断 VTK 内建快捷键和 host 命令双响应。
    context->GetInteractor()->AddObserver(vtkCommand::KeyPressEvent, observer, kHostCommandHotkeyObserverPriority);
    context->GetInteractor()->AddObserver(vtkCommand::KeyReleaseEvent, observer, kHostCommandHotkeyObserverPriority);
    context->GetInteractor()->AddObserver(vtkCommand::CharEvent, observer, kHostCommandHotkeyObserverPriority);
}
}

void HostFeatureBindings::RegisterFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_core = core;
    m_renderViews = &renderViews;
    m_gapAnalysisOverlayController = std::make_shared<GapAnalysisOverlayController>();
    if (m_core.orthogonalCropBridge) {
        // submit 后需要刷新所有现有视图的共享数据，但不应该把“哪些窗口参与 preview”的语义写进 reload。
        // 因此这里保存弱引用集合，只做数据更新通知；窗口消失时 weak_ptr 自然失效。
        std::vector<std::weak_ptr<MedicalVizService>> renderViewServices;
        renderViewServices.reserve(renderViews.GetViews().size());
        for (const auto& view : renderViews.GetViews()) {
            renderViewServices.push_back(view.service);
        }

        // DataManager 与 submit reload 是裁切 feature 的稳定能力边界；具体窗口目标等激活请求到来后再绑定。
        // submit 回调只提交新图像和刷新共享状态，不持有窗口角色，避免裁切结果反向耦合到某一组视图。
        m_core.orthogonalCropBridge->SetDataManager(m_core.sharedDataMgr);
        m_core.orthogonalCropBridge->SetSubmitReloadHandler(
            [
                sharedDataMgr = m_core.sharedDataMgr,
                sharedState = m_core.sharedState,
                renderViewServices = std::move(renderViewServices)
            ](
                vtkSmartPointer<vtkImageData> image,
                std::function<void(bool success)> onComplete) {
                if (!sharedDataMgr || !sharedState || !image) {
                    return false;
                }

                if (sharedState->GetFileLoadState() == LoadState::Loading
                    || sharedState->GetReloadLoadState() == LoadState::Loading)
                {
                    // reload 必须串行，否则裁切 submit 产生的新 image 和后台加载 image 可能抢同一个 shared DataManager。
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
                for (const auto& service : renderViewServices) {
                    if (const auto lockedService = service.lock()) {
                        lockedService->ProcessPendingUpdates();
                    }
                }

                if (onComplete) {
                    onComplete(true);
                }
                return true;
            });
    }
}

bool HostFeatureBindings::ActivateOrthogonalCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    // 裁切至少需要一个 reference view，因为 widget interactor、renderer 和输入模型坐标都来自这一路。
    // preview view 可以为空；那只意味着不显示预览，不影响 reference 链路边界。
    if (request.referenceViewId.empty() && !request.useReferenceRole) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view was not specified." << std::endl;
        return false;
    }

    return ConfigureOrthogonalCrop(request);
}

bool HostFeatureBindings::ActivateGapAnalysisDisplay(
    const HostGapAnalysisActivationRequest& request)
{
    // 激活孔隙显示必须先解析目标窗口；如果这里允许空目标，Timer 完成后就只能猜测要全局显示还是丢弃结果。
    if (!m_renderViews || !m_gapAnalysisOverlayController) {
        std::cerr << "[Host] Gap Analysis display activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }

    HostGapAnalysisActivationRequest resolvedRequest = request;
    std::vector<const HostRenderViewRuntime*> targetViews =
        m_renderViews->GetViewsByIdsAndRoles(resolvedRequest.targetViewIds, resolvedRequest.targetViewRoles);
    if (targetViews.empty() && resolvedRequest.useDefaultOverlayRoles) {
        // 默认 overlay 角色必须由请求显式打开；空 ids/roles 本身不等于“所有窗口”，避免漏配目标时全局接管。
        targetViews = m_renderViews->GetDefaultGapOverlayViews();
    }
    if (targetViews.empty()) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no overlay render view target was found." << std::endl;
        return false;
    }

    auto controller = std::dynamic_pointer_cast<GapAnalysisOverlayController>(m_gapAnalysisOverlayController);
    if (!controller) {
        return false;
    }
    // 每次激活都重新绑定目标窗口，使上位机可以在运行期切换 overlay 目标而不重建 session。
    controller->SetRenderViews(BuildGapAnalysisRenderBindings(targetViews));
    const bool activated = controller->ActivateAnalysisDisplay(resolvedRequest);
    if (activated) {
        std::cout << "[Host] Gap Analysis display mode requested. Analysis will start after volume data is ready." << std::endl;
    }
    return activated;
}

bool HostFeatureBindings::ToggleGapAnalysisOverlayVisibility()
{
    return m_gapAnalysisOverlayController
        && m_gapAnalysisOverlayController->ToggleAnalysisOverlayVisibility();
}

bool HostFeatureBindings::ExitGapAnalysisDisplay()
{
    return m_gapAnalysisOverlayController
        && m_gapAnalysisOverlayController->ExitAnalysisDisplay();
}

bool HostFeatureBindings::GetGapAnalysisDisplayActive() const
{
    return m_gapAnalysisOverlayController
        && m_gapAnalysisOverlayController->GetAnalysisDisplayActive();
}

bool HostFeatureBindings::ToggleInteractiveCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    // Toggle 前先 Activate，是为了让上位机切换窗口布局后，裁切 bridge 始终使用最新 reference/preview 目标。
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }
    return m_core.orthogonalCropBridge->ToggleInteractiveCrop();
}

bool HostFeatureBindings::ToggleInteractivePlanarCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }
    return m_core.orthogonalCropBridge->ToggleInteractivePlanarCrop();
}

bool HostFeatureBindings::ToggleCropPreview(
    const HostOrthogonalCropActivationRequest& request,
    HostCropPreviewMode previewMode)
{
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }

    m_core.orthogonalCropBridge->TogglePreview(GetCropRemovalMode(previewMode));
    return true;
}

bool HostFeatureBindings::ApplyCropSubmit(
    const HostOrthogonalCropActivationRequest& request)
{
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }

    m_core.orthogonalCropBridge->ApplySubmit();
    return true;
}

bool HostFeatureBindings::ExitInteractiveCrop()
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->ExitInteractiveCrop();
}

bool HostFeatureBindings::GetInteractiveCropActive() const
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->GetInteractiveCropActive();
}

std::function<bool()> HostFeatureBindings::BuildOrthogonalCropInputRefreshHandler() const
{
    // 返回函数而不是立即刷新，是因为初始加载异步完成后才有 vtkImageData；
    // session 可以把同一刷新点传给加载回调，避免裁切链路自己监听文件 I/O。
    return [
        orthogonalCropBridge = m_core.orthogonalCropBridge,
        sharedDataMgr = m_core.sharedDataMgr
    ]() {
        if (!orthogonalCropBridge || !sharedDataMgr) {
            return false;
        }

        orthogonalCropBridge->SetInputImage(sharedDataMgr->GetVtkImage());
        if (!orthogonalCropBridge->GetInputImage()) {
            std::cerr << "[Host] Orthogonal crop init failed: input image missing." << std::endl;
            return false;
        }
        return true;
    };
}

bool HostFeatureBindings::RefreshOrthogonalCropInputImage()
{
    auto refreshInput = BuildOrthogonalCropInputRefreshHandler();
    if (!refreshInput) {
        return false;
    }
    return refreshInput();
}

void HostFeatureBindings::AttachStandaloneHotkeys(
    const HostCommandInputConfig& inputConfig,
    const HostHotkeyBindings& hotkeys)
{
    // standalone hotkey 是可选适配层；关闭时，所有 feature 仍可由 VtkAppHostSession public 命令驱动。
    if (!inputConfig.enableStandaloneHotkeys || !m_renderViews) {
        return;
    }

    std::vector<const HostRenderViewRuntime*> targetViews =
        m_renderViews->GetViewsByIdsAndRoles(inputConfig.targetViewIds, inputConfig.targetViewRoles);
    if (targetViews.empty()) {
        std::cerr << "[Host] Standalone feature hotkeys skipped: no target render view was requested." << std::endl;
        return;
    }

    auto observer = vtkSmartPointer<HostCommandHotkeyObserver>::New();
    const auto self = weak_from_this().lock();
    if (!self) {
        std::cerr << "[Host] Standalone feature hotkeys skipped: feature bindings are not owned by session." << std::endl;
        return;
    }

    // VTK observer 由 interactor 持有，不能保存强 shared_ptr 指回 HostFeatureBindings；weak_ptr 可让 session 生命周期单向结束。
    observer->featureBindings = self;
    observer->getHostToolModeActive = BuildHostToolModeActiveHandler(targetViews);
    observer->exitHostToolMode = BuildExitHostToolModeHandler(targetViews);
    observer->orthogonalCropRequest = inputConfig.orthogonalCropRequest;
    observer->gapAnalysisRequest = inputConfig.gapAnalysisRequest;
    observer->hotkeys = hotkeys;

    // hotkey 绑定范围由 host config 或上位机决定；空范围直接跳过，避免 feature 默认接管所有窗口。
    for (const auto* view : targetViews) {
        if (view) {
            AttachHostCommandHotkeyObserverToContext(view->context, observer);
        }
    }
}

void HostFeatureBindings::AttachGapAnalysisTimer()
{
    if (!m_renderViews) {
        return;
    }

    const auto* primaryView = m_renderViews->GetPrimaryView();
    const auto* timerView = m_renderViews->GetStandaloneStartView();
    if (!timerView || !timerView->context || !timerView->context->GetInteractor()) {
        std::cerr << "[Host] Gap Analysis timer skipped: timer render view is missing." << std::endl;
        return;
    }

    auto controller = std::dynamic_pointer_cast<GapAnalysisOverlayController>(m_gapAnalysisOverlayController);
    if (!controller) {
        return;
    }

    // Timer 挂在当前 host 选择的事件循环承载视图上；它只消费显式请求，不代表默认进入孔隙分析模式。
    // 这样 Qt host 将来可以换事件循环承载点，而不用把孔隙算法写进窗口创建流程。
    auto observer = vtkSmartPointer<GapAnalysisOverlayCommitObserver>::New();
    observer->gapAnalysis = m_core.gapAnalysis;
    observer->primaryService = primaryView ? primaryView->service : nullptr;
    observer->gapAnalysisOverlayController = controller;
    observer->dataMgr = m_core.sharedDataMgr;
    observer->sharedState = m_core.sharedState;
    timerView->context->GetInteractor()->AddObserver(vtkCommand::TimerEvent, observer, 0.2f);
}

bool HostFeatureBindings::ConfigureOrthogonalCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    // 这一步是 host 窗口语义到裁切 bridge 的唯一转换点：
    // 1. reference view 提供 renderer/interactor，并决定 widget 所在坐标语境。
    // 2. preview views 只提供 AbstractInteractiveService，用于显示预览和刷新 dirty 状态。
    // 3. 空 preview 不失败，空 reference 必须失败。
    if (!m_renderViews || !m_core.orthogonalCropBridge) {
        std::cerr << "[Host] Orthogonal crop activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }

    const HostRenderViewRuntime* referenceView = nullptr;
    if (!request.referenceViewId.empty()) {
        referenceView = m_renderViews->GetViewById(request.referenceViewId);
    }
    else if (request.useReferenceRole) {
        referenceView = m_renderViews->GetFirstViewByRole(request.referenceRole);
    }

    if (!referenceView || !referenceView->service || !referenceView->context) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view is missing." << std::endl;
        return false;
    }

    std::vector<const HostRenderViewRuntime*> previewViews =
        m_renderViews->GetViewsByIdsAndRoles(request.previewViewIds, request.previewViewRoles);
    if (previewViews.empty() && request.useConfiguredPreviewViews) {
        // 裁切预览目标也必须由请求允许后才退到配置默认值；空请求不全选，避免误把新窗口纳入 preview。
        previewViews = m_renderViews->GetConfiguredCropPreviewViews();
    }

    auto bridge = m_core.orthogonalCropBridge;
    // 裁切 reference view 按请求中的 id/role 选择，而不是按第几个窗口选择；后续 Qt 多/少窗口布局仍可复用同一 bridge。
    bridge->SetReferenceRenderService(referenceView->service);
    bridge->SetReferenceRenderer(referenceView->context->GetRenderer());
    bridge->SetPrimaryInteractor(referenceView->context->GetInteractor());
    bridge->SetPreviewRenderServices(m_renderViews->BuildInteractiveServices(previewViews));

    if (!RefreshOrthogonalCropInputImage()) {
        return false;
    }

    return true;
}
