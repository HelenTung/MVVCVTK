#include "Host/HostFeatureBindings.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppState.h"
#include "DataManager.h"
#include "Host/HostGapAnalysisBinding.h"
#include "Interaction/OrthogonalCropInteractionBridgeService.h"
#include "StdRenderContext.h"
#include "OrthogonalCropTypes.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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
                // 同一宿主显隐命令按状态分派：未进入时激活显示模式，已进入时只切换 overlay 显隐。
                // 彻底退出由独立宿主退出命令负责，避免“临时隐藏”误清本轮分析结果。
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
        // 宿主退出命令只退出当前活跃工具模式，不做全局关闭；顺序体现用户正在操作的显式模式优先级。
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

HostFeatureBindings::HostFeatureBindings()
    : m_gapAnalysisBinding(std::make_unique<HostGapAnalysisBinding>())
{
}

HostFeatureBindings::~HostFeatureBindings() = default;

void HostFeatureBindings::RegisterFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_core = core;
    m_renderViews = &renderViews;
    if (m_gapAnalysisBinding) {
        m_gapAnalysisBinding->Register(core, renderViews);
    }
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
    return m_gapAnalysisBinding
        && m_gapAnalysisBinding->ActivateDisplay(request);
}

bool HostFeatureBindings::ToggleGapAnalysisOverlayVisibility()
{
    return m_gapAnalysisBinding
        && m_gapAnalysisBinding->ToggleOverlayVisibility();
}

bool HostFeatureBindings::ExitGapAnalysisDisplay()
{
    return m_gapAnalysisBinding
        && m_gapAnalysisBinding->ExitDisplay();
}

bool HostFeatureBindings::GetGapAnalysisDisplayActive() const
{
    return m_gapAnalysisBinding
        && m_gapAnalysisBinding->GetDisplayActive();
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

void HostFeatureBindings::AttachGapAnalysisTimer(
    const HostGapAnalysisEventPumpConfig& eventPumpConfig)
{
    if (m_gapAnalysisBinding) {
        m_gapAnalysisBinding->AttachTimer(eventPumpConfig);
    }
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
