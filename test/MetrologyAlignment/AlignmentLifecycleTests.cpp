// 测试用途：验证计量对齐的生命周期、输入当前性、显示、重入、故障恢复与工作通知。
#include "../TestDataPort.h"
#include "../../MVVCVTK/features/common/FeatureResultScopes.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include "AlignmentData.h"
#include "AlignmentInspectionData.h"
#include "AlignmentReport.h"
#include "AlignmentVoxelVolumeData.h"
#include "Host/FeatureModelTransformPort.h"
#include "AlignmentMath.h"
#include "AlignmentOverlay.h"
#include "AlignmentSceneDisplay.h"
#include "AlignmentPrealignTestSupport.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkPropCollection.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <thread>

namespace AlignmentReceiptAllocations {
thread_local bool enabled = false;
thread_local std::size_t count = 0;
}
// 只计数回执接收期间的普通 C++ 分配；不注入 noexcept 内异常，不影响其它线程。
void* operator new(std::size_t bytes) {
    if (AlignmentReceiptAllocations::enabled) ++AlignmentReceiptAllocations::count;
    if (auto* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
int failures = 0;
void Check(bool value, const char *message) {
    if (!value) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
class FaultDataPort final : public TestDataPort {
  public:
    bool rejectsCommit = false, throwsAfterCommit = false;
    bool throwsAfterCommitOnce = false;
    std::function<bool(const DataTransaction&)> afterCommitTarget;
    std::size_t injectedCommits = 0;
    DataCommitStatus injectedStatus = DataCommitStatus::Rejected;
    bool injectedActivation = false;
    bool rejectsRetirement = false, throwsBeforeCommit = false, omitsGraph = false;
    enum class RecoveryQuery { None, Lifetime, Graph, Binding };
    RecoveryQuery recoveryQuery = RecoveryQuery::None;
    bool recoveryArmed = false;
    mutable std::size_t recoveryGraphFaults = 0, recoveryBindingFaults = 0;
    bool rejectsChangeObservation = false;
    std::size_t activeDataObservers = 0, observedDataChanges = 0;
    std::function<void()> beforeCommitFault;
    std::size_t underlyingCommitCalls = 0;
    std::size_t receiptCalls = 0, receiptAllocations = 0;
    mutable std::size_t receiptQueries = 0;
    bool isReceiving = false, receiptOnOwner = true;
    bool reversesCommitOrder = false;
    DataRevisionRef receiptFirstOutput;
    std::optional<DataRevisionRef> receiptFirstTarget;
    const std::thread::id receiptOwner = std::this_thread::get_id();
    DataEntityId attemptedScope, committedScope;
    std::size_t unknownRetirements = 0;
    DataGraphSnapshot GetDataGraph() const override {
        if (isReceiving) ++receiptQueries;
        if (omitsGraph) return {};
        if (recoveryArmed && recoveryQuery == RecoveryQuery::Graph) {
            ++recoveryGraphFaults;
            throw std::runtime_error("recovery graph failure");
        }
        return TestDataPort::GetDataGraph();
    }
    DataLifetimeState GetDataLifetime(const DataEntityId& id) const override {
        if (isReceiving) ++receiptQueries;
        if (recoveryArmed && recoveryQuery == RecoveryQuery::Lifetime)
            throw std::runtime_error("recovery lifetime failure");
        return TestDataPort::GetDataLifetime(id);
    }
    std::optional<DataBinding> GetDataBinding(const DataGraphSnapshot& graph, std::string_view name) const override {
        if (isReceiving) ++receiptQueries;
        if (recoveryArmed && recoveryQuery == RecoveryQuery::Binding) {
            ++recoveryBindingFaults;
            throw std::runtime_error("recovery binding failure");
        }
        return TestDataPort::GetDataBinding(graph, name);
    }
    DataObserverId AttachDataChange(DataChangeCallback callback) override {
        if (!callback || rejectsChangeObservation) return 0;
        const auto id = TestDataPort::AttachDataChange([this, callback = std::move(callback)](const DataChangeSet& change) {
            ++observedDataChanges;
            callback(change);
        });
        if (id) ++activeDataObservers;
        return id;
    }
    bool DetachDataChange(DataObserverId id) override {
        const bool detached = TestDataPort::DetachDataChange(id);
        if (detached) --activeDataObservers;
        return detached;
    }
    enum class ReadFault { None, Throws, Omits };
    ReadFault readFault = ReadFault::None;
    std::optional<DataRevisionRef> faultReadRef;
    mutable std::size_t faultReadCalls = 0;
    std::size_t commitCalls = 0, retirementInUseCalls = 0;
    DataSnapshot GetData(const DataGraphSnapshot& graph, const DataRevisionRef& ref) const override {
        if (isReceiving) ++receiptQueries;
        if (faultReadRef && ref == *faultReadRef) {
            ++faultReadCalls;
            if (readFault == ReadFault::Throws) throw std::runtime_error("derived read fault");
            if (readFault == ReadFault::Omits) return {};
        }
        return TestDataPort::GetData(graph, ref);
    }
    std::function<void()> onCommit;
    DataCommitResult SetDataCommit(DataTransaction tx, DataCommitReceipt* receipt = nullptr) override {
        ++commitCalls;
        if (rejectsCommit || (rejectsRetirement && !tx.retireScopes.empty()))
            return {};
        if (!tx.outputs.empty()) attemptedScope = tx.outputs.back().lifetimeScope.value_or(DataEntityId{});
        for (const auto& retirement : tx.retireScopes)
            if (TestDataPort::GetDataLifetime(retirement.scopeId).status == DataLifetimeStatus::Unknown) ++unknownRetirements;
        if (throwsBeforeCommit) {
            if (beforeCommitFault) beforeCommitFault();
            recoveryArmed = true;
            throw std::bad_alloc();
        }
        const bool isRetirement = !tx.retireScopes.empty();
        const bool injectAfter = throwsAfterCommit && (!afterCommitTarget || afterCommitTarget(tx));
        if (reversesCommitOrder) {
            std::reverse(tx.outputs.begin(), tx.outputs.end());
            std::reverse(tx.bindings.begin(), tx.bindings.end());
        }
        ++underlyingCommitCalls;
        struct ForwardReceipt final : DataCommitReceipt {
            FaultDataPort& port;
            DataCommitReceipt* next;
            ForwardReceipt(FaultDataPort& owner, DataCommitReceipt* receiver) : port(owner), next(receiver) {}
            void SetDataCommitted(const DataCommitResult& actual) noexcept override {
                ++port.receiptCalls;
                port.receiptFirstOutput = actual.published.empty() ? DataRevisionRef{} : actual.published.front()->self;
                port.receiptFirstTarget = actual.bindings.empty() ? std::optional<DataRevisionRef>{} : actual.bindings.front().target;
                port.receiptOnOwner = port.receiptOnOwner && std::this_thread::get_id() == port.receiptOwner;
                port.isReceiving = true;
                const auto before = AlignmentReceiptAllocations::count;
                const bool wasEnabled = AlignmentReceiptAllocations::enabled;
                AlignmentReceiptAllocations::enabled = true;
                next->SetDataCommitted(actual);
                AlignmentReceiptAllocations::enabled = wasEnabled;
                port.receiptAllocations += AlignmentReceiptAllocations::count - before;
                port.isReceiving = false;
            }
        } forward(*this, receipt);
        auto r = TestDataPort::SetDataCommit(std::move(tx), receipt ? &forward : nullptr);
        if (!r.published.empty()) committedScope = r.published.back()->lifetimeScope;
        if (isRetirement && r.failureReason == DataCommitFailure::ResultInUse) ++retirementInUseCalls;
        if (onCommit)
            onCommit();
        if (injectAfter) {
            ++injectedCommits; injectedStatus = r.status; injectedActivation = r.isActivated;
            if (throwsAfterCommitOnce) throwsAfterCommit = false;
            recoveryArmed = true;
            throw std::runtime_error("after commit");
        }
        return r;
    }
};
class OverlayPort final : public OverlayService {
  public:
    bool rejectsAttach = false;
    std::function<void()> onAttach;
    std::function<void()> onRemove;
    std::vector<std::shared_ptr<FeatureOverlay>> overlays;
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> value) override {
        overlays.push_back(value);
        if (onAttach) onAttach();
        return !rejectsAttach;
    }
    void RemoveOverlay(std::shared_ptr<FeatureOverlay> value) noexcept override {
        overlays.erase(std::remove(overlays.begin(), overlays.end(), value), overlays.end());
        if (onRemove) onRemove();
    }
    void ClearOverlays() noexcept override {
        overlays.clear();
    }
};
class Views final : public FeatureViewDirectory {
  public:
    std::shared_ptr<OverlayPort> overlay = std::make_shared<OverlayPort>();
    std::vector<HostFeatureView> GetViews(const HostViewTargets &) const override {
        return {{"3d", HostRenderViewRole::Primary3D}};
    }
    std::shared_ptr<FeatureViewService> GetFeaturePort(const std::string &) const override {
        return {};
    }
    std::shared_ptr<OverlayService> GetOverlayPort(const std::string &) const override {
        return overlay;
    }
    std::optional<HostInputView> GetInputView(const HostViewTarget &) const override {
        return {};
    }
};
class WorkControl : public FeatureHostControl {
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
class PoseControl final : public WorkControl,public FeatureModelTransformPort {
public:
    ModelTransformSnapshot state;AlignmentMatrix queued=alignmentIdentity;bool cancel=false,rejectCommit=false,rejectStopOnce=false,throwAfterQueue=false,throwStart=false;
    PoseControl() { state.sessionGeneration=1;state.bindingRevision=1; }
    std::optional<ModelTransformSnapshot> GetTransformState() const override { return state; }
    std::optional<std::uint64_t> StartTransform(const ModelTransformSnapshot &expected) override {
        if(throwStart)throw std::runtime_error("start pose fault");
        if(state.editToken || expected.dataRevision!=state.dataRevision || expected.transformRevision!=state.transformRevision)return {};
        state.editToken=++next;cancel=false;return state.editToken;
    }
    bool SetTransformPreview(std::uint64_t,std::uint64_t,const AlignmentMatrix &) override { return false; }
    bool SetTransformCommit(std::uint64_t token,std::uint64_t,const AlignmentMatrix &matrix) override {
        if(rejectCommit || token!=state.editToken)return false;queued=matrix;state.hasPending=true;
        if(throwAfterQueue)throw std::runtime_error("queued pose fault");return true;
    }
    bool StopTransform(std::uint64_t token) override {
        if(rejectStopOnce){rejectStopOnce=false;return false;}
        if(!token || token!=state.editToken)return false;cancel=true;state.hasPending=true;return true;
    }
    void Frame() {
        if(!state.hasPending)return;
        if(!cancel){state.modelToWorld=queued;state.worldToModel=AlignmentMath::Inverse(queued);++state.transformRevision;}
        state.completedToken=state.editToken;state.editToken=0;state.hasPending=false;
        state.completion=cancel ? ModelTransformStatus::Cancelled : ModelTransformStatus::Committed;
    }
private: std::uint64_t next=0;
};
struct Fixture final {
    std::shared_ptr<FaultDataPort> data = std::make_shared<FaultDataPort>();
    std::shared_ptr<MetrologyAlignmentHostFeature> feature =
        std::make_shared<MetrologyAlignmentHostFeature>();
    AlignmentInput input;
    AlignmentRecipe recipe;
    DataRevisionRef recipeRef;
    std::shared_ptr<const SurfaceMeshPayload> mesh;
    Fixture(std::shared_ptr<Views> views = {}, std::shared_ptr<FeatureHostControl> host = {}, bool hasSectionMesh=false,std::size_t sourceDepth=0, bool isSourceScoped=false) {
        input.source = {data->CreateDataEntityId(), 1};
        input.mesh = {data->CreateDataEntityId(), 1};
        input.scopeData = {data->CreateDataEntityId(), 1};
        input.sourceFrameId = "scan-1";
        input.coordinateFrame = "RAS";
        input.scope = "part-1";
        input.sourceBinding = "test.source";
        input.meshBinding = "test.mesh";
        const DataRevisionRef nominal{data->CreateDataEntityId(), 1};
        auto table = std::make_shared<const RecordTablePayload>(
            DataTypes::recordTable, "test",
            std::vector<RecordColumn>{{"value", std::vector<double>{1}}});
        std::vector<double> vertices{0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4};
        std::vector<std::uint64_t> triangles;
        if (hasSectionMesh) {
            constexpr std::size_t sides=32;
            for (int z=-1;z<=1;z+=2) for (std::size_t i=0;i<sides;++i) {
                const auto a=2*std::acos(-1.0)*i/sides;
                vertices.insert(vertices.end(),{5*std::cos(a),5*std::sin(a),static_cast<double>(z)});
            }
            for (std::size_t i=0;i<sides;++i) {
                const auto a=4+i,b=4+(i+1)%sides,c=a+sides,d=b+sides;
                triangles.insert(triangles.end(),{a,b,c,b,d,c});
            }
        }
        mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::move(triangles));
        DataTransaction tx;
        for (const auto ref : {input.source, input.scopeData, nominal})
            tx.outputs.push_back({ref.entityId, 0, DataTypes::recordTable, {}, table, {}});
        if (isSourceScoped) {
            tx.outputs.front().lifetimeScope = data->CreateDataEntityId();
            tx.outputs.front().lifetimePolicy = DataLifetimePolicy::OwnerRetirement;
        }
        auto source=input.source;
        for(std::size_t i=0;i<sourceDepth;++i){const DataRevisionRef next{data->CreateDataEntityId(),1};
            tx.outputs.push_back({next.entityId,0,DataTypes::recordTable,{{"parent",source}},table,{}});source=next;}
        tx.outputs.push_back(
            {input.mesh.entityId, 0, DataTypes::surfaceMesh, {{"source", source}}, mesh, {}});
        tx.bindings = {{input.sourceBinding, 0, true, {}, input.source},
                       {input.meshBinding, 0, true, {}, input.mesh}};
        Check(data->SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded,
              "fixture graph committed");
        recipe.id = "alignment";
        recipe.targetFrameId = "part-frame";
        recipe.method = AlignmentMethod::Rps;
        recipe.datumCount = 0;
        recipe.nominalData = nominal;
        for (std::size_t i = 0; i < 4; ++i) {
            AlignmentGeometrySpec spec;
            spec.id = "point-" + std::to_string(i);
            spec.kind = AlignmentGeometryKind::Point;
            spec.region.pinnedMesh = input.mesh;
            spec.region.vertexIds = {i};
            recipe.geometries.push_back(spec);
            for (std::size_t j = 0; j < 3; ++j) {
                AlignmentConstraint constraint;
                constraint.geometryIndex = i;
                constraint.targetDirection = {0, 0, 0};
                constraint.targetDirection[j] = 1;
                constraint.nominalPoint = {vertices[i * 3] + 0.1, vertices[i * 3 + 1] - 0.2,
                                           vertices[i * 3 + 2] + 0.3};
                recipe.constraints.push_back(constraint);
            }
        }
        HostFeatureContext context;
        context.data = data;
        context.views = views;
        context.host = std::move(host);
        Check(feature->AttachHost(context), "attach data-only feature");
        Check(!feature->AttachHost(context), "duplicate attach rejected");
        AlignmentRequest save;
        save.action = AlignmentAction::SaveRecipe;
        save.recipe = recipe;
        const auto admission =
            feature->SendRequest(save, [&](AlignmentResult r) { recipeRef = r.recipe; });
        Check(admission.status == AlignmentAdmissionStatus::Accepted &&
                  GetDataRevisionRefValid(recipeRef),
              "save versioned recipe");
    }
    AlignmentRequest Request() const {
        AlignmentRequest r;
        r.input = input;
        r.recipeRef = recipeRef;
        return r;
    }
    void Drain() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (feature->GetState().isBusy && std::chrono::steady_clock::now() < deadline) {
            feature->OnHostTick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (feature->GetState().isBusy) {
            // 超时后禁止继续使用仍引用本测试栈变量的异步回调。
            Check(false, "bounded task completion; stopping test before callback captures leave scope");
            std::abort();
        }
    }
    AlignmentResult Solve() {
        AlignmentResult result;
        int count = 0;
        Check(feature->SendRequest(Request(),
                                   [&](AlignmentResult r) {
                                       result = std::move(r);
                                       ++count;
                                   })
                      .status == AlignmentAdmissionStatus::Accepted,
              "start accepted");
        Drain();
        Check(count == 1, "one completion");
        return result;
    }
    ~Fixture() {
        Drain();
        Check(feature->DetachHost(), "detach succeeds");
        Check(feature->DetachHost(), "detach idempotent");
    }
};
// d0d0660b 引入的旧累计门槛为 1024；首个越界请求和真实存活账本均须覆盖。
constexpr std::size_t ownerScopeSubmissions = 1024 + 1;

struct ScopePublication final {
    DataCommitStatus status = DataCommitStatus::Rejected;
    DataRevisionRef ref;
    DataEntityId scope;
};
struct ScopeFixture final {
    FaultDataPort data;
    FeatureInternal::ResultScopes scopes;
    DataRevisionRef source{data.CreateDataEntityId(), 1};
    ScopeFixture() {
        DataTransaction tx;
        DataRevisionDraft draft{source.entityId, 0, DataTypes::recordTable, {}, Payload(), {}};
        draft.lifetimeScope = data.CreateDataEntityId();
        tx.outputs.push_back(std::move(draft));
        Check(data.SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded, "scope fixture source publishes");
    }
    static std::shared_ptr<const IDataPayload> Payload() {
        return std::make_shared<const RecordTablePayload>(DataTypes::recordTable, "scope-test",
            std::vector<RecordColumn>{{"value", std::vector<double>{1}}});
    }
    DataTransaction Binding(const std::string& name, std::optional<DataRevisionRef> target) {
        const auto old = data.GetDataBinding(data.GetDataGraph(), name);
        DataTransaction tx;
        tx.bindings.push_back({name, old ? old->revision : 0, true, old ? old->target : std::nullopt, target});
        return tx;
    }
    ScopePublication Publish(const std::string& binding = "scope.active", const std::string& operation = "calculate",
                             std::optional<DataRevisionRef> dependency = {}, bool isHistorical = false,
                             DataPreparedResource resource = {}) {
        ScopePublication publication;
        publication.ref = {data.CreateDataEntityId(), 1};
        auto tx = binding.empty() ? DataTransaction{} : Binding(binding, publication.ref);
        if (isHistorical) {
            tx.policy = DataPublishPolicy::AllowHistoricalResult;
            DataExpectation expected;
            expected.kind = DataExpectationKind::Binding;
            expected.use = DataExpectationUse::Activation;
            expected.binding = "scope.missing-activation";
            expected.expectedBindingRevision = 1;
            tx.expectations.push_back(std::move(expected));
        }
        std::vector<DataInputRef> inputs{{"source", source}};
        if (dependency) inputs.push_back({"dependency", *dependency});
        tx.outputs.push_back({publication.ref.entityId, 0, DataTypes::recordTable, std::move(inputs), Payload(),
            DataProvenance{"scope-test", operation, "1", "{}"}});
        if (resource.lease) tx.outputs.back().preparedResources.push_back(std::move(resource));
        const auto committed = scopes.Commit(data, std::move(tx));
        publication.status = committed.status;
        if (const auto node = data.GetData(data.GetDataGraph(), publication.ref)) publication.scope = node->lifetimeScope;
        return publication;
    }
};

void ResultScopeLifecycle() {
    {
        ScopeFixture f;
        const DataRevisionRef primary{f.data.CreateDataEntityId(), 1};
        DataTransaction input;
        input.outputs.push_back({primary.entityId, 0, DataTypes::recordTable, {}, ScopeFixture::Payload(), {}});
        Check(f.data.SetDataCommit(std::move(input)).status == DataCommitStatus::Succeeded, "unscoped primary input publishes");
        const auto policySource = f.data.GetData(f.data.GetDataGraph(), primary);
        const DataRevisionRef output{f.data.CreateDataEntityId(), 1};
        DataTransaction tx;
        tx.outputs.push_back({output.entityId, 0, DataTypes::recordTable,
            {{"primary", primary}, {"other-input", f.source}}, ScopeFixture::Payload(), {}});
        const auto result = f.scopes.Commit(f.data, std::move(tx), policySource);
        const auto node = f.data.GetData(f.data.GetDataGraph(), output);
        Check(result.status == DataCommitStatus::Succeeded && node && !GetDataEntityIdValid(node->lifetimeScope)
              && f.scopes.GetScopeCount() == 0,
              "explicit policy source preserves Part primary-input inheritance without adopting another input owner");
    }
    {
        ScopeFixture f;
        DataEntityId previous;
        for (std::size_t submission = 0; submission < ownerScopeSubmissions; ++submission) {
            // ArtifactReduction 同样没有输出绑定；同类成功结果本身就是替换信号。
            const auto result = f.Publish("");
            Check(result.status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1,
                  "unbound replacement retains exactly the current owner scope beyond the former limit");
            if (GetDataEntityIdValid(previous))
                Check(f.data.GetDataLifetime(previous).status == DataLifetimeStatus::Released,
                      "superseded unbound output releases its actual lifecycle before its ledger entry disappears");
            previous = result.scope;
        }
        Check(f.scopes.Clear(f.data) && f.scopes.GetScopeCount() == 0, "unbound scope ledger clears completely");
    }
    {
        ScopeFixture f;
        for (std::size_t submission = 0; submission < ownerScopeSubmissions; ++submission)
            Check(f.Publish("scope.live." + std::to_string(submission)).status == DataCommitStatus::Succeeded,
                  "genuinely live bound results have no fixed scope-count admission limit");
        Check(f.scopes.GetScopeCount() == ownerScopeSubmissions, "live scope ownership is retained rather than discarded to cap the ledger");
        Check(f.scopes.Clear(f.data) && f.scopes.GetScopeCount() == 0, "all live scopes retire together when their producer clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        std::shared_ptr<const DataResourceLease> reader;
        {
            const auto node = f.data.GetData(f.data.GetDataGraph(), first.ref);
            reader = node->lifetime.lock()->StartResourceUse(first.ref, "scope-reader");
        }
        const auto second = f.Publish();
        Check(reader && second.status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "Reader lease blocks retirement without rejecting the replacement publication");
        reader.reset();
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released,
              "ended Reader lease permits retirement and ledger collection on the next commit");
        Check(f.scopes.Clear(f.data), "reader scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        auto held = f.data.GetData(f.data.GetDataGraph(), first.ref);
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Releasing,
              "retired payload snapshot keeps the producer ledger until actual release");
        held.reset();
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released,
              "ended payload owner releases the pending scope and shrinks the ledger");
        Check(f.scopes.Clear(f.data), "payload owner scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        Check(f.data.SetDataCommit(f.Binding("scope.keep", first.ref)).status == DataCommitStatus::Succeeded,
              "external keep binding publishes");
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "another consumer binding protects the old result after replacement");
        Check(f.data.SetDataCommit(f.Binding("scope.keep", {})).status == DataCommitStatus::Succeeded, "external keep binding clears");
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released,
              "released external keep allows old scope collection");
        Check(f.scopes.Clear(f.data), "external binding scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        const auto dependent = f.Publish("scope.dependent", "dependent", first.ref);
        const auto other = f.Publish("scope.other", "other-dependent", first.ref);
        Check(f.Publish().status == DataCommitStatus::Succeeded
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "live downstream results protect the superseded input scope");
        Check(f.scopes.Commit(f.data, f.Binding("scope.dependent", {})).status == DataCommitStatus::Succeeded
              && f.scopes.GetScopeCount() == 3
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published
              && f.data.GetDataLifetime(dependent.scope).status == DataLifetimeStatus::Released,
              "ending one descendant preserves the other descendant and its ancestor");
        Check(f.scopes.Commit(f.data, f.Binding("scope.other", {})).status == DataCommitStatus::Succeeded
              && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released
              && f.data.GetDataLifetime(other.scope).status == DataLifetimeStatus::Released,
              "ending the last descendant permits collection on the next commit");
        Check(f.scopes.Clear(f.data), "dependent scenario clears");
    }
    {
        ScopeFixture f;
        const DataRevisionRef first{f.data.CreateDataEntityId(), 1}, second{f.data.CreateDataEntityId(), 1};
        auto tx = f.Binding("scope.active", second);
        tx.outputs.push_back({first.entityId, 0, DataTypes::recordTable, {{"source", f.source}},
            ScopeFixture::Payload(), DataProvenance{"scope-test", "calculate", "1", "{}"}});
        tx.outputs.push_back({second.entityId, 0, DataTypes::recordTable, {{"internal", first}},
            ScopeFixture::Payload(), DataProvenance{"scope-test", "calculate", "1", "{}"}});
        Check(f.scopes.Commit(f.data, std::move(tx)).status == DataCommitStatus::Succeeded,
              "same-scope dependent outputs publish together");
        const auto scope = f.data.GetData(f.data.GetDataGraph(), first)->lifetimeScope;
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(scope).status == DataLifetimeStatus::Released,
              "an internal derived edge does not prevent its complete owner scope retiring");
        Check(f.scopes.Clear(f.data), "internal edge scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        const DataRevisionRef external{f.data.CreateDataEntityId(), 1};
        DataTransaction tx;
        tx.outputs.push_back({external.entityId, 0, DataTypes::recordTable, {{"source", first.ref}}, ScopeFixture::Payload(), {}});
        Check(f.data.SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded,
              "unscoped external descendant publishes outside the producer ledger");
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "an unscoped readable descendant also protects the old scope");
        const auto before = f.data.GetDataGraph().commitId;
        const auto active = f.data.GetDataBinding(f.data.GetDataGraph(), "scope.active");
        Check(!f.scopes.Clear(f.data) && f.scopes.GetScopeCount() == 2
              && f.data.GetDataGraph().commitId == before
              && f.data.GetDataBinding(f.data.GetDataGraph(), "scope.active")->target == active->target
              && f.data.GetData(f.data.GetDataGraph(), external),
              "grouped Clear preserves bindings and ownership while an external descendant remains live");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        const auto child = f.Publish("scope.child", "dependent", first.ref);
        Check(f.Publish().status == DataCommitStatus::Succeeded, "ancestor replacement publishes before child retirement");
        auto held = f.data.GetData(f.data.GetDataGraph(), child.ref);
        auto tx = f.Binding("scope.child", {});
        tx.retireScopes.push_back({child.scope, DataLifetimeStatus::Published, {child.ref}, true});
        Check(f.data.SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded
              && f.data.GetDataLifetime(child.scope).status == DataLifetimeStatus::Releasing,
              "retired descendant awaits its held snapshot");
        const auto hasHistoricalEdge = [&] {
            const auto graph = f.data.GetDataGraph();
            const auto edges = graph.view->GetDerivedData(first.ref);
            return std::find(edges.begin(), edges.end(), child.ref) != edges.end();
        };
        Check(hasHistoricalEdge() && !f.data.GetData(f.data.GetDataGraph(), child.ref),
              "reverse index retains the edge while the Releasing child is no longer readable");
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released
              && f.data.GetDataLifetime(child.scope).status == DataLifetimeStatus::Releasing,
              "a historical edge to a Releasing descendant does not keep its ancestor Published");
        held.reset();
        f.scopes.Collect(f.data);
        Check(hasHistoricalEdge() && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(child.scope).status == DataLifetimeStatus::Released,
              "the Released descendant is collected despite the historical reverse edge");
        Check(f.scopes.Clear(f.data), "historical reverse edge scenario clears");
    }
    {
        ScopeFixture f;
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(2, 2, 2);
        image->AllocateScalars(VTK_FLOAT, 1);
        auto resource = VtkPreparedDataView::BuildResourceUse(image, nullptr);
        const auto first = f.Publish("scope.active", "calculate", {}, false, resource);
        Check(resource.lease && first.status == DataCommitStatus::Succeeded
              && f.Publish().status == DataCommitStatus::Succeeded
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Releasing,
              "prepared render resource delays release after successful retirement");
        resource.lease.reset();
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Releasing,
              "the actual prepared VTK object retains its resource lease independently");
        image = nullptr;
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released,
              "ending the prepared resource permits release and ledger collection");
        Check(f.scopes.Clear(f.data), "prepared resource scenario clears");
    }
    for (const auto fault : {FaultDataPort::ReadFault::Throws, FaultDataPort::ReadFault::Omits}) {
        ScopeFixture f;
        const auto first = f.Publish();
        const auto child = f.Publish("scope.child", "dependent", first.ref);
        f.data.faultReadRef = child.ref;
        f.data.readFault = fault;
        const auto rejectedBefore = f.data.retirementInUseCalls;
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.data.faultReadCalls > 0
              && f.scopes.GetScopeCount() == 3
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "derived query fault preserves the successful publication and protected owner ledger");
        if (fault == FaultDataPort::ReadFault::Omits)
            Check(f.data.retirementInUseCalls > rejectedBefore,
                  "a query false negative still reaches the atomic transaction which rejects unsafe retirement");
        f.data.readFault = FaultDataPort::ReadFault::None;
        const auto queriedBefore = f.data.faultReadCalls;
        f.scopes.Collect(f.data);
        Check(f.data.faultReadCalls > queriedBefore
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "the next collection re-queries the descendant without a cached exception or blocker");
        Check(f.scopes.Commit(f.data, f.Binding("scope.child", {})).status == DataCommitStatus::Succeeded
              && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released
              && f.data.GetDataLifetime(child.scope).status == DataLifetimeStatus::Released,
              "ending the real descendant permits collection after the query fault is removed");
        Check(f.scopes.Clear(f.data), "query fault scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        const auto before = f.data.GetDataGraph().commitId;
        f.data.rejectsCommit = true;
        Check(f.Publish().status == DataCommitStatus::Rejected && f.scopes.GetScopeCount() == 1
              && f.data.GetDataGraph().commitId == before
              && f.data.GetDataBinding(f.data.GetDataGraph(), "scope.active")->target == first.ref
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "rejected publication preserves graph, active binding, and original owner ledger");
        f.data.rejectsCommit = false;
        f.data.throwsBeforeCommit = true;
        bool threw = false;
        try { (void)f.Publish(); } catch (const std::bad_alloc&) { threw = true; }
        f.data.throwsBeforeCommit = false;
        Check(threw && f.scopes.GetScopeCount() == 1 && f.data.GetDataGraph().commitId == before,
              "prepublication allocation failure leaves the old graph and ledger intact");
        f.data.rejectsRetirement = true;
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "cleanup transaction rejection retains retry ownership and does not undo successful publication");
        f.data.rejectsRetirement = false;
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released,
              "cleanup retry collects previously blocked scopes");
        Check(f.scopes.Clear(f.data), "failure scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        const auto historical = f.Publish("scope.active", "calculate", {}, true);
        Check(historical.status == DataCommitStatus::SucceededHistorical && f.scopes.GetScopeCount() == 2
              && f.data.GetDataBinding(f.data.GetDataGraph(), "scope.active")->target == first.ref
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Published,
              "historical publication retains ownership without replacing the active scope or adopting an unapplied binding");
        Check(f.scopes.Commit(f.data, f.Binding("scope.unrelated", {})).status == DataCommitStatus::Succeeded
              && f.scopes.GetScopeCount() == 2
              && f.data.GetDataLifetime(historical.scope).status == DataLifetimeStatus::Published,
              "unrelated binding commit preserves the never-activated historical result");
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(historical.scope).status == DataLifetimeStatus::Released,
              "the next current result replaces and collects its historical family member");
        Check(f.scopes.Clear(f.data), "historical scenario clears");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        f.data.throwsAfterCommit = true;
        bool threw = false;
        try { (void)f.scopes.Commit(f.data, f.Binding("scope.alias", first.ref)); }
        catch (const std::runtime_error&) { threw = true; }
        f.data.throwsAfterCommit = false;
        Check(threw && f.scopes.GetScopeCount() == 1
              && f.data.GetDataBinding(f.data.GetDataGraph(), "scope.alias")->target == first.ref,
              "binding-only transaction remains committed after a port wrapper exception");
        Check(f.scopes.Clear(f.data) && f.scopes.GetScopeCount() == 0
              && !f.data.GetDataBinding(f.data.GetDataGraph(), "scope.alias")->target,
              "postcommit binding ownership is adopted so producer Clear can remove its alias");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish();
        f.data.throwsAfterCommit = true;
        bool threw = false;
        try { (void)f.Publish(); } catch (const std::runtime_error&) { threw = true; }
        f.data.throwsAfterCommit = false;
        const auto active = f.data.GetDataBinding(f.data.GetDataGraph(), "scope.active");
        auto node = active && active->target ? f.data.GetData(f.data.GetDataGraph(), *active->target) : DataSnapshot{};
        const auto publishedScope = node ? node->lifetimeScope : DataEntityId{};
        Check(threw && node && active->target != first.ref && f.scopes.GetScopeCount() == 2,
              "postpublication port exception still adopts the committed scope ownership");
        // 解除本测试持有的 payload 后，生产者必须能完整退役两次发布。
        Check(f.scopes.Clear(f.data) == false && f.scopes.GetScopeCount() == 1,
              "postcommit scope remains recorded while its actual payload is held");
        Check(f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released
              && f.data.GetDataLifetime(publishedScope).status == DataLifetimeStatus::Releasing,
              "postcommit exception does not lose retirement ownership");
        node.reset();
        Check(f.scopes.Clear(f.data) && f.scopes.GetScopeCount() == 0,
              "postcommit owner ledger empties after the held payload ends");
    }
    {
        ScopeFixture f;
        const auto first = f.Publish("");
        f.data.throwsAfterCommit = true;
        for (int repeat = 0; repeat < 3; ++repeat) {
            bool original = false;
            try { (void)f.Publish(""); }
            catch (const std::runtime_error& error) { original = std::strcmp(error.what(), "after commit") == 0; }
            Check(original, "repeated committed exceptions preserve the original error");
        }
        const auto latest = f.data.committedScope;
        f.data.throwsAfterCommit = false;
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 1
              && f.data.GetDataLifetime(first.scope).status == DataLifetimeStatus::Released
              && f.data.GetDataLifetime(latest).status == DataLifetimeStatus::Published,
              "three committed exceptions replace the same unbound result family and collect to one scope");
        Check(f.scopes.Clear(f.data), "repeated committed exceptions clear every owned scope");
    }
    for (const auto query : {FaultDataPort::RecoveryQuery::Lifetime, FaultDataPort::RecoveryQuery::Graph,
                             FaultDataPort::RecoveryQuery::Binding}) {
        ScopeFixture f;
        f.data.throwsAfterCommit = true;
        f.data.recoveryQuery = query;
        bool original = false;
        try { (void)f.Publish(); }
        catch (const std::runtime_error& error) { original = std::strcmp(error.what(), "after commit") == 0; }
        Check(original, "a recovery query cannot replace the original postcommit error");
        Check(f.scopes.GetScopeCount() == 1, "a committed scope remains owned when its recovery query throws");
        bool cleared = false;
        try { cleared = f.scopes.Clear(f.data); } catch (...) {}
        Check(!cleared, "Clear cannot report success while publication confirmation is unavailable");
        const auto published = f.data.committedScope;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        f.data.throwsAfterCommit = false;
        Check(f.scopes.Clear(f.data) && f.scopes.GetScopeCount() == 0
              && f.data.GetDataLifetime(published).status == DataLifetimeStatus::Released,
              "retry releases the exact postcommit scope after recovery queries recover");
    }
    {
        ScopeFixture f;
        f.data.throwsBeforeCommit = true;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::Lifetime;
        bool original = false;
        try { (void)f.Publish(); } catch (const std::bad_alloc&) { original = true; } catch (...) {}
        Check(original, "unconfirmed precommit failure preserves its original allocation error");
        bool cleared = false;
        try { cleared = f.scopes.Clear(f.data); } catch (...) {}
        Check(!cleared, "an unconfirmed candidate cannot be silently forgotten by Clear");
        const auto unpublished = f.data.attemptedScope;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        f.data.throwsBeforeCommit = false;
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 0 && f.scopes.Clear(f.data)
              && f.data.GetDataLifetime(unpublished).status == DataLifetimeStatus::Unknown
              && f.data.unknownRetirements == 0,
              "a positively unpublished candidate is discarded without retiring an unknown lifetime");
    }
    {
        ScopeFixture f;
        const auto current = f.Publish();
        f.data.throwsAfterCommit = true;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::Binding;
        try { (void)f.Publish("scope.active", "calculate", {}, true); } catch (...) {}
        const auto historical = f.data.committedScope;
        f.data.throwsAfterCommit = false;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 2
              && f.data.GetDataBinding(f.data.GetDataGraph(), "scope.active")->target == current.ref
              && f.data.GetDataLifetime(current.scope).status == DataLifetimeStatus::Published
              && f.data.GetDataLifetime(historical).status == DataLifetimeStatus::Published,
              "uncertain historical publication never supersedes the current bound result");
        Check(f.Publish().status == DataCommitStatus::Succeeded && f.scopes.GetScopeCount() == 1,
              "the next confirmed current publication replaces its historical family after recovery");
        Check(f.scopes.Clear(f.data), "historical exception recovery clears");
    }
}

void ScopeBindingAndUnscopedRecovery() {
    for (const auto query : {FaultDataPort::RecoveryQuery::None, FaultDataPort::RecoveryQuery::Graph,
                             FaultDataPort::RecoveryQuery::Binding}) {
        ScopeFixture f;const auto first=f.Publish("");
        auto activate=f.Binding("scope.recovered",first.ref);
        f.data.throwsAfterCommit=true;f.data.throwsAfterCommitOnce=true;f.data.recoveryQuery=query;
        bool original=false;
        try{(void)f.scopes.Commit(f.data,std::move(activate));}
        catch(const std::runtime_error& error){original=std::strcmp(error.what(),"after commit")==0;}
        Check(original&&f.data.injectedCommits==1&&f.data.injectedStatus==DataCommitStatus::Succeeded
              &&f.data.injectedActivation,"unbound owned result is truly activated before one original port exception");
        if(query!=FaultDataPort::RecoveryQuery::None) {
            const auto calls=f.data.commitCalls;DataTransaction blocked;
            blocked.bindings.push_back({"scope.recovered",1,true,first.ref,{}});
            bool changed=false;
            try{changed=f.scopes.Commit(f.data,std::move(blocked)).status==DataCommitStatus::Succeeded;}catch(...){}
            const auto actual=f.data.TestDataPort::GetDataBinding(f.data.TestDataPort::GetDataGraph(),"scope.recovered");
            Check(!changed&&f.data.commitCalls==calls&&actual&&actual->target==first.ref,
                  "unresolved recovery cannot erase the sole real activation evidence through a later unbind");
        }
        f.data.recoveryQuery=FaultDataPort::RecoveryQuery::None;
        const auto binding=f.data.GetDataBinding(f.data.GetDataGraph(),"scope.recovered");
        if(binding&&binding->target)Check(f.scopes.Commit(f.data,f.Binding("scope.recovered",{})).status==DataCommitStatus::Succeeded,
                                       "activation recovery permits a normal unbind after query recovery");
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount()==0&&f.data.GetDataLifetime(first.scope).status==DataLifetimeStatus::Released,
              "exceptional first activation then ordinary deactivation collects the owned scope before Clear");
        Check(f.scopes.Clear(f.data),"binding recovery cleanup");
    }
    // 每个模式使用独立图；普通输入不继承 owned scope，同族身份由独立的 producer/operation/type 给出。
    enum class Mode { Normal, Committed, Before, Rejected, Historical, HistoricalBinding, OtherKey, ExistingRevision };
    for(const auto mode:{Mode::Normal,Mode::Committed,Mode::Before,Mode::Rejected,Mode::Historical,Mode::HistoricalBinding,Mode::OtherKey,Mode::ExistingRevision}) {
        ScopeFixture f;const auto first=f.Publish("");
        const DataRevisionRef ordinary{f.data.CreateDataEntityId(),1},output{f.data.CreateDataEntityId(),1};
        DataTransaction input;input.outputs.push_back({ordinary.entityId,0,DataTypes::recordTable,{},ScopeFixture::Payload(),{}});
        Check(f.data.SetDataCommit(std::move(input)).status==DataCommitStatus::Succeeded,"ordinary input publishes without an owner scope");
        if(mode==Mode::HistoricalBinding) {
            DataTransaction binding;binding.bindings.push_back({"ordinary.active",0,true,{},ordinary});
            Check(f.data.SetDataCommit(std::move(binding)).status==DataCommitStatus::Succeeded,"historical CAS control starts with its desired target already bound");
        }
        const auto transaction=[&] {
            DataTransaction tx;
            tx.outputs.push_back({output.entityId,0,DataTypes::recordTable,{{"source",ordinary}},ScopeFixture::Payload(),
                DataProvenance{"scope-test",mode==Mode::OtherKey?"different":"calculate","1","{}"}});
            if(mode==Mode::Historical) {
                tx.policy=DataPublishPolicy::AllowHistoricalResult;DataExpectation expected;
                expected.kind=DataExpectationKind::Binding;expected.use=DataExpectationUse::Activation;
                expected.binding="missing-activation";expected.expectedBindingRevision=1;tx.expectations.push_back(expected);
            }
            if(mode==Mode::HistoricalBinding) {
                tx.policy=DataPublishPolicy::AllowHistoricalResult;
                tx.bindings.push_back({"ordinary.active",0,true,{},ordinary});
            }
            return tx;
        };
        if(mode==Mode::ExistingRevision)Check(f.data.SetDataCommit(transaction()).status==DataCommitStatus::Succeeded,
                                            "preexisting exact revision control publishes outside this helper attempt");
        f.data.rejectsCommit=mode==Mode::Rejected;
        f.data.throwsBeforeCommit=mode==Mode::Before||mode==Mode::ExistingRevision;
        f.data.throwsAfterCommit=mode==Mode::Committed||mode==Mode::Historical||mode==Mode::HistoricalBinding||mode==Mode::OtherKey;
        f.data.throwsAfterCommitOnce=true;
        bool original=false;DataCommitResult returned;
        try{returned=f.scopes.Commit(f.data,transaction());}
        catch(const std::bad_alloc&){original=mode==Mode::Before||mode==Mode::ExistingRevision;}
        catch(const std::runtime_error& error){original=std::strcmp(error.what(),"after commit")==0;}
        f.data.throwsBeforeCommit=false;f.data.rejectsCommit=false;f.data.throwsAfterCommit=false;
        if(mode==Mode::Normal)Check(returned.status==DataCommitStatus::Succeeded,"normal ordinary replacement commits without an exception");
        else if(mode==Mode::Rejected)Check(returned.status==DataCommitStatus::Rejected,"ordinary replacement can be rejected before publication");
        else Check(original,"ordinary replacement fault preserves the original exception");
        const bool published=mode==Mode::Normal||mode==Mode::Committed||mode==Mode::Historical||mode==Mode::HistoricalBinding||mode==Mode::OtherKey||mode==Mode::ExistingRevision;
        Check(bool(f.data.GetData(f.data.GetDataGraph(),output))==published,"ordinary replacement exact output publication is independently observable");
        if(mode==Mode::Historical||mode==Mode::HistoricalBinding)Check(f.data.injectedStatus==DataCommitStatus::SucceededHistorical&&!f.data.injectedActivation,
                                      "historical replacement control truly publishes without activation");
        if(mode==Mode::Committed)Check(f.data.injectedStatus==DataCommitStatus::Succeeded&&f.data.injectedActivation,
                                     "ordinary same-family replacement truly commits and activates before the wrapper throws");
        f.scopes.Collect(f.data);
        const bool replaced=mode==Mode::Normal||mode==Mode::Committed;
        Check(f.scopes.GetScopeCount()==(replaced?std::size_t{0}:std::size_t{1})
              &&f.data.GetDataLifetime(first.scope).status==(replaced?DataLifetimeStatus::Released:DataLifetimeStatus::Published),
              replaced?"non-scoped same-family exceptional publication collects the previous owned result before Clear"
                      :"precommit rejection, history, other family and preexisting revision never falsely supersede an unbound owner");
        Check(f.scopes.Clear(f.data),"ordinary replacement recovery cleanup");
    }
    {
        Fixture f({}, {}, false,0,true);
        AlignmentResult produced;int starts=0;
        auto start=f.Request();start.isActivationRequested=false;
        Check(f.feature->SendRequest(start,[&](AlignmentResult value){produced=std::move(value);++starts;}).status==AlignmentAdmissionStatus::Accepted,
              "real Alignment accepts a never-activated owned result");f.Drain();
        Check(starts==1&&produced.status==AlignmentStatus::FullyDetermined&&!produced.isActivated
              &&GetDataRevisionRefValid(produced.result),"real Alignment publishes exactly once without activating");
        DataEntityId scope;
        {const auto node=f.data->GetData(f.data->GetDataGraph(),produced.result);
         Check(node&&node->lifetimePolicy==DataLifetimePolicy::OwnerRetirement,"real Alignment result owns its scoped lifetime");
         if(node)scope=node->lifetimeScope;}
        f.data->throwsAfterCommit=true;f.data->throwsAfterCommitOnce=true;
        f.data->afterCommitTarget=[ref=produced.result](const DataTransaction& tx){return tx.outputs.empty()&&tx.bindings.size()==1&&tx.bindings.front().target==ref;};
        AlignmentRequest activate;activate.action=AlignmentAction::Activate;activate.resultRef=produced.result;
        AlignmentResult activated;int activations=0;
        Check(f.feature->SendRequest(activate,[&](AlignmentResult value){activated=std::move(value);++activations;}).status==AlignmentAdmissionStatus::Accepted
              &&activations==1&&activated.status==AlignmentStatus::FullyDetermined&&activated.isActivated
              &&f.data->injectedCommits==1&&f.data->injectedStatus==DataCommitStatus::Succeeded&&f.data->injectedActivation,
              "real Alignment recovers one truly committed first activation and sends one successful completion");
        AlignmentRequest deactivate;deactivate.action=AlignmentAction::Deactivate;deactivate.input=f.input;
        AlignmentResult deactivated;int deactivations=0;
        Check(f.feature->SendRequest(deactivate,[&](AlignmentResult value){deactivated=std::move(value);++deactivations;}).status==AlignmentAdmissionStatus::Accepted
              &&deactivations==1&&deactivated.status==AlignmentStatus::FullyDetermined&&!f.feature->GetScopeState(f.input.scope).isCurrent,
              "real Alignment normally deactivates after the recovered first activation");
        f.feature->OnHostTick();
        Check(f.data->GetDataLifetime(scope).status==DataLifetimeStatus::Released,
              "real Alignment releases the previously unbound owned result after exceptional activation and normal deactivation");
    }
}

std::string GetScopeAliasRef(const std::optional<DataRevisionRef>& ref) {
    if (!ref) return "null";
    std::string text;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : ref->entityId.bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text + ":" + std::to_string(ref->generation);
}

// 真实端口提交与绑定时钟是独立预期；不将通用 helper 夹具冒称实际 Feature 入口。
void ScopeAliasOwnership() {
    {
        ScopeFixture f; const auto owned = f.Publish("scope.graph-retry");
        f.data.omitsGraph = true;
        f.scopes.Collect(f.data);
        f.data.omitsGraph = false;
        const auto binding = f.data.GetDataBinding(f.data.GetDataGraph(), "scope.graph-retry");
        Check(binding && binding->revision == 1 && binding->target == owned.ref,
              "unavailable graph fixture leaves the real producer binding unchanged");
        const bool cleared = f.scopes.Clear(f.data);
        std::cout << "SCOPE_ALIAS_EMPTY_GRAPH clear=" << cleared << " expected_clear=1 ledger="
                  << f.scopes.GetScopeCount() << " expected_ledger=0" << std::endl;
        Check(cleared && f.scopes.GetScopeCount() == 0
              && f.data.GetDataLifetime(owned.scope).status == DataLifetimeStatus::Released,
              "unavailable graph preserves known ownership evidence for a later successful Clear");
    }
    for (const auto query : {FaultDataPort::RecoveryQuery::None, FaultDataPort::RecoveryQuery::Graph,
                             FaultDataPort::RecoveryQuery::Binding}) {
        ScopeFixture f; const auto owned = f.Publish("");
        const std::string alias = "scope.external.alias";
        Check(f.data.SetDataCommit(f.Binding(alias, owned.ref)).status == DataCommitStatus::Succeeded,
              "external consumer creates its own alias through the real port");
        const auto before = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
        const auto graphBefore = f.data.GetDataGraph().commitId;
        const auto callsBefore = f.data.commitCalls;
        auto transaction = f.Binding(alias, owned.ref);
        f.data.throwsBeforeCommit = true; f.data.recoveryQuery = query;
        bool original = false; std::string message;
        try { (void)f.scopes.Commit(f.data, std::move(transaction)); }
        catch (const std::bad_alloc& error) { original = true; message = error.what(); }
        catch (const std::exception& error) { message = error.what(); }
        f.data.throwsBeforeCommit = false;
        const auto graph = f.data.TestDataPort::GetDataGraph();
        const auto unchanged = f.data.TestDataPort::GetDataBinding(graph, alias);
        Check(original && f.data.recoveryArmed && f.data.commitCalls == callsBefore + 1
              && graph.commitId == graphBefore && before && unchanged && before->revision == 1
              && unchanged->revision == before->revision && unchanged->target == before->target,
              "alias fixture preserves the original precommit bad_alloc and proves no underlying commit");
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        const bool cleared = f.scopes.Clear(f.data);
        const auto after = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
        const bool kept = after && after->revision == 1 && after->target == owned.ref;
        std::cout << "SCOPE_ALIAS_CLEAR query=" << int(query) << " original_bad_alloc=" << original
                  << " what=[" << message << "] before_revision=" << (before ? before->revision : 0)
                  << " after_revision=" << (after ? after->revision : 0) << " expected_revision=1"
                  << " after_target=" << GetScopeAliasRef(after ? after->target : std::optional<DataRevisionRef>{})
                  << " expected_target=" << GetScopeAliasRef(owned.ref)
                  << " clear=" << cleared << " expected_clear=0 kept=" << kept << " expected_kept=1" << std::endl;
        Check(kept && !cleared, "failed producer binding cannot acquire or clear an external alias");
        if (after && after->target) Check(f.data.SetDataCommit(f.Binding(alias, {})).status == DataCommitStatus::Succeeded,
                                         "external alias owner performs its own unbind");
        f.scopes.Collect(f.data);
        Check(f.scopes.GetScopeCount() == 1 && f.data.GetDataLifetime(owned.scope).status == DataLifetimeStatus::Published,
              "failed alias adoption cannot create activation history for a never-activated owner");
        Check(f.scopes.Clear(f.data), "owner can clear after the real external alias is released");
    }
    for (const bool fault : {false, true}) {
        ScopeFixture f; const auto owned = f.Publish("");
        const std::string alias = "scope.external.collect";
        Check(f.data.SetDataCommit(f.Binding(alias, owned.ref)).status == DataCommitStatus::Succeeded,
              "collect control externally binds a never-activated result");
        bool original = false; std::string message;
        if (fault) {
            f.data.throwsBeforeCommit = true;
            try { (void)f.scopes.Commit(f.data, f.Binding(alias, owned.ref)); }
            catch (const std::bad_alloc& error) { original = true; message = error.what(); }
            f.data.throwsBeforeCommit = false;
            Check(original, "collect control preserves the precommit exception");
        }
        Check(f.data.SetDataCommit(f.Binding(alias, {})).status == DataCommitStatus::Succeeded,
              "collect control externally unbinds without producer Clear");
        f.scopes.Collect(f.data);
        const auto status = f.data.GetDataLifetime(owned.scope).status;
        std::cout << "SCOPE_ALIAS_COLLECT fault=" << fault << " original_bad_alloc=" << original
                  << " what=[" << message << "] status=" << int(status) << " expected_status=" << int(DataLifetimeStatus::Published)
                  << " ledger=" << f.scopes.GetScopeCount() << " expected_ledger=1" << std::endl;
        Check(status == DataLifetimeStatus::Published && f.scopes.GetScopeCount() == 1,
              "foreign binding history does not supersede the never-activated owner after a failed attempt");
        Check(f.scopes.Clear(f.data), "collect control owner cleanup");
    }
    for (const bool collect : {false, true})
        for (const auto query : {FaultDataPort::RecoveryQuery::None, FaultDataPort::RecoveryQuery::Graph,
                                 FaultDataPort::RecoveryQuery::Binding}) {
            ScopeFixture f; const auto owned = f.Publish("");
            const std::string alias = "scope.external.pending";
            Check(f.data.SetDataCommit(f.Binding(alias, owned.ref)).status == DataCommitStatus::Succeeded,
                  "pending control starts with a real external alias");
            const auto before = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
            const auto beforeCommit = f.data.GetDataGraph().commitId;
            auto transaction = f.Binding(alias, owned.ref);
            f.data.throwsBeforeCommit = true; f.data.recoveryQuery = query;
            const auto receiptsBefore = f.data.receiptCalls;
            bool original = false; std::string message;
            try { (void)f.scopes.Commit(f.data, std::move(transaction)); }
            catch (const std::bad_alloc& error) { original = true; message = error.what(); }
            catch (const std::exception& error) { message = error.what(); }
            f.data.throwsBeforeCommit = false;
            const auto unchangedGraph = f.data.TestDataPort::GetDataGraph();
            const auto unchanged = f.data.TestDataPort::GetDataBinding(unchangedGraph, alias);
            Check(original && before && unchanged && before->revision == 1
                  && unchangedGraph.commitId == beforeCommit && unchanged->revision == before->revision
                  && unchanged->target == before->target && f.data.receiptCalls == receiptsBefore,
                  "pending fixture proves the original bad_alloc precedes any underlying commit");
            if (!before || !unchanged) continue;
            Check(query != FaultDataPort::RecoveryQuery::Graph || f.data.recoveryGraphFaults > 0,
                  "pending fixture actually hits the delayed graph recovery failure");
            Check(query != FaultDataPort::RecoveryQuery::Binding || f.data.recoveryBindingFaults > 0,
                  "pending fixture actually hits the delayed binding recovery failure");
            DataTransaction outside;
            outside.bindings.push_back({alias, unchanged->revision, true, unchanged->target, owned.ref});
            const auto externalCommit = f.data.TestDataPort::SetDataCommit(std::move(outside));
            const auto external = f.data.TestDataPort::GetDataBinding(f.data.TestDataPort::GetDataGraph(), alias);
            Check(externalCommit.status == DataCommitStatus::Succeeded && external
                  && external->revision == 2 && external->target == owned.ref,
                  "pending fixture's exact predicted r2 is written by the independent external owner");
            if (externalCommit.status != DataCommitStatus::Succeeded || !external) continue;
            f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
            if (!collect) {
                const bool cleared = f.scopes.Clear(f.data);
                const auto after = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
                const bool kept = after && after->revision == 2 && after->target == owned.ref;
                std::cout << "SCOPE_ALIAS_PENDING_CLEAR query=" << int(query) << " original_bad_alloc=" << original
                          << " what=[" << message << "] graph_fault_hits=" << f.data.recoveryGraphFaults
                          << " binding_fault_hits=" << f.data.recoveryBindingFaults
                          << " external_revision=" << (external ? external->revision : 0)
                          << " after_revision=" << (after ? after->revision : 0) << " expected_revision=2"
                          << " after_target=" << GetScopeAliasRef(after ? after->target : std::optional<DataRevisionRef>{})
                          << " expected_target=" << GetScopeAliasRef(owned.ref) << " clear=" << cleared
                          << " expected_clear=0 kept=" << kept << " expected_kept=1" << std::endl;
                Check(!cleared && kept, "pending recovery never acquires or clears an external successor write");
                if (after && after->target)
                    Check(f.data.SetDataCommit(f.Binding(alias, {})).status == DataCommitStatus::Succeeded,
                          "pending alias owner independently unbinds after the Clear observation");
            } else {
                f.scopes.Collect(f.data);
                Check(f.data.SetDataCommit(f.Binding(alias, {})).status == DataCommitStatus::Succeeded,
                      "pending alias owner independently unbinds after recovery Collect");
                f.scopes.Collect(f.data);
                const auto status = f.data.GetDataLifetime(owned.scope).status;
                std::cout << "SCOPE_ALIAS_PENDING_COLLECT query=" << int(query) << " original_bad_alloc=" << original
                          << " what=[" << message << "] graph_fault_hits=" << f.data.recoveryGraphFaults
                          << " binding_fault_hits=" << f.data.recoveryBindingFaults << " status=" << int(status)
                          << " expected_status=" << int(DataLifetimeStatus::Published)
                          << " ledger=" << f.scopes.GetScopeCount() << " expected_ledger=1" << std::endl;
                Check(status == DataLifetimeStatus::Published && f.scopes.GetScopeCount() == 1,
                      "pending recovery never invents activation history from the external successor");
            }
            Check(f.scopes.Clear(f.data), "pending alias control owner cleanup");
        }
    enum class Control { Normal, After, OwnedBefore, ExternalRewrite };
    for (const auto mode : {Control::Normal, Control::After, Control::OwnedBefore, Control::ExternalRewrite}) {
        ScopeFixture f; const std::string alias = "scope.ownership.control";
        const bool wasOwned = mode == Control::OwnedBefore || mode == Control::ExternalRewrite;
        const auto owned = f.Publish(wasOwned ? alias : "");
        if (!wasOwned || mode == Control::ExternalRewrite)
            Check(f.data.SetDataCommit(f.Binding(alias, owned.ref)).status == DataCommitStatus::Succeeded,
                  "ownership control has a real external alias write");
        if (mode != Control::ExternalRewrite) {
            auto transaction = f.Binding(alias, owned.ref);
            f.data.throwsBeforeCommit = mode == Control::OwnedBefore;
            f.data.throwsAfterCommit = mode == Control::After; f.data.throwsAfterCommitOnce = true;
            bool original = false; DataCommitResult result;
            try { result = f.scopes.Commit(f.data, std::move(transaction)); }
            catch (const std::bad_alloc&) { original = mode == Control::OwnedBefore; }
            catch (const std::runtime_error& error) { original = mode == Control::After && std::strcmp(error.what(), "after commit") == 0; }
            f.data.throwsBeforeCommit = false; f.data.throwsAfterCommit = false;
            Check(mode == Control::Normal ? result.status == DataCommitStatus::Succeeded : original,
                  "ownership control observes true success or the original phase-specific exception");
        }
        const bool cleared = f.scopes.Clear(f.data);
        const auto after = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
        const bool shouldClear = mode != Control::ExternalRewrite;
        std::cout << "SCOPE_ALIAS_CONTROL mode=" << int(mode) << " clear=" << cleared << " expected_clear=" << shouldClear
                  << " revision=" << (after ? after->revision : 0)
                  << " target=" << GetScopeAliasRef(after ? after->target : std::optional<DataRevisionRef>{}) << std::endl;
        Check(cleared == shouldClear && after && (shouldClear ? !after->target : after->target == owned.ref),
              "real producer writes clean up once, prior ownership survives failure, and later external writes remain external");
        if (!shouldClear) {
            Check(f.data.SetDataCommit(f.Binding(alias, {})).status == DataCommitStatus::Succeeded,
                  "later external owner releases its binding");
            f.scopes.Collect(f.data);
            Check(f.scopes.Clear(f.data), "later external rewrite cleanup");
        }
    }
}

void ScopeAliasReceipts() {
    const auto allocationCount = AlignmentReceiptAllocations::count;
    AlignmentReceiptAllocations::enabled = true;
    auto* calibration = ::operator new(1);
    AlignmentReceiptAllocations::enabled = false;
    ::operator delete(calibration);
    Check(AlignmentReceiptAllocations::count == allocationCount + 1,
          "receipt allocation monitor detects an explicit ordinary C++ allocation");
    enum class Mode { Observed, Delayed, Refused, Throwing };
    for (const auto mode : {Mode::Observed, Mode::Delayed, Mode::Refused, Mode::Throwing}) {
        ScopeFixture f; const auto owned = f.Publish("");
        const std::string alias = "scope.receipt.control";
        auto delayed = mode == Mode::Delayed ? f.data.StartDataChanges() : std::unique_ptr<DataChangeBatch>{};
        f.data.rejectsChangeObservation = mode == Mode::Refused;
        const auto receiptsBefore = f.data.receiptCalls;
        std::size_t notifications = 0;
        bool receiptPrecedesNotice = true;
        const auto observer = f.data.AttachDataChange([&](const DataChangeSet&) {
            ++notifications;
            receiptPrecedesNotice = receiptPrecedesNotice && f.data.receiptCalls == receiptsBefore + 1;
            if (mode == Mode::Throwing) throw std::runtime_error("observer after receipt");
        });
        f.data.throwsAfterCommit = true; f.data.throwsAfterCommitOnce = true;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::Graph;
        bool original = false;
        try { (void)f.scopes.Commit(f.data, f.Binding(alias, owned.ref)); }
        catch (const std::runtime_error& error) { original = std::strcmp(error.what(), "after commit") == 0; }
        const auto actual = f.data.TestDataPort::GetDataBinding(f.data.TestDataPort::GetDataGraph(), alias);
        Check(original && f.data.injectedCommits == 1 && f.data.injectedStatus == DataCommitStatus::Succeeded
              && f.data.injectedActivation && actual && actual->revision == 1 && actual->target == owned.ref,
              "receipt control truly commits first activation before the original exception and query failure");
        Check(f.data.recoveryGraphFaults > 0 && f.data.receiptCalls == receiptsBefore + 1
              && f.data.receiptAllocations == 0 && f.data.receiptQueries == 0 && f.data.receiptOnOwner,
              "one real owner-thread receipt records recovery without allocation or data queries");
        if (mode == Mode::Delayed) Check(notifications == 0,
              "an outer notification batch does not delay the actual synchronous receipt");
        delayed.reset();
        Check(receiptPrecedesNotice && notifications == (observer ? 1u : 0u),
              "actual receipt precedes every delivered notice, including an observer that throws");
        if (observer) Check(f.data.DetachDataChange(observer), "receipt fixture releases its own observer");
        Check(f.data.activeDataObservers == 0, "the helper never requires an observation registration");
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        const bool cleared = f.scopes.Clear(f.data);
        const auto after = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
        std::cout << "SCOPE_RECEIPT mode=" << int(mode) << " original_after_commit=" << original
                  << " observer_callbacks=" << f.data.observedDataChanges
                  << " active_observers=" << f.data.activeDataObservers << " clear=" << cleared
                  << " expected_clear=1 revision=" << (after ? after->revision : 0)
                  << " target=" << GetScopeAliasRef(after ? after->target : std::optional<DataRevisionRef>{})
                  << " receipt_delta=" << f.data.receiptCalls - receiptsBefore << " expected_receipts=1"
                  << " receipt_allocations=" << f.data.receiptAllocations << " receipt_queries=" << f.data.receiptQueries
                  << " owner_thread=" << f.data.receiptOnOwner << std::endl;
        Check(cleared && after && !after->target && f.scopes.GetScopeCount() == 0
              && f.data.GetDataLifetime(owned.scope).status == DataLifetimeStatus::Released,
              "actual receipt preserves automatic cleanup even without notices or after observer exceptions");
        Check(f.scopes.Clear(f.data), "receipt control cleanup");
    }
    // 原子事务按实体/绑定名字识别；真实回执重排集合不改变其中的提交身份。
    for (const bool multipleOutputs : {false, true}) {
        ScopeFixture f; const auto owned = f.Publish("");
        DataTransaction transaction;
        DataRevisionRef expectedFirst;
        if (multipleOutputs) {
            const DataRevisionRef first{f.data.CreateDataEntityId(), 1}, second{f.data.CreateDataEntityId(), 1};
            for (const auto ref : {first, second})
                transaction.outputs.push_back({ref.entityId, 0, DataTypes::recordTable, {{"source", f.source}},
                    ScopeFixture::Payload(), DataProvenance{"scope-test", "calculate", "1", "{}"}});
            expectedFirst = second;
        } else {
            const auto second = f.Publish("", "other");
            transaction.bindings.push_back({"scope.receipt.order.first", 0, true, {}, owned.ref});
            transaction.bindings.push_back({"scope.receipt.order.second", 0, true, {}, second.ref});
            expectedFirst = second.ref;
        }
        const auto receiptsBefore = f.data.receiptCalls;
        f.data.reversesCommitOrder = true;
        f.data.throwsAfterCommit = true; f.data.throwsAfterCommitOnce = true;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::Graph;
        bool original = false;
        try { (void)f.scopes.Commit(f.data, std::move(transaction)); }
        catch (const std::runtime_error& error) { original = std::strcmp(error.what(), "after commit") == 0; }
        f.data.reversesCommitOrder = false;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        Check(original && f.data.injectedCommits == 1 && f.data.injectedStatus == DataCommitStatus::Succeeded
              && f.data.injectedActivation && f.data.receiptCalls == receiptsBefore + 1
              && (multipleOutputs ? f.data.receiptFirstOutput == expectedFirst : f.data.receiptFirstTarget == expectedFirst),
              "reordered receipt fixture proves one actual successful commit, reversed identity order, and original exception");
        if (multipleOutputs) {
            f.scopes.Collect(f.data);
            const auto state = f.data.GetDataLifetime(owned.scope).status;
            std::cout << "SCOPE_RECEIPT_OUTPUT_ORDER old_status=" << int(state)
                      << " expected_status=" << int(DataLifetimeStatus::Released)
                      << " ledger=" << f.scopes.GetScopeCount() << " expected_ledger=1" << std::endl;
            Check(state == DataLifetimeStatus::Released && f.scopes.GetScopeCount() == 1,
                  "actual same-family multi-output receipt replaces the prior owner independent of vector order");
        } else {
            const bool cleared = f.scopes.Clear(f.data);
            std::cout << "SCOPE_RECEIPT_BINDING_ORDER clear=" << cleared << " expected_clear=1"
                      << " ledger=" << f.scopes.GetScopeCount() << " expected_ledger=0" << std::endl;
            Check(cleared && f.scopes.GetScopeCount() == 0,
                  "actual multi-binding receipt authorizes only its real owned clocks independent of vector order");
            if (!cleared) {
                (void)f.data.SetDataCommit(f.Binding("scope.receipt.order.first", {}));
                (void)f.data.SetDataCommit(f.Binding("scope.receipt.order.second", {}));
            }
        }
        Check(f.data.receiptAllocations == 0 && f.data.receiptQueries == 0 && f.data.receiptOnOwner,
              "multi-entry receipt matching performs no allocation/query and stays on the owner");
        Check(f.scopes.Clear(f.data), "reordered receipt fixture cleanup");
    }
    // 同一同步调用内仍可能经端口/准备钩子重入；事件值与抵达时间都不是作者身份。
    for (const bool collect : {false, true}) {
        ScopeFixture f; const auto owned = f.Publish("");
        const std::string alias = "scope.receipt.reentrant";
        Check(f.data.SetDataCommit(f.Binding(alias, owned.ref)).status == DataCommitStatus::Succeeded,
              "reentrant receipt control starts with an external alias at r1");
        const auto before = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
        auto attempted = f.Binding(alias, owned.ref);
        const auto originalStoreCalls = f.data.underlyingCommitCalls;
        const auto originalReceipts = f.data.receiptCalls;
        DataCommitResult outside;
        f.data.beforeCommitFault = [&] {
            DataTransaction transaction;
            transaction.bindings.push_back({alias, 1, true, owned.ref, owned.ref});
            outside = f.data.TestDataPort::SetDataCommit(std::move(transaction));
        };
        f.data.throwsBeforeCommit = true;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::Graph;
        bool original = false;
        try { (void)f.scopes.Commit(f.data, std::move(attempted)); }
        catch (const std::bad_alloc&) { original = true; }
        catch (...) {}
        f.data.beforeCommitFault = {};
        f.data.throwsBeforeCommit = false;
        f.data.recoveryQuery = FaultDataPort::RecoveryQuery::None;
        const auto external = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
        const auto attemptedStoreCalls = f.data.underlyingCommitCalls - originalStoreCalls;
        Check(original && before && before->revision == 1 && outside.status == DataCommitStatus::Succeeded
              && outside.bindings.size() == 1 && outside.bindings.front().revision == 2
              && attemptedStoreCalls == 0 && f.data.receiptCalls == originalReceipts && external
              && external->revision == 2 && external->target == owned.ref,
              "reentrant fixture proves only the external transaction reached the real store");
        if (!collect) {
            const bool cleared = f.scopes.Clear(f.data);
            const auto after = f.data.GetDataBinding(f.data.GetDataGraph(), alias);
            std::cout << "SCOPE_RECEIPT_REENTRANT_CLEAR original_store_calls_delta="
                      << attemptedStoreCalls
                      << " external_returned_revision=" << (outside.bindings.empty() ? 0 : outside.bindings.front().revision)
                      << " observer_callbacks=" << f.data.observedDataChanges << " clear=" << cleared
                      << " expected_clear=0 after_revision=" << (after ? after->revision : 0)
                      << " expected_revision=2 after_target=" << GetScopeAliasRef(after ? after->target : std::optional<DataRevisionRef>{})
                      << " expected_target=" << GetScopeAliasRef(owned.ref) << std::endl;
            Check(!cleared && after && after->revision == 2 && after->target == owned.ref,
                  "an in-call matching event from reentrant external work never authorizes Clear");
            if (after && after->target) (void)f.data.SetDataCommit(f.Binding(alias, {}));
        } else {
            f.scopes.Collect(f.data);
            Check(f.data.SetDataCommit(f.Binding(alias, {})).status == DataCommitStatus::Succeeded,
                  "reentrant receipt external owner independently unbinds");
            f.scopes.Collect(f.data);
            const auto state = f.data.GetDataLifetime(owned.scope).status;
            std::cout << "SCOPE_RECEIPT_REENTRANT_COLLECT state=" << int(state)
                      << " expected_state=" << int(DataLifetimeStatus::Published)
                      << " ledger=" << f.scopes.GetScopeCount() << " expected_ledger=1" << std::endl;
            Check(state == DataLifetimeStatus::Published && f.scopes.GetScopeCount() == 1,
                  "an in-call matching event from reentrant external work never proves producer activation history");
        }
        Check(f.scopes.Clear(f.data), "reentrant receipt control cleanup");
    }
}

// 相同临时输入的公开 Start 请求没有次数限制；1025 是被审计门槛的首个越界请求。
void OwnerScopedRepeatedAlignment() {
    Fixture f({}, {}, false, 0, true);
    const auto source = f.data->GetData(f.data->GetDataGraph(), f.input.source);
    const auto recipe = f.data->GetData(f.data->GetDataGraph(), f.recipeRef);
    Check(source && GetDataEntityIdValid(source->lifetimeScope)
              && source->lifetimePolicy == DataLifetimePolicy::OwnerRetirement,
          "repeated alignment uses the same owner-retired source revision");
    Check(recipe && !GetDataEntityIdValid(recipe->lifetimeScope),
          "fixture recipe does not consume a scoped result submission");
    std::size_t succeeded = 0, firstFailure = 0;
    std::vector<DataEntityId> scopes;
    for (std::size_t submission = 1; submission <= ownerScopeSubmissions; ++submission) {
        const auto before = f.feature->GetState().activeResult;
        const auto commitsBefore = f.data->commitCalls;
        const auto result = f.Solve();
        const bool published = result.status == AlignmentStatus::FullyDetermined
            && GetDataRevisionRefValid(result.result) && result.isActivated;
        if (published) {
            ++succeeded;
            const auto node = f.data->GetData(f.data->GetDataGraph(), result.result);
            Check(node && node->lifetimePolicy == DataLifetimePolicy::OwnerRetirement
                      && GetDataEntityIdValid(node->lifetimeScope),
                  "published alignment result retains its temporary-input lifetime");
            if (node) scopes.push_back(node->lifetimeScope);
            if (scopes.size() > 1)
                Check(f.data->GetDataLifetime(scopes[scopes.size() - 2]).status != DataLifetimeStatus::Published,
                      "replaced public alignment result begins retirement before another request");
        } else {
            if (!firstFailure) firstFailure = submission;
            std::cout << "OWNER_SCOPE_FAILURE submission=" << submission
                      << " status=" << static_cast<int>(result.status)
                      << " data_commit_calls=" << f.data->commitCalls - commitsBefore
                      << " old_active_preserved=" << (f.feature->GetState().activeResult == before)
                      << '\n';
        }
        Check(published, "each accepted alignment including submission 1025 must publish");
    }
    std::cout << "OWNER_SCOPE_SUMMARY attempted=1025 succeeded=" << succeeded
              << " first_failure=" << firstFailure << '\n';
    Check(succeeded == 1025 && firstFailure == 0,
          "temporary source must not impose an undocumented cumulative request limit");
    Check(f.feature->DetachHost(), "detach clears scoped alignment publications");
    for (const auto& scope : scopes)
        Check(f.data->GetDataLifetime(scope).status == DataLifetimeStatus::Released,
              "every replaced public alignment scope releases after detach");
    HostFeatureContext context; context.data = f.data;
    Check(f.feature->AttachHost(context), "same feature can reattach after scope cleanup");
    const auto recovered = f.Solve();
    Check(recovered.status == AlignmentStatus::FullyDetermined && GetDataRevisionRefValid(recovered.result),
          "same input publishes again after explicit detach cleanup");
    std::cout << "OWNER_SCOPE_RECOVERY after_detach="
              << (recovered.status == AlignmentStatus::FullyDetermined) << '\n';
}

void DeepSourceGraph() {
    Fixture f({}, {},false,1100);
    const auto result=f.Solve();
    Check(result.status==AlignmentStatus::FullyDetermined,"source graphs beyond 1024 nodes remain valid alignment inputs");
    const auto archive=f.feature->GetArchive(result.result);
    AlignmentRequest restore;restore.action=AlignmentAction::Restore;restore.archive=archive;
    restore.input=f.input;restore.restoredNominal=f.recipe.nominalData;
    AlignmentResult restored;
    Check(archive&&f.feature->SendRequest(restore,[&](auto value){restored=std::move(value);}).status==AlignmentAdmissionStatus::Accepted
        &&GetDataRevisionRefValid(restored.recipe),"archive restore traverses the complete deep source graph");
}

void Lifecycle() {
    Fixture f;
    const auto contract = f.feature->GetDataContract();
    Check(contract.inputs.size() == 4 && contract.outputs.size() == 10,
        "feature declares recipe/mesh inputs and all formal output roles");
    const auto result = f.Solve();
    Check(result.status == AlignmentStatus::FullyDetermined && result.isActivated,
          "valid result activated without view");
    Check(!result.isDisplayReady, "no view does not imply display success");
    Check(f.feature->GetState().isCurrent, "current result");
    const auto graph = f.data->GetDataGraph();
    const auto stored = AlignmentData::GetResult(graph, result.result);
    Check(bool(stored), "typed result readable");
    if (stored) {
        Check(graph.view->GetData(stored->record.transform) &&
                  graph.view->GetData(stored->record.geometry) &&
                  graph.view->GetData(stored->record.residuals),
              "all result outputs published");
    }
    const auto archive = f.feature->GetArchive(result.result);
    Check(archive && std::abs(archive->sourceToTarget[3] - 0.1) < 1e-6,
          "archive derives sole transform");
    const auto before = f.feature->GetState().activeResult;
    f.data->rejectsCommit = true;
    const auto failed = f.Solve();
    Check(failed.status == AlignmentStatus::InternalError &&
              !GetDataRevisionRefValid(failed.result),
          "commit rejection has no result");
    Check(f.feature->GetState().activeResult == before, "failed request retains old active");
    f.data->rejectsCommit = false;
    int callbackCount = 0;
    f.feature->SendRequest(f.Request(), [&](AlignmentResult r) {
        ++callbackCount;
        Check(r.status == AlignmentStatus::Cancelled, "explicit cancellation status");
    });
    AlignmentRequest cancel;
    cancel.action = AlignmentAction::Cancel;
    Check(f.feature->SendRequest(cancel).status == AlignmentAdmissionStatus::Accepted,
          "cancel accepted");
    f.Drain();
    Check(callbackCount == 1, "cancel one completion");
    int detachedCount = 0;
    f.feature->SendRequest(f.Request(), [&](AlignmentResult r) {
        ++detachedCount;
        Check(r.status == AlignmentStatus::Cancelled, "detach cancellation");
    });
    Check(!f.feature->DetachHost(), "detach pending worker preserves lifetime");
    f.Drain();
    Check(detachedCount == 1 && f.feature->DetachHost(), "detach retries after terminal callback");
}
void Currentness() {
    Fixture f;
    const auto first = f.Solve();
    AlignmentResult late;
    f.feature->SendRequest(f.Request(), [&](AlignmentResult r) { late = std::move(r); });
    const auto graph = f.data->GetDataGraph();
    const auto binding = graph.view->GetDataBinding(f.input.sourceBinding);
    const auto old = graph.view->GetData(f.input.source);
    DataTransaction tx;
    tx.outputs.push_back({f.input.source.entityId, 1, old->type, {}, old->payload, {}});
    tx.bindings.push_back({binding->name, binding->revision, true, binding->target,
                           DataRevisionRef{f.input.source.entityId, 2}});
    Check(f.data->SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded,
          "source revision advanced");
    Check(!f.feature->GetState().isCurrent, "source change visible before tick");
    f.Drain();
    Check(late.status == AlignmentStatus::Stale && !late.isActivated,
          "late source result rejected");
    AlignmentRequest activate;
    activate.action = AlignmentAction::Activate;
    activate.resultRef = first.result;
    AlignmentResult activation;
    f.feature->SendRequest(activate, [&](AlignmentResult r) { activation = r; });
    Check(activation.status == AlignmentStatus::Stale, "old result cannot reactivate");
    Check(bool(f.feature->GetArchive(first.result)), "historical result remains exportable");
}
void Display() {
    auto views = std::make_shared<Views>();
    Fixture f(views);
    auto result = f.Solve();
    Check(result.isDisplayReady && views->overlay->overlays.size() == 1, "one independent overlay");
    const auto commitId = f.data->GetDataGraph().commitId;
    AlignmentRequest visibility;
    visibility.action = AlignmentAction::SetVisibility;
    visibility.isVisible = false;
    f.feature->SendRequest(visibility);
    Check(views->overlay->overlays.empty() && f.data->GetDataGraph().commitId == commitId,
          "visibility does not publish a measurement revision");
    visibility.isVisible = true;
    views->overlay->rejectsAttach = true;
    f.feature->SendRequest(visibility);
    Check(views->overlay->overlays.empty() && f.feature->GetState().isCurrent &&
              !f.feature->GetState().isDisplayReady,
          "display failure preserves measurement and cleans partial attach");
    AlignmentMatrix pose = alignmentIdentity;
    pose[3] = 10;
    const auto poly = AlignmentOverlay::BuildData(pose, {}, f.recipe, 1);
    double p[3]{};
    poly->GetPoint(0, p);
    Check(std::abs(p[0] + 10) < 1e-12, "overlay target origin mapped to source by inverse");
    std::vector<AlignmentGeometry> geometries(3);
    for(std::size_t index=0;index<geometries.size();++index) {
        geometries[index].kind=AlignmentGeometryKind::Point;
        geometries[index].sourceCenter={double(index),double(index+1),double(index+2)};
    }
    AlignmentRecipe emptyRecipe;
    const auto markers=AlignmentOverlay::BuildData(alignmentIdentity,geometries,emptyRecipe,1);
    Check(markers && markers->GetNumberOfVerts()==3 && markers->GetNumberOfLines()==3,
        "alignment keeps one coordinate triad and point markers instead of per-datum crosses");
    for(std::size_t index=0;index<geometries.size();++index) {
        markers->GetPoint(6+static_cast<vtkIdType>(index),p);
        Check(std::equal(p,p+3,geometries[index].sourceCenter.begin()),"datum marker retains exact fitted center");
    }
    AlignmentRecipe bestFit; bestFit.method=AlignmentMethod::ConstrainedBestFit;
    AlignmentFitPair pair; pair.vertexId=0; pair.nominalPoint={2,3,4}; bestFit.fitPairs={pair};
    const MeshReadArray<double> modelVertices(std::vector<double>{1,2,3});
    const auto pairs=AlignmentOverlay::BuildData(alignmentIdentity,{},bestFit,1,&modelVertices);
    Check(pairs && pairs->GetNumberOfVerts()==1 && pairs->GetNumberOfLines()==4,
        "best-fit inputs have a position marker and actual correspondence line with one coordinate triad");
    pairs->GetPoint(6,p);
    Check(std::equal(p,p+3,modelVertices.begin()),"best-fit marker uses exact published mesh coordinates");
    std::vector<AlignmentGeometry> holes(3);
    holes[0].kind=AlignmentGeometryKind::Plane;holes[0].sourceDirection={0,0,1};
    for(std::size_t index=1;index<holes.size();++index) {
        holes[index].kind=AlignmentGeometryKind::Cylinder;
        holes[index].sourceCenter={double(index*3),2,4};holes[index].sourceDirection={0,0,1};
        holes[index].radius=double(index)*.75;
    }
    const auto cylinders=AlignmentOverlay::BuildData(alignmentIdentity,holes,emptyRecipe,10);
    Check(cylinders && cylinders->GetNumberOfLines()==397 && cylinders->GetNumberOfVerts()==0,
        "plane and two fitted cylinders retain three rings, generators and one axis per cylinder with a single triad");
    cylinders->GetPoint(14,p);
    Check(std::abs(std::hypot(p[0]-holes[1].sourceCenter[0],p[1]-holes[1].sourceCenter[1])-holes[1].radius)<1e-12
        && p[2]==holes[1].sourceCenter[2],
        "cylinder display consumes the fitted center and radius without refitting the measurement");
}
void Reentry() {
    Fixture f;
    bool rejected = false, detachRejected = false;
    f.data->onCommit = [&] {
        rejected = f.feature->SendRequest(f.Request()).status == AlignmentAdmissionStatus::Busy;
        detachRejected = !f.feature->DetachHost();
    };
    auto r = f.Solve();
    Check(rejected && detachRejected && r.isActivated,
          "commit observer reentry rejected by feature");
    f.data->onCommit = {};
    int first = 0, second = 0;
    f.feature->SendRequest(f.Request(), [&](AlignmentResult) {
        ++first;
        Check(f.feature->SendRequest(f.Request(), [&](AlignmentResult) { ++second; }).status ==
                  AlignmentAdmissionStatus::Accepted,
              "completion callback may start next request");
        throw std::runtime_error("callback exception");
    });
    f.Drain();
    Check(first == 1 && second == 1, "reentrant callbacks exactly once despite exception");
}
void ScopeRestoreAndCommitFault() {
    Fixture f;
    const auto first = f.Solve();
    const auto snapshot = f.feature->GetResult(first.result);
    Check(snapshot && snapshot->geometries.size() == 4 && snapshot->residuals.size() == 12,
          "public snapshot exposes fitted geometry and directional residuals");
    auto secondInput = f.input;
    secondInput.scope = "part-2";
    secondInput.scopeData = {f.data->CreateDataEntityId(), 1};
    const auto oldScope = f.data->GetDataGraph().view->GetData(f.input.scopeData);
    DataTransaction addScope;
    addScope.outputs.push_back(
        {secondInput.scopeData.entityId, 0, oldScope->type, {}, oldScope->payload, {}});
    Check(f.data->SetDataCommit(std::move(addScope)).status == DataCommitStatus::Succeeded,
          "second scope created");
    auto request = f.Request();
    request.input = secondInput;
    AlignmentResult second;
    f.feature->SendRequest(request, [&](AlignmentResult result) { second = std::move(result); });
    f.Drain();
    Check(second.isActivated && f.feature->GetScopeState("part-1").activeResult == first.result &&
              f.feature->GetScopeState("part-2").activeResult == second.result,
          "scope bindings are independent");
    DataTransaction advanceScope;
    advanceScope.outputs.push_back(
        {f.input.scopeData.entityId, 1, oldScope->type, {}, oldScope->payload, {}});
    f.data->SetDataCommit(std::move(advanceScope));
    f.feature->OnHostTick();
    Check(!f.feature->GetScopeState("part-1").isCurrent &&
              f.feature->GetScopeState("part-2").isCurrent,
          "one stale scope does not invalidate another");
    const auto archive = f.feature->GetArchive(second.result);
    AlignmentRequest restore;
    restore.action = AlignmentAction::Restore;
    restore.archive = archive;
    restore.input = secondInput;
    restore.restoredNominal = f.recipe.nominalData;
    for (const auto* version : {"metrology-alignment-1", "metrology-alignment-2"}) {
        auto obsolete = restore;
        if (obsolete.archive) obsolete.archive->algorithmVersion = version;
        Check(f.feature->SendRequest(obsolete).status == AlignmentAdmissionStatus::InvalidRequest,
              "restore rejects obsolete versions even without new optional parameters");
    }
    AlignmentResult restored;
    Check(f.feature->SendRequest(restore, [&](AlignmentResult result) { restored = result; })
                      .status == AlignmentAdmissionStatus::Accepted &&
              GetDataRevisionRefValid(restored.recipe) && restored.recipe != f.recipeRef,
          "restore validates explicit source and nominal revision mapping into a new recipe");
    Check(f.feature->GetScopeState("part-2").activeResult == second.result,
          "restore does not activate archive matrix");
    restore.input.reset();
    Check(f.feature->SendRequest(restore).status == AlignmentAdmissionStatus::InvalidRequest,
          "restore requires explicit mapping");
    f.input = secondInput;
    f.data->throwsAfterCommit = true;
    const auto recovered = f.Solve();
    Check(GetDataRevisionRefValid(recovered.result) && recovered.isActivated &&
              f.feature->GetState().isCurrent,
          "post-commit exception cannot erase committed activation");
    AlignmentRequest save;
    save.action = AlignmentAction::SaveRecipe;
    save.recipe = f.recipe;
    AlignmentResult saved;
    f.feature->SendRequest(save, [&](AlignmentResult value) { saved = value; });
    Check(saved.status == AlignmentStatus::FullyDetermined && GetDataRevisionRefValid(saved.recipe),
          "recipe save recovers a post-commit exception");
    AlignmentRequest deactivate;
    deactivate.action = AlignmentAction::Deactivate;
    deactivate.input = f.input;
    AlignmentResult deactivated;
    f.feature->SendRequest(deactivate, [&](AlignmentResult value) { deactivated = value; });
    Check(deactivated.status == AlignmentStatus::FullyDetermined &&
              !f.feature->GetScopeState("part-2").isCurrent,
          "deactivation recovers a post-commit exception");
    AlignmentRequest activate;
    activate.action = AlignmentAction::Activate;
    activate.resultRef = recovered.result;
    AlignmentResult activated;
    f.feature->SendRequest(activate, [&](AlignmentResult value) { activated = value; });
    Check(activated.isActivated && f.feature->GetScopeState("part-2").isCurrent,
          "explicit activation recovers a post-commit exception");
    f.data->throwsAfterCommit = false;
    int completions = 0;
    f.feature->SendRequest(f.Request(), [&](AlignmentResult) { ++completions; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool detached = false;
    while (!detached && std::chrono::steady_clock::now() < deadline) {
        detached = f.feature->DetachHost();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(detached && completions == 1,
          "Detach retries consume completed worker without external tick");
}
void WorkNotifications() {
    auto host = std::make_shared<WorkControl>();
    Fixture f({}, host);
    const auto before = host->notifications.load();
    int completed = 0;
    Check(f.feature->SendRequest(f.Request(), [&](AlignmentResult) { ++completed; }).status
        == AlignmentAdmissionStatus::Accepted, "notification request accepted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (host->notifications.load() == before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(host->notifications.load() > before && completed == 0 && f.feature->GetState().isBusy,
        "worker wakes owner after result publication without consuming completion");
    f.feature->OnHostTick();
    Check(completed == 1 && !f.feature->GetState().isBusy,
        "one notified owner tick consumes the finished task");
    const auto binding = f.data->GetDataBinding(f.data->GetDataGraph(), f.input.sourceBinding);
    const auto changesBefore = host->notifications.load();
    DataTransaction change;
    if (binding) change.bindings.push_back({binding->name, binding->revision, true, binding->target, {}});
    Check(f.data->SetDataCommit(std::move(change)).status == DataCommitStatus::Succeeded
        && host->notifications.load() > changesBefore, "data invalidation wakes owner for stale cleanup");
}
void SurfaceReferenceLifecycle() {
    Fixture f;
    const std::vector<double> referenceVertices{0,0,0, 10,0,0, 0,10,0,
        0,0,0, 0,10,0, 0,0,10, 0,0,0, 0,0,10, 10,0,0};
    const std::vector<std::uint64_t> triangles{0,1,2, 3,4,5, 6,7,8};
    auto reference = std::make_shared<const SurfaceMeshPayload>(referenceVertices, triangles);
    std::vector<double> measured{1,1,0, 3,1,0, 1,3,0,
        0,1,1, 0,3,1, 0,1,3, 1,0,1, 1,0,3, 3,0,1};
    for (std::size_t i = 0; i < measured.size(); i += 3) {
        measured[i] -= 0.1;
        measured[i+1] += 0.2;
        measured[i+2] -= 0.3;
    }
    f.mesh = std::make_shared<const SurfaceMeshPayload>(measured, triangles);
    f.input.mesh = {f.data->CreateDataEntityId(), 1};
    f.input.meshBinding.clear();
    f.recipe.nominalData = {f.data->CreateDataEntityId(), 1};
    DataTransaction inputs;
    inputs.outputs.push_back({f.input.mesh.entityId, 0, DataTypes::surfaceMesh,
        {{"source", f.input.source}}, f.mesh, {}});
    inputs.outputs.push_back({f.recipe.nominalData.entityId, 0, DataTypes::surfaceMesh,
        {}, reference, {}});
    Check(f.data->SetDataCommit(std::move(inputs)).status == DataCommitStatus::Succeeded,
          "source and reference surfaces published");
    f.recipe.geometries.clear();
    f.recipe.constraints.clear();
    f.recipe.method = AlignmentMethod::ConstrainedBestFit;
    f.recipe.surfaceFit = AlignmentSurfaceFit{};
    f.recipe.surfaceFit->referenceTriangles = {0,1,2};
    AlignmentRequest save;
    save.action = AlignmentAction::SaveRecipe;
    save.recipe = f.recipe;
    Check(f.feature->SendRequest(save, [&](AlignmentResult r) { f.recipeRef = r.recipe; }).status ==
          AlignmentAdmissionStatus::Accepted, "surface recipe accepted through public entry");
    const auto result = f.Solve();
    const auto archive = f.feature->GetArchive(result.result);
    Check(result.status == AlignmentStatus::FullyDetermined && result.isActivated && archive &&
          std::abs(archive->sourceToTarget[3] - 0.1) < 1e-7 &&
          std::abs(archive->sourceToTarget[7] + 0.2) < 1e-7 &&
          std::abs(archive->sourceToTarget[11] - 0.3) < 1e-7,
          "public dynamic surface alignment publishes analytic transform");
    Check(f.mesh->GetPointValues() == MeshReadArray<double>(measured) && reference->GetPointValues() == MeshReadArray<double>(referenceVertices),
          "public alignment preserves both immutable input meshes");
    DataTransaction replace;
    replace.outputs.push_back({f.recipe.nominalData.entityId, 1, DataTypes::surfaceMesh, {}, reference, {}});
    Check(f.data->SetDataCommit(std::move(replace)).status == DataCommitStatus::Succeeded,
          "reference revision advanced");
    Check(!f.feature->GetState().isCurrent, "reference change invalidates active surface alignment");
    Check(f.feature->GetArchive(result.result).has_value(), "historical surface alignment remains readable");
    AlignmentRequest restore;
    restore.action = AlignmentAction::Restore;
    restore.archive = archive;
    restore.input = f.input;
    restore.restoredNominal = {f.recipe.nominalData.entityId, 2};
    Check(f.feature->SendRequest(restore).status == AlignmentAdmissionStatus::InvalidRequest,
          "restore cannot reuse reference triangle IDs on another revision");
}
void InspectionLifecycle(bool hasSectionMesh=false) {
    auto inspectionViews=std::make_shared<Views>();Fixture f(inspectionViews, {}, hasSectionMesh);
    const auto alignment=f.Solve();
    AlignmentRequest request; request.action=AlignmentAction::Inspect; request.inspection=InspectionPlan{};
    auto &plan=*request.inspection; plan.id="drawing-inspection"; plan.alignmentResult=alignment.result;
    InspectionGeometrySpec point; point.fit=f.recipe.geometries[0]; plan.geometries.push_back(point);
    InspectionItem item; item.id="center-x"; item.measured.metric=InspectionMetric::CenterX;
    item.nominal=.1; item.nominalSource="drawing"; item.deviationLimits=std::array<double,2>{-1e-7,1e-7}; plan.items.push_back(item);
    if (hasSectionMesh) {
        InspectionGeometrySpec circle; circle.fit.id="section"; circle.fit.kind=AlignmentGeometryKind::Circle;
        circle.fit.maxFitRms=.1; circle.section=InspectionSection{}; circle.section->pinnedMesh=f.input.mesh;
        circle.section->planePoint={.1,-.2,.3}; plan.geometries.push_back(circle);
        InspectionItem diameter; diameter.id="section-diameter"; diameter.measured.first.geometry=1;
        diameter.nominal=10; diameter.nominalSource="analytic regular polygon cylinder";
        const double chordBound=10*(1-std::cos(std::acos(-1.0)/32));
        diameter.deviationLimits=std::array<double,2>{-chordBound,chordBound}; plan.items.push_back(diameter);
    }
    AlignmentResult evaluated; int completed=0;
    const auto run=[&] {
        Check(f.feature->SendRequest(request,[&](AlignmentResult r) { evaluated=std::move(r); ++completed; }).status==
              AlignmentAdmissionStatus::Accepted,"independent inspection accepted");
    };
    run(); request.inspection->items[0].nominal=99; f.Drain(); request.inspection->items[0].nominal=.1;
    Check(completed==1 && evaluated.status==AlignmentStatus::FullyDetermined && GetDataRevisionRefValid(evaluated.inspectionResult),
          "inspection publishes one immutable report");
    const auto reportRef=evaluated.inspectionResult;
    const auto report=f.feature->GetInspection(reportRef);
    Check(report && report->isCurrent && report->report->verdict==AlignmentInspectionVerdict::Pass &&
          report->plan->items[0].nominal==.1 && report->sourceMesh==f.input.mesh && report->alignmentResult==alignment.result,
          "inspection freezes plan and reports explicit alignment/mesh identity");
    const auto again=f.feature->GetInspection(reportRef);
    Check(report && again && report->report==again->report && report->plan==again->plan,"report polling shares immutable data and ROI plan");
    if (report) {
        Check(report->report->geometries[0].fitted.support.has_value(),"public inspection publishes sample support");
        auto forged=*report; forged.result={f.data->CreateDataEntityId(),1};
        auto body=std::make_shared<InspectionReport>(*report->report);
        body->geometries[0].fitted.support=AlignmentGeometrySupport{};
        body->geometries[0].fitted.support->principalRmsSpreads[0]=-1; forged.report=body;
        DataTransaction invalid; invalid.outputs.push_back({forged.result.entityId,0,AlignmentInspectionData::resultType,{},
            std::make_shared<const AlignmentInspectionData::Payload>(forged,std::vector<DataExpectation>{}),{}});
        Check(f.data->SetDataCommit(std::move(invalid)).status!=DataCommitStatus::Succeeded,"report rejects invalid support metadata");
    }
    Check(f.feature->GetState().activeResult==alignment.result,"inspection does not realign or reactivate measurement");
    AlignmentRequest display;display.action=AlignmentAction::SetSceneDisplay;display.sceneDisplay=AlignmentSceneDisplay{};
    display.sceneDisplay->anchorAlignment=alignment.result;display.sceneDisplay->roiMarkerLimit=3;
    const std::string selectedItem=hasSectionMesh ? "section-diameter" : "center-x";
    display.sceneDisplay->parts={{"inspection",alignment.result,hasSectionMesh,false,reportRef,selectedItem}};
    AlignmentResult shown;Check(f.feature->SendRequest(display,[&](AlignmentResult value){shown=std::move(value);}).status==AlignmentAdmissionStatus::Accepted && shown.isDisplayReady,
        "inspection scene displays published geometry without fitting again");
    const auto selection=f.feature->GetSceneItem("inspection",selectedItem);
    Check(selection && report && selection->plan==report->plan && selection->report==report->report && selection->inspectionResult==reportRef &&
        selection->isCurrent && selection->isSelected && selection->geometries==std::vector<std::size_t>{hasSectionMesh ? 1u : 0u},
        "selected report item shares original plan/report and exact geometry mapping");
    if(selection)Check(selection->displayedRegions.size()==1 && selection->displayedRegions[0].kind==(hasSectionMesh ? AlignmentSceneRegionKind::SectionSamples : AlignmentSceneRegionKind::VertexCandidates) &&
        selection->displayedRegions[0].drawnMarkers==(hasSectionMesh ? 3u : 1u) && selection->displayedRegions[0].sourceCount.has_value(),
        "display distinguishes actual section samples from vertex candidates and caps visual markers");
    const auto frozenDisplay=f.feature->GetState().sceneDisplay;
    MetrologyReportRequest sceneCapture;sceneCapture.id="current-scene";sceneCapture.captureCurrentDisplay=true;
    const auto currentSceneReport=f.feature->GetReport(sceneCapture);
    Check(currentSceneReport.report && currentSceneReport.report->presentation.source==MetrologyReportDisplaySource::CurrentFeatureState &&
        currentSceneReport.report->presentation.scene && currentSceneReport.report->presentation.scene->parameters==frozenDisplay &&
        currentSceneReport.report->inspections.size()==1 && currentSceneReport.report->alignments.size()==1,
        "current display capture shares configuration and automatically includes inspection and alignment");
    sceneCapture.sceneDisplay=*frozenDisplay;Check(f.feature->GetReport(sceneCapture).status==AlignmentStatus::InvalidInput,"current and explicit display capture cannot be mixed");
    sceneCapture.captureCurrentDisplay=false;const auto explicitSceneReport=f.feature->GetReport(sceneCapture);sceneCapture.sceneDisplay->parts[0].selectedItem="changed-after-capture";
    Check(explicitSceneReport.report && explicitSceneReport.report->presentation.source==MetrologyReportDisplaySource::ExplicitConfiguration &&
        explicitSceneReport.report->presentation.scene->parameters->parts[0].selectedItem==selectedItem,"explicit scene configuration freezes without aliasing mutable caller parameters");
    display.sceneDisplay->parts[0].selectedItem="missing-item";
    Check(f.feature->SendRequest(display).status==AlignmentAdmissionStatus::InvalidRequest && f.feature->GetState().sceneDisplay==frozenDisplay,
        "missing report item cannot replace a valid inspection scene");
    Check(!f.feature->GetSceneItem("inspection","missing-item"),"unknown report item selection is empty");
    display.sceneDisplay->parts[0].selectedItem=selectedItem;const auto otherAlignment=f.Solve();display.sceneDisplay->parts[0].alignmentResult=otherAlignment.result;
    Check(f.feature->SendRequest(display).status==AlignmentAdmissionStatus::InvalidRequest && f.feature->GetState().sceneDisplay==frozenDisplay,
        "inspection from another alignment cannot be silently attached to a scene part");display.sceneDisplay->parts[0].alignmentResult=alignment.result;


    if (hasSectionMesh && report && again) {
        const auto trace=report->report->geometries[1].section;
        Check(trace && trace==again->report->geometries[1].section && trace->sourceTriangleCount==64,
            "public section inspection preserves shared sample provenance and source triangle count");
        if (trace) {
            auto forged=*report; forged.result={f.data->CreateDataEntityId(),1};
            auto body=std::make_shared<InspectionReport>(*report->report);
            auto invalidTrace=std::make_shared<InspectionSectionTrace>(*trace);
            invalidTrace->samples[0].triangle=trace->sourceTriangleCount;
            body->geometries[1].section=invalidTrace; forged.report=body;
            DataTransaction invalid; invalid.outputs.push_back({forged.result.entityId,0,AlignmentInspectionData::resultType,{},
                std::make_shared<const AlignmentInspectionData::Payload>(forged,std::vector<DataExpectation>{}),{}});
            Check(f.data->SetDataCommit(std::move(invalid)).status!=DataCommitStatus::Succeeded,
                "report validator rejects out-of-range section provenance");
        }
    }
    if (report) {
        auto forged=*report; forged.result={f.data->CreateDataEntityId(),1};
        auto body=std::make_shared<InspectionReport>(*report->report); body->verdict=AlignmentInspectionVerdict::Fail; forged.report=body;
        DataTransaction invalid; invalid.outputs.push_back({forged.result.entityId,0,AlignmentInspectionData::resultType,{},
            std::make_shared<const AlignmentInspectionData::Payload>(forged,std::vector<DataExpectation>{}),{}});
        Check(f.data->SetDataCommit(std::move(invalid)).status!=DataCommitStatus::Succeeded,
              "report validator rejects aggregate verdict contradicting item requirements");
    }

    run(); AlignmentRequest cancel; cancel.action=AlignmentAction::Cancel; f.feature->SendRequest(cancel); f.Drain();
    Check(evaluated.status==AlignmentStatus::Cancelled && !GetDataRevisionRefValid(evaluated.inspectionResult),"cancelled inspection publishes nothing");
    f.data->rejectsCommit=true; run(); f.Drain(); f.data->rejectsCommit=false;
    Check(evaluated.status==AlignmentStatus::InternalError && !GetDataRevisionRefValid(evaluated.inspectionResult),"inspection commit failure clears result ref");
    f.data->throwsAfterCommit=true; run(); f.Drain(); f.data->throwsAfterCommit=false;
    Check(evaluated.status==AlignmentStatus::FullyDetermined && f.feature->GetInspection(evaluated.inspectionResult),
          "inspection preserves committed result after observer exception");
    run();
    DataTransaction changed; changed.outputs.push_back({f.input.mesh.entityId,1,DataTypes::surfaceMesh,{{"source",f.input.source}},f.mesh,{}});
    Check(f.data->SetDataCommit(std::move(changed)).status==DataCommitStatus::Succeeded,"inspection source changes while worker is pending");
    f.Drain();
    Check(evaluated.status==AlignmentStatus::Stale && !GetDataRevisionRefValid(evaluated.inspectionResult),"stale inspection is not published");
    Check(!f.feature->GetState().isSceneDisplayReady && !f.feature->GetSceneItem("inspection",selectedItem),"source revision change retires inspection display and selection");
    const auto historical=f.feature->GetInspection(reportRef);
    Check(historical && !historical->isCurrent && report && historical->report==report->report,"historical report retains values with explicit stale state");
    Check(f.feature->SendRequest(request).status==AlignmentAdmissionStatus::InvalidRequest,"inspection refuses stale alignment source");
}
void SceneLifecycle() {
    auto views=std::make_shared<Views>();Fixture f(views,{},true);
    const DataRevisionRef nominal{f.data->CreateDataEntityId(),1};DataTransaction tx;
    tx.outputs.push_back({nominal.entityId,0,DataTypes::surfaceMesh,{},f.mesh,{}});
    Check(f.data->SetDataCommit(std::move(tx)).status==DataCommitStatus::Succeeded,"scene nominal surface publishes");f.recipe.nominalData=nominal;
    const auto save=[&]{AlignmentRequest r;r.action=AlignmentAction::SaveRecipe;r.recipe=f.recipe;
        Check(f.feature->SendRequest(r,[&](AlignmentResult value){f.recipeRef=value.recipe;}).status==AlignmentAdmissionStatus::Accepted,"scene recipe saves");};
    save();const auto first=f.Solve();for(auto &c:f.recipe.constraints)c.nominalPoint[0]+=5;f.input.scope="part-2";save();const auto second=f.Solve();
    AlignmentRequest request;request.action=AlignmentAction::SetSceneDisplay;request.sceneDisplay=AlignmentSceneDisplay{};
    auto &scene=*request.sceneDisplay;scene.anchorAlignment=first.result;scene.parts={{"first",first.result},{"second",second.result}};
    AlignmentResult result;const auto show=[&]{return f.feature->SendRequest(request,[&](AlignmentResult value){result=std::move(value);});};
    const auto commit=f.data->GetDataGraph().commitId;Check(show().status==AlignmentAdmissionStatus::Accepted && result.isDisplayReady,"multi-part scene publishes display");
    const auto frozen=f.feature->GetState().sceneDisplay;const auto before=views->overlay->overlays;
    auto displayed=std::dynamic_pointer_cast<AlignmentSceneOverlay>(views->overlay->overlays.back());auto renderer=vtkSmartPointer<vtkRenderer>::New();
    Check(bool(displayed),"scene owns a typed overlay");if(displayed){displayed->AttachRenderer(renderer);Check(renderer->GetViewProps()->GetNumberOfItems()==3,"two measured parts share one drawn nominal surface");displayed->DetachRenderer(renderer);}
    Check(frozen && f.feature->GetState().isSceneDisplayReady && f.data->GetDataGraph().commitId==commit,"scene is display-only with frozen request");
    Check(show().status==AlignmentAdmissionStatus::Accepted && f.feature->GetState().sceneDisplay==frozen && views->overlay->overlays==before,"identical scene reuses actors and render product");
    const auto a=f.feature->GetSceneVertex("first",false,0),b=f.feature->GetSceneVertex("second",false,0),n=f.feature->GetSceneVertex("first",true,0);
    Check(a && b && n && a->mesh==f.input.mesh && a->anchorAlignment==first.result && std::abs(a->modelPoint[0])<1e-7 &&
        std::abs(b->modelPoint[0]-5)<1e-7 && std::abs(n->modelPoint[0]+.1)<1e-7,"vertex trace uses each formal pose and one pinned anchor");
    Check(!f.feature->GetSceneVertex("missing",false,0) && !f.feature->GetSceneVertex("first",false,UINT64_MAX),"scene vertex bounds and identities checked");
    scene.selectedPart="second";views->overlay->rejectsAttach=true;Check(show().status==AlignmentAdmissionStatus::Accepted && result.status==AlignmentStatus::InternalError &&
        f.feature->GetState().sceneDisplay==frozen && views->overlay->overlays==before,"failed scene replacement preserves prior valid display");views->overlay->rejectsAttach=false;
    scene.selectedPart.reset();scene.parts.push_back(scene.parts.front());Check(show().status==AlignmentAdmissionStatus::InvalidRequest,"duplicate scene identity refused");scene.parts.pop_back();
    f.recipe.targetFrameId="other-target";save();const auto otherFrame=f.Solve();scene.parts.push_back({"wrong-frame",otherFrame.result});
    Check(show().status==AlignmentAdmissionStatus::InvalidRequest && f.feature->GetState().sceneDisplay==frozen,"scene refuses mixed target frames without replacing current display");scene.parts.pop_back();
    AlignmentSceneDisplayController controller;AlignmentConfig config;
    Check(controller.Set(*f.data,views.get(),config,scene).isDisplayReady,"scene has no resource admission quota");
    scene.selectedPart="second";Check(controller.Set(*f.data,views.get(),config,scene).isDisplayReady,"scene replacement reuses source geometry without a byte quota");controller.Clear();
    views->overlay->onAttach=[&]{AlignmentRequest nested=request;Check(f.feature->SendRequest(nested).status==AlignmentAdmissionStatus::Busy,"scene attach callback cannot reenter feature mutation");};
    Check(show().status==AlignmentAdmissionStatus::Accepted && result.isDisplayReady,"scene selection switches");views->overlay->onAttach={};
    scene.isVisible=false;Check(show().status==AlignmentAdmissionStatus::Accepted && !f.feature->GetState().sceneDisplay,"scene hide removes only its overlay");scene.isVisible=true;
    Check(show().status==AlignmentAdmissionStatus::Accepted && result.isDisplayReady,"scene restores");
    views->overlay->onAttach=[&]{DataTransaction changed;changed.outputs.push_back({nominal.entityId,1,DataTypes::surfaceMesh,{},f.mesh,{}});
        Check(f.data->SetDataCommit(std::move(changed)).status==DataCommitStatus::Succeeded,"scene dependency changes during attach");};
    scene.selectedPart="first";Check(show().status==AlignmentAdmissionStatus::Accepted && result.status==AlignmentStatus::Stale && !result.isDisplayReady,"scene refuses stale attach result");
    views->overlay->onAttach={};f.feature->OnHostTick();Check(!f.feature->GetState().isSceneDisplayReady && !f.feature->GetState().sceneDisplay,"stale scene is retired on owner tick");
}
void ModelPoseLifecycle() {
    {
        Fixture f;const auto solved=f.Solve();AlignmentRequest apply;apply.action=AlignmentAction::ApplyModelPose;apply.resultRef=solved.result;
        Check(f.feature->SendRequest(apply).status==AlignmentAdmissionStatus::Unavailable,"data-only attachment does not promise model rendering");
    }
    auto host=std::make_shared<PoseControl>();Fixture f({},host);host->state.dataRevision=f.input.mesh;
    const auto solved=f.Solve();const auto archive=f.feature->GetArchive(solved.result);const auto active=f.feature->GetState().activeResult;
    const auto graphBefore=f.data->GetDataGraph().commitId;const auto *vertices=&f.mesh->GetPointValues();
    AlignmentRequest apply;apply.action=AlignmentAction::ApplyModelPose;apply.resultRef=solved.result;
    int calls=0;AlignmentResult completed;
    const auto start=[&] { return f.feature->SendRequest(apply,[&](AlignmentResult r){completed=std::move(r);++calls;}); };
    auto wrong=host->state.dataRevision;host->state.dataRevision=f.input.scopeData;
    Check(start().status==AlignmentAdmissionStatus::InvalidRequest,"model pose refuses a different displayed revision");host->state.dataRevision=wrong;
    host->throwStart=true;int faultCalls=0;
    const auto fault=f.feature->SendRequest(apply,[&](AlignmentResult r){++faultCalls;Check(r.status==AlignmentStatus::InternalError,"pose start exception is explicit");});
    Check(fault.status==AlignmentAdmissionStatus::Accepted && faultCalls==1,"pre-token exception preserves exactly-once callback ownership");host->throwStart=false;
    const auto admitted=start();Check(admitted.status==AlignmentAdmissionStatus::Accepted && calls==0 && f.feature->GetState().isBusy,
        "model pose remains pending until Host frame acknowledgement");
    f.feature->OnHostTick();Check(calls==0 && host->state.modelToWorld==alignmentIdentity,"queued pose is not reported as applied");
    Check(start().status==AlignmentAdmissionStatus::Busy,"pending pose serializes feature work");
    host->Frame();f.feature->OnHostTick();
    Check(calls==1 && completed.status==AlignmentStatus::FullyDetermined && completed.isModelPoseApplied && archive &&
        host->state.modelToWorld==archive->sourceToTarget && f.feature->GetState().modelPose.isCurrent,"acknowledged absolute pose matches formal result");
    Check(f.feature->GetState().activeResult==active && f.data->GetDataGraph().commitId==graphBefore && &f.mesh->GetPointValues()==vertices,
        "display pose changes no data graph, active result or mesh storage");
    Check(!GetDataRevisionRefValid(f.feature->GetScopeState("unrelated").modelPose.alignmentResult),"scope state does not attribute model pose to another part");
    host->state.modelToWorld[3]+=2;++host->state.transformRevision;
    Check(!f.feature->GetState().modelPose.isCurrent,"external visual edit invalidates pose-current claim");
    const auto previous=host->state.modelToWorld;
    auto requestId=start().requestId;AlignmentRequest cancel;cancel.action=AlignmentAction::Cancel;cancel.targetRequestId=requestId;
    host->rejectStopOnce=true;Check(f.feature->SendRequest(cancel).status==AlignmentAdmissionStatus::Accepted,"transient stop rejection still accepts queued cancellation");f.feature->OnHostTick();host->Frame();f.feature->OnHostTick();
    Check(calls==2 && completed.status==AlignmentStatus::Cancelled && host->state.modelToWorld==previous,"transient stop rejection retains ownership until cancellation frame commits");
    host->rejectCommit=true;Check(start().status==AlignmentAdmissionStatus::Accepted,"commit failure retains accepted completion");host->Frame();f.feature->OnHostTick();host->rejectCommit=false;
    Check(calls==3 && completed.status==AlignmentStatus::InternalError && !completed.isModelPoseApplied,"failed pose commit completes without fabricated application");
    host->throwAfterQueue=true;Check(start().status==AlignmentAdmissionStatus::Accepted,"exception after queue keeps token and callback");host->Frame();f.feature->OnHostTick();host->throwAfterQueue=false;
    Check(calls==4 && completed.status==AlignmentStatus::InternalError && !host->state.editToken,"queued exception cancellation releases pose token");
    Check(start().status==AlignmentAdmissionStatus::Accepted,"stale pose case starts");DataTransaction changed;
    changed.outputs.push_back({f.input.mesh.entityId,1,DataTypes::surfaceMesh,{{"source",f.input.source}},f.mesh,{}});
    Check(f.data->SetDataCommit(std::move(changed)).status==DataCommitStatus::Succeeded,"pose dependency changes before frame");
    f.feature->OnHostTick();host->Frame();f.feature->OnHostTick();
    Check(calls==5 && completed.status==AlignmentStatus::Stale && !completed.isModelPoseApplied,"stale candidate cancels before application");
    {
        auto otherHost=std::make_shared<PoseControl>();Fixture other({},otherHost);otherHost->state.dataRevision=other.input.mesh;
        apply.resultRef=other.Solve().result;int detachedCalls=0;
        Check(other.feature->SendRequest(apply,[&](AlignmentResult r){++detachedCalls;Check(r.status==AlignmentStatus::Cancelled,"detach cancels pose");}).status==AlignmentAdmissionStatus::Accepted,"detach pose starts");
        Check(!other.feature->DetachHost(),"detach waits for pose token retirement");otherHost->Frame();other.feature->OnHostTick();
        Check(detachedCalls==1 && other.feature->DetachHost(),"pose completion occurs exactly once before detach releases port");
    }
}
void VoxelVolumeLifecycle() {
    std::shared_ptr<const MetrologyReport> retained;std::weak_ptr<const std::vector<std::uint8_t>> sourceLifetime;
    {
    Fixture f;GridGeometry3D geometry;geometry.dimensions={3,3,3};geometry.extent={0,2,0,2,0,2};geometry.spacing={.5,.5,.5};
    std::vector<float> values(27,0);values[13]=100;
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(values.size()*sizeof(float));std::memcpy(bytes->data(),values.data(),bytes->size());
    auto image=std::make_shared<const ImageGrid3DPayload>(geometry,ImageValueType::Float32,1,bytes);
    const DataRevisionRef source{f.data->CreateDataEntityId(),1};DataTransaction add;
    add.outputs.push_back({source.entityId,0,DataTypes::imageGrid3D,{},image,{}});
    Check(f.data->SetDataCommit(std::move(add)).status==DataCommitStatus::Succeeded,"voxel source publishes");
    AlignmentRequest request;request.action=AlignmentAction::EvaluateVoxelVolume;request.voxelVolume=VoxelVolumeRequest{};
    request.voxelVolume->id="occupied-cell";request.voxelVolume->source=source;request.voxelVolume->storedValueRange=std::array<double,2>{50,200};request.voxelVolume->requireEnclosedSelection=true;
    AlignmentResult completed;int calls=0;const auto before=f.feature->GetState().activeResult;
    const auto run=[&] { Check(f.feature->SendRequest(request,[&](AlignmentResult r) { completed=std::move(r);++calls; }).status==AlignmentAdmissionStatus::Accepted,"voxel evaluation admitted"); };
    run();(*request.voxelVolume->storedValueRange)[0]=150;f.Drain();(*request.voxelVolume->storedValueRange)[0]=50;
    const auto resultRef=completed.voxelVolumeResult;const auto snapshot=f.feature->GetVoxelVolume(resultRef);
    Check(calls==1 && completed.status==AlignmentStatus::FullyDetermined && snapshot && snapshot->isCurrent && snapshot->statistics.selected==1 &&
        snapshot->statistics.volumeMm3 && std::abs(*snapshot->statistics.volumeMm3-.125)<1e-12 && !snapshot->statistics.touchesRegionBoundary,
        "public voxel endpoint freezes parameters and computes enclosed analytic cell volume without alignment");
    Check(f.feature->GetState().activeResult==before,"voxel measurement does not change active pose");
    sourceLifetime=bytes;
    const auto storageOwners=bytes.use_count();
    MetrologyReportRequest volumeReport;volumeReport.id="voxel-only";volumeReport.voxelVolumes={resultRef};
    const auto frozen=f.feature->GetReport(volumeReport);retained=frozen.report;
    Check(frozen.report && frozen.report->alignments.empty() && frozen.report->voxelVolumes.size()==1 && snapshot &&
        frozen.report->voxelVolumes[0].parameters==snapshot->parameters && bytes.use_count()==storageOwners,
        "voxel-only report shares frozen parameters without retaining scalar storage or fabricating alignment");
    auto duplicate=volumeReport;duplicate.voxelVolumes.push_back(resultRef);
    Check(f.feature->GetReport(duplicate).status==AlignmentStatus::InvalidInput,"duplicate voxel report revisions rejected");
    auto missing=volumeReport;missing.voxelVolumes[0].generation=999;
    Check(f.feature->GetReport(missing).status==AlignmentStatus::InvalidInput,"missing voxel report source rejected");
    if(snapshot) {
        auto forged=*snapshot;forged.result={f.data->CreateDataEntityId(),1};forged.statistics.cellVolumeMm3=2;
        forged.statistics.observedVolumeMm3=2;forged.statistics.volumeMm3=2;
        DataTransaction invalid;invalid.outputs.push_back({forged.result.entityId,0,AlignmentVoxelVolumeData::resultType,{{"source",source}},
            std::make_shared<const AlignmentVoxelVolumeData::Payload>(forged,std::vector<DataExpectation>{}),{}});
        Check(f.data->SetDataCommit(std::move(invalid)).status!=DataCommitStatus::Succeeded,"volume publication rejects cell volume inconsistent with source geometry");
    }
    run();AlignmentRequest cancel;cancel.action=AlignmentAction::Cancel;f.feature->SendRequest(cancel);f.Drain();
    Check(completed.status==AlignmentStatus::Cancelled && !GetDataRevisionRefValid(completed.voxelVolumeResult),"cancelled voxel scan publishes nothing");
    f.data->rejectsCommit=true;run();f.Drain();f.data->rejectsCommit=false;
    Check(completed.status==AlignmentStatus::InternalError && !GetDataRevisionRefValid(completed.voxelVolumeResult),"failed volume commit exposes no output identity");
    f.data->throwsAfterCommit=true;run();f.Drain();f.data->throwsAfterCommit=false;
    Check(completed.status==AlignmentStatus::FullyDetermined && f.feature->GetVoxelVolume(completed.voxelVolumeResult),"volume observer exception preserves committed result");
    run();DataTransaction change;change.outputs.push_back({source.entityId,1,DataTypes::imageGrid3D,{},image,{}});
    Check(f.data->SetDataCommit(std::move(change)).status==DataCommitStatus::Succeeded,"voxel source advances during work");f.Drain();
    Check(completed.status==AlignmentStatus::Stale && !GetDataRevisionRefValid(completed.voxelVolumeResult),"stale voxel candidate cannot publish");
    const auto history=f.feature->GetVoxelVolume(resultRef);
    Check(history && !history->isCurrent && snapshot && history->parameters==snapshot->parameters,"voxel history is readable with explicit staleness");
    Check(f.feature->SendRequest(request).status==AlignmentAdmissionStatus::InvalidRequest,"stale source cannot start new formal volume measurement");
    const auto stale=f.feature->GetReport(volumeReport);
    Check(stale.report && !stale.report->wereInputsCurrentAtCapture && !stale.report->voxelVolumes[0].isCurrent &&
        frozen.report && frozen.report->wereInputsCurrentAtCapture,"voxel report freezes capture-time currentness");
    }
    Check(sourceLifetime.expired(),"frozen voxel report does not retain source bytes after Host destruction");
    if(retained) { bool written=false;std::thread writer([&]{std::ostringstream stream;
        written=WriteMetrologyReport(*retained,stream).status==MetrologyReportWriteStatus::Succeeded;});writer.join();
        Check(written,"voxel-only frozen report writes after Host and source destruction"); }
}

void ReportLifecycle() {
    std::shared_ptr<const MetrologyReport> retained;
    {
        Fixture f;
        MetrologyReportRequest request; request.id="multi-alignment"; request.title="global and local evaluation";
        for (const double x:{.1,.6}) {
            if (x>.1) {
                for (auto &c:f.recipe.constraints) c.nominalPoint[0]+=.5;
                AlignmentRequest save; save.action=AlignmentAction::SaveRecipe; save.recipe=f.recipe;
                Check(f.feature->SendRequest(save,[&](AlignmentResult r) { f.recipeRef=r.recipe; }).status==AlignmentAdmissionStatus::Accepted,
                    "second report coordinate relation saved");
            }
            const auto alignment=f.Solve();
            if (request.alignments.empty()) request.alignments.push_back(alignment.result);
            AlignmentRequest inspect; inspect.action=AlignmentAction::Inspect; inspect.inspection=InspectionPlan{};
            auto &plan=*inspect.inspection; plan.id="report-row"; plan.alignmentResult=alignment.result;
            InspectionGeometrySpec geometry; geometry.fit=f.recipe.geometries[0]; plan.geometries.push_back(geometry);
            InspectionItem item; item.id="x"; item.measured.metric=InspectionMetric::CenterX; item.nominal=x;
            item.nominalSource="analytic coordinate relation"; item.deviationLimits=std::array<double,2>{-1e-7,1e-7}; plan.items.push_back(item);
            AlignmentResult evaluated;
            Check(f.feature->SendRequest(inspect,[&](AlignmentResult r) { evaluated=std::move(r); }).status==AlignmentAdmissionStatus::Accepted,
                "report row evaluation accepted"); f.Drain();
            Check(evaluated.status==AlignmentStatus::FullyDetermined,"report row evaluation completed");
            request.inspections.push_back(evaluated.inspectionResult);
        }
        const auto graph=f.data->GetDataGraph();
        const auto captured=f.feature->GetReport(request); retained=captured.report;
        Check(captured.status==AlignmentStatus::FullyDetermined && retained && retained->capturedCommit==graph.commitId &&
            retained->alignments.size()==2 && retained->inspections.size()==2 && retained->wereInputsCurrentAtCapture,
            "one graph capture includes both alignment owners without duplicating explicit owner");
        if (!retained) return;
        for (std::size_t i=0;i<2;++i) {
            const auto &inspection=retained->inspections[i];
            const auto owner=std::find_if(retained->alignments.begin(),retained->alignments.end(),[&](const auto &a) { return a.result==inspection.alignmentResult; });
            Check(owner!=retained->alignments.end() && std::abs(owner->sourceToTarget[3]-(i ? .6 : .1))<1e-7 &&
                inspection.report->items[0].verdict==AlignmentInspectionVerdict::Pass,"each report row retains its own coordinate relation");
            const auto original=f.feature->GetInspection(request.inspections[i]);
            Check(original && original->plan==inspection.plan && original->report==inspection.report,"capture shares immutable inspection plan and report");
            if (owner!=retained->alignments.end()) {
                const auto recipe=std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(graph.view->GetData(owner->recipeRevision)->payload);
                Check(recipe && owner->recipe.get()==&recipe->recipe,"report aliases recipe without copying ROI arrays");
            }
        }
        auto duplicate=request; duplicate.inspections.push_back(request.inspections.front());
        Check(f.feature->GetReport(duplicate).status==AlignmentStatus::InvalidInput,"duplicate report entries are rejected");
        auto missing=request; missing.inspections[0].generation=999;
        Check(f.feature->GetReport(missing).status==AlignmentStatus::InvalidInput,"missing report revision produces no partial report");
        bool ownerRejected=false; std::thread foreign([&] { ownerRejected=f.feature->GetReport(request).status==AlignmentStatus::Unavailable; }); foreign.join();
        Check(ownerRejected,"report capture is owner-thread only");
        f.recipe.constraints.resize(3);
        AlignmentRequest underSave; underSave.action=AlignmentAction::SaveRecipe; underSave.recipe=f.recipe;
        Check(f.feature->SendRequest(underSave,[&](AlignmentResult r) { f.recipeRef=r.recipe; }).status==AlignmentAdmissionStatus::Accepted,
            "underdetermined report recipe saved");
        const auto under=f.Solve();
        MetrologyReportRequest diagnostic; diagnostic.id="underconstrained"; diagnostic.alignments={under.result};
        const auto incomplete=f.feature->GetReport(diagnostic);
        Check(under.status==AlignmentStatus::Underconstrained && incomplete.report &&
            incomplete.report->alignments[0].diagnostics->status==AlignmentStatus::Underconstrained,
            "report capture retains underconstrained alignment diagnostics without converting them to measurement success");
        DataTransaction changed; changed.outputs.push_back({f.input.mesh.entityId,1,DataTypes::surfaceMesh,{{"source",f.input.source}},f.mesh,{}});
        Check(f.data->SetDataCommit(std::move(changed)).status==DataCommitStatus::Succeeded,"report source advances");
        const auto historical=f.feature->GetReport(request);
        Check(historical.report && !historical.report->wereInputsCurrentAtCapture && retained->wereInputsCurrentAtCapture &&
            historical.report->capturedCommit>retained->capturedCommit && historical.report->inspections[0].report==retained->inspections[0].report,
            "recapture reports stale inputs while prior capture retains its explicit capture-time state");
    }
    Check(retained && retained->inspections[0].report->items[0].measured && retained->alignments[0].recipe,
        "frozen report values remain readable after feature and graph owner destruction");
    if(retained) {
        bool written=false;
        std::thread background([&] {
            std::ostringstream stream;const auto result=WriteMetrologyReport(*retained,stream);
            written=result.status==MetrologyReportWriteStatus::Succeeded && stream.str().find("multi-alignment")!=std::string::npos;
        });background.join();
        Check(written,"frozen report exports on a background thread after Host destruction");
    }
}
void AutomaticReferenceLifecycle() {
    auto views = std::make_shared<Views>();
    Fixture f(views);
    const auto model = AlignmentTestModels::Model();
    f.mesh = model.mesh; f.recipe = model.recipe;
    f.recipe.surfaceFit->scales = {{.4,.8,40},{0,.8,40}};
    f.input.mesh = {f.data->CreateDataEntityId(), 1}; f.input.meshBinding.clear();
    f.recipe.nominalData = {f.data->CreateDataEntityId(), 1};
    DataTransaction inputs;
    inputs.outputs.push_back({f.input.mesh.entityId, 0, DataTypes::surfaceMesh,
        {{"source", f.input.source}}, f.mesh, {}});
    inputs.outputs.push_back({f.recipe.nominalData.entityId, 0, DataTypes::surfaceMesh,
        {}, model.referenceMesh, {}});
    Check(f.data->SetDataCommit(std::move(inputs)).status == DataCommitStatus::Succeeded,
          "automatic reference fixture committed");
    const auto save = [&] {
        AlignmentRequest request; request.action = AlignmentAction::SaveRecipe; request.recipe = f.recipe;
        Check(f.feature->SendRequest(request, [&](AlignmentResult result) { f.recipeRef = result.recipe; }).status ==
              AlignmentAdmissionStatus::Accepted, "automatic recipe accepted");
    };
    save();
    const auto result = f.Solve();
    Check(result.status == AlignmentStatus::FullyDetermined && result.isActivated && result.diagnostics.prealignment,
          "public automatic alignment activates only after formal solve");
    const auto stored = AlignmentData::GetResult(f.data->GetDataGraph(), result.result);
    Check(stored && !stored->record.initialPoses.empty() &&
          stored->record.initialPoses != std::vector<AlignmentMatrix>{alignmentIdentity},
          "published result records generated poses instead of default identity");
    const auto archive = f.feature->GetArchive(result.result);
    Check(archive && archive->algorithmVersion == "metrology-alignment-3" && archive->recipe.prealignment &&
          archive->recipe.prealignment->randomSeed == f.recipe.prealignment->randomSeed &&
          archive->recipe.surfaceFit->scales.size() == 2 && result.diagnostics.surfaceStages.size() == 2,
          "archive preserves algorithm version and repeatable automatic parameters");
    AlignmentRequest restore; restore.action = AlignmentAction::Restore;
    restore.archive = archive; restore.input = f.input; restore.restoredNominal = f.input.scopeData;
    Check(f.feature->SendRequest(restore).status == AlignmentAdmissionStatus::InvalidRequest,
          "automatic restore validates mapped reference surface before saving recipe");
    restore.restoredNominal = f.recipe.nominalData;
    if (restore.archive) restore.archive->algorithmVersion = "metrology-alignment-1";
    Check(f.feature->SendRequest(restore).status == AlignmentAdmissionStatus::InvalidRequest,
          "legacy archive version cannot claim automatic prealignment parameters");
    if (restore.archive) restore.archive->algorithmVersion = "metrology-alignment-2";
    Check(f.feature->SendRequest(restore).status == AlignmentAdmissionStatus::InvalidRequest,
          "version two archive cannot claim multiscale parameters");
    restore.archive = archive;
    Check(f.feature->SendRequest(restore).status == AlignmentAdmissionStatus::Accepted,
          "version three restores multiscale recipe");
    AlignmentRequest evaluate; evaluate.action = AlignmentAction::EvaluateSurface;
    evaluate.deviation = AlignmentDeviationRequest{};
    evaluate.deviation->alignmentResult = result.result;
    evaluate.deviation->maxDistance = .1;
    evaluate.deviation->tolerance = std::array<double,2>{0,1e-7};
    AlignmentResult evaluated; int completions = 0;
    const auto runEvaluation = [&] {
        Check(f.feature->SendRequest(evaluate, [&](AlignmentResult r) { evaluated = std::move(r); ++completions; }).status ==
              AlignmentAdmissionStatus::Accepted, "bound surface evaluation accepted");
    };
    runEvaluation();
    Check(f.feature->SendRequest(evaluate).status == AlignmentAdmissionStatus::Busy, "evaluation shares single worker admission");
    f.Drain();
    Check(completions == 1 && evaluated.status == AlignmentStatus::FullyDetermined &&
          GetDataRevisionRefValid(evaluated.deviationResult), "full field committed with one owner completion");
    const auto deviationRef = evaluated.deviationResult;
    const auto deviation = f.feature->GetDeviation(deviationRef);
    Check(deviation && deviation->isCurrent && deviation->alignmentResult == result.result &&
          deviation->sourceMesh == f.input.mesh && deviation->referenceMesh == f.recipe.nominalData &&
          deviation->verdict == AlignmentInspectionVerdict::Pass && deviation->forward->samples.size() == f.mesh->GetPointValues().size()/3,
          "dense evaluation independently records mesh/alignment/parameters and verdict");
    const auto again = f.feature->GetDeviation(deviationRef);
    Check(deviation && again && deviation->forward == again->forward && deviation->reverse == again->reverse && deviation->parameters == again->parameters,
          "result queries share immutable fields without full-array copy");
    MetrologyReportRequest reportRequest; reportRequest.id="deviation-report"; reportRequest.deviations={deviationRef};
    const auto reportCapture=f.feature->GetReport(reportRequest);
    Check(reportCapture.report && reportCapture.report->alignments.size()==1 && reportCapture.report->deviations.size()==1 &&
        reportCapture.report->deviations[0].forward==deviation->forward && reportCapture.report->deviations[0].reverse==deviation->reverse &&
        reportCapture.report->deviations[0].parameters==deviation->parameters,"report capture shares dense fields and evaluation selections");
    Check(f.feature->GetState().activeResult == result.result, "evaluation leaves alignment activation unchanged");
    AlignmentRequest display; display.action = AlignmentAction::SetDeviationDisplay;
    display.deviationDisplay = AlignmentDeviationDisplay{};
    display.deviationDisplay->result = deviationRef;display.deviationDisplay->rangeMode=AlignmentRangeMode::Manual; display.deviationDisplay->range = {0,.1};
    display.deviationDisplay->selectedVertex = 0;
    AlignmentResult displayed;
    const auto displayCommit = f.data->GetDataGraph().commitId;
    const auto show = [&] {
        Check(f.feature->SendRequest(display,[&](AlignmentResult r) { displayed = std::move(r); }).status ==
              AlignmentAdmissionStatus::Accepted,"deviation display request accepted");
    };
    show();
    Check(displayed.isDisplayReady && f.feature->GetState().isDeviationDisplayReady &&
          views->overlay->overlays.size() == 2 && f.data->GetDataGraph().commitId == displayCommit,
          "deviation colors coexist with alignment marker without republishing numerical data");
    const auto selected = f.feature->GetDeviationSample(deviationRef,0);
    Check(selected && selected->isCurrent && selected->sample.distance < 1e-7 &&
          std::abs(selected->alignedPoint[0]-model.referenceMesh->GetPointValues()[0]) < 1e-7,
          "selected deviation locates original sample and aligned reference-space point");
    Check(!f.feature->GetDeviationSample(deviationRef,f.mesh->GetPointValues().size()/3),"selection bounds checked");
    MetrologyReportRequest paletteRequest;paletteRequest.id="current-colorbar";paletteRequest.captureCurrentDisplay=true;
    const auto paletteCapture=f.feature->GetReport(paletteRequest);
    if(!paletteCapture.report)std::cerr<<"Palette capture failure: "<<int(paletteCapture.status)<<" "<<paletteCapture.message<<"\n";
    Check(paletteCapture.report && paletteCapture.report->deviations.size()==1 && paletteCapture.report->presentation.deviations.size()==1 &&
        paletteCapture.report->presentation.deviations[0].parameters.range==std::array<double,2>{0,.1} &&
        paletteCapture.report->presentation.deviations[0].selection && selected && paletteCapture.report->presentation.deviations[0].selection->sourcePoint==selected->sourcePoint &&
        paletteCapture.report->presentation.deviations[0].selection->alignedPoint==selected->alignedPoint,"colorbar capture freezes original point/sample and auto-includes its result");
    paletteRequest.captureCurrentDisplay=false;paletteRequest.deviationDisplays={*display.deviationDisplay};paletteRequest.deviationDisplays.push_back(*display.deviationDisplay);
    Check(f.feature->GetReport(paletteRequest).status==AlignmentStatus::InvalidInput,"duplicate colorbar ownership rejected");paletteRequest.deviationDisplays.pop_back();
    paletteRequest.deviationDisplays[0].range={0,0};Check(f.feature->GetReport(paletteRequest).status==AlignmentStatus::InvalidInput,"invalid report colorbar range rejected");

    const auto oldOverlay = views->overlay->overlays.back();
    show();
    Check(displayed.isDisplayReady && views->overlay->overlays.back()==oldOverlay,
          "identical display request reuses geometry, colors and overlay objects");
    display.deviationDisplay->range={0,.2};show();
    Check(displayed.isDisplayReady && views->overlay->overlays.back()==oldOverlay,"same-result recolor does not replace overlay bindings");
    display.deviationDisplay->range={0,.1};show();
    views->overlay->rejectsAttach=true;display.deviationDisplay->result=DataRevisionRef{};
    Check(f.feature->SendRequest(display).status==AlignmentAdmissionStatus::InvalidRequest,"invalid display target rejected before touching current props");
    views->overlay->rejectsAttach=false;display.deviationDisplay->result=deviationRef;
    display.deviationDisplay->range = {1,0};
    Check(f.feature->SendRequest(display).status == AlignmentAdmissionStatus::InvalidRequest &&
          views->overlay->overlays.back() == oldOverlay,"invalid color range leaves current display untouched");
    display.deviationDisplay->range = {0,.1}; display.deviationDisplay->isVisible = false; show();
    Check(!f.feature->GetState().isDeviationDisplayReady && views->overlay->overlays.size() == 2 &&
          f.data->GetDataGraph().commitId == displayCommit,"hiding deviation releases overlay without changing detection data");
    display.deviationDisplay->isVisible = true;
    views->overlay->onAttach = [&] {
        Check(f.feature->SendRequest(display).status == AlignmentAdmissionStatus::Busy && !f.feature->DetachHost(),
              "display attachment blocks reentrant mutation and detach until atomic replacement ends");
    };
    show(); views->overlay->onAttach = {};
    Check(f.feature->GetScopeState(f.input.scope).isDeviationDisplayReady &&
          !f.feature->GetScopeState("other-part").deviationDisplay,
          "deviation display state is scoped to its bound alignment part");
    display.deviationDisplay->rangeMode=AlignmentRangeMode::Result;show();
    const auto automaticDisplay=f.feature->GetState().deviationDisplay;
    Check(automaticDisplay && displayed.isDisplayReady && !automaticDisplay->useToleranceColors &&
          f.feature->GetState().deviationDisplayRange &&
          (*f.feature->GetState().deviationDisplayRange)[0]<=deviation->forward->statistics.minimum &&
          (*f.feature->GetState().deviationDisplayRange)[1]>=deviation->forward->statistics.maximum,
          "public default display resolves the current field range before storing visible state");
    paletteRequest.deviations.clear();paletteRequest.deviationDisplays.clear();paletteRequest.captureCurrentDisplay=true;
    const auto automaticCapture=f.feature->GetReport(paletteRequest);
    if(!automaticCapture.report)std::cerr<<"Automatic capture failure: "<<int(automaticCapture.status)<<" "<<automaticCapture.message<<"\n";
    Check(automaticDisplay && automaticCapture.report && !automaticCapture.report->presentation.deviations.empty() &&
          automaticCapture.report->presentation.deviations[0].mappingRange==f.feature->GetState().deviationDisplayRange,
          "capturing automatic display freezes the same resolved range as the live overlay");
    display.deviationDisplay->rangeMode=AlignmentRangeMode::Manual;display.deviationDisplay->range={0,.1};
    AlignmentConfig limitedConfig;
    auto limitedDisplay = std::make_shared<MetrologyAlignmentHostFeature>(limitedConfig);
    HostFeatureContext limitedContext; limitedContext.data=f.data; limitedContext.views=views;
    Check(limitedDisplay->AttachHost(limitedContext),"bounded display consumer attaches to existing numerical results");
    AlignmentResult limitedResult;
    Check(limitedDisplay->SendRequest(display,[&](AlignmentResult r) { limitedResult=std::move(r); }).status ==
          AlignmentAdmissionStatus::Accepted && limitedResult.isDisplayReady,"budget admits one set of display arrays");
    auto replacement = display; replacement.deviationDisplay->range[1]=.2;
    Check(limitedDisplay->SendRequest(replacement,[&](AlignmentResult r) { limitedResult=std::move(r); }).status ==
          AlignmentAdmissionStatus::Accepted && limitedResult.status == AlignmentStatus::FullyDetermined &&
          limitedDisplay->GetState().isDeviationDisplayReady,
          "display replacement has no byte budget rejection");
    Check(limitedDisplay->DetachHost() && views->overlay->overlays.size()==2,"bounded consumer removes only its own display");
    std::thread otherThread([&] {
        Check(f.feature->SendRequest(display).status == AlignmentAdmissionStatus::Unavailable &&
              !f.feature->GetDeviationSample(deviationRef,0),"display and selection reject non-owner calls");
    });
    otherThread.join();

    runEvaluation();
    AlignmentRequest cancel; cancel.action = AlignmentAction::Cancel;
    Check(f.feature->SendRequest(cancel).status == AlignmentAdmissionStatus::Accepted, "surface evaluation cancellation accepted");
    f.Drain();
    Check(evaluated.status == AlignmentStatus::Cancelled && !GetDataRevisionRefValid(evaluated.deviationResult),
          "cancelled evaluation publishes no partial field");
    f.data->rejectsCommit = true; runEvaluation(); f.Drain(); f.data->rejectsCommit = false;
    Check(evaluated.status == AlignmentStatus::InternalError && !GetDataRevisionRefValid(evaluated.deviationResult),
          "rejected evaluation commit exposes no result reference");
    f.data->throwsAfterCommit = true; runEvaluation(); f.Drain(); f.data->throwsAfterCommit = false;
    Check(evaluated.status == AlignmentStatus::FullyDetermined && f.feature->GetDeviation(evaluated.deviationResult).has_value(),
          "post-commit exception retains published evaluation fact");
    f.recipe.id = "unsupported-neighborhood"; f.recipe.prealignment->normalRadius = 1e-5;
    save();
    const auto commit = f.data->GetDataGraph().commitId;
    const auto failed = f.Solve();
    Check(failed.status == AlignmentStatus::NotConverged && !GetDataRevisionRefValid(failed.result) &&
          !GetDataRevisionRefValid(failed.transform) && f.data->GetDataGraph().commitId == commit,
          "failed coarse registration publishes no placeholder transform or result");
    Check(f.feature->GetState().activeResult == result.result && f.feature->GetState().isCurrent,
          "failed coarse registration preserves previous valid active result");
    runEvaluation();
    DataTransaction changed;
    changed.outputs.push_back({f.input.mesh.entityId, 1, DataTypes::surfaceMesh,
        {{"source", f.input.source}}, f.mesh, {}});
    Check(f.data->SetDataCommit(std::move(changed)).status == DataCommitStatus::Succeeded,
          "measurement revision advances while evaluation is pending");
    views->overlay->onRemove = [&] {
        AlignmentRequest hide = display; hide.deviationDisplay->isVisible = false;
        Check(f.feature->SendRequest(hide).status == AlignmentAdmissionStatus::Busy && !f.feature->DetachHost(),
              "stale display removal prevents recursive clear or detach through port callback");
    };
    f.Drain();
    views->overlay->onRemove = {};
    Check(evaluated.status == AlignmentStatus::Stale && !GetDataRevisionRefValid(evaluated.deviationResult),
          "input change prevents stale field publication");
    Check(!f.feature->GetState().isDeviationDisplayReady && views->overlay->overlays.empty(),
          "source revision invalidates and removes both dependent displays");
    show();
    Check(displayed.status == AlignmentStatus::Stale && !displayed.isDisplayReady,"stale deviation cannot be displayed");
    const auto staleSample = f.feature->GetDeviationSample(deviationRef,0);
    Check(staleSample && !staleSample->isCurrent,"historical selected value has explicit stale status");
    const auto historical = f.feature->GetDeviation(deviationRef);
    Check(historical && !historical->isCurrent && historical->forward == deviation->forward,
          "historical deviation stays readable with unchanged arrays and explicit stale state");
    Check(f.feature->SendRequest(evaluate).status == AlignmentAdmissionStatus::InvalidRequest,
          "new evaluation refuses stale alignment inputs");

}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--result-scope-lifecycle") == 0) {
        ResultScopeLifecycle();
        ScopeBindingAndUnscopedRecovery();
        ScopeAliasOwnership();
        ScopeAliasReceipts();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--owner-scope-limit") == 0) {
        OwnerScopedRepeatedAlignment();
        return failures ? 1 : 0;
    }
    const auto run = [](const char *name, auto test) { std::cerr << "CASE " << name << '\n'; test(); };
    run("InspectionLifecycle", [] { InspectionLifecycle(); });
    run("ReportLifecycle", ReportLifecycle);
    run("VoxelVolumeLifecycle", VoxelVolumeLifecycle);
    run("ModelPoseLifecycle", ModelPoseLifecycle);
    run("SceneLifecycle", SceneLifecycle);
    run("SectionInspectionLifecycle", [] { InspectionLifecycle(true); });
    run("AutomaticReferenceLifecycle", AutomaticReferenceLifecycle);
    run("SurfaceReferenceLifecycle", SurfaceReferenceLifecycle);
    run("WorkNotifications", WorkNotifications);
    run("Lifecycle", Lifecycle);
    run("DeepSourceGraph", DeepSourceGraph);
    run("Currentness", Currentness);
    run("Display", Display);
    run("Reentry", Reentry);
    run("ScopeRestoreAndCommitFault", ScopeRestoreAndCommitFault);
    return failures ? 1 : 0;
}
