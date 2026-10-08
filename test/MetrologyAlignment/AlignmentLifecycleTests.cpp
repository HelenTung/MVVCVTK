// 测试用途：验证计量对齐的生命周期、输入当前性、显示、重入、故障恢复与工作通知。
#include "../TestDataPort.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include "AlignmentData.h"
#include "AlignmentMath.h"
#include "AlignmentOverlay.h"
#include "AlignmentPrealignTestSupport.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkPolyData.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <thread>

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
    std::function<void()> onCommit;
    DataCommitResult SetDataCommit(DataTransaction tx) override {
        if (rejectsCommit)
            return {};
        auto r = TestDataPort::SetDataCommit(std::move(tx));
        if (onCommit)
            onCommit();
        if (throwsAfterCommit)
            throw std::runtime_error("after commit");
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
struct Fixture final {
    std::shared_ptr<FaultDataPort> data = std::make_shared<FaultDataPort>();
    std::shared_ptr<MetrologyAlignmentHostFeature> feature =
        std::make_shared<MetrologyAlignmentHostFeature>();
    AlignmentInput input;
    AlignmentRecipe recipe;
    DataRevisionRef recipeRef;
    std::shared_ptr<const SurfaceMeshPayload> mesh;
    Fixture(std::shared_ptr<Views> views = {}, std::shared_ptr<FeatureHostControl> host = {}) {
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
        mesh = std::make_shared<const SurfaceMeshPayload>(vertices, std::vector<std::uint64_t>{});
        DataTransaction tx;
        for (const auto ref : {input.source, input.scopeData, nominal})
            tx.outputs.push_back({ref.entityId, 0, DataTypes::recordTable, {}, table, {}});
        tx.outputs.push_back(
            {input.mesh.entityId, 0, DataTypes::surfaceMesh, {{"source", input.source}}, mesh, {}});
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
void Lifecycle() {
    Fixture f;
    const auto contract = f.feature->GetDataContract();
    Check(contract.inputs.size() == 2 && contract.outputs.size() == 8,
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
    const std::vector<double> modelVertices{1,2,3};
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
    Check(f.mesh->GetVertices() == measured && reference->GetVertices() == referenceVertices,
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
          deviation->verdict == AlignmentInspectionVerdict::Pass && deviation->forward->samples.size() == f.mesh->GetVertices().size()/3,
          "dense evaluation independently records mesh/alignment/parameters and verdict");
    const auto again = f.feature->GetDeviation(deviationRef);
    Check(deviation && again && deviation->forward == again->forward && deviation->reverse == again->reverse && deviation->parameters == again->parameters,
          "result queries share immutable fields without full-array copy");
    Check(f.feature->GetState().activeResult == result.result, "evaluation leaves alignment activation unchanged");
    AlignmentRequest display; display.action = AlignmentAction::SetDeviationDisplay;
    display.deviationDisplay = AlignmentDeviationDisplay{};
    display.deviationDisplay->result = deviationRef; display.deviationDisplay->range = {0,.1};
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
          std::abs(selected->alignedPoint[0]-model.referenceMesh->GetVertices()[0]) < 1e-7,
          "selected deviation locates original sample and aligned reference-space point");
    Check(!f.feature->GetDeviationSample(deviationRef,f.mesh->GetVertices().size()/3),"selection bounds checked");
    const auto oldOverlay = views->overlay->overlays.back();
    show();
    Check(displayed.isDisplayReady && views->overlay->overlays.back()==oldOverlay,
          "identical display request reuses geometry, colors and overlay objects");
    views->overlay->rejectsAttach = true; display.deviationDisplay->range = {0,.2}; show(); views->overlay->rejectsAttach = false;
    Check(displayed.status == AlignmentStatus::InternalError && views->overlay->overlays.size() == 2 &&
          views->overlay->overlays.back() == oldOverlay && f.feature->GetState().isDeviationDisplayReady &&
          f.feature->GetState().deviationDisplay->range[1] == .1,
          "failed display replacement preserves previous complete overlay and settings");
    views->overlay->onAttach = [] { throw 42; }; show(); views->overlay->onAttach = {};
    Check(displayed.status == AlignmentStatus::InternalError && views->overlay->overlays.size() == 2 &&
          views->overlay->overlays.back() == oldOverlay,"nonstandard port exception rolls back partial display attachment");
    display.deviationDisplay->range = {1,0};
    Check(f.feature->SendRequest(display).status == AlignmentAdmissionStatus::InvalidRequest &&
          views->overlay->overlays.back() == oldOverlay,"invalid color range leaves current display untouched");
    display.deviationDisplay->range = {0,.1}; display.deviationDisplay->isVisible = false; show();
    Check(!f.feature->GetState().isDeviationDisplayReady && views->overlay->overlays.size() == 1 &&
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
    AlignmentConfig limitedConfig; limitedConfig.workingBytes = 100000;
    auto limitedDisplay = std::make_shared<MetrologyAlignmentHostFeature>(limitedConfig);
    HostFeatureContext limitedContext; limitedContext.data=f.data; limitedContext.views=views;
    Check(limitedDisplay->AttachHost(limitedContext),"bounded display consumer attaches to existing numerical results");
    AlignmentResult limitedResult;
    Check(limitedDisplay->SendRequest(display,[&](AlignmentResult r) { limitedResult=std::move(r); }).status ==
          AlignmentAdmissionStatus::Accepted && limitedResult.isDisplayReady,"budget admits one set of display arrays");
    auto replacement = display; replacement.deviationDisplay->range[1]=.2;
    Check(limitedDisplay->SendRequest(replacement,[&](AlignmentResult r) { limitedResult=std::move(r); }).status ==
          AlignmentAdmissionStatus::Accepted && limitedResult.status == AlignmentStatus::BudgetExceeded &&
          limitedDisplay->GetState().isDeviationDisplayReady,
          "replacement charges simultaneously retained old arrays and preserves old display on budget failure");
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
int main() {
    const auto run = [](const char *name, auto test) { std::cerr << "CASE " << name << '\n'; test(); };
    run("AutomaticReferenceLifecycle", AutomaticReferenceLifecycle);
    run("SurfaceReferenceLifecycle", SurfaceReferenceLifecycle);
    run("WorkNotifications", WorkNotifications);
    run("Lifecycle", Lifecycle);
    run("Currentness", Currentness);
    run("Display", Display);
    run("Reentry", Reentry);
    run("ScopeRestoreAndCommitFault", ScopeRestoreAndCommitFault);
    return failures ? 1 : 0;
}
