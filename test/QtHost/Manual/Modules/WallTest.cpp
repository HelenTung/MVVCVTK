// 测试用途：通过公开 Feature 请求测试壁厚计算、评估、显示、结果激活与采样定位。
#include "ModuleFactories.h"
#include "Host/WallThicknessHostFeature.h"
#include <QPointer>
namespace Manual {
namespace {
QString Status(ThicknessStatus status)
{
    switch (status) {
    case ThicknessStatus::Succeeded: return "Succeeded";
    case ThicknessStatus::Cancelled: return "Cancelled";
    case ThicknessStatus::InvalidInput: return "InvalidInput";
    case ThicknessStatus::StaleInput: return "SourceChanged";
    case ThicknessStatus::DisplayFailed: return "SucceededWithDisplayFailure";
    default: return "Failed";
    }
}
std::size_t Count(const QJsonObject& p, const char* key, std::size_t limit)
{
    const auto n = GetNumber(p, key);
    if (n < 1 || std::trunc(n) != n || n > static_cast<double>(limit))
        throw std::invalid_argument(std::string(key) + " 必须为范围内的正整数");
    return static_cast<std::size_t>(n);
}
QJsonObject Statistics(const ThicknessSnapshot& result)
{
    const auto& s = result.statistics;
    return {{"result", GetRefText(result.result)}, {"isCurrent", result.isCurrent},
        {"sampleCount", QString::number(s.sampleCount)}, {"validCount", QString::number(s.validCount)},
        {"coverage", s.coverage}, {"minimum", s.minimum ? QJsonValue(*s.minimum) : QJsonValue()},
        {"maximum", s.maximum ? QJsonValue(*s.maximum) : QJsonValue()}, {"mean", s.mean ? QJsonValue(*s.mean) : QJsonValue()},
        {"regionCount", QString::number(result.regions.size())}};
}
ThicknessEvaluation Evaluation(const QJsonObject& p)
{
    ThicknessEvaluation value; value.lower = GetNumber(p, "lower"); value.upper = GetNumber(p, "upper");
    value.histogramRange = GetArray<double, 2>(p["histogramRange"]);
    value.histogramBins = Count(p, "histogramBins", 65536); value.minRegionArea = GetNumber(p, "minRegionArea");
    return value;
}
ThicknessDisplay Display(const QJsonObject& p)
{
    ThicknessDisplay value; value.targetViews = GetAllViews();
    value.mode = GetEnum<ThicknessDisplayMode>(p, "mode", {{"Continuous", ThicknessDisplayMode::Continuous}, {"Tolerance", ThicknessDisplayMode::Tolerance}});
    value.range = GetArray<double, 2>(p["range"]); value.opacity = GetNumber(p, "opacity");
    value.isVisible = GetBool(p, "isVisible"); value.hasLegend = GetBool(p, "hasLegend"); return value;
}
}
ModulePanel* CreateWallTest(TestContext context, std::shared_ptr<WallThicknessHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Wall", parent);
    panel->SetNotice("先完成零件分割及局部自适应/梯度峰值测量表面，再计算壁厚。默认长度参数按 0.1537 mm 体素设置，可按零件尺寸调整。");
    auto visible = std::make_shared<bool>(true);
    const auto resolveResult = [feature](const QJsonObject& p) {
        return GetText(p, "result") == "current" ? feature->GetState().result : GetRef(p["result"]);
    };
    const auto send = [panel, feature, visible](std::uint64_t id, ThicknessRequest request) {
        const auto display = request.display;
        const QPointer<ModulePanel> owner(panel);
        const auto admission = feature->SendRequest(std::move(request), [owner, feature, visible, display, id](const ThicknessResult& result) {
            if (!owner) return;
            if (display && result.status == ThicknessStatus::Succeeded) *visible = display->isVisible;
            QJsonObject info{{"requestId", QString::number(result.requestId)}, {"result", GetRefText(result.result)},
                {"statusCode", static_cast<int>(result.status)}, {"message", QString::fromStdString(result.message)},
                {"isActivated", result.isActivated}, {"isDisplayReady", result.isDisplayReady}};
            if (const auto snapshot = feature->GetResult(result.result)) {
                const auto stats = Statistics(*snapshot); for (auto it = stats.begin(); it != stats.end(); ++it) info[it.key()] = it.value();
            }
            owner->SetComplete(id, Status(result.status), info);
        });
        panel->SetAdmission(id, admission.status == ThicknessAdmissionStatus::Accepted,
            {{"requestId", QString::number(admission.requestId)}, {"admissionStatus", static_cast<int>(admission.status)}});
    };
    panel->AttachAction("Start", GetJson(R"({"source":"current","labels":"parts","mesh":"surface","materialLabel":"1","unit":"Millimeter",
        "maxDistance":15.37,"sampleSpacing":0.3074,"reverseTolerance":0.1537,"maxFitResidual":0.1,
        "maxLocalizationSigma":0.1537,"minSupportRatio":0.8,"coneAngleDegrees":10,"directionCount":9,
        "minOppositeCosine":0.5,"sharpNormalCosine":0.5,"ambiguityAbsolute":0.0,"ambiguityRelative":0.15,
        "maxBoundaryError":0.1537,"evaluationBounds":null})"), [panel, send](auto id, const auto& p) {
        const auto current = panel->GetSession()->GetImageDescriptor();
        if (!current) throw std::invalid_argument("请先加载体数据");
        ThicknessInput input;
        input.source = GetText(p, "source") == "current" ? current->dataRevision : GetRef(p["source"]);
        auto& workflow = panel->GetContext().workflow;
        input.labels = GetText(p, "labels") == "parts" ? (workflow.getPartLabels ? workflow.getPartLabels() : DataRevisionRef{}) : GetRef(p["labels"]);
        if (GetText(p, "mesh") == "surface") {
            if (workflow.GetSurfaceSource() != input.source) throw std::invalid_argument("请先生成同一输入的有效测量表面");
            input.mesh = workflow.GetSurfaceMesh();
        } else input.mesh = GetRef(p["mesh"]);
        if (!GetDataRevisionRefValid(input.labels) || !GetDataRevisionRefValid(input.mesh))
            throw std::invalid_argument("需要当前零件标签图和正式测量表面；预览网格不能用于壁厚测量");
        input.materialLabel = GetId(p["materialLabel"]);
        input.unit = GetEnum<ThicknessUnit>(p, "unit", {{"Millimeter", ThicknessUnit::Millimeter}, {"Meter", ThicknessUnit::Meter}});
        ThicknessParams params;
        params.maxDistance = GetNumber(p, "maxDistance"); params.sampleSpacing = GetNumber(p, "sampleSpacing");
        params.reverseTolerance = GetNumber(p, "reverseTolerance"); params.maxFitResidual = GetNumber(p, "maxFitResidual");
        params.maxLocalizationSigma = GetNumber(p, "maxLocalizationSigma"); params.minSupportRatio = GetNumber(p, "minSupportRatio");
        params.coneAngleDegrees = GetNumber(p, "coneAngleDegrees"); params.directionCount = static_cast<std::uint32_t>(Count(p, "directionCount", 4096));
        params.minOppositeCosine = GetNumber(p, "minOppositeCosine"); params.sharpNormalCosine = GetNumber(p, "sharpNormalCosine");
        params.ambiguityAbsolute = GetNumber(p, "ambiguityAbsolute"); params.ambiguityRelative = GetNumber(p, "ambiguityRelative");
        params.maxBoundaryError = GetNumber(p, "maxBoundaryError");
        if (!p["evaluationBounds"].isNull()) params.evaluationBounds = GetArray<double, 6>(p["evaluationBounds"]);
        ThicknessRequest request; request.action = ThicknessAction::Start; request.input = input; request.params = params;
        ThicknessDisplay display; display.targetViews = GetAllViews(); display.range = {0, params.maxDistance}; request.display = display;
        ThicknessEvaluation evaluation; evaluation.upper = params.maxDistance; evaluation.histogramRange = display.range; request.evaluation = evaluation;
        send(id, std::move(request));
    }, TestPolicy::Compute, true);
    panel->AttachAction("Cancel", {{"targetRequestId", "0"}}, [send](auto id, const auto& p) {
        ThicknessRequest r; r.action = ThicknessAction::Cancel; r.targetRequestId = GetId(p["targetRequestId"]); send(id, r);
    }, TestPolicy::Stop);
    panel->AttachAction("Clear", {}, [send](auto id, const auto&) {
        ThicknessRequest r; r.action = ThicknessAction::Clear; send(id, r);
    });
    panel->AttachAction("SetEvaluation", GetJson(R"({"lower":0.3074,"upper":3.074,"histogramRange":[0,15.37],"histogramBins":32,"minRegionArea":0})"),
        [send](auto id, const auto& p) { ThicknessRequest r; r.action = ThicknessAction::SetEvaluation; r.evaluation = Evaluation(p); send(id, r); }, TestPolicy::Compute);
    panel->AttachAction("SetDisplay", GetJson(R"({"mode":"Continuous","range":[0,15.37],"opacity":1,"isVisible":true,"hasLegend":true})"),
        [send](auto id, const auto& p) { ThicknessRequest r; r.action = ThicknessAction::SetDisplay; r.display = Display(p); send(id, r); }, TestPolicy::View);
    panel->AttachAction("SetActive", {{"result", "current"}}, [send, resolveResult](auto id, const auto& p) {
        ThicknessRequest r; r.action = ThicknessAction::SetActive; r.resultRevision = resolveResult(p); send(id, r);
    });
    panel->AttachAction("SelectSample", {{"result", "current"}, {"sampleIndex", "0"}}, [send, resolveResult](auto id, const auto& p) {
        ThicknessRequest r; r.action = ThicknessAction::SelectSample; r.resultRevision = resolveResult(p); r.sampleIndex = GetId(p["sampleIndex"]); send(id, r);
    }, TestPolicy::View);
    panel->AttachAction("Result", {{"result", "current"}}, [panel, feature, resolveResult](auto id, const auto& p) {
        const auto result = feature->GetResult(resolveResult(p));
        if (!result) throw std::invalid_argument("所选壁厚结果不可用");
        panel->SetComplete(id, "Observed", Statistics(*result));
    }, TestPolicy::Read);
    panel->onObserve = [panel, feature, visible] {
        const auto state = feature->GetState();
        QJsonObject summary{{"isBusy", state.isBusy}, {"isCurrent", state.isCurrent}, {"requestId", QString::number(state.requestId)},
            {"result", GetRefText(state.result)}, {"isDisplayReady", state.isDisplayReady}, {"isVisible", *visible}};
        const auto result = feature->GetResult(state.result); summary["hasResult"] = result.has_value();
        if (result) { const auto stats = Statistics(*result); for (auto it = stats.begin(); it != stats.end(); ++it) summary[it.key()] = it.value(); }
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Cancel", {{"targetRequestId", "0"}}); };
    return panel;
}
}
