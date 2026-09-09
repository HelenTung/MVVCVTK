#include "Host/MetrologyAlignmentHostFeature.h"
#include "../../common/FeatureResultScopes.h"
#include "AlignmentData.h"
#include "AlignmentMath.h"
#include "AlignmentOverlay.h"
#include "AlignmentSolver.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkPolyData.h>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
void NotifyOwner(const std::weak_ptr<FeatureHostControl>& weakHost) noexcept {
    try { if (const auto host = weakHost.lock()) (void)host->SendWorkAvailable(); }
    catch (...) {}
}
class CommitGuard final {
  public:
    explicit CommitGuard(bool &value) : m_value(value) {
        m_value = true;
    }
    ~CommitGuard() {
        m_value = false;
    }

  private:
    bool &m_value;
};
void SendComplete(AlignmentCallback callback, AlignmentResult result) noexcept {
    if (callback)
        try {
            callback(std::move(result));
        } catch (...) { /* 用户回调异常不能反转已发布事务。 */
        }
}
bool GetSourceRelated(const DataGraphSnapshot &graph, const DataRevisionRef &mesh,
                      const DataRevisionRef &source) {
    std::vector<DataRevisionRef> pending{mesh}, visited;
    while (!pending.empty() && visited.size() < 1024) {
        const auto ref = pending.back();
        pending.pop_back();
        if (ref == source)
            return true;
        if (std::find(visited.begin(), visited.end(), ref) != visited.end())
            continue;
        visited.push_back(ref);
        const auto data = graph.view->GetData(ref);
        if (!data)
            return false;
        if (data->inputs.size() > 1024 - std::min<std::size_t>(pending.size(), 1024)) return false;
        for (const auto &input : data->inputs)
            pending.push_back(input.source);
    }
    return false;
}
} // namespace

class MetrologyAlignmentHostFeature::Impl final {
  public:
    explicit Impl(AlignmentConfig config) : m_config(std::move(config)) {
        if (m_config.pointLimit == 0 || m_config.pointLimit > 100000 ||
            m_config.constraintLimit == 0 || m_config.constraintLimit > 4096 ||
            m_config.workingBytes < 1024 || m_config.deadlineMs == 0 ||
            m_config.deadlineMs > 3600000 || !std::isfinite(m_config.axisLength) ||
            m_config.axisLength <= 0)
            throw std::invalid_argument("Invalid alignment resource configuration.");
        m_state.isOverlayVisible = m_config.isOverlayVisible;
    }
    ~Impl() {
        if (m_task) {
            m_task->work.cancelled->store(true);
            if (m_task->worker.joinable())
                m_task->worker.join();
        }
        if (GetIsOwner()) {
            ClearDisplay();
            if (m_data && m_observer)
                m_data->DetachDataChange(m_observer);
        }
    }
    bool GetIsOwner() const {
        return m_owner == std::this_thread::get_id();
    }
    bool AttachHost(const HostFeatureContext &context) {
        if (m_state.isAttached || !context.data)
            return false;
        m_owner = std::this_thread::get_id();
        try {
            if (!AlignmentData::SetTypes(*context.data))
                return false;
            auto dirty = std::make_shared<std::atomic<bool>>(false);
            const auto observer = context.data->AttachDataChange(
                [dirty, host = std::weak_ptr<FeatureHostControl>(context.host)](const DataChangeSet &) {
                    dirty->store(true, std::memory_order_release);
                    NotifyOwner(host);
                });
            if (!observer)
                return false;
            m_dirty = std::move(dirty);
            m_observer = observer;
            m_data = context.data;
            m_views = context.views;
            m_host = context.host;
            m_state.isAttached = true;
            return true;
        } catch (...) {
            return false;
        }
    }
    bool DetachHost() {
        if (!m_state.isAttached)
            return true;
        if (!GetIsOwner() || m_isCommitting)
            return false;
        m_state.isStopping = true;
        if (m_task) {
            m_task->work.cancelled->store(true);
            if (!m_task->isDone.load(std::memory_order_acquire))
                return false;
            OnHostTick();
            if (!m_state.isAttached)
                return true;
        }
        if (m_observer && !m_data->DetachDataChange(m_observer))
            return false;
        m_observer = 0;
        ClearDisplay();
        if (!m_resultScopes.Clear(*m_data)) return false;
        m_data.reset();
        m_views.reset();
        m_host.reset();
        m_dirty.reset();
        m_state.isAttached = false;
        m_state.isStopping = false;
        m_state.isCurrent = false;
        return true;
    }
    AlignmentState GetState() const {
        if (!GetIsOwner())
            return {};
        auto state = m_state;
        if (m_data && GetDataRevisionRefValid(state.activeResult)) {
            const auto graph = m_data->GetDataGraph();
            const auto result = AlignmentData::GetResult(graph, state.activeResult);
            state.isCurrent =
                result &&
                AlignmentData::GetExpectationsCurrent(graph, result->record.expectations) &&
                AlignmentData::GetBinding(graph, result->record.input.scope).target ==
                    state.activeResult;
        }
        return state;
    }
    std::optional<AlignmentArchive> GetArchive(const DataRevisionRef &ref) const {
        if (!GetIsOwner() || !m_data)
            return {};
        const auto graph = m_data->GetDataGraph();
        const auto result = AlignmentData::GetResult(graph, ref);
        if (!result)
            return {};
        const auto recipe = std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(
            graph.view->GetData(result->record.recipe)->payload);
        const auto transform = std::dynamic_pointer_cast<const Transform3DPayload>(
            graph.view->GetData(result->record.transform)->payload);
        if (!recipe || !transform)
            return {};
        return AlignmentArchive{1, "metrology-alignment-1", recipe->recipe, result->record.input,
                                transform->GetSourceToTarget()};
    }
    AlignmentState GetScopeState(const std::string &scope) const {
        if (!GetIsOwner() || !m_data)
            return {};
        auto state = m_state;
        const auto graph = m_data->GetDataGraph();
        const auto binding = AlignmentData::GetBinding(graph, scope);
        state.activeResult = binding.target.value_or(DataRevisionRef{});
        const auto result = AlignmentData::GetResult(graph, state.activeResult);
        state.isCurrent =
            result && AlignmentData::GetExpectationsCurrent(graph, result->record.expectations);
        state.isDisplayReady =
            state.isCurrent && m_state.isDisplayReady && state.activeResult == m_state.activeResult;
        return state;
    }
    std::optional<AlignmentSnapshot> GetResult(const DataRevisionRef &ref) const {
        if (!GetIsOwner() || !m_data)
            return {};
        const auto graph = m_data->GetDataGraph();
        const auto result = AlignmentData::GetResult(graph, ref);
        if (!result)
            return {};
        const auto geometry = std::dynamic_pointer_cast<const AlignmentData::GeometryPayload>(
            graph.view->GetData(result->record.geometry)->payload);
        const auto table = std::dynamic_pointer_cast<const RecordTablePayload>(
            graph.view->GetData(result->record.residuals)->payload);
        if (!geometry || !table)
            return {};
        AlignmentSnapshot value{
            ref,
            result->record.transform,
            result->record.recipe,
            result->record.input,
            geometry->geometries,
            {},
            result->record.diagnostics,
            AlignmentData::GetExpectationsCurrent(graph, result->record.expectations)};
        const auto &columns = table->GetColumns();
        if (columns.size() != 6)
            return {};
        const auto *ids = std::get_if<std::vector<std::string>>(&columns[0].values);
        const auto *residuals = std::get_if<std::vector<double>>(&columns[1].values);
        const auto *tolerances = std::get_if<std::vector<double>>(&columns[2].values);
        const auto *kinds = std::get_if<std::vector<std::uint64_t>>(&columns[3].values);
        const auto *priorities = std::get_if<std::vector<std::uint64_t>>(&columns[4].values);
        const auto *used = std::get_if<std::vector<std::uint8_t>>(&columns[5].values);
        if (!ids || !residuals || !tolerances || !kinds || !priorities || !used ||
            !table->GetValid())
            return {};
        for (std::size_t i = 0; i < ids->size(); ++i) {
            if ((*kinds)[i] > 2 || (*priorities)[i] > 63 || (*used)[i] > 1)
                return {};
            value.residuals.push_back({(*ids)[i], static_cast<AlignmentConstraintKind>((*kinds)[i]),
                                       static_cast<std::uint32_t>((*priorities)[i]),
                                       (*residuals)[i], (*tolerances)[i], (*used)[i] != 0});
        }
        return value;
    }
    AlignmentAdmission SendRequest(AlignmentRequest request, AlignmentCallback callback) {
        if (!GetIsOwner() || !m_state.isAttached || m_state.isStopping)
            return {AlignmentAdmissionStatus::Unavailable, 0};
        if (m_isCommitting)
            return {AlignmentAdmissionStatus::Busy, 0};
        if (static_cast<unsigned>(request.action) > static_cast<unsigned>(AlignmentAction::Restore))
            return {};
        if ((request.targetRequestId && request.action != AlignmentAction::Cancel) ||
            (GetDataRevisionRefValid(request.recipeRef) &&
             request.action != AlignmentAction::Start &&
             request.action != AlignmentAction::SaveRecipe) ||
            (GetDataRevisionRefValid(request.resultRef) &&
             request.action != AlignmentAction::Activate) ||
            (GetDataRevisionRefValid(request.restoredNominal) &&
             request.action != AlignmentAction::Restore) ||
            (request.archive && request.action != AlignmentAction::Restore) ||
            (request.action != AlignmentAction::Start &&
             request.initialPoses != std::vector<AlignmentMatrix>{alignmentIdentity}))
            return {};
        if (m_task && request.action != AlignmentAction::Cancel)
            return {AlignmentAdmissionStatus::Busy, 0};
        if (m_nextRequest == std::numeric_limits<std::uint64_t>::max())
            return {AlignmentAdmissionStatus::Unavailable, 0};
        const auto id = m_nextRequest + 1;
        AlignmentResult result;
        result.requestId = id;
        try {
            if (request.action == AlignmentAction::Start) {
                if (!request.input || request.recipe || request.archive || request.isVisible ||
                    GetDataRevisionRefValid(request.resultRef) ||
                    !GetDataRevisionRefValid(request.recipeRef))
                    return {};
                auto task = BuildTask(request, id, std::move(callback));
                if (!task)
                    return {};
                m_nextRequest = id;
                // 槽位先有唯一 owner；worker 只发布结果并通知 owner，不执行业务回调或显示。
                m_task = task;
                m_state.isBusy = true;
                m_state.requestId = id;
                try {
                    task->worker = std::thread([task, host = std::weak_ptr<FeatureHostControl>(m_host)] {
                        try {
                            task->candidate = AlignmentSolver::BuildResult(task->work);
                        } catch (...) {
                            task->candidate.diagnostics.status = AlignmentStatus::InternalError;
                        }
                        task->isDone.store(true, std::memory_order_release);
                        NotifyOwner(host);
                    });
                } catch (...) {
                    m_task.reset();
                    m_state.isBusy = false;
                    result.status = AlignmentStatus::InternalError;
                    SendComplete(std::move(task->callback), std::move(result));
                }
                return {AlignmentAdmissionStatus::Accepted, id};
            }
            if (request.action == AlignmentAction::Cancel) {
                if (!m_task || (request.targetRequestId && request.targetRequestId != m_task->id) ||
                    request.input || request.recipe || request.archive || request.isVisible)
                    return {};
                m_task->work.cancelled->store(true);
                result.status = AlignmentStatus::Cancelled;
            } else if (request.action == AlignmentAction::SaveRecipe ||
                       request.action == AlignmentAction::Restore) {
                if (request.action == AlignmentAction::Restore) {
                    if (!request.archive || !request.input ||
                        !AlignmentData::GetInputValid(*request.input) || request.recipe ||
                        request.archive->schemaVersion != 1 ||
                        request.archive->algorithmVersion != "metrology-alignment-1" ||
                        !AlignmentMath::Rigid(request.archive->sourceToTarget) ||
                        !GetDataRevisionRefValid(request.restoredNominal) ||
                        request.input->unit != request.archive->recipe.unit)
                        return {};
                    request.recipe = request.archive->recipe;
                    request.recipe->nominalData = request.restoredNominal;
                    const auto graph = m_data->GetDataGraph();
                    if (!GetSourceRelated(graph, request.input->mesh, request.input->source))
                        return {};
                    const auto meshData = graph.view->GetData(request.input->mesh);
                    const auto mesh =
                        meshData
                            ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(meshData->payload)
                            : nullptr;
                    if (!mesh || mesh->GetCoordinateFrame() != request.input->coordinateFrame)
                        return {};
                    for (const auto &spec : request.recipe->geometries) {
                        if (!spec.region.vertexIds.empty() &&
                            spec.region.pinnedMesh != request.input->mesh)
                            return {};
                    }
                    if (!request.recipe->fitPairs.empty() &&
                        request.recipe->exactMesh != request.input->mesh)
                        return {};
                    // 只恢复经显式修订映射校验的配方；旧顶点编号不能映射到新网格。
                    (void)AlignmentData::BuildExpectations(graph, *request.input, {},
                                                           request.restoredNominal);
                    request.input.reset();
                }
                if (!request.recipe || request.input || request.isVisible ||
                    GetDataRevisionRefValid(request.resultRef) ||
                    !AlignmentGeometryFit::GetRecipeValid(*request.recipe, m_config))
                    return {};
                result = SetRecipe(*request.recipe, request.recipeRef, id);
            } else if (request.action == AlignmentAction::SetVisibility) {
                if (!request.isVisible || request.recipe || request.input || request.archive)
                    return {};
                m_state.isOverlayVisible = *request.isVisible;
                ClearDisplay();
                if (*request.isVisible && GetState().isCurrent)
                    SetDisplay(m_state.activeResult);
                result.status = AlignmentStatus::FullyDetermined;
                result.isDisplayReady = m_state.isDisplayReady;
            } else if (request.action == AlignmentAction::Activate) {
                if (request.recipe || request.input || request.archive || request.isVisible ||
                    !GetDataRevisionRefValid(request.resultRef))
                    return {};
                result = SetActive(request.resultRef, id);
            } else if (request.action == AlignmentAction::Deactivate) {
                if (request.recipe || request.archive || request.isVisible || !request.input ||
                    !AlignmentData::GetInputValid(*request.input))
                    return {};
                const auto graph = m_data->GetDataGraph();
                const auto binding = AlignmentData::GetBinding(graph, request.input->scope);
                DataTransaction tx;
                tx.bindings.push_back({binding.name, binding.revision, true, binding.target, {}});
                {
                    CommitGuard guard(m_isCommitting);
                    try {
                        const auto commit = m_resultScopes.Commit(*m_data,std::move(tx));
                        result.status = commit.status == DataCommitStatus::Succeeded
                                            ? AlignmentStatus::FullyDetermined
                                            : AlignmentStatus::Stale;
                    } catch (...) {
                        const auto current =
                            AlignmentData::GetBinding(m_data->GetDataGraph(), request.input->scope);
                        if (current.target || current.revision != binding.revision + 1)
                            throw;
                        result.status = AlignmentStatus::FullyDetermined;
                    }
                }
                if (result.status == AlignmentStatus::FullyDetermined &&
                    binding.target == m_state.activeResult) {
                    m_state.activeResult = {};
                    m_state.isCurrent = false;
                    ClearDisplay();
                }
            }
            m_nextRequest = id;
            m_state.lastStatus = result.status;
        } catch (const std::invalid_argument &) {
            return {};
        } catch (...) {
            result.status = AlignmentStatus::InternalError;
            m_nextRequest = id;
        }
        SendComplete(std::move(callback), std::move(result));
        return {AlignmentAdmissionStatus::Accepted, id};
    }
    bool OnHostTick() {
        if (!GetIsOwner() || !m_state.isAttached || m_isCommitting)
            return false;
        if (m_dirty && m_dirty->exchange(false, std::memory_order_acq_rel)) {
            const auto graph = m_data->GetDataGraph();
            if (m_task && !AlignmentData::GetExpectationsCurrent(graph, m_task->expectations)) {
                m_task->isStale = true;
                m_task->work.cancelled->store(true);
            }
            if (GetDataRevisionRefValid(m_state.activeResult) && !GetState().isCurrent) {
                m_state.isCurrent = false;
                ClearDisplay();
            }
        }
        if (!m_task || !m_task->isDone.load(std::memory_order_acquire))
            return true;
        auto task = std::move(m_task);
        if (task->worker.joinable())
            task->worker.join();
        m_state.isBusy = false;
        AlignmentResult result;
        result.requestId = task->id;
        result.recipe = task->recipeRef;
        result.diagnostics = task->candidate.diagnostics;
        result.status = result.diagnostics.status;
        if (task->isStale ||
            !AlignmentData::GetExpectationsCurrent(m_data->GetDataGraph(), task->expectations))
            result.status = AlignmentStatus::Stale;
        else if (task->work.cancelled->load() || m_state.isStopping)
            result.status = AlignmentStatus::Cancelled;
        else if (result.status == AlignmentStatus::FullyDetermined ||
                 result.status == AlignmentStatus::Underconstrained ||
                 result.status == AlignmentStatus::Conflicting ||
                 result.status == AlignmentStatus::NotConverged) {
            try {
                auto tx = AlignmentData::BuildTransaction(
                    *m_data, task->work, task->recipeRef, task->candidate, task->expectations,
                    task->active, task->isActivationRequested, result.result, result.transform);
                DataCommitResult commit;
                {
                    CommitGuard guard(m_isCommitting);
                    commit = m_resultScopes.Commit(*m_data,std::move(tx));
                }
                if (commit.status != DataCommitStatus::Succeeded) {
                    result.status = commit.failureReason == DataCommitFailure::ExpectationFailed
                                        ? AlignmentStatus::Stale
                                        : AlignmentStatus::InternalError;
                    result.result = {};
                    result.transform = {};
                } else {
                    const auto graph = m_data->GetDataGraph();
                    result.isActivated =
                        AlignmentData::GetBinding(graph, task->work.input.scope).target ==
                            result.result &&
                        AlignmentData::GetExpectationsCurrent(graph, task->expectations);
                    if (result.isActivated) {
                        m_state.activeResult = result.result;
                        m_state.isCurrent = true;
                        ClearDisplay();
                        if (m_state.isOverlayVisible)
                            SetDisplay(result.result);
                    }
                    result.isDisplayReady = result.isActivated && m_state.isDisplayReady;
                }
            } catch (...) {
                // 对提交后抛异常的适配端口重新读取正式节点；存在时保留已发布事实。
                const auto graph = m_data->GetDataGraph();
                const auto published = AlignmentData::GetResult(graph, result.result);
                if (!published) {
                    result.result = {};
                    result.transform = {};
                    result.status = AlignmentStatus::InternalError;
                } else {
                    result.isActivated =
                        AlignmentData::GetBinding(graph, published->record.input.scope).target ==
                            result.result &&
                        AlignmentData::GetExpectationsCurrent(graph,
                                                              published->record.expectations);
                    if (result.isActivated) {
                        m_state.activeResult = result.result;
                        m_state.isCurrent = true;
                        ClearDisplay();
                    }
                }
            }
        }
        m_state.lastStatus = result.status;
        result.message = result.diagnostics.message;
        SendComplete(std::move(task->callback), std::move(result));
        return true;
    }

  private:
    struct Task final {
        std::uint64_t id = 0;
        AlignmentWork work;
        DataRevisionRef recipeRef;
        std::vector<DataExpectation> expectations;
        DataBinding active;
        AlignmentCallback callback;
        AlignmentCandidate candidate;
        std::atomic<bool> isDone{false};
        bool isStale = false, isActivationRequested = true;
        std::thread worker;
    };
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> port;
        std::shared_ptr<AlignmentOverlay> overlay;
    };
    std::shared_ptr<Task> BuildTask(const AlignmentRequest &request, std::uint64_t id,
                                    AlignmentCallback callback) {
        if (!AlignmentData::GetInputValid(*request.input) || request.initialPoses.empty() ||
            request.initialPoses.size() > 8 ||
            !std::all_of(request.initialPoses.begin(), request.initialPoses.end(),
                         AlignmentMath::Rigid))
            return {};
        const auto graph = m_data->GetDataGraph();
        if (!graph.view)
            return {};
        const auto recipeData = graph.view->GetData(request.recipeRef),
                   meshData = graph.view->GetData(request.input->mesh);
        if (!recipeData || !meshData)
            return {};
        const auto recipe =
            std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(recipeData->payload);
        const auto mesh = std::dynamic_pointer_cast<const SurfaceMeshPayload>(meshData->payload);
        if (!recipe || !mesh || !AlignmentGeometryFit::GetRecipeValid(recipe->recipe, m_config) ||
            recipe->recipe.unit != request.input->unit ||
            mesh->GetCoordinateFrame() != request.input->coordinateFrame ||
            !GetSourceRelated(graph, request.input->mesh, request.input->source))
            return {};
        auto task = std::make_shared<Task>();
        task->id = id;
        task->recipeRef = request.recipeRef;
        task->work.input = *request.input;
        task->work.recipe = recipe->recipe;
        task->work.config = m_config;
        task->work.mesh = mesh;
        task->work.poses = request.initialPoses;
        task->work.cancelled = std::make_shared<std::atomic<bool>>(false);
        task->work.deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(m_config.deadlineMs);
        task->expectations = AlignmentData::BuildExpectations(
            graph, *request.input, request.recipeRef, recipe->recipe.nominalData);
        task->active = AlignmentData::GetBinding(graph, request.input->scope);
        task->callback = std::move(callback);
        task->isActivationRequested = request.isActivationRequested;
        return task;
    }
    AlignmentResult SetRecipe(const AlignmentRecipe &recipe, const DataRevisionRef &previous,
                              std::uint64_t id) {
        const auto graph = m_data->GetDataGraph();
        AlignmentResult result;
        result.requestId = id;
        std::vector<DataInputRef> inputs{{"nominal", recipe.nominalData}};
        for (std::size_t i = 0; i < recipe.geometries.size(); ++i)
            if (!recipe.geometries[i].region.vertexIds.empty())
                inputs.push_back(
                    {"selection-" + std::to_string(i), recipe.geometries[i].region.pinnedMesh});
        if (!recipe.fitPairs.empty())
            inputs.push_back({"correspondence", recipe.exactMesh});
        DataTransaction tx;
        for (const auto &input : inputs) {
            if (!graph.view->GetData(input.source))
                throw std::invalid_argument("Recipe input is missing.");
        }
        DataEntityId entity;
        DataGeneration generation = 0;
        if (GetDataRevisionRefValid(previous)) {
            const auto old = graph.view->GetData(previous);
            if (!old || old->type != AlignmentData::recipeType ||
                previous.generation == std::numeric_limits<DataGeneration>::max())
                throw std::invalid_argument("Invalid recipe revision.");
            entity = previous.entityId;
            generation = previous.generation;
            if (graph.view->GetData({entity, generation + 1}))
                throw std::invalid_argument("Recipe is no longer current.");
        } else
            entity = m_data->CreateDataEntityId();
        tx.outputs.push_back({entity, generation, AlignmentData::recipeType, std::move(inputs),
                              std::make_shared<const AlignmentData::RecipePayload>(recipe),
                              DataProvenance{"metrology-alignment", "save-recipe",
                                             "metrology-alignment-1", "typed-recipe-payload"}});
        DataCommitResult commit;
        {
            CommitGuard guard(m_isCommitting);
            try {
                commit = m_resultScopes.Commit(*m_data,std::move(tx));
            } catch (...) {
                const auto published =
                    m_data->GetDataGraph().view->GetData({entity, generation + 1});
                if (!published || published->type != AlignmentData::recipeType)
                    throw;
                commit.status = DataCommitStatus::Succeeded;
            }
        }
        if (commit.status == DataCommitStatus::Succeeded) {
            result.status = AlignmentStatus::FullyDetermined;
            result.recipe = {entity, generation + 1};
        } else
            result.status = AlignmentStatus::InvalidInput;
        return result;
    }
    AlignmentResult SetActive(const DataRevisionRef &ref, std::uint64_t id) {
        AlignmentResult result;
        result.requestId = id;
        const auto graph = m_data->GetDataGraph();
        const auto value = AlignmentData::GetResult(graph, ref);
        if (!value) {
            result.status = AlignmentStatus::InvalidInput;
            return result;
        }
        const auto &record = value->record;
        result.diagnostics = record.diagnostics;
        result.status = record.diagnostics.status;
        if (record.diagnostics.status != AlignmentStatus::FullyDetermined ||
            !record.diagnostics.isQualityPassed)
            return result;
        if (!AlignmentData::GetExpectationsCurrent(graph, record.expectations)) {
            result.status = AlignmentStatus::Stale;
            return result;
        }
        const auto active = AlignmentData::GetBinding(graph, record.input.scope);
        DataTransaction tx;
        tx.expectations = record.expectations;
        tx.bindings.push_back({active.name, active.revision, true, active.target, ref});
        DataCommitResult commit;
        {
            CommitGuard guard(m_isCommitting);
            try {
                commit = m_resultScopes.Commit(*m_data,std::move(tx));
            } catch (...) {
                const auto current =
                    AlignmentData::GetBinding(m_data->GetDataGraph(), record.input.scope);
                if (current.target != ref || current.revision != active.revision + 1)
                    throw;
                commit.status = DataCommitStatus::Succeeded;
            }
        }
        result.isActivated =
            commit.status == DataCommitStatus::Succeeded &&
            AlignmentData::GetBinding(m_data->GetDataGraph(), record.input.scope).target == ref &&
            AlignmentData::GetExpectationsCurrent(m_data->GetDataGraph(), record.expectations);
        if (result.isActivated) {
            m_state.activeResult = ref;
            m_state.isCurrent = true;
            ClearDisplay();
            if (m_state.isOverlayVisible)
                SetDisplay(ref);
            result.result = ref;
            result.transform = record.transform;
            result.recipe = record.recipe;
            result.isDisplayReady = m_state.isDisplayReady;
        } else
            result.status = AlignmentStatus::Stale;
        return result;
    }
    void ClearDisplay() {
        for (auto i = m_overlays.rbegin(); i != m_overlays.rend(); ++i)
            i->port->RemoveOverlay(i->overlay);
        m_overlays.clear();
        m_state.isDisplayReady = false;
    }
    void SetDisplay(const DataRevisionRef &ref) {
        if (!m_views)
            return;
        std::vector<OverlayBinding> next;
        try {
            const auto graph = m_data->GetDataGraph();
            const auto result = AlignmentData::GetResult(graph, ref);
            if (!result)
                return;
            const auto geometry = std::dynamic_pointer_cast<const AlignmentData::GeometryPayload>(
                graph.view->GetData(result->record.geometry)->payload);
            const auto recipe = std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(
                graph.view->GetData(result->record.recipe)->payload);
            const auto matrix = std::dynamic_pointer_cast<const Transform3DPayload>(
                graph.view->GetData(result->record.transform)->payload);
            if (!geometry || !recipe || !matrix)
                return;
            const auto poly =
                AlignmentOverlay::BuildData(matrix->GetSourceToTarget(), geometry->geometries,
                                            recipe->recipe, m_config.axisLength);
            if (!poly)
                return;
            const auto views = m_views->GetViews(m_config.targetViews);
            if (views.empty())
                return;
            for (const auto &view : views) {
                if (view.role != HostRenderViewRole::Primary3D &&
                    view.role != HostRenderViewRole::Composite3D)
                    throw std::invalid_argument("Alignment overlay requires an explicit 3D view.");
                const auto port = m_views->GetOverlayPort(view.id);
                if (!port)
                    throw std::runtime_error("Overlay port unavailable.");
                auto overlay = std::make_shared<AlignmentOverlay>();
                overlay->SetInputData(poly);
                next.push_back({port, overlay});
                if (!port->AttachOverlay(overlay))
                    throw std::runtime_error("Overlay attach failed.");
            }
            m_overlays = std::move(next);
            m_state.isDisplayReady = true;
        } catch (...) {
            for (auto i = next.rbegin(); i != next.rend(); ++i)
                i->port->RemoveOverlay(i->overlay);
            m_state.isDisplayReady = false;
        }
    }
    AlignmentConfig m_config;
    AlignmentState m_state;
    std::thread::id m_owner;
    std::shared_ptr<TrustedDataPort> m_data;
    FeatureInternal::ResultScopes m_resultScopes;
    std::shared_ptr<FeatureViewDirectory> m_views;
    // 持有挂载端口；worker/observer 仍只捕获弱引用，解绑后不延长其生命期。
    std::shared_ptr<FeatureHostControl> m_host;
    std::shared_ptr<std::atomic<bool>> m_dirty;
    DataObserverId m_observer = 0;
    std::uint64_t m_nextRequest = 0;
    bool m_isCommitting = false;
    std::shared_ptr<Task> m_task;
    std::vector<OverlayBinding> m_overlays;
};

MetrologyAlignmentHostFeature::MetrologyAlignmentHostFeature(AlignmentConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}
MetrologyAlignmentHostFeature::~MetrologyAlignmentHostFeature() noexcept = default;
std::string_view MetrologyAlignmentHostFeature::GetFeatureId() const noexcept {
    return "metrology-alignment";
}
FeatureDataContract MetrologyAlignmentHostFeature::GetDataContract() const {
    return {{{"mesh", DataFacets::surfaceMesh, true},
             {"recipe", {"org.mvvcvtk.metrology-alignment.recipe"}, true}},
            {{"recipe", AlignmentData::recipeType, {{"org.mvvcvtk.metrology-alignment.recipe"}}},
             {"geometry", AlignmentData::geometryType, {{"org.mvvcvtk.metrology-alignment.geometry"}}},
             {"residuals", DataTypes::recordTable, {DataFacets::tabularRecords}},
             {"result", AlignmentData::resultType, {{"org.mvvcvtk.metrology-alignment.result"}}},
             {"transform", DataTypes::transform3D, {DataFacets::transform3D}}}};
}
bool MetrologyAlignmentHostFeature::AttachHost(const HostFeatureContext &context) {
    return m_impl->AttachHost(context);
}
bool MetrologyAlignmentHostFeature::DetachHost() {
    const auto keepAlive = weak_from_this().lock();
    return m_impl->DetachHost();
}
bool MetrologyAlignmentHostFeature::OnHostTick() {
    const auto keepAlive = weak_from_this().lock();
    return m_impl->OnHostTick();
}
AlignmentAdmission MetrologyAlignmentHostFeature::SendRequest(AlignmentRequest request,
                                                              AlignmentCallback onComplete) {
    const auto keepAlive = weak_from_this().lock();
    return m_impl->SendRequest(std::move(request), std::move(onComplete));
}
AlignmentState MetrologyAlignmentHostFeature::GetState() const {
    return m_impl->GetState();
}
AlignmentState MetrologyAlignmentHostFeature::GetScopeState(const std::string &scope) const {
    return m_impl->GetScopeState(scope);
}
std::optional<AlignmentSnapshot> MetrologyAlignmentHostFeature::GetResult(
    const DataRevisionRef &ref) const {
    return m_impl->GetResult(ref);
}
std::optional<AlignmentArchive> MetrologyAlignmentHostFeature::GetArchive(
    const DataRevisionRef &ref) const {
    return m_impl->GetArchive(ref);
}
