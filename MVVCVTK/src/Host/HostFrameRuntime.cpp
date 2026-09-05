#include "App/Services/FeatureViewService.h"
#include "Host/Internal/HostFrameRuntime.h"
#include "Interaction/AbstractViewContext.h"
#include "Data/DataPayloads.h"
#include <vtkMatrix3x3.h>
#include <array>
#include <set>
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
void HostFrameRuntime::SetDataRead(std::shared_ptr<const TrustedDataReadPort> data)
{
    m_dataRead = std::move(data);
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
    m_sceneGraph = {};
    m_sceneBindings.clear();
    m_dataRead.reset();
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
        state.inputs.clear();
        state.displays.clear();
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

bool HostFrameRuntime::GetSceneInputsValid(
    const DataGraphSnapshot& graph, const FeatureSceneDelta& delta)
{
    if (!graph.view) return delta.inputs.empty() && delta.expectations.empty()
        && delta.displays.empty();
    for (const auto& input : delta.inputs) {
        const auto data = graph.view->GetData(input.source);
        if (!data || data->self != input.source) return false;
    }
    for (const auto& display : delta.displays) {
        const auto data = graph.view->GetData(display.data);
        if (!data || data->self != display.data) return false;
    }
    for (const auto& expected : delta.expectations) {
        if (expected.kind == DataExpectationKind::Binding) {
            if (expected.binding.empty()) return false;
            const auto binding = graph.view->GetDataBinding(expected.binding);
            if ((binding ? binding->revision : 0) != expected.expectedBindingRevision
                || (expected.isTargetChecked
                    && (binding ? binding->target : std::optional<DataRevisionRef>{})
                        != expected.expectedTarget)) return false;
        }
        else if (expected.kind == DataExpectationKind::EntityHead) {
            if (!GetDataEntityIdValid(expected.entityId)) return false;
            DataQuery query;
            query.entityId = expected.entityId;
            const auto result = graph.view->GetDataQuery(query);
            DataGeneration generation = 0;
            for (const auto& data : result.data) {
                if (data) generation = (std::max)(generation, data->self.generation);
            }
            if (generation != expected.expectedGeneration) return false;
        }
        else return false;
    }
    return true;
}

std::optional<DataRevisionRef> HostFrameRuntime::GetDataSource(
    const DataGraphSnapshot& graph, const DataRevisionRef& data)
{
    if (!graph.view) return std::nullopt;
    std::set<DataRevisionRef> visited;
    std::vector<DataRevisionRef> pending{ data };
    std::optional<DataRevisionRef> source;
    while (!pending.empty()) {
        const auto ref = pending.back();
        pending.pop_back();
        if (!visited.insert(ref).second) continue;
        const auto revision = graph.view->GetData(ref);
        if (!revision) return std::nullopt;
        if (revision->inputs.empty()
            && dynamic_cast<const ImageGrid3DPayload*>(revision->payload.get())) {
            if (source && *source != ref) return std::nullopt;
            source = ref;
        }
        for (const auto& input : revision->inputs) pending.push_back(input.source);
    }
    return source;
}

bool HostFrameRuntime::GetGridCompatible(
    const GridGeometry3D& data, const GridGeometry3D& source, const bool isLabel)
{
    if (!GetGridGeometryValid(data) || !GetGridGeometryValid(source)
        || data.coordinateFrame != source.coordinateFrame) return false;
    constexpr double tolerance = 1e-7;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(data.spacing[axis] - source.spacing[axis])
            > tolerance * source.spacing[axis]) return false;
    }
    for (std::size_t index = 0; index < 9; ++index) {
        if (std::abs(data.direction[index] - source.direction[index]) > tolerance)
            return false;
    }
    if (std::abs(vtkMatrix3x3::Determinant(source.direction.data())) < tolerance)
        return false;
    double inverse[9];
    vtkMatrix3x3::Invert(source.direction.data(), inverse);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        double offset = 0.0;
        for (std::size_t component = 0; component < 3; ++component) {
            offset += inverse[axis * 3 + component]
                * (data.origin[component] - source.origin[component]);
        }
        offset /= source.spacing[axis];
        if (!std::isfinite(offset) || std::abs(offset - std::round(offset)) > tolerance)
            return false;
        const double minimum = offset + data.extent[axis * 2];
        const double maximum = offset + data.extent[axis * 2 + 1];
        if (minimum < source.extent[axis * 2] - tolerance
            || maximum > source.extent[axis * 2 + 1] + tolerance) return false;
        if (isLabel && (std::abs(minimum - source.extent[axis * 2]) > tolerance
            || std::abs(maximum - source.extent[axis * 2 + 1]) > tolerance)) return false;
    }
    return true;
}

bool HostFrameRuntime::GetDisplayInputsValid(
    const DataGraphSnapshot& graph, const FeatureSceneDelta& delta)
{
    if (delta.displays.empty()) return true;
    const auto sourceInput = std::find_if(delta.inputs.begin(), delta.inputs.end(),
        [](const auto& input) { return input.role == "source-volume"; });
    if (sourceInput == delta.inputs.end() || !graph.view) return false;
    const auto source = graph.view->GetData(sourceInput->source);
    const auto* image = source
        ? dynamic_cast<const ImageGrid3DPayload*>(source->payload.get()) : nullptr;
    const auto root = GetDataSource(graph, sourceInput->source);
    if (!image || !root) return false;
    std::vector<DataRevisionRef> references;
    for (const auto& input : delta.inputs) references.push_back(input.source);
    for (const auto& display : delta.displays) {
        if (GetDataSource(graph, display.data) != root) return false;
        references.push_back(display.data);
    }
    const auto rootData = graph.view->GetData(*root);
    const auto* rootImage = rootData
        ? dynamic_cast<const ImageGrid3DPayload*>(rootData->payload.get()) : nullptr;
    if (!rootImage) return false;
    std::set<DataRevisionRef> visited;
    while (!references.empty()) {
        const auto ref = references.back();
        references.pop_back();
        if (!visited.insert(ref).second) continue;
        const auto data = graph.view->GetData(ref);
        if (!data || data->self != ref) return false;
        for (const auto& input : data->inputs) references.push_back(input.source);
        if (const auto* labels = dynamic_cast<const LabelMap3DPayload*>(data->payload.get())) {
            const auto declared = std::find_if(data->inputs.begin(), data->inputs.end(),
                [](const auto& input) { return input.role == "source-volume"; });
            const auto labelSource = declared != data->inputs.end()
                ? graph.view->GetData(declared->source) : nullptr;
            const auto* labelImage = labelSource
                ? dynamic_cast<const ImageGrid3DPayload*>(labelSource->payload.get()) : nullptr;
            if (GetDataSource(graph, ref) != root
                || !labelImage
                || !GetGridCompatible(labels->GetGeometry(), labelImage->GetGeometry(), true))
                return false;
        }
        else if (const auto* grid = dynamic_cast<const ImageGrid3DPayload*>(data->payload.get())) {
            if (GetDataSource(graph, ref) != root
                || !GetGridCompatible(grid->GetGeometry(), rootImage->GetGeometry(), false))
                return false;
        }
        else if (const auto* mesh = dynamic_cast<const SurfaceMeshPayload*>(data->payload.get())) {
            if (mesh->GetCoordinateFrame() != rootImage->GetGeometry().coordinateFrame) return false;
        }
        else if (const auto* transform = dynamic_cast<const Transform3DPayload*>(data->payload.get())) {
            // 本协议没有跨 frame 放置能力；显式非 identity 变换须由后续坐标契约处理。
            constexpr std::array<double, 16> identity{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            if (transform->GetSourceFrame() != rootImage->GetGeometry().coordinateFrame
                || transform->GetTargetFrame() != transform->GetSourceFrame()
                || transform->GetSourceToTarget() != identity) return false;
        }
    }
    return true;
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
            || (intent.attachment && !intent.attachment->load())
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
        // 只在有真实场景变化时冻结一次图；图的全局提交号不是渲染失效条件。
        // 后续 View 只在这份图中解析已采用的修订，不能逐 View 跟随最新图。
        stage.graph = m_dataRead ? m_dataRead->GetDataGraph()
            : DataGraphSnapshot{};
        stage.bindings = m_sceneBindings;
        for (auto current = stage.bindings.begin(); current != stage.bindings.end();) {
            const auto& intent = current->second;
            const auto index = GetViewIndexById(current->first.first);
            const auto activeFeatures = index && m_views[*index].isAvailable
                ? m_onFeatureIds(m_views[*index]) : std::vector<std::string>{};
            if ((intent.attachment && !intent.attachment->load())
                || std::find(activeFeatures.begin(), activeFeatures.end(), intent.featureId)
                    == activeFeatures.end()) current = stage.bindings.erase(current);
            else ++current;
        }
        for (const auto& intent : m_frameIntents) {
            if ((intent.attachment && !intent.attachment->load())
                || intent.sessionGeneration != m_sessionGeneration
                || intent.baseSceneEpoch != m_committedEpoch) continue;
            std::vector<std::size_t> targets;
            for (const auto& viewId : intent.delta.viewIds) {
                const auto index = GetViewIndexById(viewId);
                if (index && m_views[*index].isAvailable
                    && m_views[*index].GetIntentStampValid(intent.delta.inputStamp)) targets.push_back(*index);
            }
            if (targets.empty() || (intent.delta.scope != FeatureSceneScope::BestEffort
                && targets.size() != intent.delta.viewIds.size())) continue;
            if (!GetSceneInputsValid(stage.graph, intent.delta)) {
                restoreDirty();
                return HostFrameStageStatus::Failed;
            }
            if (!intent.delta.hasDisplayUpdate) continue;
            for (const auto& viewId : intent.delta.viewIds) {
                const auto index = GetViewIndexById(viewId);
                if (!index || !m_views[*index].isAvailable
                    || !m_views[*index].GetIntentStampValid(intent.delta.inputStamp)) continue;
                const auto key = std::make_pair(viewId, intent.featureId);
                const bool hasDisplay = std::any_of(intent.delta.displays.begin(),
                    intent.delta.displays.end(), [&](const auto& display) {
                        return display.viewId == viewId;
                    });
                if (hasDisplay) {
                    stage.bindings[key] = intent;
                    // expectation 只控制本次激活；已采用历史的依据是固定 ref，不能每帧重新激活。
                    stage.bindings[key].delta.expectations.clear();
                }
                else stage.bindings.erase(key);
            }
        }

        std::map<std::string, std::pair<DataRevisionRef, DataBindingRevision>> sceneInputs;
        std::map<std::string, DataRevisionRef> sceneSources;
        for (std::size_t index = 0; index < m_views.size(); ++index) {
            auto state = m_views[index].BuildSceneViewState(m_onFeatureIds(m_views[index]));
            if (m_views[index].isAvailable
                && (!state.presentation || !state.camera)) {
                restoreDirty();
                return HostFrameStageStatus::Failed;
            }
            if (m_views[index].isAvailable && state.presentation) {
                const auto& currentRevision = state.presentation->dataRevision;
                const auto currentInput = std::make_pair(
                    currentRevision, state.presentation->bindingRevision);
                if (currentRevision != DataRevisionRef{}) {
                    const auto data = GetDataRevisionRefValid(currentRevision)
                        && stage.graph.view
                        ? stage.graph.view->GetData(currentRevision) : nullptr;
                    if (!data || data->self != currentRevision
                        || currentInput.second == 0) {
                        restoreDirty();
                        return HostFrameStageStatus::Failed;
                    }
                    // 只比较已有输入的 View；辅助视图可以尚未采用主体数据。
                    // 比较已应用 binding，不要求它等于图中最新的 primary binding。
                    const auto& group = m_views[index].config.synchronizationGroup;
                    const auto sceneInput = sceneInputs.find(group);
                    if (m_views[index].config.syncPolicy == HostViewSyncPolicy::SamePrimary
                        && sceneInput != sceneInputs.end() && sceneInput->second != currentInput) {
                        restoreDirty();
                        return HostFrameStageStatus::Failed;
                    }
                    sceneInputs[group] = currentInput;
                    if (m_views[index].config.syncPolicy == HostViewSyncPolicy::ExplicitInputs) {
                        const auto source = GetDataSource(stage.graph, currentRevision);
                        const auto previous = sceneSources.find(group);
                        if (!source || (previous != sceneSources.end() && previous->second != *source)) {
                            restoreDirty();
                            return HostFrameStageStatus::Failed;
                        }
                        FeatureSceneDelta primary;
                        primary.inputs = { { "source-volume", *source } };
                        primary.displays.push_back({ state.id, {}, {}, currentRevision, {} });
                        if (!GetDisplayInputsValid(stage.graph, primary)) {
                            restoreDirty();
                            return HostFrameStageStatus::Failed;
                        }
                        sceneSources[group] = *source;
                    }
                    state.inputs.push_back({ "primary", currentRevision });
                }
                else if (currentInput.second != 0) {
                    restoreDirty();
                    return HostFrameStageStatus::Failed;
                }
            }
            state.sceneEpoch = nextEpoch;
            state.graphCommitId = stage.graph.commitId;
            for (const auto& [key, binding] : stage.bindings) {
                if (key.first != state.id) continue;
                if (!GetSceneInputsValid(stage.graph, binding.delta)) {
                    restoreDirty();
                    return HostFrameStageStatus::Failed;
                }
                if (m_views[index].config.syncPolicy == HostViewSyncPolicy::ExplicitInputs) {
                    if (!GetDisplayInputsValid(stage.graph, binding.delta)) {
                        restoreDirty();
                        return HostFrameStageStatus::Failed;
                    }
                    for (const auto& display : binding.delta.displays) {
                        if (display.viewId != state.id) continue;
                        const auto source = GetDataSource(stage.graph, display.data);
                        const auto& group = m_views[index].config.synchronizationGroup;
                        const auto previous = sceneSources.find(group);
                        if (!source || (previous != sceneSources.end() && previous->second != *source)) {
                            restoreDirty();
                            return HostFrameStageStatus::Failed;
                        }
                        sceneSources[group] = *source;
                    }
                }
                for (const auto& input : binding.delta.inputs) {
                    state.inputs.push_back({ key.second + "/" + input.role, input.source });
                }
                for (const auto& display : binding.delta.displays) {
                    if (display.viewId == state.id) state.displays.push_back(display);
                }
            }
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
    std::swap(m_sceneGraph, m_frameStage->graph);
    m_sceneBindings.swap(m_frameStage->bindings);
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
