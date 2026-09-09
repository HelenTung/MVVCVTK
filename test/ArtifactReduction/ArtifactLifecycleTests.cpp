// 测试用途：验证伪影任务启动、通知、候选发布、取消与生命周期清理。
#include "ArtifactTestSupport.h"
#include "Host/ArtifactReductionHostFeature.h"
#include "TestDataPort.h"

#include <thread>
#include <atomic>

namespace {
class WorkControl final : public FeatureHostControl {
public:
    bool SetActiveViews(const std::vector<std::string>&) override { return true; }
    bool SetViewStatus(const std::vector<std::string>&, const std::string&) override { return true; }
    bool SendSceneDelta(FeatureSceneDelta) override { return true; }
    bool AttachInput(HostInputBinding) override { return true; }
    bool DetachInput(std::string_view) override { return true; }
    bool SendOwnerComplete(std::function<void()>) override { return false; }
    bool SendWorkAvailable() override { ++notifications; return true; }
    std::atomic<int> notifications{0};
};
class CommitTestPort final : public TestDataPort {
public:
    bool shouldThrow = false;
    DataCommitResult SetDataCommit(DataTransaction transaction) override
    {
        if (shouldThrow) throw std::runtime_error("injected commit failure");
        return TestDataPort::SetDataCommit(std::move(transaction));
    }
};
DataRevisionRef Publish(TestDataPort& port, std::shared_ptr<const IDataPayload> payload,
    bool primary = false, std::optional<DataRevisionRef> previous = {})
{
    const auto id = previous ? previous->entityId : port.CreateDataEntityId();
    const auto generation = previous ? previous->generation : 0;
    DataTransaction transaction;
    transaction.outputs.push_back({ id, generation, payload->GetDataType(), {}, payload, {} });
    const DataRevisionRef ref{ id, generation + 1 };
    if (primary) {
        const auto binding = port.GetDataBinding(port.GetDataGraph(), primaryVolumeBinding);
        transaction.bindings.push_back({ std::string(primaryVolumeBinding), binding ? binding->revision : 0,
            true, binding ? binding->target : std::optional<DataRevisionRef>{}, ref });
    }
    Require(port.SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded, "test data publication");
    return ref;
}

DataRevisionRef PublishRoi(TestDataPort& port,const DataRevisionRef& source,const DataRevisionRef& mask,
    std::optional<DataRevisionRef> previous={})
{
    const auto graph=port.GetDataGraph();
    const auto catalog=graph.view->GetDataBinding(roiCatalogBinding);
    RoiNode node; node.primitive.shape=RoiShape::MaskReference; node.primitive.mask=mask;
    RoiRequest request;
    request.action=previous ? RoiAction::SetGeometry:RoiAction::Create;
    request.definition={source,{node}};
    request.expectedRoi=previous;
    request.expectedCatalogRevision=catalog ? catalog->revision:0;
    if (!previous) request.metadata.name="Artifact test ROI";
    const auto result=port.SetRoi(request);
    Require(result.error==RoiError::None && result.roi,"public ROI transaction");
    return result.roi->revision;
}

ArtifactAdmission Prepare(ArtifactReductionHostFeature& feature, const ArtifactRequest& request)
{
    return feature.SendRequest({ ArtifactAction::Prepare, request, 0 });
}

ArtifactState WaitResult(ArtifactReductionHostFeature& feature)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        feature.OnHostTick();
        const auto state = feature.GetState();
        if (state.status != ArtifactStatus::Running && state.status != ArtifactStatus::Cancelling) return state;
        std::this_thread::yield();
    }
    throw std::runtime_error("feature completion deadline");
}
}

void TestLifecycle()
{
    auto port = std::make_shared<CommitTestPort>();
    const auto grid = CreateGrid(8, 7, 6);
    const auto image = CreateImage(grid, ImageValueType::Float32, std::vector<float>(336, 100));
    const auto source = Publish(*port, image, true);
    const auto mask = Publish(*port, CreateMask(grid, std::vector<std::uint8_t>(336, 1)));
    ArtifactReductionHostFeature feature;
    const auto roi = PublishRoi(*port, source, mask);
    ArtifactRequest request; request.source = source; request.processingRoi = roi;
    Require(Prepare(feature, request).error == ArtifactError::Unavailable, "not attached");
    HostFeatureContext context; context.data = port;
    {
        auto host = std::make_shared<WorkControl>();
        auto notifiedContext = context;
        notifiedContext.host = host;
        ArtifactReductionHostFeature notified;
        Require(notified.AttachHost(notifiedContext), "notified feature attaches");
        for (int iteration = 0; iteration < 20; ++iteration) {
            const auto before = host->notifications.load();
            Require(Prepare(notified, request).error == ArtifactError::None, "notified candidate accepted");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (host->notifications.load() == before && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Require(host->notifications.load() == before + 1 && notified.GetState().status == ArtifactStatus::Running,
                "worker notification leaves candidate consumption on owner");
            Require(notified.OnHostTick() && notified.GetState().status == ArtifactStatus::Ready,
                "notification guarantees a ready future for one owner tick");
            Require(notified.SendRequest({ArtifactAction::Discard, {}, 0}).error == ArtifactError::None,
                "notified task can be discarded and restarted");
        }
        Require(notified.DetachHost(), "notified worker joins before detach");
    }
    Require(feature.AttachHost(context) && !feature.AttachHost(context), "attach exactly once");
    Require(feature.GetDataContract().outputs.size() == 2, "data contract");
    Require(feature.SendRequest({ ArtifactAction::Prepare, {}, 0 }).error == ArtifactError::InvalidRequest, "missing prepare payload");
    Require(feature.SendRequest({ ArtifactAction::Prepare, request, 1 }).error == ArtifactError::InvalidRequest, "prepare rejects id");
    for (const auto action : { ArtifactAction::Cancel, ArtifactAction::Commit, ArtifactAction::Discard }) {
        Require(feature.SendRequest({ action, request, 0 }).error == ArtifactError::InvalidRequest, "non-prepare rejects prepare payload");
    }
    Require(feature.SendRequest({ ArtifactAction::Cancel, {}, 1 }).error == ArtifactError::InvalidRequest, "cancel rejects id");
    Require(feature.SendRequest({ ArtifactAction::Discard, {}, 1 }).error == ArtifactError::InvalidRequest, "discard rejects id");
    Require(feature.SendRequest({ static_cast<ArtifactAction>(255), {}, 0 }).error == ArtifactError::InvalidRequest, "unknown action");
    ArtifactError wrongThread = ArtifactError::None;
    std::thread wrong([&] { wrongThread = Prepare(feature, request).error; }); wrong.join();
    Require(wrongThread == ArtifactError::WrongThread, "owner thread gate");
    const auto admission = Prepare(feature, request);
    Require(admission.error == ArtifactError::None && Prepare(feature, request).error == ArtifactError::Busy, "single task admission");
    Require(WaitResult(feature).status == ArtifactStatus::Ready, "candidate ready");
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, admission.requestId + 1 }).error == ArtifactError::InvalidRequest, "candidate id gate");
    port->shouldThrow = true;
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, admission.requestId }).error == ArtifactError::CommitFailed
        && feature.GetState().error == ArtifactError::CommitFailed && feature.GetState().status == ArtifactStatus::Ready,
        "commit exception retains candidate and state error");
    port->shouldThrow = false;
    const auto before = port->GetDataBinding(port->GetDataGraph(), primaryVolumeBinding);
    Publish(*port, CreateMask(grid, std::vector<std::uint8_t>(336, 0)));
    bool observed = false;
    bool isReentryRejected = false;
    const auto observer = port->AttachDataChange([&](const DataChangeSet&) {
        observed = true;
        isReentryRejected = feature.GetState().status == ArtifactStatus::Publishing
            && feature.SendRequest({ ArtifactAction::Discard, {}, 0 }).error == ArtifactError::Busy
            && Prepare(feature, request).error == ArtifactError::Busy
            && feature.SendRequest({ ArtifactAction::Cancel, {}, 0 }).error == ArtifactError::Busy
            && feature.SendRequest({ ArtifactAction::Commit, {}, admission.requestId }).error == ArtifactError::Busy
            && !feature.DetachHost();
        throw std::runtime_error("observer exception must not undo committed data");
    });
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, admission.requestId }).error == ArtifactError::None, "unrelated commit does not stale candidate");
    Require(observed && isReentryRejected, "graph observer reentry is rejected and exception isolated");
    port->DetachDataChange(observer);
    const auto published = feature.GetState();
    Require(published.correctedVolume && published.qualityReport, "both output refs");
    Require(port->GetDataBinding(port->GetDataGraph(), primaryVolumeBinding)->target == before->target, "no primary activation");
    const auto output = port->GetData(port->GetDataGraph(), *published.correctedVolume);
    Require(output && output->inputs.size() == 3 && output->inputs[1].role == "processing-roi" && output->inputs[1].source == roi && output->provenance->canonicalParameters.find("3377fed") != std::string::npos, "complete provenance");
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, admission.requestId }).error == ArtifactError::InvalidRequest, "no duplicate publication");
    const auto maskCandidate = Prepare(feature, request);
    Require(WaitResult(feature).status == ArtifactStatus::Ready, "mask CAS candidate");
    const auto nextMask = Publish(*port, CreateMask(grid, std::vector<std::uint8_t>(336, 1)), false, mask);
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, maskCandidate.requestId }).error == ArtifactError::SourceChanged, "mask head CAS");
    feature.SendRequest({ ArtifactAction::Discard, {}, 0 });
    request.processingRoi = PublishRoi(*port, source, nextMask, roi);
    const auto next = Prepare(feature, request);
    Require(WaitResult(feature).status == ArtifactStatus::Ready, "second candidate");
    Publish(*port, image, false, source); // head改变，binding仍指原修订。
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, next.requestId }).error == ArtifactError::SourceChanged, "explicit head CAS");
    Require(feature.GetState().status == ArtifactStatus::Ready, "failed commit retains candidate");
    feature.SendRequest({ ArtifactAction::Discard, {}, 0 });
    request.inputMode = ArtifactInputMode::ExplicitRevision;
    const auto historical = Prepare(feature, request);
    Require(WaitResult(feature).status == ArtifactStatus::Ready, "historical candidate");
    Require(feature.SendRequest({ ArtifactAction::Commit, {}, historical.requestId }).error == ArtifactError::None
        && feature.GetState().commitStatus == DataCommitStatus::SucceededHistorical, "historical publication");
    const auto cancelled = Prepare(feature, request);
    Require(cancelled.error == ArtifactError::None, "cancel setup");
    // 等待worker已完成但不tick，覆盖迟到取消竞争；progress仅由worker更新。
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (feature.GetState().progressPercent != 100 && std::chrono::steady_clock::now() < readyDeadline) std::this_thread::yield();
    Require(feature.SendRequest({ ArtifactAction::Cancel, {}, 0 }).error == ArtifactError::None, "cancel accepted before owner consumption");
    Require(WaitResult(feature).error == ArtifactError::Cancelled, "ready result cannot win cancellation");
    Require(feature.DetachHost() && feature.DetachHost(), "detach idempotent");
    Require(feature.AttachHost(context), "reattach");
    Require(feature.GetState().publishedBytes >= published.publishedBytes, "history budget survives reattach");
    Require(feature.DetachHost(), "final detach");
    ArtifactConfig small; small.publishBudgetBytes = 1;
    ArtifactReductionHostFeature limited(small);
    Require(limited.AttachHost(context), "limited attach");
    const auto limitedId = Prepare(limited, request);
    Require(WaitResult(limited).status == ArtifactStatus::Ready, "limited candidate");
    Require(limited.SendRequest({ ArtifactAction::Commit, {}, limitedId.requestId }).error == ArtifactError::TooLarge, "publish budget rejection");
    Require(limited.DetachHost(), "limited detach");
    {
        ArtifactReductionHostFeature destructorTest;
        Require(destructorTest.AttachHost(context), "destructor setup");
        Require(Prepare(destructorTest, request).error == ArtifactError::None, "destructor cancels/joins pending task");
    }
    // 原生切片内部不能即时取消；超时Detach必须保留可重试状态，不能释放worker。
    const auto largeGrid = CreateGrid(768, 768, 2);
    const auto largeSource = Publish(*port, CreateImage(largeGrid, ImageValueType::Float32,
        std::vector<float>(768 * 768 * 2, 100.0f)));
    ArtifactRequest largeRequest; largeRequest.source = largeSource; largeRequest.inputMode = ArtifactInputMode::ExplicitRevision;
    ArtifactRingParams ring; ring.centerIndex = { 384, 384 }; ring.threshMin = 0; ring.threshMax = 200; ring.threshold = 50;
    largeRequest.ring = ring;
    ArtifactConfig immediate; immediate.stopTimeoutMs = 0;
    ArtifactReductionHostFeature stopping(immediate);
    Require(stopping.AttachHost(context) && Prepare(stopping, largeRequest).error == ArtifactError::None, "detach deadline setup");
    const auto startedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (stopping.GetState().progressPercent == 0 && std::chrono::steady_clock::now() < startedDeadline) std::this_thread::yield();
    Require(stopping.GetState().progressPercent > 0, "native work started");
    Require(!stopping.DetachHost(), "detach deadline preserves running task");
    Require(stopping.GetState().status == ArtifactStatus::Stopping && Prepare(stopping, largeRequest).error == ArtifactError::Busy,
        "stopping rejects new work");
    const auto stoppedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool isDetached = false;
    while (!(isDetached = stopping.DetachHost()) && std::chrono::steady_clock::now() < stoppedDeadline) std::this_thread::yield();
    Require(isDetached && stopping.GetState().status == ArtifactStatus::Detached, "detach retry succeeds after native slice");
}
