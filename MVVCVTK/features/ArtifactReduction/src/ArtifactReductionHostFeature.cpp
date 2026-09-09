#include "Host/ArtifactReductionHostFeature.h"
#include "../../common/FeatureResultScopes.h"
#include "ArtifactReductionAlgorithm.h"

#include <chrono>
#include <future>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

class ArtifactReductionHostFeature::Impl final {
public:
    explicit Impl(ArtifactConfig config) : m_config(config) {}
    ~Impl() noexcept
    {
        // 最后防线必须 join；不能以析构超时为由释放 worker 持有的数据或 detach 线程。
        if (m_control) m_control->isCancelled.store(true, std::memory_order_relaxed);
        if (m_future.valid()) m_future.wait();
        if (m_worker.joinable()) m_worker.join();
    }

    ArtifactError GetAccessError() const noexcept
    {
        if (m_owner != std::thread::id{} && m_owner != std::this_thread::get_id()) return ArtifactError::WrongThread;
        if (!m_data) return ArtifactError::Unavailable;
        if (m_state.status == ArtifactStatus::Stopping || m_state.status == ArtifactStatus::Publishing) return ArtifactError::Busy;
        return ArtifactError::None;
    }

    bool GetHeadMatched(const DataGraphSnapshot& graph, const DataRevisionRef& ref) const
    {
        DataQuery query;
        query.entityId = ref.entityId;
        const auto found = m_data->GetDataQuery(graph, query);
        DataGeneration head = 0;
        for (const auto& entry : found.data) {
            if (entry && entry->self.generation > head) head = entry->self.generation;
        }
        return head == ref.generation;
    }

    ArtifactError BuildInputs(const ArtifactRequest& request, ArtifactReduction::AlgorithmInput& input,
        std::vector<DataInputRef>& inputs, std::vector<DataExpectation>& expectations) const
    {
        const auto graph = m_data->GetDataGraph();
        if (!graph.view || !GetDataRevisionRefValid(request.source)) return ArtifactError::InvalidRequest;
        const auto source = m_data->GetData(graph, request.source);
        input.image = source ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(source->payload) : nullptr;
        if (!input.image) return ArtifactError::InvalidData;
        inputs.push_back({ "source-volume", request.source });
        const auto region = [&](const char* role, const std::optional<DataRevisionRef>& ref,
            RoiReadSnapshot& target) {
            if (!ref) return true;
            const auto resolved=m_data->GetRoi(graph,*ref,request.source);
            if (resolved.error!=RoiError::None || !resolved.roi) return false;
            target=resolved.roi;
            inputs.push_back({role,*ref});
            std::size_t index=0;
            for (const auto& dependency:target->GetDependencies()) {
                if (dependency==*ref || dependency==request.source) continue;
                inputs.push_back({std::string(role)+".input-"+std::to_string(index++),dependency});
                const auto data=m_data->GetData(graph,dependency);
                if (!data) return false;
                std::size_t bytes=0;
                if (const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get())) bytes=mask->GetValues()->size();
                if (const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get()))
                    bytes=std::visit([](const auto& values){return values->size()*sizeof((*values)[0]);},labels->GetValues());
                if (bytes>std::numeric_limits<std::size_t>::max()-input.roiBytes) return false;
                input.roiBytes+=bytes;
            }
            return true;
        };
        if (!region("processing-roi", request.processingRoi, input.processing)
            || !region("protection-roi", request.protectionRoi, input.protection)
            || !region("quality-roi", request.qualityRoi, input.material)) return ArtifactError::InvalidData;
        for (const auto& entry : inputs) {
            if (request.inputMode == ArtifactInputMode::CurrentPrimary && !GetHeadMatched(graph, entry.source))
                return ArtifactError::SourceChanged;
            DataExpectation expectation;
            expectation.use = request.inputMode == ArtifactInputMode::ExplicitRevision
                ? DataExpectationUse::Activation : DataExpectationUse::Required;
            expectation.entityId = entry.source.entityId;
            expectation.expectedGeneration = entry.source.generation;
            expectations.push_back(std::move(expectation));
        }
        if (request.inputMode == ArtifactInputMode::CurrentPrimary) {
            const auto binding = m_data->GetDataBinding(graph, primaryVolumeBinding);
            if (!binding || binding->target != request.source) return ArtifactError::SourceChanged;
            DataExpectation expectation;
            expectation.kind = DataExpectationKind::Binding;
            expectation.binding = std::string(primaryVolumeBinding);
            expectation.expectedBindingRevision = binding->revision;
            expectation.isTargetChecked = true;
            expectation.expectedTarget = request.source;
            expectations.push_back(std::move(expectation));
        }
        return ArtifactError::None;
    }

    ArtifactConfig m_config;
    std::thread::id m_owner;
    std::shared_ptr<TrustedDataPort> m_data;
    FeatureInternal::ResultScopes m_resultScopes;
    // 挂载上下文是临时值；Feature 必须持有通知端口直到成功解绑。
    std::shared_ptr<FeatureHostControl> m_host;
    ArtifactState m_state;
    ArtifactInputMode m_inputMode = ArtifactInputMode::CurrentPrimary;
    std::shared_ptr<ArtifactReduction::TaskControl> m_control;
    std::future<ArtifactReduction::AlgorithmResult> m_future;
    std::thread m_worker;
    ArtifactReduction::AlgorithmResult m_candidate;
    std::vector<DataInputRef> m_inputs;
    std::vector<DataExpectation> m_expectations;
    std::uint64_t m_nextId = 1;
};

ArtifactReductionHostFeature::ArtifactReductionHostFeature(ArtifactConfig config)
    : m_impl(std::make_unique<Impl>(config)) {}

ArtifactReductionHostFeature::~ArtifactReductionHostFeature() noexcept = default;

std::string_view ArtifactReductionHostFeature::GetFeatureId() const noexcept
{
    return "artifact-reduction";
}

FeatureDataContract ArtifactReductionHostFeature::GetDataContract() const
{
    return {
        { { "source-volume", DataFacets::scalarGrid3D, true },
          { "processing-roi", DataFacets::roiGeometry, false },
          { "protection-roi", DataFacets::roiGeometry, false },
          { "quality-roi", DataFacets::roiGeometry, false } },
        { { "corrected-volume", DataTypes::imageGrid3D, { DataFacets::scalarGrid3D } },
          { "quality-report", DataTypes::recordTable, { DataFacets::tabularRecords } } }
    };
}

bool ArtifactReductionHostFeature::AttachHost(const HostFeatureContext& context)
{
    auto& state = *m_impl;
    if (state.m_data || state.m_future.valid() || !context.data
        || state.m_config.memoryBudgetBytes == 0 || state.m_config.publishBudgetBytes == 0
        || state.m_config.stopTimeoutMs > 60000) return false;
    state.m_owner = std::this_thread::get_id();
    state.m_data = context.data;
    state.m_host = context.host;
    state.m_state.status = ArtifactStatus::Idle;
    state.m_state.error = ArtifactError::None;
    return true;
}

bool ArtifactReductionHostFeature::DetachHost()
{
    auto& state = *m_impl;
    if (!state.m_data) return true;
    if (state.m_owner != std::this_thread::get_id() || state.m_state.status == ArtifactStatus::Publishing) return false;
    state.m_state.status = ArtifactStatus::Stopping;
    if (state.m_control) state.m_control->isCancelled.store(true, std::memory_order_relaxed);
    if (state.m_future.valid()) {
        if (state.m_future.wait_for(std::chrono::milliseconds(state.m_config.stopTimeoutMs)) != std::future_status::ready) return false;
        try { static_cast<void>(state.m_future.get()); }
        catch (...) { /* 取消退出仍须清理；任务错误不改变已发布图。 */ }
    }
    if (state.m_worker.joinable()) state.m_worker.join();
    state.m_candidate = {};
    state.m_control.reset();
    state.m_inputs.clear();
    state.m_expectations.clear();
    if (!state.m_resultScopes.Clear(*state.m_data)) return false;
    state.m_data.reset();
    state.m_host.reset();
    state.m_owner = {};
    const auto publishedBytes = state.m_state.publishedBytes;
    state.m_state = {};
    state.m_state.publishedBytes = publishedBytes;
    return true;
}

ArtifactAdmission ArtifactReductionHostFeature::SendRequest(ArtifactHostRequest request)
{
    if (const auto error = m_impl->GetAccessError(); error != ArtifactError::None) return { error, 0 };
    if (request.action == ArtifactAction::Prepare) {
        if (!request.prepare || request.requestId != 0) return { ArtifactError::InvalidRequest, 0 };
        return StartCandidate(std::move(*request.prepare));
    }
    if (request.prepare) return { ArtifactError::InvalidRequest, 0 };
    if (request.action == ArtifactAction::Commit) {
        if (request.requestId == 0) return { ArtifactError::InvalidRequest, 0 };
        const auto result = SetCandidate(request.requestId);
        return { result.error, result.error == ArtifactError::None ? request.requestId : 0 };
    }
    if (request.requestId != 0) return { ArtifactError::InvalidRequest, 0 };
    if (request.action == ArtifactAction::Cancel) return { StopCandidate(), 0 };
    if (request.action == ArtifactAction::Discard) return { ClearCandidate(), 0 };
    return { ArtifactError::InvalidRequest, 0 };
}

ArtifactAdmission ArtifactReductionHostFeature::StartCandidate(ArtifactRequest request)
{
    auto& state = *m_impl;
    if (const auto error = state.GetAccessError(); error != ArtifactError::None) return { error, 0 };
    if (state.m_future.valid() || state.m_state.status == ArtifactStatus::Ready) return { ArtifactError::Busy, 0 };
    if (state.m_nextId == std::numeric_limits<std::uint64_t>::max()) return { ArtifactError::TooLarge, 0 };
    try {
        auto control = std::make_shared<ArtifactReduction::TaskControl>();
        control->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeoutMs);
        ArtifactReduction::AlgorithmInput input;
        std::vector<DataInputRef> inputs;
        std::vector<DataExpectation> expectations;
        auto error = state.BuildInputs(request, input, inputs, expectations);
        if (error != ArtifactError::None) return { error, 0 };
        std::size_t requiredBytes = 0;
        error = ArtifactReduction::GetInputError(input, request, state.m_config, requiredBytes);
        if (error != ArtifactError::None) return { error, 0 };
        // Publish the future before notifying HostDriven work; notification must never precede readiness.
        std::packaged_task<ArtifactReduction::AlgorithmResult()> task(
            [input = std::move(input), request, config = state.m_config, control]() {
                return ArtifactReduction::BuildArtifactCandidate(input, request, config, *control);
            });
        auto future = task.get_future();
        std::thread worker([task = std::move(task), host = std::weak_ptr<FeatureHostControl>(state.m_host)]() mutable {
            task();
            try { if (const auto controlPort = host.lock()) (void)controlPort->SendWorkAvailable(); }
            catch (...) {}
        });
        state.m_worker = std::move(worker);
        state.m_future = std::move(future);
        state.m_control = std::move(control);
        state.m_candidate = {};
        state.m_inputs = std::move(inputs);
        state.m_expectations = std::move(expectations);
        state.m_inputMode = request.inputMode;
        const auto publishedBytes = state.m_state.publishedBytes;
        state.m_state = {};
        state.m_state.publishedBytes = publishedBytes;
        state.m_state.status = ArtifactStatus::Running;
        state.m_state.requestId = state.m_nextId++;
        state.m_state.requiredBytes = requiredBytes;
        return { ArtifactError::None, state.m_state.requestId };
    }
    catch (const std::bad_alloc&) { return { ArtifactError::TooLarge, 0 }; }
    catch (...) { return { ArtifactError::Unavailable, 0 }; }
}

bool ArtifactReductionHostFeature::OnHostTick()
{
    auto& state = *m_impl;
    if (state.GetAccessError() != ArtifactError::None) return false;
    if (!state.m_future.valid() || state.m_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return true;
    try {
        auto candidate = state.m_future.get();
        if (state.m_state.status == ArtifactStatus::Cancelling) candidate.error = ArtifactError::Cancelled;
        state.m_state.error = candidate.error;
        if (candidate.error == ArtifactError::None) {
            state.m_state.quality = candidate.quality;
            state.m_candidate = std::move(candidate);
            state.m_state.status = ArtifactStatus::Ready;
        }
        else state.m_state.status = ArtifactStatus::Failed;
    }
    catch (...) {
        state.m_state.status = ArtifactStatus::Failed;
        state.m_state.error = ArtifactError::InvalidData;
    }
    if (state.m_worker.joinable()) state.m_worker.join();
    return true;
}

ArtifactState ArtifactReductionHostFeature::GetState() const
{
    const auto& state = *m_impl;
    if (state.m_owner != std::thread::id{} && state.m_owner != std::this_thread::get_id()) {
        ArtifactState result;
        result.error = ArtifactError::WrongThread;
        return result;
    }
    auto result = state.m_state;
    if (state.m_control) result.progressPercent = state.m_control->progress.load(std::memory_order_relaxed);
    return result;
}

ArtifactError ArtifactReductionHostFeature::StopCandidate()
{
    auto& state = *m_impl;
    if (const auto error = state.GetAccessError(); error != ArtifactError::None) return error;
    if (!state.m_future.valid()) return ArtifactError::InvalidRequest;
    state.m_state.status = ArtifactStatus::Cancelling;
    state.m_control->isCancelled.store(true, std::memory_order_relaxed);
    return ArtifactError::None;
}

ArtifactError ArtifactReductionHostFeature::ClearCandidate()
{
    auto& state = *m_impl;
    if (const auto error = state.GetAccessError(); error != ArtifactError::None) return error;
    if (state.m_future.valid()) return ArtifactError::Busy;
    state.m_candidate = {};
    state.m_control.reset();
    state.m_inputs.clear();
    state.m_expectations.clear();
    const auto publishedBytes = state.m_state.publishedBytes;
    state.m_state = {};
    state.m_state.status = ArtifactStatus::Idle;
    state.m_state.publishedBytes = publishedBytes;
    return ArtifactError::None;
}

ArtifactCommitResult ArtifactReductionHostFeature::SetCandidate(std::uint64_t requestId)
{
    auto& state = *m_impl;
    ArtifactCommitResult result;
    result.error = state.GetAccessError();
    if (result.error != ArtifactError::None) return result;
    result.error = ArtifactError::InvalidRequest;
    if (state.m_state.status != ArtifactStatus::Ready || state.m_state.requestId != requestId) return result;
    if (state.m_candidate.publishBytes > state.m_config.publishBudgetBytes - state.m_state.publishedBytes) {
        result.error = ArtifactError::TooLarge;
        state.m_state.error = result.error;
        return result;
    }
    // Store 的同步 observer 可以重入；正式提交期间拒绝 Start/Clear/Detach/Set。
    state.m_state.status = ArtifactStatus::Publishing;
    try {
        const auto volumeId = state.m_data->CreateDataEntityId();
        const auto reportId = state.m_data->CreateDataEntityId();
        DataProvenance provenance{ std::string(GetFeatureId()), "artifact-reduction", "1.0.0", state.m_candidate.parameters };
        DataTransaction transaction;
        transaction.policy = state.m_inputMode == ArtifactInputMode::CurrentPrimary
            ? DataPublishPolicy::RequireCurrentInputs : DataPublishPolicy::AllowHistoricalResult;
        transaction.expectations = state.m_expectations;
        transaction.outputs = {
            { volumeId, 0, DataTypes::imageGrid3D, state.m_inputs, state.m_candidate.image, provenance },
            { reportId, 0, DataTypes::recordTable, state.m_inputs, state.m_candidate.report, provenance }
        };
        const auto commit = state.m_resultScopes.Commit(*state.m_data,std::move(transaction));
        result.status = commit.status;
        state.m_state.commitStatus = commit.status;
        if (commit.status == DataCommitStatus::Rejected) {
            result.error = commit.failureReason == DataCommitFailure::ExpectationFailed
                ? ArtifactError::SourceChanged : ArtifactError::CommitFailed;
            state.m_state.status = ArtifactStatus::Ready;
            state.m_state.error = result.error;
            return result;
        }
        result.error = ArtifactError::None;
        result.correctedVolume = DataRevisionRef{ volumeId, 1 };
        result.qualityReport = DataRevisionRef{ reportId, 1 };
        state.m_state.publishedBytes += state.m_candidate.publishBytes;
        state.m_state.correctedVolume = result.correctedVolume;
        state.m_state.qualityReport = result.qualityReport;
        state.m_candidate = {};
        state.m_inputs.clear();
        state.m_expectations.clear();
        state.m_state.status = ArtifactStatus::Idle;
        state.m_state.error = ArtifactError::None;
    }
    catch (const std::bad_alloc&) {
        result.error = ArtifactError::TooLarge;
        state.m_state.status = ArtifactStatus::Ready;
        state.m_state.error = result.error;
    }
    catch (...) {
        result.error = ArtifactError::CommitFailed;
        state.m_state.status = ArtifactStatus::Ready;
        state.m_state.error = result.error;
    }
    return result;
}
