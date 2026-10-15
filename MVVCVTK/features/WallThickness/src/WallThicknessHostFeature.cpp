#include "Host/WallThicknessHostFeature.h"
#include "../../common/FeatureResultScopes.h"
#include "../../common/FeatureInputReads.h"
#include "ThicknessData.h"
#include "ThicknessOverlay.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <stdexcept>
#include <utility>

namespace
{
constexpr std::string_view featureId = "wall-thickness";
bool GetDisplayValid(const ThicknessDisplay& requested) noexcept
{
    try {
    auto display = requested;
    // 自动范围的输入占位不参与判定；正式范围在准备显示时由本 Feature 解析。
    if (display.rangeMode != ThicknessRangeMode::Manual) display.range = {0,1};
    return ThicknessOverlay::GetColorValid(display) && ThicknessAlgorithm::GetDisplayValid(display);
    } catch(...) { return false; }
}
void SendComplete(ThicknessCallback callback, const ThicknessResult &result) noexcept
{
    try
    {
        if (callback)
            callback(result);
    }
    catch (...)
    {
    }
}
} // namespace
class WallThicknessHostFeature::Impl final
{
    struct Signal final
    {
        std::atomic<bool> dirty{false};
        Impl *owner = nullptr; // 只在owner输入回调使用，数据通知不访问。
    };
    struct Completion final
    {
        std::uint64_t requestId = 0;
        ThicknessCallback callback;
        DataRevisionRef result;
        bool isSent = false;
    };
    static void SendCompletion(const std::shared_ptr<Completion> &complete,
                               const ThicknessResult &result) noexcept
    {
        if (!complete || complete->isSent)
            return;
        complete->isSent = true;
        SendComplete(std::move(complete->callback), result);
    }
    struct Task final
    {
        std::uint64_t requestId = 0;
        ThicknessAlgorithm::Work work;
        std::vector<DataExpectation> expectations;
        FeatureInternal::InputReads readers;
        DataBinding binding;
        std::shared_ptr<Completion> completion;
        std::optional<ThicknessDisplay> display;
        ThicknessAlgorithm::Candidate candidate;
        std::atomic<bool> isDone{false};
        std::thread worker;
        bool isStale = false;
    };
    struct Binding final
    {
        std::string viewId;
        std::shared_ptr<OverlayService> port;
        std::shared_ptr<ThicknessOverlay> overlay;
    };
    ThicknessConfig m_config;
    HostFeatureContext m_context;
    FeatureInternal::ResultScopes m_resultScopes;
    ThicknessState m_state;
    FeatureOperationState m_operation, m_displayOperation;
    std::vector<std::pair<DataRevisionRef,ThicknessDisplay>> m_resultDisplays;
    ThicknessDisplayData m_displayData;
    DataRevisionRef m_geometryMesh;
    std::shared_ptr<const std::vector<ThicknessSample>> m_geometrySamples;
    ThicknessDisplay GetSavedDisplay(const DataRevisionRef& ref) const {
        for(const auto& entry:m_resultDisplays)if(entry.first==ref)return entry.second;
        return ThicknessDisplay{};
    }
    void SaveDisplay(ThicknessDisplay&& requested,const std::array<double,2>& range) {
        auto found=std::find_if(m_resultDisplays.begin(),m_resultDisplays.end(),[&](const auto& e){return e.first==m_state.result;});
        if(found==m_resultDisplays.end())m_resultDisplays.emplace_back(m_state.result,std::move(requested));else found->second=std::move(requested);
        m_state.displayRange=m_active && !m_active->GetRecord().statistics.validCount ? std::nullopt:std::optional<std::array<double,2>>(range);
        m_state.displayResult=m_state.result;
    }
    std::thread::id m_ownerThread;
    std::shared_ptr<Signal> m_signal;
    DataObserverId m_observer = 0;
    bool m_hasInput = false;
    std::uint64_t m_nextId = 1, m_generation = 0;
    std::unique_ptr<Task> m_task;
    std::shared_ptr<Completion> m_completing;
    std::vector<Binding> m_bindings;
    std::shared_ptr<const ThicknessData::ResultPayload> m_active;
    bool m_isClickActive = false;
    bool m_isDisplaying=false;
    InteractionEventKind m_clickReleaseKind = InteractionEventKind::None;

    bool GetOwner() const
    {
        return m_state.isAttached && m_ownerThread == std::this_thread::get_id();
    }
    std::uint64_t NextId()
    {
        if (m_nextId == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Request id exhausted.");
        return m_nextId++;
    }
    static void RemoveBindings(std::vector<Binding> &bindings) noexcept
    {
        for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
            if (it->port && it->overlay)
                it->port->RemoveOverlay(it->overlay);
        bindings.clear();
    }
    bool SendDisplayDelta(const std::vector<Binding> &bindings, bool isRemoved = false)
    {
        if (!m_context.host || !m_context.data)
            return false;
        FeatureSceneDelta delta;
        delta.requestId = NextId();
        delta.priority = FeatureScenePriority::Overlay;
        delta.scope = FeatureSceneScope::RequiredAllViews;
        delta.hasDisplayUpdate = true;
        const auto graph = m_context.data->GetDataGraph();
        const auto primary =
            graph.view ? graph.view->GetDataBinding(primaryVolumeBinding) : std::nullopt;
        if (isRemoved)
        {
            if (primary && primary->target)
                delta.inputStamp = {*primary->target};
        }
        else
        {
            if (!m_active)
                return false;
            const auto &record = m_active->GetRecord();
            delta.inputStamp = {record.archive.input.source};
            delta.expectations = record.expectations;
            delta.inputs = {{"source-volume", record.archive.input.source},
                            {"labels", record.archive.input.labels},
                            {"mesh", record.archive.input.mesh},
                            {"result", m_state.result}};
        }
        for (const auto &b : bindings)
        {
            delta.viewIds.push_back(b.viewId);
            if (!isRemoved)
                delta.displays.push_back({b.viewId, std::string(featureId), "thickness",
                                          m_state.result, m_displayOperation.operation});
        }
        if (!isRemoved)
            for (const auto &old : m_bindings)
                if (std::find(delta.viewIds.begin(), delta.viewIds.end(), old.viewId) ==
                    delta.viewIds.end())
                    delta.viewIds.push_back(old.viewId);
        return delta.viewIds.empty() || m_context.host->SendSceneDelta(std::move(delta));
    }
    void RemoveDisplay() noexcept
    {
        try
        {
            if (!m_bindings.empty())
                SendDisplayDelta(m_bindings, true);
        }
        catch (...)
        {
        }
        RemoveBindings(m_bindings);
        m_state.isDisplayReady = false;
        m_state.displayResult={};
    }
    bool SetDisplay(const ThicknessDisplay &requested)
    {
        if (!m_active || !ThicknessOverlay::GetColorValid(requested) || !m_context.views)
            return false;
        if(m_isDisplaying)return false;
        struct Guard { bool& flag;~Guard(){flag=false;} };m_isDisplaying=true;const Guard guard{m_isDisplaying};
        const auto resultRef=m_state.result;const auto active=m_active;const auto generation=m_generation;
        const auto stillCurrent=[&] {
            if(!GetOwner() || generation!=m_generation || m_active!=active || m_state.result!=resultRef || !m_context.data)return false;
            const auto current=m_context.data->GetDataGraph();return ThicknessData::GetBinding(current).target==resultRef
                && ThicknessData::GetCurrent(current,active->GetRecord().expectations);
        };
        auto display = requested;
        const auto& record = m_active->GetRecord();
        auto saved=requested;AnalysisColorStyle::SetTailColors(saved.colorBand);
        if(std::none_of(m_resultDisplays.begin(),m_resultDisplays.end(),[&](const auto& e){return e.first==resultRef;}))
            m_resultDisplays.reserve(m_resultDisplays.size()+1);
        if (display.rangeMode == ThicknessRangeMode::Result) {
            display.range=record.statistics.minimum && record.statistics.maximum
                ? std::array<double,2>{*record.statistics.minimum,*record.statistics.maximum}:std::array<double,2>{0,0};
        } else if (display.rangeMode == ThicknessRangeMode::Histogram)
            display.range = record.archive.evaluation.histogramRange;
        if(display.mode==ThicknessDisplayMode::Tolerance) {
            if(display.rangeMode==ThicknessRangeMode::Manual &&
                (display.range[0]>record.archive.evaluation.lower || display.range[1]<record.archive.evaluation.upper))return false;

        }
        auto checked=display;
        if(checked.range[0]==checked.range[1])checked.range={0,1};
        if (!ThicknessAlgorithm::GetDisplayValid(checked)) return false;
        const auto graph = m_context.data->GetDataGraph();
        if (ThicknessData::GetBinding(graph).target != m_state.result ||
            !ThicknessData::GetCurrent(graph, m_active->GetRecord().expectations))
            return false;
        if (!display.isVisible ||
            (display.targetViews.viewIds.empty() && display.targetViews.viewRoles.empty()))
        {
            RemoveDisplay();
            SaveDisplay(std::move(saved),display.range);
            return true;
        }
        auto views = m_context.views->GetViews(display.targetViews);
        if (views.empty())
            return false;
        const auto meshRef = m_active->GetRecord().archive.input.mesh;
        const auto meshData = graph.view->GetData(meshRef);
        const auto mesh =
            meshData ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(meshData->payload)
                     : nullptr;
        if (!mesh)
            return false;
        const bool reuse=m_displayData.mesh && m_geometryMesh==meshRef && m_geometrySamples==record.field.samples;
        auto ramp=ThicknessOverlay::BuildData; // 只用同一结果的几何；发布新结果时失效。
        auto prepared=ramp(record,*mesh,display,reuse ? &m_displayData:nullptr);
        prepared.resultLabel=AnalysisColorStyle::GetResultLabel(m_state.result);
        const bool sameViews=reuse && views.size()==m_bindings.size() && std::equal(views.begin(),views.end(),m_bindings.begin(),
            [](const auto& view,const auto& binding){return view.id==binding.viewId;});
        if(sameViews) {
            if(!SendDisplayDelta(m_bindings) || !stillCurrent())return false;
            for(auto& b:m_bindings) {
                auto appearance=display;appearance.legendPosition=AnalysisColorStyle::GetLegendPosition(display,b.viewId);appearance.hasLegend=display.hasLegend && std::find(display.hiddenLegendViews.begin(),display.hiddenLegendViews.end(),b.viewId)==display.hiddenLegendViews.end();
                b.overlay->SetDisplayData(prepared,appearance);
                if(auto view=m_context.views->GetFeaturePort(b.viewId))view->SetRenderNeeded();
            }
            m_displayData=std::move(prepared);SaveDisplay(std::move(saved),display.range);m_state.isDisplayReady=true;return true;
        }
        std::vector<Binding> candidate;
        try
        {
            for (const auto &view : views)
            {
                if (view.role == HostRenderViewRole::Auxiliary)
                    throw std::invalid_argument("Unsupported thickness view.");
                const auto port = m_context.views->GetOverlayPort(view.id);
                if (!port)
                    throw std::runtime_error("Overlay port unavailable.");
                auto appearance=display;appearance.legendPosition=AnalysisColorStyle::GetLegendPosition(display,view.id);appearance.hasLegend=display.hasLegend && std::find(display.hiddenLegendViews.begin(),display.hiddenLegendViews.end(),view.id)==display.hiddenLegendViews.end();
                auto overlay = std::make_shared<ThicknessOverlay>(
                    prepared, appearance, m_active->GetRecord().archive.input.unit, view.role);
                candidate.push_back({view.id, port, overlay});
                if (!port->AttachOverlay(overlay))
                    throw std::runtime_error("Overlay attach rejected.");
                if (m_state.selectedSample &&
                    *m_state.selectedSample < m_active->GetRecord().field.samples->size())
                    overlay->SetSelection(
                        &(*m_active->GetRecord().field.samples)[*m_state.selectedSample]);
            }
            // 新投影与props准备好后再替换；任何失败保留旧显示。
            if (!SendDisplayDelta(candidate) || !stillCurrent())
                throw std::runtime_error("Scene projection rejected.");
        }
        catch (...)
        {
            RemoveBindings(candidate);
            return false;
        }
        RemoveBindings(m_bindings);
        m_bindings = std::move(candidate);
        m_displayData=std::move(prepared);m_geometryMesh=meshRef;m_geometrySamples=record.field.samples;
        SaveDisplay(std::move(saved),display.range);
        m_state.isDisplayReady = true;
        return true;
    }
    bool SelectSample(const DataRevisionRef &ref, std::size_t id)
    {
        if (!m_active || !GetState().isCurrent || ref != m_state.result ||
            id >= m_active->GetRecord().field.samples->size() ||
            !ThicknessData::GetCurrent(m_context.data->GetDataGraph(),
                                       m_active->GetRecord().expectations))
            return false;
        m_state.selectedSample = id;
        const auto &sample = (*m_active->GetRecord().field.samples)[id];
        for (auto &b : m_bindings)
        {
            b.overlay->SetSelection(&sample);
            const auto view = m_context.views->GetFeaturePort(b.viewId);
            if (view)
                view->SetRenderNeeded();
        }
        return true;
    }
    std::optional<HostSemanticTarget> GetTarget(const InteractionEvent &event)
    {
        if (!GetOwner() || m_state.isStopping || !m_active || !GetState().isCurrent)
            return {};
        auto target = m_context.host->GetDisplayTarget(event.viewId, "thickness");
        if (!target || target->display.data != m_state.result ||
            !ThicknessData::GetCurrent(m_context.data->GetDataGraph(),
                                       m_active->GetRecord().expectations))
            return {};
        const auto view =
            m_context.views->GetInputView({event.viewId, false, HostRenderViewRole::Auxiliary});
        const auto lease = view ? view->lease.lock() : nullptr;
        if (!view || !lease || !lease->GetIsActive() || !lease->GetIsOwnerThread())
            return {};
        for (auto &b : m_bindings)
            if (b.viewId == event.viewId)
            {
                const auto sample = b.overlay->GetPickedSample(event.x, event.y, view->renderer);
                if (!sample)
                    return {};
                target->objectId = std::to_string(*sample);
                target->resultRevision = m_state.result.generation;
                return target;
            }
        return {};
    }
    InteractionResult OnTarget(const InteractionEvent &event, const HostSemanticTarget &target)
    {
        if (event.eventKind == InteractionEventKind::Cancel
            || event.eventKind == m_clickReleaseKind) {
            m_isClickActive = false;
            m_clickReleaseKind = InteractionEventKind::None;
            return {true, true, true};
        }
        if (!GetOwner() || !m_context.host->GetSemanticTargetValid(target) ||
            target.display.data != m_state.result)
            return {};
        if(event.actionId!="wallThickness.selectSample"||event.eventKind!=InteractionEventKind::PrimaryPress&&event.eventKind!=InteractionEventKind::SecondaryPress&&event.eventKind!=InteractionEventKind::MiddlePress)
            return {true, true, true};
        try
        {
            std::size_t consumed = 0;
            const auto id = std::stoull(target.objectId, &consumed);
            if (consumed != target.objectId.size() || id > std::numeric_limits<std::size_t>::max())
                return {};
            const bool selected = SelectSample(
                target.display.data, static_cast<std::size_t>(id));
            if (selected) {
                m_isClickActive = true;
                m_clickReleaseKind =
                    event.eventKind == InteractionEventKind::MiddlePress ? InteractionEventKind::MiddleRelease : event.eventKind == InteractionEventKind::PrimaryPress
                    ? InteractionEventKind::PrimaryRelease
                    : InteractionEventKind::SecondaryRelease;
            }
            return {true, true, selected};
        }
        catch (...)
        {
            return {};
        }
    }
    void SetStale()
    {
        if (!m_active)
            return;
        const auto graph = m_context.data->GetDataGraph();
        if (ThicknessData::GetBinding(graph).target != m_state.result ||
            !ThicknessData::GetCurrent(graph, m_active->GetRecord().expectations))
        {
            m_state.isCurrent = false;
            RemoveDisplay();
        }
    }
    FeatureOperationState BuildOperation(std::uint64_t id, const ThicknessArchive &archive)
    {
        FeatureOperationState operation;
        operation.operation = {std::string(featureId), m_context.host->GetAttachmentId(), id};
        operation.stateRevision = 1;
        operation.status = FeatureRunStatus::Running;
        operation.progress = 0;
        operation.inputs = {{"source-volume", archive.input.source},
                            {"labels", archive.input.labels},
                            {"mesh", archive.input.mesh}};
        return operation;
    }

  public:
    explicit Impl(ThicknessConfig config) : m_config(config) {}
    ~Impl() noexcept
    {
        if (m_task)
        {
            m_task->work.cancelled->store(true);
            if (m_task->worker.joinable())
                m_task->worker.join();
        }
        if (m_state.isAttached && m_ownerThread == std::this_thread::get_id())
            DetachHost();
        if (m_signal)
            m_signal->owner = nullptr;
    }
    bool AttachHost(const HostFeatureContext &context)
    {
        if (m_state.isAttached || !context.data || !context.host ||
            !ThicknessAlgorithm::GetConfigValid(m_config))
            return false;
        DataObserverId observer = 0;
        bool inputAttempted = false;
        try
        {
            if (!ThicknessData::SetType(*context.data))
                return false;
            auto signal = std::make_shared<Signal>();
            signal->owner = this;
            const std::weak_ptr<Signal> weak = signal;
            const std::weak_ptr<FeatureHostControl> host = context.host;
            observer = context.data->AttachDataChange(
                [weak, host](const DataChangeSet &)
                {
                    if (const auto s = weak.lock())
                        s->dirty.store(true);
                    if (const auto h = host.lock())
                        try
                        {
                            h->SendWorkAvailable();
                        }
                        catch (...)
                        {
                        }
                });
            if (observer == 0)
                return false;
            HostInputBinding input;
            input.featureId = std::string(featureId);
            input.targetViews.viewRoles = {
                HostRenderViewRole::Primary3D, HostRenderViewRole::Composite3D,
                HostRenderViewRole::TopDownSlice, HostRenderViewRole::FrontBackSlice,
                HostRenderViewRole::LeftRightSlice};
            input.getTarget =
                [weak](const InteractionEvent &event) -> std::optional<HostSemanticTarget>
            {
                const auto s = weak.lock();
                return s && s->owner ? s->owner->GetTarget(event) : std::nullopt;
            };
            input.onTargetInput =
                [weak](const InteractionEvent &event, const HostSemanticTarget &target)
            {
                const auto s = weak.lock();
                return s && s->owner ? s->owner->OnTarget(event, target) : InteractionResult{};
            };
            InteractionActionInfo select;select.actionId="wallThickness.selectSample";select.ownerId=input.featureId;
            select.bindings.push_back({std::string(ThicknessBindingKeys::SelectSample),{InputTriggerKind::PointerPress,InputMouseButton::Primary},true,{},"target:thickness",{InputTriggerKind::PointerPress}});
            input.actions.push_back(std::move(select));
            input.getIsCaptureActive=[weak](std::string_view){const auto f=weak.lock();return f&&f->owner&&f->owner->m_isClickActive;};
            const auto targetCallback=input.onTargetInput;
            input.onAction=[targetCallback](std::string_view,const InteractionEvent& e,const std::optional<HostSemanticTarget>& target){return target?targetCallback(e,*target):InteractionResult{};};
            inputAttempted = true;
            if (!context.host->AttachInput(std::move(input)))
            {
                inputAttempted = false;
                context.data->DetachDataChange(observer);
                return false;
            }
            m_context = context;
            m_signal = std::move(signal);
            m_observer = observer;
            m_hasInput = true;
            m_ownerThread = std::this_thread::get_id();
            ++m_generation;
            m_state = {};
            m_state.isAttached = true;
            m_state.status = ThicknessStatus::Succeeded;
            return true;
        }
        catch (...)
        {
            try
            {
                if (inputAttempted)
                    context.host->DetachInput(featureId);
            }
            catch (...)
            {
            }
            try
            {
                if (observer)
                    context.data->DetachDataChange(observer);
            }
            catch (...)
            {
            }
            return false;
        }
    }
    bool DetachHost()
    {
        if (!m_state.isAttached)
            return true;
        if (!GetOwner())
            return false;
        m_state.isStopping = true;
        if (m_task)
        {
            m_task->work.cancelled->store(true);
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(m_config.stopTimeoutMilliseconds);
            while (!m_task->isDone.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            if (!m_task->isDone.load(std::memory_order_acquire))
                return false;
        }
        RemoveDisplay();
        m_active.reset();
        // 先释放自身的几何/采样缓存；外部快照仍由 scope 的 Reader/probe 门禁保护。
        m_displayData={};m_geometryMesh={};m_geometrySamples.reset();
        if (!m_resultScopes.Clear(*m_context.data)) return false;
        if (m_hasInput && !m_context.host->DetachInput(featureId))
            return false;
        m_hasInput = false;
        if (m_observer && !m_context.data->DetachDataChange(m_observer))
            return false;
        m_observer = 0;
        auto task = std::move(m_task);
        if (task && task->worker.joinable())
            task->worker.join();
        auto completing = std::move(m_completing);
        DataRevisionRef publishedDuringDetach;
        if (completing && GetDataRevisionRefValid(completing->result) &&
            ThicknessData::GetResult(m_context.data->GetDataGraph(), completing->result))
            publishedDuringDetach = completing->result;
        RemoveDisplay();
        m_active.reset();
        if (m_signal)
            m_signal->owner = nullptr;
        m_signal.reset();
        m_context = {};
        m_resultDisplays.clear();m_displayData={};m_geometryMesh={};m_geometrySamples.reset();
        ++m_generation;
        m_state = {};
        m_isClickActive = false;
        m_clickReleaseKind = InteractionEventKind::None;
        m_operation = {};
        m_displayOperation = {};
        if (task)
            SendCompletion(task->completion, {task->requestId,
                                              ThicknessStatus::Cancelled,
                                              {},
                                              false,
                                              false,
                                              "Feature detached."});
        if (completing)
            SendCompletion(completing, {completing->requestId, ThicknessStatus::Cancelled,
                                        publishedDuringDetach, false, false,
                                        "Feature detached during publication."});
        return true;
    }
    ThicknessAdmission SendRequest(ThicknessRequest request, ThicknessCallback callback)
    {
        if (!GetOwner())
            return {ThicknessAdmissionStatus::Unavailable, 0};
        if (m_state.isStopping)
            return {ThicknessAdmissionStatus::Stopping, 0};
        if(m_isDisplaying && !std::holds_alternative<ThicknessCancelRequest>(request)
            && !std::holds_alternative<ThicknessSampleRequest>(request))return {ThicknessAdmissionStatus::Busy,0};
        const auto* start = std::get_if<ThicknessStartRequest>(&request);
        const auto* cancel = std::get_if<ThicknessCancelRequest>(&request);
        const auto* evaluation = std::get_if<ThicknessEvaluationRequest>(&request);
        const auto* displayRequest = std::get_if<ThicknessDisplayRequest>(&request);
        const auto* activation = std::get_if<ThicknessActivationRequest>(&request);
        const auto* sample = std::get_if<ThicknessSampleRequest>(&request);
        const auto* display = start && start->display ? &*start->display
            : displayRequest ? &displayRequest->display
            : activation && activation->display ? &*activation->display : nullptr;
        const auto* evaluationParams = evaluation ? &evaluation->evaluation
            : start && start->evaluation ? &*start->evaluation : nullptr;
        if (request.valueless_by_exception() ||
            (start && !ThicknessAlgorithm::GetParamsValid(start->params)) ||
            (evaluationParams && !ThicknessAlgorithm::GetEvaluationValid(*evaluationParams)) ||
            (display && !GetDisplayValid(*display)))
            return {};
        if ((m_task || m_completing) && !cancel && !displayRequest && !sample)
            return {ThicknessAdmissionStatus::Busy, 0};
        try
        {
            SetStale();
            if (start || evaluation)
            {
                auto task = std::make_unique<Task>();
                ThicknessArchive archive;
                std::shared_ptr<const ThicknessData::ResultPayload> previous;
                if (start)
                {
                    archive.input = start->input;
                    archive.params = start->params;
                    archive.limits = m_config;
                    archive.evaluation = start->evaluation.value_or(ThicknessEvaluation{});
                }
                else
                {
                    if (!m_active || !m_state.isCurrent)
                        return {};
                    previous = m_active;
                    archive = previous->GetRecord().archive;
                    archive.evaluation = evaluation->evaluation;
                    archive.limits = m_config;
                }
                const auto graph = m_context.data->GetDataGraph();
                task->work = ThicknessData::BuildWork(*m_context.data, graph, archive, task->expectations);
                auto readers=FeatureInternal::StartInputReads(graph,task->expectations,"wall-thickness-reader");
                if (!readers) return {};
                task->readers=std::move(*readers);
                task->binding = ThicknessData::GetBinding(graph);
                task->requestId = NextId();
                task->completion = std::make_shared<Completion>();
                task->completion->requestId = task->requestId;
                task->completion->callback = std::move(callback);
                task->display = display ? std::optional<ThicknessDisplay>(*display)
                    : previous ? std::optional<ThicknessDisplay>(GetSavedDisplay(m_state.result)):std::nullopt;
                const auto id = task->requestId;
                auto operation = BuildOperation(id, archive);
                Task *raw = task.get();
                const std::weak_ptr<FeatureHostControl> host = m_context.host;
                task->worker = std::thread(
                    [raw, host, previous]
                    {
                        raw->candidate =
                            previous
                                ? ThicknessAlgorithm::BuildEvaluation(
                                      previous->GetRecord().field, raw->work.archive.evaluation,
                                      raw->work.archive.params, raw->work.archive.limits,
                                      raw->work.cancelled, raw->work.deadline)
                                : ThicknessAlgorithm::BuildField(raw->work);
                        if (previous)
                            raw->candidate.field = previous->GetRecord().field;
                        raw->isDone.store(true, std::memory_order_release);
                        if (const auto h = host.lock())
                            try
                            {
                                h->SendWorkAvailable();
                            }
                            catch (...)
                            {
                            }
                    });
                m_task = std::move(task);
                m_state.isBusy = true;
                m_state.requestId = id;
                m_operation = std::move(operation);
                return {ThicknessAdmissionStatus::Accepted, id};
            }
            const auto id = NextId();
            const auto generation = m_generation;
            const auto data = m_context.data;
            bool succeeded = false;
            ThicknessStatus controlStatus = ThicknessStatus::Succeeded;
            std::optional<DataRevisionRef> controlRef;
            if (cancel)
            {
                if (!m_task ||
                    (cancel->targetRequestId && cancel->targetRequestId != m_task->requestId))
                    return {};
                m_task->work.cancelled->store(true);
                succeeded = true;
            }
            else if (displayRequest) {
                if(!GetDataRevisionRefValid(displayRequest->result) || displayRequest->result!=m_state.result)return {};
                succeeded=SetDisplay(displayRequest->display);
            }
            else if (sample)
                succeeded = SelectSample(sample->resultRevision, sample->sampleIndex);
            else if (std::holds_alternative<ThicknessClearRequest>(request))
            {
                const auto graph = m_context.data->GetDataGraph();
                const auto binding = ThicknessData::GetBinding(graph);
                DataTransaction transaction;
                transaction.bindings.push_back({std::string(ThicknessData::bindingName),
                                                binding.revision,
                                                true,
                                                binding.target,
                                                {}});
                const auto committed = m_resultScopes.Commit(*data,std::move(transaction));
                succeeded = committed.status == DataCommitStatus::Succeeded;
                controlRef = DataRevisionRef{};
                if (succeeded)
                {
                    if (!GetOwner() || generation != m_generation)
                        controlStatus = ThicknessStatus::Cancelled;
                    else if (ThicknessData::GetBinding(data->GetDataGraph()).target)
                    {
                        controlStatus = ThicknessStatus::StaleInput;
                        SetStale();
                    }
                    else
                    {
                        RemoveDisplay();
                        m_active.reset();m_resultDisplays.clear();m_displayData={};m_geometryMesh={};m_geometrySamples.reset();
                        m_state.appliedDisplay.reset();m_state.displayRange.reset();
                        m_state.result = {};
                        m_state.isCurrent = false;
                        m_state.selectedSample.reset();
                        succeeded=m_resultScopes.Clear(*data);
                    }
                }
            }
            else if (activation)
            {
                const auto graph = m_context.data->GetDataGraph();
                const auto result = ThicknessData::GetResult(graph, activation->resultRevision);
                if (!result || !ThicknessData::GetCurrent(graph, result->GetRecord().expectations))
                    return {};
                const auto binding = ThicknessData::GetBinding(graph);
                DataTransaction transaction;
                transaction.expectations = result->GetRecord().expectations;
                transaction.bindings.push_back({std::string(ThicknessData::bindingName),
                                                binding.revision, true, binding.target,
                                                activation->resultRevision});
                succeeded = m_resultScopes.Commit(*data,std::move(transaction)).status ==
                            DataCommitStatus::Succeeded;
                controlRef = activation->resultRevision;
                if (succeeded && (!GetOwner() || generation != m_generation))
                    controlStatus = ThicknessStatus::Cancelled;
                if (succeeded && GetOwner() && generation == m_generation)
                {
                    const auto current = data->GetDataGraph();
                    if (ThicknessData::GetBinding(current).target == activation->resultRevision &&
                        ThicknessData::GetCurrent(current, result->GetRecord().expectations))
                    {
                        m_active = result;
                        m_state.result = activation->resultRevision;
                        m_state.isCurrent = true;
                        m_state.selectedSample.reset();
                        m_displayOperation = BuildOperation(id, result->GetRecord().archive);
                        m_displayOperation.status = FeatureRunStatus::Succeeded;
                        m_displayOperation.outputs = {activation->resultRevision};
                        m_displayOperation.progress = 1;
                        m_state.isDisplayReady=false;m_state.appliedDisplay.reset();m_state.displayRange.reset();
                        if (!SetDisplay(activation->display.value_or(GetSavedDisplay(activation->resultRevision)))) {
                            RemoveDisplay();controlStatus=ThicknessStatus::DisplayFailed;
                        }
                    }
                    else
                    {
                        controlStatus = ThicknessStatus::StaleInput;
                        SetStale();
                    }
                }
            }
            if (!succeeded)
                return {};
            const bool isCurrent =
                (controlStatus==ThicknessStatus::Succeeded || controlStatus==ThicknessStatus::DisplayFailed) && GetState().isCurrent;
            const ThicknessResult result{id,
                                         controlStatus,
                                         controlRef.value_or(m_state.result),
                                         isCurrent,
                                         isCurrent && m_state.isDisplayReady,
                                         {}};
            SendComplete(std::move(callback), result);
            return {ThicknessAdmissionStatus::Accepted, id};
        }
        catch (...)
        {
            return {};
        }
    }
    bool OnHostTick()
    {
        if (!GetOwner())
            return false;
        if (m_state.isStopping || m_completing)
            return true;
        if (m_signal->dirty.exchange(false))
        {
            SetStale();
            if (m_task &&
                !ThicknessData::GetCurrent(m_context.data->GetDataGraph(), m_task->expectations))
            {
                m_task->isStale = true;
                m_task->work.cancelled->store(true);
            }
        }
        if (!m_task || !m_task->isDone.load(std::memory_order_acquire))
            return true;
        auto task = std::move(m_task);
        if (task->worker.joinable())
            task->worker.join();
        m_completing = task->completion;
        const auto completion = task->completion;
        const auto generation = m_generation;
        const auto data = m_context.data;
        ThicknessResult result{task->requestId,
                               task->candidate.status,
                               {},
                               false,
                               false,
                               std::move(task->candidate.message)};
        DataRevisionRef pendingRef;
        try
        {
            auto status = task->candidate.status;
            if (task->isStale ||
                !ThicknessData::GetCurrent(data->GetDataGraph(), task->expectations))
                status = ThicknessStatus::StaleInput;
            else if (task->work.cancelled->load())
                status = ThicknessStatus::Cancelled;
            result.status = status;
            if (status == ThicknessStatus::Succeeded || status == ThicknessStatus::NoValidSamples)
            {
                const DataRevisionRef ref{data->CreateDataEntityId(), 1};
                pendingRef = ref;
                completion->result = ref;
                ThicknessData::Record record{task->work.archive, task->candidate.field,
                                             std::move(task->candidate.statistics),
                                             std::move(task->candidate.regions),
                                             task->expectations};
                auto payload =
                    std::make_shared<const ThicknessData::ResultPayload>(std::move(record));
                DataTransaction transaction;
                transaction.expectations = task->expectations;
                const auto &input = task->work.archive.input;
                transaction.outputs.push_back(
                    {ref.entityId,
                     0,
                     ThicknessData::resultType,
                     {{"source-volume", input.source},
                      {"labels", input.labels},
                      {"mesh", input.mesh}},
                     payload,
                     DataProvenance{std::string(featureId), "global-automatic-surface-wall-thickness",
                                    task->work.archive.algorithmVersion,
                                    ThicknessData::GetParameters(task->work.archive)}});
                if(task->work.archive.params.evaluationRoi)
                    transaction.outputs.back().inputs.push_back({"evaluation-roi",*task->work.archive.params.evaluationRoi});
                transaction.bindings.push_back({std::string(ThicknessData::bindingName),
                                                task->binding.revision, true, task->binding.target,
                                                ref});
                DataCommitResult committed;
                try
                {
                    committed = m_resultScopes.Commit(*data,std::move(transaction));
                }
                catch (...)
                {
                }
                // observer可重入；查询提交事实，不能把已经发布的结果补偿掉。
                const auto graph = data->GetDataGraph();
                const auto published = ThicknessData::GetResult(graph, ref);
                const auto binding = ThicknessData::GetBinding(graph);
                if (published)
                {
                    result.result = ref;
                    result.isActivated = binding.target == ref &&
                                         ThicknessData::GetCurrent(graph, task->expectations);
                    if (GetOwner() && generation == m_generation &&
                        m_state.requestId == task->requestId && result.isActivated)
                    {
                        m_active = published;
                        m_state.result = ref;
                        m_state.isCurrent = true;
                        m_state.selectedSample.reset();
                        m_operation.status = FeatureRunStatus::Succeeded;
                        m_operation.outputs = {ref};
                        m_operation.progress = 1;
                        ++m_operation.stateRevision;
                        m_displayOperation = m_operation;
                        try
                        {
                            m_state.isDisplayReady=false;m_state.appliedDisplay.reset();m_state.displayRange.reset();
                            const bool applied=SetDisplay(task->display.value_or(ThicknessDisplay{}));
                            result.isDisplayReady=applied && m_state.isDisplayReady && m_state.displayResult==ref;
                            if(!applied)result.message="Result is ready; display is not ready.";
                        }
                        catch (...)
                        {
                            result.isDisplayReady = false;
                        }
                        if (!result.isDisplayReady)
                            RemoveDisplay();
                    }
                }
                else
                {
                    result.status = committed.failureReason == DataCommitFailure::ExpectationFailed
                                        ? ThicknessStatus::StaleInput
                                    : committed.failureReason == DataCommitFailure::OutOfMemory
                                        ? ThicknessStatus::BudgetExceeded
                                    : committed.failureReason == DataCommitFailure::PayloadInvalid
                                        ? ThicknessStatus::InvalidInput
                                        : ThicknessStatus::InternalError;
                    result.message = "Result publication rejected: " + committed.message;
                }
            }
            if (GetOwner() && generation == m_generation && m_state.requestId == task->requestId)
            {
                m_state.status = result.status;
                m_operation.status = (result.status == ThicknessStatus::Succeeded ||
                                      result.status == ThicknessStatus::NoValidSamples)
                                         ? FeatureRunStatus::Succeeded
                                     : result.status == ThicknessStatus::Cancelled
                                         ? FeatureRunStatus::Cancelled
                                         : FeatureRunStatus::Failed;
                ++m_operation.stateRevision;
            }
        }
        catch (...)
        {
            result.status = ThicknessStatus::InternalError;
            // 即使提交后的状态/显示分配失败，也不能丢失已接纳请求的最终完成。
            try
            {
                const auto graph = data->GetDataGraph();
                if (GetDataRevisionRefValid(pendingRef) &&
                    ThicknessData::GetResult(graph, pendingRef))
                {
                    result.result = pendingRef;
                    result.status = task->candidate.status;
                    result.isActivated = ThicknessData::GetBinding(graph).target == pendingRef &&
                                         ThicknessData::GetCurrent(graph, task->expectations);
                }
            }
            catch (...)
            {
            }
        }
        if (GetOwner() && generation == m_generation)
        {
            m_state.isBusy = false;
            if (m_completing == completion)
                m_completing.reset();
        }
        SendCompletion(completion, result);
        return true;
    }
    ThicknessState GetState() const
    {
        auto state = m_state;
        for(const auto& e:m_resultDisplays)if(e.first==state.result){state.appliedDisplay=e.second;break;}
        if (GetOwner() && m_active)
        {
            const auto graph = m_context.data->GetDataGraph();
            state.isCurrent = ThicknessData::GetBinding(graph).target == state.result &&
                              ThicknessData::GetCurrent(graph, m_active->GetRecord().expectations);
        }
        if (!state.isCurrent || state.displayResult!=state.result)
            state.isDisplayReady = false;
        return state;
    }
    std::optional<ThicknessSnapshot> GetResult(const DataRevisionRef &ref) const
    {
        if (!GetOwner())
            return {};
        const auto graph = m_context.data->GetDataGraph();
        const auto payload = ThicknessData::GetResult(graph, ref);
        if (!payload)
            return {};
        const auto &r = payload->GetRecord();
        return ThicknessSnapshot{ref,
                                 r.archive,
                                 r.field.samples,
                                 r.field.nodes,
                                 r.statistics,
                                 r.regions,
                                 r.field.subdivisions,
                                 ThicknessData::GetCurrent(graph, r.expectations),
                                 [&]() -> std::optional<ThicknessDisplay> {
                                     for(const auto& e:m_resultDisplays)if(e.first==ref)return e.second;
                                     return {};
                                 }()};
    }
    std::vector<FeatureOperationState> GetOperations() const
    {
        return m_operation.operation.requestId ? std::vector<FeatureOperationState>{m_operation}
                                               : std::vector<FeatureOperationState>{};
    }
    std::optional<double> GetThicknessValue(const DataRevisionRef &ref, const ThicknessPoint &point) const
    {
        if (!GetOwner()) return {};
        const auto payload = ThicknessData::GetResult(m_context.data->GetDataGraph(), ref);
        if (!payload) return {};
        const auto &record = payload->GetRecord();
        if(record.archive.params.evaluationRoi) {
            const auto roi=m_context.data->GetRoi(m_context.data->GetDataGraph(),
                *record.archive.params.evaluationRoi,record.archive.input.source);
            if(roi.error!=RoiError::None || !roi.roi || !roi.roi->GetContains(point))return {};
        }
        if (record.archive.params.evaluationBounds)
            for (unsigned a = 0; a < 3; ++a)
                if (point[a] < (*record.archive.params.evaluationBounds)[2 * a] - record.field.coordinateTolerance ||
                    point[a] > (*record.archive.params.evaluationBounds)[2 * a + 1] + record.field.coordinateTolerance) return {};
        return ThicknessAlgorithm::GetValue(record.field, point);
    }
};
WallThicknessHostFeature::WallThicknessHostFeature(ThicknessConfig config)
    : m_impl(std::make_unique<Impl>(config))
{
}
WallThicknessHostFeature::~WallThicknessHostFeature() noexcept = default;

std::string_view WallThicknessHostFeature::GetFeatureId() const noexcept
{
    return featureId;
}
FeatureDataContract WallThicknessHostFeature::GetDataContract() const
{
    return {{{"source-volume", DataFacets::scalarGrid3D, true},
             {"labels", DataFacets::labelMap3D, true},
             {"mesh", DataFacets::surfaceMesh, true},
             {"evaluation-roi", DataFacets::roiGeometry, false}},
            {{"result", ThicknessData::resultType, {ThicknessData::resultFacet}}}};
}
std::vector<FeatureOperationState> WallThicknessHostFeature::GetOperationStates() const
{
    return m_impl->GetOperations();
}
bool WallThicknessHostFeature::AttachHost(const HostFeatureContext &context)
{
    return m_impl->AttachHost(context);
}
bool WallThicknessHostFeature::DetachHost()
{
    auto keep = weak_from_this().lock();
    return m_impl->DetachHost();
}
bool WallThicknessHostFeature::OnHostTick()
{
    auto keep = weak_from_this().lock();
    return m_impl->OnHostTick();
}
ThicknessAdmission WallThicknessHostFeature::SendRequest(ThicknessRequest request,
                                                         ThicknessCallback callback)
{
    auto keep = weak_from_this().lock();
    return m_impl->SendRequest(std::move(request), std::move(callback));
}
ThicknessState WallThicknessHostFeature::GetState() const
{
    return m_impl->GetState();
}
std::optional<double> WallThicknessHostFeature::GetThicknessValue(
    const DataRevisionRef &ref, const ThicknessPoint &modelPoint) const
{
    return m_impl->GetThicknessValue(ref, modelPoint);
}
std::optional<ThicknessSnapshot>
WallThicknessHostFeature::GetResult(const DataRevisionRef &ref) const
{
    return m_impl->GetResult(ref);
}
std::optional<ThicknessArchive>
WallThicknessHostFeature::GetArchive(const DataRevisionRef &ref) const
{
    const auto result = GetResult(ref);
    return result ? std::optional<ThicknessArchive>(result->archive) : std::nullopt;
}
