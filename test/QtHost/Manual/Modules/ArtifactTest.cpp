// 测试用途：通过伪影页面测试环形校正、扩散、候选发布与校正数据选择。
#include "ModuleFactories.h"
#include "Host/ArtifactReductionHostFeature.h"
namespace Manual {
namespace {
QString GetArtifactErrorText(ArtifactError error)
{
    switch (error) {
    case ArtifactError::None: return {};
    case ArtifactError::Unavailable: return "伪影功能当前不可用";
    case ArtifactError::WrongThread: return "请求未在会话所属线程执行";
    case ArtifactError::Busy: return "已有伪影任务或候选，请先完成、发布或丢弃";
    case ArtifactError::InvalidRequest: return "伪影参数无效，请检查灰度范围、阈值、迭代次数和超时";
    case ArtifactError::InvalidData: return "输入数据或掩码网格不匹配";
    case ArtifactError::UnsupportedType: return "当前数据类型不支持伪影校正";
    case ArtifactError::SourceChanged: return "计算期间源数据已变化";
    case ArtifactError::TooLarge: return "工作集或累计发布量超过本次测试预算；请检查本次资源上限，累计发布过多时需重新启动测试会话释放历史数据";
    case ArtifactError::Cancelled: return "伪影任务已取消";
    case ArtifactError::TimedOut: return "伪影计算超过所设超时";
    case ArtifactError::InsufficientEvidence: return "校正质量检查未通过，不能发布候选";
    case ArtifactError::CommitFailed: return "校正结果发布失败";
    case ArtifactError::UnsupportedGeometry: return "环形几何不受支持：中心应位于截面内部，并为环宽保留足够半径";
    case ArtifactError::UnsupportedValidity: return "当前有效性掩码不受支持";
    case ArtifactError::KernelFailed: return "伪影算法内核执行失败";
    }
    return "未知伪影错误";
}
QJsonObject GetArtifact(const ArtifactState& state)
{
    const auto& q = state.quality;
    return {{"status", static_cast<int>(state.status)}, {"errorCode", static_cast<int>(state.error)}, {"message", GetArtifactErrorText(state.error)},
        {"requestId", QString::number(state.requestId)}, {"progressPercent", static_cast<int>(state.progressPercent)},
        {"requiredBytes", QString::number(state.requiredBytes)}, {"publishedBytes", QString::number(state.publishedBytes)},
        {"commitStatus", static_cast<int>(state.commitStatus)},
        {"correctedVolume", state.correctedVolume ? GetRefText(*state.correctedVolume) : QString()},
        {"qualityReport", state.qualityReport ? GetRefText(*state.qualityReport) : QString()},
        {"quality", QJsonObject{{"validCount", QString::number(q.validCount)}, {"changedCount", QString::number(q.changedCount)},
            {"meanBefore", q.meanBefore}, {"meanAfter", q.meanAfter}, {"meanDelta", q.meanDelta},
            {"rmsDelta", q.rmsDelta}, {"maxDelta", q.maxDelta}, {"materialCount", QString::number(q.materialCount)},
            {"hasMaterial", q.hasMaterial}, {"materialMeanBefore", q.materialMeanBefore}, {"materialMeanAfter", q.materialMeanAfter},
            {"materialStdBefore", q.materialStdBefore}, {"materialStdAfter", q.materialStdAfter},
            {"gradientDeltaRms", q.gradientDeltaRms}, {"ringCorrectionRms", q.ringCorrectionRms}, {"fidelityVerified", q.fidelityVerified}}}};
}
int GetInt(const QJsonObject& p, const char* key)
{
    const auto value = GetNumber(p, key);
    if (value != std::trunc(value) || value < 0 || value > 1000000) throw std::invalid_argument("整数参数越界");
    return static_cast<int>(value);
}
}
ModulePanel* CreateArtifactTest(TestContext context, std::shared_ptr<ArtifactReductionHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Artifact", parent);
    panel->SetNotice("处理轴使用网格轴，环形中心使用截面索引。计算候选 → 发布校正结果 → 使用校正数据。灰度统计变化不能单独证明校正保真。");
    struct Pending { std::uint64_t operation = 0; DataRevisionRef source; DataRevisionRef output; };
    auto pending = std::make_shared<Pending>();
    const auto defaults = GetJson(R"({"source":"current","processingMask":null,"protectionMask":null,"materialMask":null,"timeoutMs":600000,"ring":{"axis":2,"centerIndex":null,"threshMin":0,"threshMax":1,"threshold":1,"angularMin":30,"ringWidth":3,"mode":"Wrap","strength":1,"maxCorrection":1},"diffusion":{"iterations":4,"threshold":1,"factor":0.25,"slabDepth":8}})");
    for (const QString combination : {QString("Ring"), QString("Diffusion"), QString("Combined")}) {
        auto parameters = defaults;
        if (combination == "Ring") parameters.remove("diffusion");
        if (combination == "Diffusion") parameters.remove("ring");
        panel->AttachAction(combination, parameters, [panel, feature, pending, combination](auto id, const auto& p) {
            ArtifactRequest prepare;
            if (GetText(p, "source") == "current") {
                const auto descriptor = panel->GetSession()->GetImageDescriptor();
                if (!descriptor) throw std::invalid_argument("未加载输入");
                prepare.source = descriptor->dataRevision;
            } else { prepare.source = GetRef(p["source"]); prepare.inputMode = ArtifactInputMode::ExplicitRevision; }
            if (!p["processingMask"].isNull()) prepare.processingMask = GetRef(p["processingMask"]);
            if (!p["protectionMask"].isNull()) prepare.protectionMask = GetRef(p["protectionMask"]);
            if (!p["materialMask"].isNull()) prepare.materialMask = GetRef(p["materialMask"]);
            prepare.timeoutMs = GetInt(p, "timeoutMs");
            if (combination != "Diffusion") {
                const auto r = p["ring"].toObject();
                ArtifactRingParams ring;
                ring.axis = GetInt(r, "axis");
                if (ring.axis > 2) throw std::invalid_argument("处理轴必须为 0、1 或 2");
                if (r["centerIndex"].isNull()) {
                    const auto input = panel->GetSession()->GetImageDescriptor();
                    if (!input || input->dataRevision != prepare.source) throw std::invalid_argument("自动中心需要先将该源数据选为当前输入，或显式提供截面中心");
                    int coordinate = 0;
                    for (int axis = 0; axis < 3; ++axis) if (axis != ring.axis) ring.centerIndex[coordinate++] = (static_cast<double>(input->extent[axis*2]) + input->extent[axis*2+1])*0.5;
                } else ring.centerIndex = GetArray<double, 2>(r["centerIndex"]);
                ring.threshMin = GetNumber(r, "threshMin"); ring.threshMax = GetNumber(r, "threshMax");
                ring.threshold = GetNumber(r, "threshold"); ring.angularMin = GetInt(r, "angularMin");
                ring.ringWidth = GetInt(r, "ringWidth"); ring.strength = GetNumber(r, "strength");
                ring.maxCorrection = GetNumber(r, "maxCorrection");
                ring.mode = GetEnum<ArtifactRingMode>(r, "mode", {{"Wrap", ArtifactRingMode::Wrap}, {"Reflect", ArtifactRingMode::Reflect}});
                prepare.ring = ring;
            }
            if (combination != "Ring") {
                const auto d = p["diffusion"].toObject();
                prepare.diffusion = ArtifactDiffusionParams{GetInt(d, "iterations"), GetNumber(d, "threshold"), GetNumber(d, "factor"), GetInt(d, "slabDepth")};
            }
            ArtifactHostRequest request; request.action = ArtifactAction::Prepare; request.prepare = prepare;
            QJsonObject detail{{"workingBudgetBytes", QString::number(panel->GetContext().workflow.resources.workingBytes)}, {"publishBudgetBytes", QString::number(panel->GetContext().workflow.resources.publishBytes)}};
            if (prepare.ring) detail["ringCenterIndex"] = GetValues(prepare.ring->centerIndex);
            const auto admission = feature->SendRequest(std::move(request));
            if (admission.error == ArtifactError::None) { pending->operation = id; pending->source = prepare.source; pending->output = {}; }
            detail["errorCode"] = static_cast<int>(admission.error); detail["message"] = GetArtifactErrorText(admission.error);
            detail["requestId"] = QString::number(admission.requestId);
            panel->SetAdmission(id, admission.error == ArtifactError::None, detail);
        }, TestPolicy::Compute, true);
    }
    for (const auto& action : std::vector<std::pair<QString, ArtifactAction>>{
        {"Cancel", ArtifactAction::Cancel}, {"Discard", ArtifactAction::Discard}, {"Commit", ArtifactAction::Commit}}) {
        panel->AttachAction(action.first, action.second == ArtifactAction::Commit ? QJsonObject{{"requestId", "current"}} : QJsonObject{},
            [panel, feature, pending, action](auto id, const auto& p) {
                ArtifactHostRequest request; request.action = action.second;
                if (request.action == ArtifactAction::Commit)
                    request.requestId = GetText(p, "requestId") == "current" ? feature->GetState().requestId : GetId(p["requestId"]);
                const auto admission = feature->SendRequest(request);
                const auto state = feature->GetState();
                if (admission.error == ArtifactError::None && request.action == ArtifactAction::Commit && state.correctedVolume)
                    pending->output = *state.correctedVolume;
                auto result = GetArtifact(state);
                if (admission.error != ArtifactError::None) { result["errorCode"] = static_cast<int>(admission.error); result["message"] = GetArtifactErrorText(admission.error); }
                panel->SetComplete(id, admission.error != ArtifactError::None ? "Rejected"
                    : request.action == ArtifactAction::Commit ? "Published" : "Accepted", result);
            }, action.second == ArtifactAction::Cancel ? TestPolicy::Stop : TestPolicy::Change);
    }
    for (const bool restore : {false, true}) panel->AttachAction(restore ? "RestoreSource" : "SelectOutput", {},
        [panel, pending, restore](auto id, const auto&) {
            const auto ref = restore ? pending->source : pending->output;
            if (!GetDataRevisionRefValid(ref)) throw std::invalid_argument("当前没有可选择的修订");
            panel->SetInput(id, ref);
        }, TestPolicy::Input, true);
    panel->onObserve = [panel, feature, pending] {
        const auto state = feature->GetState();
        auto summary = GetArtifact(state);
        const auto input = panel->GetSession()->GetImageDescriptor();
        summary["isBusy"] = state.status == ArtifactStatus::Running || state.status == ArtifactStatus::Cancelling;
        summary["hasCandidate"] = state.status == ArtifactStatus::Ready;
        summary["isCandidateCurrent"] = input && input->dataRevision == pending->source;
        summary["source"] = GetRefText(pending->source); summary["output"] = GetRefText(pending->output);
        summary["canSelectOutput"] = input && GetDataRevisionRefValid(pending->output) && input->dataRevision == pending->source;
        summary["isOutputCurrent"] = input && GetDataRevisionRefValid(pending->output) && input->dataRevision == pending->output;
        if (pending->operation && state.status != ArtifactStatus::Running && state.status != ArtifactStatus::Cancelling) {
            panel->SetComplete(pending->operation, state.status == ArtifactStatus::Ready ? "Ready"
                : state.error == ArtifactError::Cancelled ? "Cancelled" : "Failed", summary);
            pending->operation = 0;
        }
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Cancel", {}); };
    return panel;
}
}
