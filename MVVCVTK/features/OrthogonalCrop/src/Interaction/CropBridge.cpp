#include "Interaction/CropBridge.h"

#include "Algorithms/CropAlgorithm.h"
#include "AppInterfaces.h"
#include "Interaction/CropBoxWidget.h"
#include "Interaction/CropPlaneWidget.h"
#include "Render/CropShaderController.h"
#include "Routing/CropRouter.h"

#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr double kVectorTolerance = 1.0e-12;
constexpr double kGeometryTolerance = 1.0e-9;

bool GetBoundsValid(const CropBoundsDouble6Array& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

RenderInputStamp GetInputStamp(const CropInputSnapshot& input)
{
    RenderInputStamp stamp;
    stamp.version = input.inputVersion;
    stamp.identity = input.dataSource
        == OrthogonalCropDataSource::ImageData
        ? static_cast<const void*>(input.imageData.GetPointer())
        : static_cast<const void*>(input.polyData.GetPointer());
    return stamp;
}
}

class CropBridge::Impl final {
public:
    struct ShaderCandidate final {
        std::vector<CropOpItem> history;
        std::size_t cursor = 0;
        std::optional<std::size_t> draftIndex;
        std::shared_ptr<const CropPredicateTable> predicateTable;
    };

    struct TargetBinding final {
        std::shared_ptr<InteractiveService> service;
        std::shared_ptr<CropShaderEffect> effect;
    };

    struct PendingShader final {
        std::vector<CropOpItem> history;
        std::size_t cursor = 0;
        std::optional<std::size_t> draftIndex;
        CropShaderPayload payload;
        std::vector<TargetBinding> targets;
        std::vector<TargetBinding> retiredTargets;
        std::shared_ptr<InteractiveService> nextReferenceService;
        vtkRenderWindowInteractor* nextInteractor = nullptr;
        bool isTargetRebind = false;
    };

    struct ExportTask final {
        std::future<CropExportResult> result;
        std::thread worker;
        CropExportCallback callback;
    };

    struct PendingBaseline final {
        CropInputSnapshot input;
        std::vector<CropOpItem> activeHistory;
        std::size_t baseNodeCount = 0;
    };

    Impl();
    ~Impl();

    bool StartView(const CropViewRequest& request);
    bool StartView(
        const CropViewRequest& request,
        CropInputSnapshot input);
    bool ClearBindings();
    bool SetCropInput(CropInputSnapshot input);
    bool StartCropBaseline(
        CropInputSnapshot input,
        std::size_t baseNodeCount);
    bool SetCropBaselineComplete() noexcept;
    bool ClearCropBaseline();
    bool SwitchCrop(CropShape geometryType);
    bool SetCropMode(CropRemovalMode removalMode);
    bool PreviousCrop();
    bool NextCrop();
    bool SetCropNode(std::size_t nodeCount);
    bool ExitCrop();
    bool GetCropActive() const;
    bool GetCropBound() const;
    CropHistoryState GetCropHistory() const;
    bool GetShaderTickNeeded() const;
    bool SendShaderCommit();
    bool ExportCrop(CropExportCallback onComplete);
    bool GetExportTickNeeded() const;
    bool SendExportResult();

private:
    bool StartViewInput(
        const CropViewRequest& request,
        std::optional<CropInputSnapshot> input);
    void OnBoxWidget(CropInteractionPhase phase);
    void OnPlaneWidget(CropInteractionPhase phase);
    bool SetCandidate(CropOpItem operation);
    bool StartCandidate(CropOpItem operation);
    bool SendNextOp();
    bool SendModeUpdate();
    bool SetPrefix(std::size_t cursor);
    bool SetShader(ShaderCandidate candidate);
    std::optional<CropOpItem> BuildBoxOp();
    std::optional<CropOpItem> BuildPlaneOp();
    bool GetOpSame(
        const CropOpItem& first,
        const CropOpItem& second) const;
    CropBoundsDouble6Array GetWorldBounds() const;
    CropMatrixDouble16Array GetWorldToInput() const;
    bool GetShaderCommitted() const;
    bool GetTargetsReady() const;
    bool ClearBaseShader();
    bool SetWidgetActive(bool isActive);
    void ClearHistory();
    void ClearShaderStage();
    void ClearShader();
    void ClearTargets();
    CropExportResult BuildExportFailure(
        CropFailure failureReason,
        const char* message) const;

    CropRouter m_exportRouter;
    CropBoxWidget m_boxWidget;
    CropPlaneWidget m_planeWidget;
    CropInputSnapshot m_input;
    std::shared_ptr<InteractiveService> m_referenceService;
    std::vector<TargetBinding> m_targets;
    std::vector<CropOpItem> m_history;
    std::vector<CropOpItem> m_allHistory;
    std::size_t m_cursor = 0;
    std::size_t m_baseNodeCount = 0;
    std::optional<std::size_t> m_draftIndex;
    CropShaderPayload m_activePayload;
    std::optional<PendingShader> m_pendingShader;
    std::deque<CropOpItem> m_pendingOps;
    std::optional<CropRemovalMode> m_pendingMode;
    std::optional<ExportTask> m_exportTask;
    std::optional<PendingBaseline> m_pendingBaseline;
    std::optional<CropOpItem> m_dragStart;
    CropShape m_geometryType = CropShape::Box;
    CropRemovalMode m_removalMode = CropRemovalMode::None;
    bool m_hasDrag = false;
    bool m_hasBaseShader = false;
    std::uint64_t m_nextOperationIndex = 1;
    std::uint64_t m_nextRevision = 1;
    bool m_isActive = false;
    bool m_isAccepting = true;
};

CropBridge::Impl::Impl()
{
    m_boxWidget.SetBoundsCallback(
        [this](const CropBoundsDouble6Array&, const CropInteractionPhase phase) {
            OnBoxWidget(phase);
        });
    m_planeWidget.SetPlaneCallback(
        [this](const CropVectorDouble3Array&, const CropVectorDouble3Array&, const CropInteractionPhase phase) {
            OnPlaneWidget(phase);
        });
}

CropBridge::Impl::~Impl()
{
    m_isAccepting = false;
    m_isActive = false;
    m_hasDrag = false;
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    ClearShader();
    ClearTargets();
    if (m_exportTask) {
        if (m_exportTask->worker.joinable()) {
            m_exportTask->worker.join();
        }
        m_exportTask.reset();
    }
}

bool CropBridge::Impl::StartView(const CropViewRequest& request)
{
    return StartViewInput(request, std::nullopt);
}

bool CropBridge::Impl::StartView(
    const CropViewRequest& request,
    CropInputSnapshot input)
{
    return StartViewInput(
        request,
        std::optional<CropInputSnapshot>{ std::move(input) });
}

bool CropBridge::Impl::StartViewInput(
    const CropViewRequest& request,
    std::optional<CropInputSnapshot> input)
{
    if (!m_isAccepting
        || m_exportTask
        || !request.interactor
        || !request.renderer
        || !request.referenceService
        || (input && !CropAlgorithm::GetInputValid(*input))) {
        return false;
    }
    const bool isInputChanged = input
        && !CropAlgorithm::GetInputSame(m_input, *input);
    std::vector<std::shared_ptr<InteractiveService>> targetServices;
    for (const auto& service : request.targetServices) {
        if (!service) {
            continue;
        }
        const auto duplicate = std::find_if(
            targetServices.begin(),
            targetServices.end(),
            [&service](const auto& current) { return current.get() == service.get(); });
        if (duplicate == targetServices.end()) {
            targetServices.push_back(service);
        }
    }
    if (targetServices.empty() && !input) {
        targetServices.push_back(request.referenceService);
    }
    if (targetServices.empty()) {
        return false;
    }
    const bool isSameReference =
        m_referenceService.get()
        == request.referenceService.get();
    const bool isSameTargets =
        targetServices.size() == m_targets.size()
        && std::all_of(
            targetServices.begin(),
            targetServices.end(),
            [this](const auto& service) {
                return std::any_of(
                    m_targets.begin(),
                    m_targets.end(),
                    [&service](const auto& target) {
                        return target.service.get()
                            == service.get();
                    });
            });
    if (m_pendingShader) {
        // 历史前缀正在换代时，同一视图的 Start 只是恢复编辑连接；
        // 它不能因为 GPU revision 尚未提交而拒绝后续 Box/Plane 手势。
        if (!isInputChanged
            && !m_pendingShader->isTargetRebind
            && (m_isActive || GetShaderCommitted())
            && isSameReference && isSameTargets) {
            m_hasDrag = false;
            m_dragStart.reset();
            m_boxWidget.SetInteractor(request.interactor);
            m_planeWidget.SetInteractor(request.interactor);
            m_isActive = true;
            return true;
        }
        return false;
    }
    m_hasDrag = false;
    m_dragStart.reset();

    std::vector<TargetBinding> targets;
    std::vector<TargetBinding> createdTargets;
    for (const auto& service : targetServices) {
        const auto existing = std::find_if(
            m_targets.begin(),
            m_targets.end(),
            [&service](const TargetBinding& target) {
                return target.service.get() == service.get();
            });
        if (existing != m_targets.end()) {
            targets.push_back(*existing);
            continue;
        }
        TargetBinding target;
        target.service = service;
        target.effect = std::make_shared<CropShaderEffect>();
        if (!target.service->AttachRenderEffect(target.effect)) {
            for (const auto& created : createdTargets) {
                (void)created.service->DetachRenderEffect(
                    created.effect.get());
            }
            return false;
        }
        createdTargets.push_back(target);
        targets.push_back(std::move(target));
    }

    const bool isShaderCommitted = GetShaderCommitted();
    if (!isInputChanged
        && (m_isActive || isShaderCommitted)
        && isSameReference && isSameTargets) {
        m_boxWidget.SetInteractor(request.interactor);
        m_planeWidget.SetInteractor(request.interactor);
        m_isActive = true;
        return true;
    }

    // 有已提交前缀时，新增目标以同一 table handle 建立新 revision 的 staged/ready/commit；
    // 只有整体成功后才清退旧目标，避免重绑定中出现部分窗口先失去裁切。
    if (!isInputChanged && isShaderCommitted) {
        const std::uint64_t revision = m_nextRevision++;
        CropShaderPayload payload = m_activePayload;
        payload.revision = revision;
        payload.nodeCount = m_cursor;
        if (!payload.predicateTable) {
            return false;
        }
        std::vector<TargetBinding> accepted;
        for (const auto& target : targets) {
            if (!target.effect
                || !target.effect->SetCropParams(payload)) {
                for (const auto& current : accepted) {
                    (void)current.effect->ClearCropStage(revision);
                }
                for (const auto& created : createdTargets) {
                    (void)created.service->DetachRenderEffect(
                        created.effect.get());
                }
                return false;
            }
            accepted.push_back(target);
        }
        m_pendingShader = PendingShader{
            m_history,
            m_cursor,
            m_draftIndex,
            std::move(payload),
            std::move(accepted),
            m_targets,
            request.referenceService,
            request.interactor,
            true
        };
        m_isActive = true;
        return true;
    }

    // 所有会失败的 target/effect 准备已经完成；从这里开始连续提交输入和 binding。
    // 输入换代必须同时退休旧 history，不能让旧 predicate table 作用到新数据。
    ClearShader();
    if (isInputChanged) {
        ClearHistory();
        m_input = std::move(*input);
    }
    for (const auto& current : m_targets) {
        const bool isRetained = std::any_of(
            targets.begin(),
            targets.end(),
            [&current](const auto& target) {
                return current.service.get() == target.service.get()
                    && current.effect.get() == target.effect.get();
            });
        if (!isRetained && current.service && current.effect) {
            (void)current.service->DetachRenderEffect(
                current.effect.get());
        }
    }
    m_referenceService = request.referenceService;
    m_targets = std::move(targets);
    m_boxWidget.SetInteractor(request.interactor);
    m_planeWidget.SetInteractor(request.interactor);
    const auto worldBounds = GetWorldBounds();
    if (GetBoundsValid(worldBounds)) {
        m_boxWidget.SetReferenceWorldBounds(worldBounds);
        m_boxWidget.SetWidgetWorldBounds(worldBounds);
        m_planeWidget.SetReferenceWorldBounds(worldBounds);
    }
    m_isActive = true;
    return true;
}

bool CropBridge::Impl::ClearBindings()
{
    // VTK Off 可能在拖拽中同步补发 EndInteraction；先关闭业务 gate，
    // 避免清理过程把未完成交互误写成新的 staged/history 操作。
    m_isActive = false;
    m_hasDrag = false;
    m_dragStart.reset();
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    ClearShader();
    ClearHistory();
    m_referenceService.reset();
    ClearTargets();
    return true;
}

bool CropBridge::Impl::SetCropInput(CropInputSnapshot input)
{
    if (!m_isAccepting || !CropAlgorithm::GetInputValid(input)) {
        return false;
    }
    if (CropAlgorithm::GetInputSame(m_input, input)) {
        return true;
    }

    ClearShader();
    ClearHistory();
    m_input = std::move(input);
    const auto worldBounds = GetWorldBounds();
    if (GetBoundsValid(worldBounds)) {
        m_boxWidget.SetReferenceWorldBounds(worldBounds);
        m_boxWidget.SetWidgetWorldBounds(worldBounds);
        m_planeWidget.SetReferenceWorldBounds(worldBounds);
    }
    return true;
}

bool CropBridge::Impl::StartCropBaseline(
    CropInputSnapshot input,
    const std::size_t baseNodeCount)
{
    if (!m_isAccepting
        || m_exportTask
        || m_pendingShader
        || m_pendingMode
        || m_hasBaseShader
        || m_pendingBaseline
        || !CropAlgorithm::GetInputValid(input)
        || input.dataSource != m_input.dataSource
        || m_cursor > m_history.size()
        || baseNodeCount > m_allHistory.size()) {
        return false;
    }

    if (m_baseNodeCount + m_history.size()
            != m_allHistory.size()) {
        return false;
    }
    const std::size_t currentBase =
        m_baseNodeCount + m_cursor;
    if (baseNodeCount != 0
        && baseNodeCount != currentBase) {
        return false;
    }
    m_pendingBaseline = PendingBaseline{
        std::move(input),
        std::vector<CropOpItem>(
            m_allHistory.begin() + baseNodeCount,
            m_allHistory.end()),
        baseNodeCount
    };
    std::cout
        << "[Crop][HistoryObjects] baseline staged"
        << " targetBase=" << baseNodeCount
        << " currentBase=" << currentBase
        << " nextActive="
        << m_pendingBaseline->activeHistory.size()
        << " allSize=" << m_allHistory.size()
        << '\n';
    return true;
}

bool CropBridge::Impl::SetCropBaselineComplete() noexcept
{
    if (!m_pendingBaseline) {
        // 这是发布后的无失败完成阶段；若预备状态丢失，继续运行会让
        // DataManager 与裁切历史永久分裂，因此按内部不变量故障终止。
        std::terminate();
    }
    auto baseline = std::move(*m_pendingBaseline);
    m_pendingBaseline.reset();
    // 旧 Strategy 在全部新输入 binding 就绪前继续显示物化前 committed 节点；
    // 立即清 active shader 会让相机交互重绘出原始 base node 0。
    ClearShaderStage();
    m_input = std::move(baseline.input);
    m_history = std::move(
        baseline.activeHistory);
    m_cursor = 0;
    m_baseNodeCount =
        baseline.baseNodeCount;
    m_draftIndex.reset();
    m_activePayload = {};
    m_pendingOps.clear();
    m_removalMode = CropRemovalMode::None;
    m_hasDrag = false;
    m_dragStart.reset();
    m_hasBaseShader = true;

    const auto worldBounds = GetWorldBounds();
    if (GetBoundsValid(worldBounds)) {
        m_boxWidget.SetReferenceWorldBounds(worldBounds);
        m_boxWidget.SetWidgetWorldBounds(worldBounds);
        m_planeWidget.SetReferenceWorldBounds(worldBounds);
    }
    for (const auto& target : m_targets) {
        if (target.service) {
            target.service->SetDirty();
        }
    }
    (void)ClearBaseShader();
    std::cout
        << "[Crop][HistoryObjects] baseline complete"
        << " activeObject="
        << static_cast<const void*>(&m_history)
        << " activeData="
        << static_cast<const void*>(m_history.data())
        << " activeNode=" << m_cursor
        << " activeSize=" << m_history.size()
        << " allObject="
        << static_cast<const void*>(&m_allHistory)
        << " allData="
        << static_cast<const void*>(m_allHistory.data())
        << " allSize=" << m_allHistory.size()
        << " baseNode=" << m_baseNodeCount
        << '\n';
    return true;
}

bool CropBridge::Impl::ClearCropBaseline()
{
    m_pendingBaseline.reset();
    return true;
}

bool CropBridge::Impl::SwitchCrop(const CropShape geometryType)
{
    if (!m_isActive
        || m_exportTask
        || !CropAlgorithm::GetInputValid(m_input)
        || (geometryType != CropShape::Box && geometryType != CropShape::Plane)) {
        return false;
    }
    m_geometryType = geometryType;
    m_hasDrag = false;
    m_dragStart.reset();
    // Switch 结束上一条操作的模式编辑权；下一次有效 Released 会追加历史。
    m_draftIndex.reset();
    const auto worldBounds = GetWorldBounds();
    if (!GetBoundsValid(worldBounds)) {
        return false;
    }

    bool isEnabled = false;
    if (geometryType == CropShape::Box) {
        m_planeWidget.SetEnabled(false);
        m_boxWidget.SetWidgetWorldBounds(worldBounds);
        isEnabled = m_boxWidget.SetEnabled(true);
    }
    else {
        m_boxWidget.SetEnabled(false);
        const CropVectorDouble3Array origin = {
            (worldBounds[0] + worldBounds[1]) * 0.5,
            (worldBounds[2] + worldBounds[3]) * 0.5,
            (worldBounds[4] + worldBounds[5]) * 0.5
        };
        const CropVectorDouble3Array normal = { 0.0, 0.0, 1.0 };
        const std::array<double, 2> half = {
            (worldBounds[1] - worldBounds[0]) * 0.5,
            (worldBounds[3] - worldBounds[2]) * 0.5
        };
        m_planeWidget.SetWidgetWorldPlane(origin, normal, half);
        isEnabled = m_planeWidget.SetEnabled(true);
    }
    // Widget 仅存在于 reference renderer；切换成功后通过 service 门铃请求
    // 下一帧，避免依赖 vtkBoxWidget2/vtkImplicitPlaneWidget2 的偶然 Render 副作用。
    if (isEnabled && m_referenceService) {
        m_referenceService->SetDirty();
    }
    return isEnabled;
}

bool CropBridge::Impl::SetCropMode(const CropRemovalMode removalMode)
{
    if (!m_isActive
        || m_exportTask
        || (removalMode != CropRemovalMode::None
            && removalMode != CropRemovalMode::KeepInside
            && removalMode != CropRemovalMode::RemoveInside)) {
        return false;
    }
    if (removalMode == m_removalMode) {
        return true;
    }
    m_hasDrag = false;
    m_dragStart.reset();
    if (m_pendingShader) {
        // 当前 revision 已进入 GPU 事务后不能原地改 payload。只记录最新模式，
        // 待它提交后仍通过 SetShader 更新同一个 editable draft。
        if (m_pendingShader->draftIndex) {
            m_pendingMode = removalMode;
            std::cout
                << "[Crop][Mode] queued"
                << " mode=" << static_cast<int>(removalMode)
                << " revision="
                << m_pendingShader->payload.revision
                << " draft="
                << *m_pendingShader->draftIndex
                << '\n';
        }
        m_removalMode = removalMode;
        return true;
    }

    // 已释放的当前 widget 操作仍是可编辑 draft；模式切换必须以新 revision
    // 同步更新这条 history，而不是延迟到下一次 Released。
    // None 只暂停当前 widget 对历史的写入，已提交前缀继续显示且不被删除。
    if (removalMode != CropRemovalMode::None
        && m_draftIndex && *m_draftIndex < m_cursor) {
        if (!GetTargetsReady()) {
            m_pendingMode = removalMode;
            m_removalMode = removalMode;
            std::cout
                << "[Crop][Mode] waiting render input"
                << " mode=" << static_cast<int>(removalMode)
                << " draft=" << *m_draftIndex
                << '\n';
            return true;
        }
        auto candidate = m_history;
        candidate[*m_draftIndex].removalMode = removalMode;
        if (!SetShader(ShaderCandidate{
                std::move(candidate),
                m_cursor,
                m_draftIndex,
                {} })) {
            return false;
        }
    }
    m_removalMode = removalMode;
    return true;
}

void CropBridge::Impl::OnBoxWidget(const CropInteractionPhase phase)
{
    if (!m_isActive
        || m_exportTask
        || m_geometryType != CropShape::Box
        || m_removalMode == CropRemovalMode::None) {
        m_hasDrag = false;
        m_dragStart.reset();
        return;
    }
    if (phase == CropInteractionPhase::Hover) {
        m_hasDrag = false;
        m_dragStart = BuildBoxOp();
        return;
    }
    if (phase == CropInteractionPhase::Dragging) {
        m_hasDrag = true;
        return;
    }
    if (phase != CropInteractionPhase::Released) {
        return;
    }

    const bool hasDrag = m_hasDrag;
    m_hasDrag = false;
    auto dragStart = std::move(m_dragStart);
    m_dragStart.reset();
    if (!hasDrag || !dragStart) {
        return;
    }
    auto operation = BuildBoxOp();
    if (!operation || GetOpSame(*dragStart, *operation)) {
        return;
    }
    (void)SetCandidate(std::move(*operation));
}

void CropBridge::Impl::OnPlaneWidget(const CropInteractionPhase phase)
{
    if (!m_isActive
        || m_exportTask
        || m_geometryType != CropShape::Plane
        || m_removalMode == CropRemovalMode::None) {
        m_hasDrag = false;
        m_dragStart.reset();
        return;
    }
    if (phase == CropInteractionPhase::Hover) {
        m_hasDrag = false;
        m_dragStart = BuildPlaneOp();
        return;
    }
    if (phase == CropInteractionPhase::Dragging) {
        m_hasDrag = true;
        return;
    }
    if (phase != CropInteractionPhase::Released) {
        return;
    }

    const bool hasDrag = m_hasDrag;
    m_hasDrag = false;
    auto dragStart = std::move(m_dragStart);
    m_dragStart.reset();
    if (!hasDrag || !dragStart) {
        return;
    }
    auto operation = BuildPlaneOp();
    if (!operation || GetOpSame(*dragStart, *operation)) {
        return;
    }
    (void)SetCandidate(std::move(*operation));
}

bool CropBridge::Impl::SetCandidate(CropOpItem operation)
{
    if (m_exportTask
        || m_removalMode == CropRemovalMode::None) {
        return false;
    }
    operation.operationIndex = 0;
    if (m_pendingShader
        || m_hasBaseShader
        || !m_pendingOps.empty()
        || !GetTargetsReady()) {
        // GPU 只允许单 revision stage；交互仍按 Released 顺序保存，
        // 当前 revision 或 render input 收敛后再逐条生成 history candidate。
        m_pendingOps.push_back(std::move(operation));
        if (!m_pendingShader) {
            std::cout
                << "[Crop][Shader] operation queued"
                << " pendingOps=" << m_pendingOps.size()
                << " inputVersion=" << m_input.inputVersion
                << '\n';
        }
        return true;
    }
    return StartCandidate(std::move(operation));
}

bool CropBridge::Impl::StartCandidate(CropOpItem operation)
{
    operation.operationIndex = m_nextOperationIndex;
    auto candidate = m_history;
    // 每次有效 Released 都是一次独立执行；若当前位于历史中间，先丢弃 redo
    // 分支再追加。m_draftIndex 只允许模式切换改写最新操作，不再吞并后续 Released。
    candidate.resize(m_cursor);
    candidate.push_back(operation);
    const std::size_t candidateCursor = candidate.size();
    const std::optional<std::size_t> candidateDraft = candidateCursor - 1;
    if (!SetShader(ShaderCandidate{
        std::move(candidate),
        candidateCursor,
        candidateDraft,
        {} })) {
        return false;
    }
    ++m_nextOperationIndex;
    return true;
}

bool CropBridge::Impl::SendNextOp()
{
    if (m_pendingShader
        || m_hasBaseShader
        || m_pendingOps.empty()
        || !GetTargetsReady()) {
        return false;
    }
    auto operation = std::move(m_pendingOps.front());
    m_pendingOps.pop_front();
    return StartCandidate(std::move(operation));
}

bool CropBridge::Impl::SendModeUpdate()
{
    if (!m_pendingMode
        || m_pendingShader
        || m_hasBaseShader) {
        return false;
    }
    const CropRemovalMode removalMode =
        *m_pendingMode;
    if (removalMode == CropRemovalMode::None
        || !m_draftIndex
        || *m_draftIndex >= m_cursor) {
        m_pendingMode.reset();
        return true;
    }
    if (!GetTargetsReady()) {
        return false;
    }

    auto candidate = m_history;
    candidate[*m_draftIndex].removalMode =
        removalMode;
    if (!SetShader(ShaderCandidate{
            std::move(candidate),
            m_cursor,
            m_draftIndex,
            {} })) {
        return false;
    }
    std::cout
        << "[Crop][Mode] staged"
        << " mode=" << static_cast<int>(removalMode)
        << " draft=" << *m_draftIndex
        << '\n';
    m_pendingMode.reset();
    return true;
}

bool CropBridge::Impl::SetPrefix(const std::size_t cursor)
{
    if (m_pendingShader
        || m_hasBaseShader
        || cursor > m_history.size()) {
        return false;
    }
    auto predicateTable = m_activePayload.predicateTable;
    if (!predicateTable || predicateTable->operationCount != m_history.size()) {
        predicateTable.reset();
    }
    return SetShader(ShaderCandidate{
        m_history,
        cursor,
        std::nullopt,
        std::move(predicateTable) });
}

bool CropBridge::Impl::SetShader(ShaderCandidate candidate)
{
    if (candidate.cursor > candidate.history.size()) {
        return false;
    }
    if (!candidate.predicateTable) {
        const auto tableResult = CropAlgorithm::BuildPredicateTable(
            candidate.history,
            candidate.history.size());
        if (!tableResult.isSucceeded || !tableResult.predicateTable) {
            return false;
        }
        candidate.predicateTable = tableResult.predicateTable;
    }

    const std::uint64_t revision = m_nextRevision++;
    CropShaderPayload payload;
    payload.revision = revision;
    payload.sourceStamp = GetInputStamp(m_input);
    payload.nodeCount = candidate.cursor;
    payload.predicateTable = candidate.predicateTable;
    std::vector<TargetBinding> accepted;
    for (const auto& target : m_targets) {
        if (!target.effect
            || !target.effect->SetCropParams(payload)) {
            for (const auto& current : accepted) {
                (void)current.effect->ClearCropStage(revision);
            }
            return false;
        }
        accepted.push_back(target);
    }
    if (accepted.empty()) {
        return false;
    }
    m_pendingShader = PendingShader{
        std::move(candidate.history),
        candidate.cursor,
        candidate.draftIndex,
        std::move(payload),
        std::move(accepted),
        {},
        {},
        nullptr,
        false
    };
    return true;
}

bool CropBridge::Impl::PreviousCrop()
{
    if (!GetCropBound()
        || !GetTargetsReady()
        || m_exportTask
        || m_cursor == 0) {
        std::cout
            << "[Crop][HistoryObjects] previous rejected"
            << " exportActive="
            << static_cast<bool>(m_exportTask)
            << " activeObject="
            << static_cast<const void*>(&m_history)
            << " activeNode=" << m_cursor
            << " activeSize=" << m_history.size()
            << " allObject="
            << static_cast<const void*>(&m_allHistory)
            << " allSize=" << m_allHistory.size()
            << " baseNode=" << m_baseNodeCount
            << '\n';
        return false;
    }
    return SetPrefix(m_cursor - 1);
}

bool CropBridge::Impl::NextCrop()
{
    return GetCropBound()
        && GetTargetsReady()
        && !m_exportTask
        && m_cursor < m_history.size()
        && SetPrefix(m_cursor + 1);
}

bool CropBridge::Impl::SetCropNode(const std::size_t nodeCount)
{
    if (!GetCropBound()
        || !GetTargetsReady()
        || m_exportTask
        || m_pendingShader
        || nodeCount > m_history.size()) {
        return false;
    }
    // 同一节点是成功的幂等请求，不重复创建 shader revision。
    return nodeCount == m_cursor || SetPrefix(nodeCount);
}

bool CropBridge::Impl::GetShaderTickNeeded() const
{
    return m_pendingShader.has_value()
        || m_pendingMode.has_value()
        || m_hasBaseShader
        || !m_pendingOps.empty();
}

bool CropBridge::Impl::SendShaderCommit()
{
    if (!m_pendingShader) {
        if (m_hasBaseShader) {
            if (!ClearBaseShader()) {
                return false;
            }
            return true;
        }
        if (m_pendingMode) {
            if (!SendModeUpdate()) {
                return false;
            }
            if (m_pendingShader) {
                return false;
            }
        }
        (void)SendNextOp();
        return false;
    }
    bool isReady = true;
    bool hasFailure = false;
    for (const auto& target : m_pendingShader->targets) {
        const auto state = target.effect->GetState();
        hasFailure = hasFailure
            || state.status == RenderEffectStatus::Failed;
        if (state.stagedRevision != m_pendingShader->payload.revision) {
            isReady = false;
            continue;
        }
        if (state.status == RenderEffectStatus::Staged) {
            target.service->SetDirty();
        }
        isReady = isReady
            && (state.status == RenderEffectStatus::Ready
                || state.status == RenderEffectStatus::Committed);
    }
    if (!isReady && !hasFailure) {
        return false;
    }

    if (hasFailure) {
        const auto failedState =
            m_pendingShader->targets.empty()
            ? RenderEffectState{}
            : m_pendingShader->targets.front()
                .effect->GetState();
        std::cout
            << "[Crop][Shader] failed"
            << " revision="
            << m_pendingShader->payload.revision
            << " status="
            << static_cast<int>(failedState.status)
            << " reason="
            << static_cast<int>(
                failedState.failureReason)
            << " message=\""
            << failedState.message << "\""
            << '\n';
        for (const auto& target : m_pendingShader->targets) {
            (void)target.effect->ClearCropStage(
                m_pendingShader->payload.revision);
        }
        m_pendingShader.reset();
        m_pendingOps.clear();
        m_pendingMode.reset();
        return false;
    }

    std::vector<TargetBinding> committedTargets;
    committedTargets.reserve(
        m_pendingShader->targets.size());
    for (const auto& target : m_pendingShader->targets) {
        if (!target.effect->StartCropCommit(
                m_pendingShader->payload.revision)) {
            for (auto committed =
                    committedTargets.rbegin();
                committed != committedTargets.rend();
                ++committed) {
                (void)committed->effect->ClearCropCommit(
                    m_pendingShader->payload.revision);
            }
            for (const auto& current :
                m_pendingShader->targets) {
                (void)current.effect->ClearCropStage(
                    m_pendingShader->payload.revision);
            }
            m_pendingShader.reset();
            m_pendingOps.clear();
            m_pendingMode.reset();
            return false;
        }
        committedTargets.push_back(target);
    }
    const bool isCommitReady = std::all_of(
        committedTargets.begin(),
        committedTargets.end(),
        [this](const auto& target) {
            return target.effect->GetCropCommitReady(
                m_pendingShader->payload.revision);
        });
    if (!isCommitReady) {
        for (auto committed = committedTargets.rbegin();
            committed != committedTargets.rend();
            ++committed) {
            (void)committed->effect->ClearCropCommit(
                m_pendingShader->payload.revision);
        }
        m_pendingShader.reset();
        m_pendingOps.clear();
        m_pendingMode.reset();
        return false;
    }
    for (const auto& target : committedTargets) {
        if (!target.effect->SetCropComplete(
                m_pendingShader->payload.revision)) {
            // 全量预检和完成在 owner thread 同一调用栈内；若此处仍失败，
            // 说明内部状态机不变量已破坏，不能发布 bridge 侧历史。
            m_pendingOps.clear();
            m_pendingShader.reset();
            m_pendingMode.reset();
            return false;
        }
    }
    m_history = std::move(m_pendingShader->history);
    m_cursor = m_pendingShader->cursor;
    m_draftIndex = m_pendingShader->draftIndex;
    m_activePayload = m_pendingShader->payload;
    m_allHistory.resize(m_baseNodeCount);
    m_allHistory.insert(
        m_allHistory.end(),
        m_history.begin(),
        m_history.end());
    if (m_pendingShader->isTargetRebind) {
        for (const auto& retired : m_pendingShader->retiredTargets) {
            const bool isStillTarget = std::any_of(
                m_pendingShader->targets.begin(),
                m_pendingShader->targets.end(),
                [&retired](const auto& target) {
                    return target.service.get()
                        == retired.service.get();
                });
            if (retired.service && retired.effect && !isStillTarget) {
                (void)retired.effect->ClearCropParams();
                (void)retired.service->DetachRenderEffect(
                    retired.effect.get());
            }
        }
        m_targets = m_pendingShader->targets;
        m_referenceService = m_pendingShader->nextReferenceService;
        m_boxWidget.SetInteractor(m_pendingShader->nextInteractor);
        m_planeWidget.SetInteractor(m_pendingShader->nextInteractor);
        const auto worldBounds = GetWorldBounds();
        if (GetBoundsValid(worldBounds)) {
            m_boxWidget.SetReferenceWorldBounds(worldBounds);
            m_boxWidget.SetWidgetWorldBounds(worldBounds);
            m_planeWidget.SetReferenceWorldBounds(worldBounds);
        }
    }
    // Stage 帧只负责让所有 binding 的资源进入 Ready，仍渲染旧 active；
    // 两阶段提交完成后必须再次置脏，下一次 Timer 才会发布新的 committed 前缀。
    for (const auto& target : committedTargets) {
        if (target.service) {
            target.service->SetDirty();
        }
    }
    m_pendingShader.reset();
    if (m_pendingMode) {
        (void)SendModeUpdate();
    }
    if (!m_pendingShader) {
        (void)SendNextOp();
    }
    return true;
}

std::optional<CropOpItem> CropBridge::Impl::BuildBoxOp()
{
    CropOpItem operation;
    operation.geometryType = CropShape::Box;
    operation.removalMode = m_removalMode;

    CropVectorDouble3Array baseCenter = {};
    CropVectorDouble3Array baseSize = {};
    CropMatrixDouble16Array baseToNow = CropAlgorithm::GetIdentityMatrix();
    if (!m_boxWidget.GetCurrentWorldBox(baseCenter, baseSize, baseToNow)) {
        return std::nullopt;
    }

    vtkNew<vtkMatrix4x4> boxToInitialWorld;
    boxToInitialWorld->Identity();
    for (int axis = 0; axis < 3; ++axis) {
        boxToInitialWorld->SetElement(axis, axis, baseSize[axis] * 0.5);
        boxToInitialWorld->SetElement(axis, 3, baseCenter[axis]);
    }
    vtkNew<vtkMatrix4x4> baseToNowMatrix;
    baseToNowMatrix->DeepCopy(baseToNow.data());
    vtkNew<vtkMatrix4x4> boxToWorld;
    vtkMatrix4x4::Multiply4x4(baseToNowMatrix, boxToInitialWorld, boxToWorld);
    vtkNew<vtkMatrix4x4> worldToInput;
    worldToInput->DeepCopy(GetWorldToInput().data());
    vtkNew<vtkMatrix4x4> boxToInput;
    vtkMatrix4x4::Multiply4x4(worldToInput, boxToWorld, boxToInput);
    vtkMatrix4x4::DeepCopy(operation.boxToInputModelMatrix.data(), boxToInput);
    return operation;
}

std::optional<CropOpItem> CropBridge::Impl::BuildPlaneOp()
{
    CropOpItem operation;
    operation.geometryType = CropShape::Plane;
    operation.removalMode = m_removalMode;

    CropVectorDouble3Array worldOrigin = {};
    CropVectorDouble3Array worldNormal = { 0.0, 0.0, 1.0 };
    if (!m_planeWidget.GetCurrentWorldPlane(worldOrigin, worldNormal)) {
        return std::nullopt;
    }
    vtkNew<vtkMatrix4x4> worldToInput;
    worldToInput->DeepCopy(GetWorldToInput().data());
    const double worldPoint[4] = { worldOrigin[0], worldOrigin[1], worldOrigin[2], 1.0 };
    double inputPoint[4] = {};
    worldToInput->MultiplyPoint(worldPoint, inputPoint);
    const double inverseW = std::abs(inputPoint[3]) > kVectorTolerance
        ? 1.0 / inputPoint[3]
        : 1.0;
    operation.planeCenterInInputModel = {
        inputPoint[0] * inverseW,
        inputPoint[1] * inverseW,
        inputPoint[2] * inverseW
    };

    vtkNew<vtkMatrix4x4> inputToWorld;
    vtkMatrix4x4::Invert(worldToInput, inputToWorld);
    inputToWorld->Transpose();
    const double worldVector[4] = { worldNormal[0], worldNormal[1], worldNormal[2], 0.0 };
    double inputVector[4] = {};
    inputToWorld->MultiplyPoint(worldVector, inputVector);
    operation.planeNormalInInputModel = {
        inputVector[0], inputVector[1], inputVector[2]
    };
    if (vtkMath::Normalize(operation.planeNormalInInputModel.data()) <= kVectorTolerance) {
        return std::nullopt;
    }
    return operation;
}

bool CropBridge::Impl::GetOpSame(
    const CropOpItem& first,
    const CropOpItem& second) const
{
    if (first.geometryType != second.geometryType) {
        return false;
    }
    const auto getValuesSame = [](const auto& firstValues,
                                   const auto& secondValues) {
        return std::equal(
            firstValues.begin(),
            firstValues.end(),
            secondValues.begin(),
            [](const double firstValue,
                const double secondValue) {
                const double scale = std::max({
                    1.0,
                    std::abs(firstValue),
                    std::abs(secondValue)
                });
                return std::abs(firstValue - secondValue)
                    <= kGeometryTolerance * scale;
            });
    };
    if (first.geometryType == CropShape::Box) {
        return getValuesSame(
            first.boxToInputModelMatrix,
            second.boxToInputModelMatrix);
    }
    if (first.geometryType == CropShape::Plane) {
        return getValuesSame(
                first.planeCenterInInputModel,
                second.planeCenterInInputModel)
            && getValuesSame(
                first.planeNormalInInputModel,
                second.planeNormalInInputModel);
    }
    return false;
}

CropBoundsDouble6Array CropBridge::Impl::GetWorldBounds() const
{
    CropBoundsDouble6Array bounds = {};
    if (!m_referenceService || !CropAlgorithm::GetInputValid(m_input)) {
        return bounds;
    }
    bounds = {
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()
    };
    for (int corner = 0; corner < 8; ++corner) {
        const double modelPoint[3] = {
            m_input.inputModelBounds[(corner & 1) ? 1 : 0],
            m_input.inputModelBounds[(corner & 2) ? 3 : 2],
            m_input.inputModelBounds[(corner & 4) ? 5 : 4]
        };
        double worldPoint[3] = {};
        m_referenceService->GetWorldPositionFromModel(modelPoint, worldPoint);
        for (int axis = 0; axis < 3; ++axis) {
            bounds[axis * 2] = std::min(bounds[axis * 2], worldPoint[axis]);
            bounds[axis * 2 + 1] = std::max(bounds[axis * 2 + 1], worldPoint[axis]);
        }
    }
    return bounds;
}

CropMatrixDouble16Array CropBridge::Impl::GetWorldToInput() const
{
    if (!m_referenceService) {
        return CropAlgorithm::GetIdentityMatrix();
    }
    vtkNew<vtkMatrix4x4> matrix;
    matrix->DeepCopy(m_referenceService->GetModelMatrix().data());
    matrix->Invert();
    CropMatrixDouble16Array values = {};
    vtkMatrix4x4::DeepCopy(values.data(), matrix);
    return values;
}

CropExportResult CropBridge::Impl::BuildExportFailure(
    const CropFailure failureReason,
    const char* message) const
{
    CropExportResult result;
    result.resolvedDataSource = m_input.dataSource;
    result.failureReason = failureReason;
    result.inputVersion = m_input.inputVersion;
    result.nodeCount = m_cursor;
    result.operations.assign(m_history.begin(), m_history.begin() + m_cursor);
    result.message = message;
    return result;
}

bool CropBridge::Impl::ExportCrop(CropExportCallback onComplete)
{
    if (!onComplete) {
        return false;
    }
    if (m_exportTask) {
        onComplete(BuildExportFailure(CropFailure::Busy, "A crop export is already running."));
        return false;
    }
    if (m_pendingShader
        || m_pendingMode
        || m_hasBaseShader
        || m_cursor == 0
        || !CropAlgorithm::GetInputValid(m_input)
        || m_activePayload.sourceStamp != GetInputStamp(m_input)
        || m_activePayload.nodeCount != m_cursor
        || !m_activePayload.predicateTable) {
        onComplete(BuildExportFailure(CropFailure::BadInput, "Crop export state is not ready."));
        return false;
    }

    CropExportRequest request;
    request.dataSource = m_input.dataSource;
    request.operations.assign(m_history.begin(), m_history.begin() + m_cursor);
    request.nodeCount = request.operations.size();
    request.inputVersion = m_input.inputVersion;
    auto task = m_exportRouter.BuildExportTask(
        m_input,
        std::move(request),
        m_activePayload);
    if (!task) {
        onComplete(BuildExportFailure(CropFailure::VersionMismatch, "Crop export snapshot is inconsistent."));
        return false;
    }

    ExportTask active;
    active.result = task->get_future();
    active.callback = std::move(onComplete);
    try {
        active.worker = std::thread(std::move(*task));
    }
    catch (...) {
        active.callback(BuildExportFailure(CropFailure::WorkerStartFailed, "Crop export worker could not start."));
        return false;
    }
    if (!active.worker.joinable()) {
        active.callback(BuildExportFailure(CropFailure::WorkerStartFailed, "Crop export worker is not joinable."));
        return false;
    }
    // worker 捕获的是当前 committed prefix；在 owner thread 消费结果前，
    // m_exportTask 同时充当历史事务门，所有会改变 active prefix 的入口均拒绝。
    m_exportTask = std::move(active);
    m_hasDrag = false;
    m_dragStart.reset();
    (void)SetWidgetActive(false);
    std::cout
        << "[Crop][Materialize] widget frozen"
        << " shape=" << static_cast<int>(m_geometryType)
        << " node=" << m_cursor
        << '\n';
    return true;
}

bool CropBridge::Impl::GetExportTickNeeded() const
{
    return m_exportTask
        && m_exportTask->result.valid()
        && m_exportTask->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool CropBridge::Impl::SendExportResult()
{
    if (!GetExportTickNeeded()) {
        return false;
    }
    auto active = std::move(*m_exportTask);
    m_exportTask.reset();
    CropExportResult result;
    try {
        result = active.result.get();
    }
    catch (const std::exception& error) {
        result = BuildExportFailure(CropFailure::WorkerFailed, error.what());
    }
    catch (...) {
        result = BuildExportFailure(CropFailure::WorkerFailed, "Crop export worker failed with an unknown exception.");
    }
    if (active.worker.joinable()) {
        active.worker.join();
    }
    if (active.callback) {
        active.callback(std::move(result));
    }
    if (m_isActive) {
        (void)SetWidgetActive(true);
        std::cout
            << "[Crop][Materialize] widget restored"
            << " shape="
            << static_cast<int>(m_geometryType)
            << " mode="
            << static_cast<int>(m_removalMode)
            << " node=" << m_cursor
            << '\n';
    }
    return true;
}

void CropBridge::Impl::ClearHistory()
{
    m_history.clear();
    m_allHistory.clear();
    m_cursor = 0;
    m_baseNodeCount = 0;
    m_draftIndex.reset();
    m_removalMode = CropRemovalMode::None;
    m_hasDrag = false;
    m_dragStart.reset();
    m_pendingOps.clear();
    m_pendingMode.reset();
    m_hasBaseShader = false;
    m_activePayload = {};
    m_pendingShader.reset();
    m_pendingBaseline.reset();
}

bool CropBridge::Impl::GetShaderCommitted() const
{
    // nodeCount=0 也是一次完整提交的无裁切基线；只要 revision、输入身份和
    // 不可变 table 一致，重入时就必须保留当前 binding，不能按“无效果”清退。
    return m_cursor <= m_history.size()
        && m_activePayload.revision != 0
        && m_activePayload.sourceStamp == GetInputStamp(m_input)
        && m_activePayload.nodeCount == m_cursor
        && m_activePayload.predicateTable
        && m_activePayload.predicateTable->operationCount == m_history.size();
}

bool CropBridge::Impl::GetTargetsReady() const
{
    if (m_targets.empty()) {
        return false;
    }
    const RenderInputStamp inputStamp =
        GetInputStamp(m_input);
    return inputStamp.identity
        && inputStamp.version != 0
        && std::all_of(
            m_targets.begin(),
            m_targets.end(),
            [&inputStamp](const auto& target) {
                return target.service
                    && target.effect
                    && target.service
                        ->GetRenderInputStamp()
                        == inputStamp;
            });
}

bool CropBridge::Impl::ClearBaseShader()
{
    if (!m_hasBaseShader) {
        return true;
    }
    if (!GetTargetsReady()) {
        return false;
    }
    bool isCleared = true;
    for (const auto& target : m_targets) {
        if (target.effect) {
            isCleared =
                target.effect->ClearCropParams()
                && isCleared;
        }
    }
    if (!isCleared) {
        return false;
    }
    m_hasBaseShader = false;
    for (const auto& target : m_targets) {
        if (target.service) {
            target.service->SetDirty();
        }
    }
    std::cout
        << "[Crop][Materialize] retired shader cleared"
        << " inputVersion=" << m_input.inputVersion
        << " baseNode=" << m_baseNodeCount
        << " activeNode=" << m_cursor
        << '\n';
    return true;
}

bool CropBridge::Impl::SetWidgetActive(
    const bool isActive)
{
    bool isSet = true;
    if (!isActive) {
        const bool isBoxSet =
            m_boxWidget.SetEnabled(false);
        const bool isPlaneSet =
            m_planeWidget.SetEnabled(false);
        isSet = isBoxSet && isPlaneSet;
    }
    else if (m_geometryType == CropShape::Box) {
        const bool isPlaneSet =
            m_planeWidget.SetEnabled(false);
        const bool isBoxSet =
            m_boxWidget.SetEnabled(true);
        isSet = isPlaneSet && isBoxSet;
    }
    else if (m_geometryType == CropShape::Plane) {
        const bool isBoxSet =
            m_boxWidget.SetEnabled(false);
        const bool isPlaneSet =
            m_planeWidget.SetEnabled(true);
        isSet = isBoxSet && isPlaneSet;
    }
    else {
        isSet = false;
    }
    if (m_referenceService) {
        m_referenceService->SetDirty();
    }
    return isSet;
}

void CropBridge::Impl::ClearShaderStage()
{
    if (m_pendingShader) {
        for (const auto& target : m_pendingShader->targets) {
            (void)target.effect->ClearCropStage(
                m_pendingShader->payload.revision);
        }
        if (m_pendingShader->isTargetRebind) {
            for (const auto& target : m_pendingShader->targets) {
                const bool isCurrentTarget = std::any_of(
                    m_targets.begin(),
                    m_targets.end(),
                    [&target](const auto& current) {
                        return current.service.get()
                                == target.service.get()
                            && current.effect.get()
                                == target.effect.get();
                    });
                if (isCurrentTarget || !target.service
                    || !target.effect) {
                    continue;
                }
                // 重绑定尚未提交时，新 target 只持有 staged/replay 资源；
                // 取消事务必须把临时 effect 一并拆除，旧 target 继续显示 committed。
                (void)target.effect->ClearCropParams();
                (void)target.service->DetachRenderEffect(
                    target.effect.get());
            }
        }
    }
    m_pendingShader.reset();
    m_pendingOps.clear();
    m_pendingMode.reset();
    m_hasDrag = false;
    m_dragStart.reset();
}

void CropBridge::Impl::ClearShader()
{
    ClearShaderStage();
    m_hasBaseShader = false;
    for (const auto& target : m_targets) {
        if (target.effect) {
            (void)target.effect->ClearCropParams();
        }
    }
}

void CropBridge::Impl::ClearTargets()
{
    for (const auto& target : m_targets) {
        if (target.service && target.effect) {
            (void)target.service->DetachRenderEffect(
                target.effect.get());
        }
    }
    m_targets.clear();
}

bool CropBridge::Impl::ExitCrop()
{
    if (!m_isActive) {
        return false;
    }
    // 先拒绝 widget 回调，再关闭 VTK 控件；Off 期间的结束事件只能清理交互态。
    m_isActive = false;
    m_hasDrag = false;
    m_dragStart.reset();
    m_boxWidget.SetEnabled(false);
    m_planeWidget.SetEnabled(false);
    // Exit 只结束编辑生命周期：取消尚未提交的 staged revision，并冻结当前
    // committed 前缀；输入换代或 ClearBindings 才负责清除可见裁切结果。
    ClearShaderStage();
    m_draftIndex.reset();
    m_removalMode = CropRemovalMode::None;
    // 当前 committed 节点保持不变；这里只发布一帧，让 reference renderer
    // 在 Timer 渲染链中刷新已经关闭的 Box/Plane 控件。
    if (m_referenceService) {
        m_referenceService->SetDirty();
    }
    return true;
}

bool CropBridge::Impl::GetCropActive() const
{
    return m_isActive;
}

bool CropBridge::Impl::GetCropBound() const
{
    // Render input 是否仍匹配由具体 history 动作检查；Host 用结构 binding
    // 在 Exit 后继续跟踪输入换代，并及时退休旧 history。
    return m_referenceService
        && !m_targets.empty();
}

CropHistoryState CropBridge::Impl::GetCropHistory() const
{
    return CropHistoryState{
        m_cursor,
        m_history.size(),
        m_removalMode,
        m_draftIndex.has_value(),
        m_isActive,
        m_baseNodeCount,
        m_allHistory.size()
    };
}

CropBridge::CropBridge()
    : m_impl(std::make_unique<Impl>())
{
}

CropBridge::~CropBridge() = default;

bool CropBridge::StartView(const CropViewRequest& request) { return m_impl->StartView(request); }
bool CropBridge::StartView(
    const CropViewRequest& request,
    CropInputSnapshot input)
{
    return m_impl->StartView(request, std::move(input));
}
bool CropBridge::ClearBindings() { return m_impl->ClearBindings(); }
bool CropBridge::SetCropInput(CropInputSnapshot input) { return m_impl->SetCropInput(std::move(input)); }
bool CropBridge::StartCropBaseline(
    CropInputSnapshot input,
    const std::size_t baseNodeCount)
{
    return m_impl->StartCropBaseline(
        std::move(input), baseNodeCount);
}
bool CropBridge::SetCropBaselineComplete() noexcept { return m_impl->SetCropBaselineComplete(); }
bool CropBridge::ClearCropBaseline() { return m_impl->ClearCropBaseline(); }
bool CropBridge::SwitchCropBox() { return m_impl->SwitchCrop(CropShape::Box); }
bool CropBridge::SwitchCropPlane() { return m_impl->SwitchCrop(CropShape::Plane); }
bool CropBridge::SetCropMode(const CropRemovalMode removalMode) { return m_impl->SetCropMode(removalMode); }
bool CropBridge::PreviousCrop() { return m_impl->PreviousCrop(); }
bool CropBridge::NextCrop() { return m_impl->NextCrop(); }
bool CropBridge::SetCropNode(const std::size_t nodeCount) { return m_impl->SetCropNode(nodeCount); }
bool CropBridge::ExitCrop() { return m_impl->ExitCrop(); }
bool CropBridge::GetCropActive() const { return m_impl->GetCropActive(); }
bool CropBridge::GetCropBound() const { return m_impl->GetCropBound(); }
CropHistoryState CropBridge::GetCropHistory() const { return m_impl->GetCropHistory(); }
bool CropBridge::GetShaderTickNeeded() const { return m_impl->GetShaderTickNeeded(); }
bool CropBridge::SendShaderCommit() { return m_impl->SendShaderCommit(); }
bool CropBridge::ExportCrop(CropExportCallback onComplete) { return m_impl->ExportCrop(std::move(onComplete)); }
bool CropBridge::GetExportTickNeeded() const { return m_impl->GetExportTickNeeded(); }
bool CropBridge::SendExportResult() { return m_impl->SendExportResult(); }
