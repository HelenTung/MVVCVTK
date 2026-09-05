#include "App/Services/FeatureViewService.h"
#include "Host/Internal/HostFrameRuntime.h"
#include "Interaction/AbstractViewContext.h"
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkCommand.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderWindow.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

class HostFrameRuntime::RenderObserver final : public vtkCommand {
public:
    static RenderObserver* New() { return new RenderObserver; }
    void Execute(vtkObject*, unsigned long eventId, void*) override
    {
        isComplete = eventId == vtkCommand::EndEvent;
    }
    bool isComplete = false;
};

void HostFrameRuntime::SetDriveMode(const HostDriveMode mode) noexcept
{
    m_isHostDriven = mode == HostDriveMode::HostDriven;
}

HostFrameRuntime::HostFrameRuntime(std::vector<HostRenderViewRuntime>& views,
    const std::shared_ptr<FeatureViewLease>& lease,
    std::function<std::vector<std::string>(const HostRenderViewRuntime&)> onFeatureIds)
    : m_views(views), m_lease(lease), m_onFeatureIds(std::move(onFeatureIds))
{
}
void HostFrameRuntime::BuildSceneStates()
{
    Clear();
    m_sceneStates.reserve(m_views.size());
    for (const auto& view : m_views)
        m_sceneStates.push_back(view.BuildSceneViewState(m_onFeatureIds(view)));
}
void HostFrameRuntime::Clear() noexcept
{
    m_frameIntents.clear();
    m_frameStage.reset();
    m_sceneStates.clear();
    m_renderOrder.clear();
    m_sessionGeneration = 0;
    m_committedEpoch = 0;
}
std::optional<HostSceneViewState> HostFrameRuntime::GetSceneState(const std::size_t index) const
{
    return index < m_sceneStates.size()
        ? std::optional<HostSceneViewState>(m_sceneStates[index]) : std::nullopt;
}
std::vector<HostSceneViewState> HostFrameRuntime::GetSceneStates() const
{
    return m_sceneStates;
}
void HostFrameRuntime::SetViewUnavailable(const std::size_t index) noexcept
{
    if (index >= m_views.size()) return;
    auto& view = m_views[index];
    view.pendingRenderEpoch = 0;
    view.renderedEpoch = view.appliedEpoch;
    const auto setUnavailable = [&view](HostSceneViewState& state) {
        state.isAvailable = false;
        state.renderedEpoch = view.renderedEpoch;
        state.presentation.reset();
        state.camera.reset();
        state.activeFeatureIds.clear();
    };
    m_renderOrder.erase(std::remove(m_renderOrder.begin(), m_renderOrder.end(), index), m_renderOrder.end());
    if (index < m_sceneStates.size()) setUnavailable(m_sceneStates[index]);
    // 不改变 topology 长度；已领取 dirty 的候选也不能再次发布停用 View。
    if (m_frameStage && index < m_frameStage->sceneStates.size()) {
        setUnavailable(m_frameStage->sceneStates[index]);
        m_frameStage->renderNeeded[index] = false;
        auto& order = m_frameStage->renderOrder;
        order.erase(std::remove(order.begin(), order.end(), index), order.end());
    }
}

std::optional<std::size_t>
HostFrameRuntime::GetViewIndexById(
    const std::string_view viewId) const
{
    for (std::size_t index = 0; index < m_views.size(); ++index) {
        const auto& id = m_views[index].config.id;
        if (id.size() == viewId.size()
            && std::equal(id.begin(), id.end(), viewId.begin())) {
            return index;
        }
    }
    return std::nullopt;
}

int HostFrameRuntime::GetRenderPriority(
    const HostSceneViewState& state) noexcept
{
    if (state.presentation && state.presentation->isInteracting) return 0;
    switch (state.role) {
    case HostRenderViewRole::TopDownSlice:
    case HostRenderViewRole::FrontBackSlice:
    case HostRenderViewRole::LeftRightSlice:
        return 1;
    case HostRenderViewRole::Primary3D:
        return 2;
    case HostRenderViewRole::Composite3D:
        return 3;
    case HostRenderViewRole::Auxiliary:
        return 4;
    }
    return 4;
}

bool HostFrameRuntime::SetFrameGeneration(
    const std::uint64_t sessionGeneration)
{
    if (sessionGeneration == 0 || !m_lease
        || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()
        || m_frameStage || GetFrameRenderPending()) {
        return false;
    }
    m_sessionGeneration = sessionGeneration;
    m_committedEpoch = 0;
    for (auto& view : m_views) {
        view.appliedEpoch = 0;
        view.renderedEpoch = 0;
        view.pendingRenderEpoch = 0;
    }
    for (auto& state : m_sceneStates) {
        state.sceneEpoch = 0;
        state.renderedEpoch = 0;
    }
    return true;
}

bool HostFrameRuntime::SetFrameIntents(
    const std::vector<HostFrameIntent>& intents)
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()
        || m_frameStage || !m_frameIntents.empty()) {
        return false;
    }
    try {
        m_frameIntents = intents;
        return true;
    }
    catch (...) {
        m_frameIntents.clear();
        return false;
    }
}

bool HostFrameRuntime::CollectFrameUpdates()
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()
        || m_frameStage || (!m_isHostDriven && GetFrameRenderPending())) {
        return false;
    }
    const PhaseGuard phase(m_executionDepth);

    // 所有 View 先完整应用本轮 pending；任一失败时不领取任何 dirty，
    // 因而第一个可见 Render 不可能越过全 View apply barrier。
    for (const auto& view : m_views) {
        if (!view.CollectUpdates()) return false;
    }
    return true;
}

bool HostFrameRuntime::ApplyFrameUpdates()
{
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()
        || m_frameStage || (!m_isHostDriven && GetFrameRenderPending())) {
        return false;
    }
    const PhaseGuard phase(m_executionDepth);
    for (const auto& view : m_views) {
        if (!view.SendPendingUpdates()) return false;
    }
    return true;
}

HostFrameStageStatus HostFrameRuntime::BuildFrameStage(
    const std::uint64_t nextEpoch)
{
    if (nextEpoch == 0 || nextEpoch != m_committedEpoch + 1
        || !m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()
        || m_frameStage || (!m_isHostDriven && GetFrameRenderPending())) {
        return HostFrameStageStatus::Failed;
    }
    const PhaseGuard phase(m_executionDepth);

    // Feature intent 必须在 App pending 全部应用之后重新校验输入戳。
    // FeatureHostControlPort 已把 featureId 固定为当前已附加 Feature，因而
    // removal delta 允许引用刚刚从 active 集合移除的目标 View。
    for (const auto& intent : m_frameIntents) {
        if (intent.featureId.empty()
            || intent.sessionGeneration != m_sessionGeneration
            || intent.baseSceneEpoch != m_committedEpoch) {
            continue;
        }

        std::vector<std::size_t> targetIndices;
        targetIndices.reserve(intent.delta.viewIds.size());
        bool hasInvalidTarget = false;
        for (const auto& viewId : intent.delta.viewIds) {
            const auto index = GetViewIndexById(viewId);
            if (!index || *index >= m_views.size()
                || !m_views[*index].isAvailable
                || !m_views[*index].interaction.update
                || !m_views[*index].GetIntentStampValid(intent.delta.inputStamp)) {
                hasInvalidTarget = true;
                if (intent.delta.scope
                    != FeatureSceneScope::BestEffort) {
                    break;
                }
                continue;
            }
            targetIndices.push_back(*index);
        }
        if ((hasInvalidTarget
                && intent.delta.scope
                    != FeatureSceneScope::BestEffort)
            || targetIndices.empty()) {
            continue;
        }

        bool isSet = true;
        for (const auto index : targetIndices) {
            isSet = m_views[index].SetRenderNeeded() && isSet;
        }
        if (!isSet
            && intent.delta.scope
                == FeatureSceneScope::RequiredAllViews) {
            return HostFrameStageStatus::Failed;
        }
    }

    std::vector<bool> renderNeeded(m_views.size(), false);
    std::vector<std::size_t> dirtyIndices;
    dirtyIndices.reserve(m_views.size());
    const auto restoreDirty = [this, &dirtyIndices]() noexcept {
        for (const auto index : dirtyIndices) {
            try {
                if (index < m_views.size()
                    && m_views[index].interaction.update) {
                    (void)m_views[index].SetRenderNeeded();
                }
            }
            catch (...) {
            }
        }
    };
    try {
        for (std::size_t index = 0; index < m_views.size(); ++index) {
            auto& view = m_views[index];
            if (!view.isAvailable) continue;
            if (view.ResetRenderNeeded()) {
                renderNeeded[index] = true;
                dirtyIndices.push_back(index);
            }
        }
    }
    catch (...) {
        restoreDirty();
        return HostFrameStageStatus::Failed;
    }
    if (dirtyIndices.empty()) {
        m_frameIntents.clear();
        return HostFrameStageStatus::Unchanged;
    }

    try {
        FrameStage stage;
        stage.epoch = nextEpoch;
        stage.renderNeeded = std::move(renderNeeded);
        stage.dirtyIndices = dirtyIndices;
        stage.renderOrder = dirtyIndices;
        // 新提交继承尚未绘制的 View，回滚仍只恢复本轮领取的 dirty。
        if (m_isHostDriven) {
            for (std::size_t index = 0; index < m_views.size(); ++index) {
                if (m_views[index].pendingRenderEpoch != 0 && !stage.renderNeeded[index]) {
                    stage.renderNeeded[index] = true;
                    stage.renderOrder.push_back(index);
                }
            }
        }
        stage.sceneStates.reserve(m_views.size());

        std::optional<DataRevisionRef> dataRevision;
        for (std::size_t index = 0; index < m_views.size(); ++index) {
            auto state = m_views[index].BuildSceneViewState(m_onFeatureIds(m_views[index]));
            if (m_views[index].isAvailable
                && (!state.presentation || !state.camera)) {
                restoreDirty();
                return HostFrameStageStatus::Failed;
            }
            if (m_views[index].isAvailable && state.presentation) {
                const auto currentRevision =
                    state.presentation->dataRevision;
                if (GetDataRevisionRefValid(currentRevision)) {
                    if (dataRevision && *dataRevision != currentRevision) {
                        restoreDirty();
                        return HostFrameStageStatus::Failed;
                    }
                    dataRevision = currentRevision;
                }
            }
            state.sceneEpoch = nextEpoch;
            state.renderedEpoch = (m_isHostDriven || stage.renderNeeded[index])
                ? m_views[index].renderedEpoch : nextEpoch;
            stage.sceneStates.push_back(std::move(state));
        }
        std::stable_sort(
            stage.renderOrder.begin(), stage.renderOrder.end(),
            [&stage](const std::size_t left, const std::size_t right) {
                return GetRenderPriority(stage.sceneStates[left])
                    < GetRenderPriority(stage.sceneStates[right]);
            });
        m_frameStage = std::move(stage);
        return HostFrameStageStatus::Ready;
    }
    catch (...) {
        restoreDirty();
        return HostFrameStageStatus::Failed;
    }
}

void HostFrameRuntime::SetFrameCommit(
    const std::uint64_t epoch) noexcept
{
    if (!m_frameStage || m_frameStage->epoch != epoch
        || m_frameStage->sceneStates.size() != m_views.size()
        || m_frameStage->renderNeeded.size() != m_views.size()) {
        std::terminate();
    }

    for (std::size_t index = 0; index < m_views.size(); ++index) {
        auto& view = m_views[index];
        if (!view.isAvailable) continue;
        view.appliedEpoch = epoch;
        if (m_frameStage->renderNeeded[index]) {
            view.pendingRenderEpoch = epoch;
        }
        else {
            view.pendingRenderEpoch = 0;
            if (!m_isHostDriven) view.renderedEpoch = epoch;
        }
    }
    m_sceneStates.swap(m_frameStage->sceneStates);
    m_renderOrder.swap(m_frameStage->renderOrder);
    m_frameIntents.clear();
    m_committedEpoch = epoch;
    m_frameStage.reset();
}

bool HostFrameRuntime::SendFrameRender(
    const std::uint64_t epoch)
{
    if (epoch == 0 || epoch != m_committedEpoch
        || !m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) {
        return false;
    }
    const PhaseGuard phase(m_executionDepth);

    const auto renderOrder = m_renderOrder;
    for (const auto index : renderOrder) {
        if (index >= m_views.size()) return false;
        auto& view = m_views[index];
        if (view.pendingRenderEpoch != epoch) continue;
        if (!view.SendRender(epoch)) continue;
        if (index < m_sceneStates.size()) {
            m_sceneStates[index].renderedEpoch = epoch;
        }
    }

    if (GetFrameRenderPending()) return false;
    m_renderOrder.clear();
    return true;
}

std::vector<std::string>
HostFrameRuntime::GetRenderViewIds() const
{
    std::vector<std::string> ids;
    if (!m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread()) return ids;
    for (const auto& view : m_views) {
        if (view.pendingRenderEpoch != 0) ids.push_back(view.config.id);
    }
    return ids;
}

HostRenderResult HostFrameRuntime::SendFrameRender(
    const HostRenderRequest& request,
    const std::function<bool()>& getIsRunning)
{
    HostRenderResult result;
    if (!m_isHostDriven || !m_lease || !m_lease->GetIsActive()
        || !m_lease->GetIsOwnerThread() || m_frameStage
        || !std::isfinite(request.desiredUpdateRate)
        || request.desiredUpdateRate <= 0.0) return result;

    const PhaseGuard phase(m_executionDepth);
    std::vector<std::size_t> indices;
    for (const auto& id : request.viewIds) {
        const auto index = GetViewIndexById(id);
        if (!index || std::find(indices.begin(), indices.end(), *index)
                != indices.end()) return result;
        indices.push_back(*index);
    }
    result.status = HostRenderStatus::Unchanged;
    result.views.reserve(indices.size());
    const auto epoch = m_committedEpoch;
    // 任一 View 存在尚未提交的更改时，不绘制混合状态。
    const bool hasUncommitted = std::any_of(m_views.begin(), m_views.end(),
        [](const HostRenderViewRuntime& view) {
            return view.isAvailable && view.interaction.update
                && view.interaction.update->GetRenderNeeded();
        });
    for (const auto index : indices) {
        auto& view = m_views[index];
        HostViewRenderResult output;
        output.viewId = view.config.id;
        output.sceneEpoch = epoch;
        output.status = HostRenderStatus::Unchanged;
        auto* window = view.context ? view.context->GetRenderWindow() : nullptr;
        auto* generic = vtkGenericOpenGLRenderWindow::SafeDownCast(window);
        if (getIsRunning && !getIsRunning()) {
            output.status = HostRenderStatus::Stopped;
        }
        else if (!view.isAvailable || !window) {
            output.status = HostRenderStatus::Failed;
        }
        else if (hasUncommitted || (generic && !generic->GetReadyForRendering())) {
            output.status = HostRenderStatus::Deferred;
        }
        else if (view.pendingRenderEpoch != 0) {
            // 状态由 VTK observer 自身拥有；异常路径不留下指向调用栈的 clientData。
            auto observer = vtkSmartPointer<RenderObserver>::New();
            const auto tag = window->AddObserver(vtkCommand::EndEvent, observer);
            try {
                window->SetDesiredUpdateRate(request.desiredUpdateRate);
                // VTK style 自身也写预算；把两个 style 源同步到当前宿主值。
                if (auto* interactor = window->GetInteractor()) {
                    interactor->SetDesiredUpdateRate(request.desiredUpdateRate);
                    interactor->SetStillUpdateRate(request.desiredUpdateRate);
                }
                const auto start = std::chrono::steady_clock::now();
                const bool isSent = tag != 0 && view.context->SendRender();
                output.durationUs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start).count());
                output.status = isSent && observer->isComplete
                    ? HostRenderStatus::Rendered : HostRenderStatus::Deferred;
            }
            catch (...) { output.status = HostRenderStatus::Failed; }
            if (tag != 0) window->RemoveObserver(tag);
            if (output.status == HostRenderStatus::Rendered) {
                view.pendingRenderEpoch = 0;
                view.renderedEpoch = epoch;
                m_sceneStates[index].renderedEpoch = epoch;
                if (view.interaction.update) view.interaction.update->SetRenderComplete(
                    std::max<std::uint64_t>(1, output.durationUs));
            }
        }
        if (output.status == HostRenderStatus::Stopped)
            result.status = HostRenderStatus::Stopped;
        else if (output.status == HostRenderStatus::Failed
            && result.status != HostRenderStatus::Stopped)
            result.status = HostRenderStatus::Failed;
        else if (output.status == HostRenderStatus::Deferred
            && result.status != HostRenderStatus::Failed
            && result.status != HostRenderStatus::Stopped)
            result.status = HostRenderStatus::Deferred;
        else if (output.status == HostRenderStatus::Rendered
            && result.status == HostRenderStatus::Unchanged)
            result.status = HostRenderStatus::Rendered;
        result.views.push_back(std::move(output));
    }
    return result;
}

bool HostFrameRuntime::GetFrameRenderPending() const noexcept
{
    return std::any_of(
        m_views.begin(), m_views.end(),
        [](const HostRenderViewRuntime& view) {
            return view.pendingRenderEpoch != 0;
        });
}

void HostFrameRuntime::SendFrameCompletions() noexcept
{
    if (!m_lease || !m_lease->GetIsOwnerThread()) return;
    const PhaseGuard phase(m_executionDepth);
    for (const auto& view : m_views) view.SendCompletions();
}

void HostFrameRuntime::ClearFrameStage() noexcept
{
    m_frameIntents.clear();
    if (!m_frameStage) return;
    for (const auto index : m_frameStage->dirtyIndices) {
        try {
            if (index < m_views.size()
                && m_views[index].interaction.update) {
                (void)m_views[index].SetRenderNeeded();
            }
        }
        catch (...) {
        }
    }
    m_frameStage.reset();
}
