#include "../TestDataPort.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include "AlignmentData.h"
#include "AlignmentMath.h"
#include "AlignmentOverlay.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkPolyData.h>
#include <chrono>
#include <atomic>
#include <iostream>
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
    std::vector<std::shared_ptr<FeatureOverlay>> overlays;
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> value) override {
        overlays.push_back(value);
        return !rejectsAttach;
    }
    void RemoveOverlay(std::shared_ptr<FeatureOverlay> value) noexcept override {
        overlays.erase(std::remove(overlays.begin(), overlays.end(), value), overlays.end());
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
        Check(!feature->GetState().isBusy, "bounded task completion");
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
    Check(contract.inputs.size() == 2 && contract.outputs.size() == 5,
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
} // namespace
int main() {
    WorkNotifications();
    Lifecycle();
    Currentness();
    Display();
    Reentry();
    ScopeRestoreAndCommitFault();
    return failures ? 1 : 0;
}
