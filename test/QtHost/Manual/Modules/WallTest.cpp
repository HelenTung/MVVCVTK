// 测试用途：通过公开 Feature 请求测试壁厚计算、评估、显示、结果激活与采样定位。
#include "ModuleFactories.h"
#include "Support/ColorSegmentInput.h"
#include "Support/ParameterEditor.h"
#include "Host/WallThicknessHostFeature.h"
#include <QPointer>
#include <QCryptographicHash>
#include <QFile>
#include <QTextStream>
#include <algorithm>
#include <limits>
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
        {"unmeasuredCount",QString::number(s.sampleCount-s.validCount)},
        {"algorithmVersion",QString::fromStdString(result.archive.algorithmVersion)},
        {"method","局部灰度射线共同法线测量"},{"hasPartialCoverage",s.coverage<1.0},
        {"coverage", s.coverage}, {"minimum", s.minimum ? QJsonValue(*s.minimum) : QJsonValue()},
        {"minimumSampleIndex", s.minimumSample ? QJsonValue(QString::number(*s.minimumSample)) : QJsonValue()},
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
    if (p.contains("range")) value.range = GetArray<double, 2>(p["range"]);
    value.opacity = GetNumber(p, "opacity");
    if (p.contains("rangeMode")) value.rangeMode = GetEnum<ThicknessRangeMode>(p, "rangeMode", {
        {"Manual", ThicknessRangeMode::Manual}, {"Result", ThicknessRangeMode::Result}, {"Histogram", ThicknessRangeMode::Histogram}});
    if (p.contains("palette")) value.colorBand.mode = GetEnum<ThicknessColorMode>(p, "palette", {
        {"Constant", ThicknessColorMode::Constant}, {"Gradient", ThicknessColorMode::Gradient},
        {"Rainbow", ThicknessColorMode::Rainbow}, {"InverseRainbow", ThicknessColorMode::InverseRainbow}, {"HueLoop", ThicknessColorMode::HueLoop}});
    if (p.contains("constantColor") && !p["constantColor"].isNull()) value.colorBand.constantColor = GetArray<double, 3>(p["constantColor"]);
    if (p.contains("lowColor") && !p["lowColor"].isNull()) value.colorBand.lowColor = GetArray<double, 3>(p["lowColor"]);
    if (p.contains("highColor") && !p["highColor"].isNull()) value.colorBand.highColor = GetArray<double, 3>(p["highColor"]);
    if (p.contains("belowColor") && !p["belowColor"].isNull()) value.colorBand.belowColor = GetArray<double, 3>(p["belowColor"]);
    if (p.contains("aboveColor") && !p["aboveColor"].isNull()) value.colorBand.aboveColor = GetArray<double, 3>(p["aboveColor"]);
    value.colorBand.segments=GetColorSegments<ThicknessColorSegment,ThicknessColorMode>(p,{
        {"Constant",ThicknessColorMode::Constant},{"Gradient",ThicknessColorMode::Gradient},{"Rainbow",ThicknessColorMode::Rainbow},
        {"InverseRainbow",ThicknessColorMode::InverseRainbow},{"HueLoop",ThicknessColorMode::HueLoop}});
    if(p.contains("displayStyle"))value.style=GetEnum<ThicknessDisplayStyle>(p,"displayStyle",{
        {"Overlay",ThicknessDisplayStyle::Overlay},{"Constant",ThicknessDisplayStyle::Constant},
        {"Inclined",ThicknessDisplayStyle::Inclined},{"InverseInclined",ThicknessDisplayStyle::InverseInclined}});
    if(p.contains("opacityRange"))value.opacityRange=GetArray<double,2>(p["opacityRange"]);
    value.isVisible = GetBool(p, "isVisible"); value.hasLegend = GetBool(p, "hasLegend"); return value;
}
}
ModulePanel* CreateWallTest(TestContext context, std::shared_ptr<WallThicknessHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Wall", parent);
    panel->SetNotice("CUDA 并行计算原始灰度场中的局部壁厚。测量采用固定九点积分；显示按各三角形局部边长细分，不受全网格最大边长影响。网格无需整体闭合或流形，缺支撑区域保留未测并计入覆盖率。可选择已有输入与结果；长度单位为 mm。");
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
            if (const auto range = feature->GetState().displayRange) info["displayRange"] = GetValues(*range);
            if (const auto snapshot = feature->GetResult(result.result)) {
                const auto stats = Statistics(*snapshot); for (auto it = stats.begin(); it != stats.end(); ++it) info[it.key()] = it.value();
            }
            owner->SetComplete(id, Status(result.status), info);
        });
        panel->SetAdmission(id, admission.status == ThicknessAdmissionStatus::Accepted,
            {{"requestId", QString::number(admission.requestId)}, {"admissionStatus", static_cast<int>(admission.status)}});
    };
    panel->AttachAction("Start", GetJson(R"({"source":"current","labels":"parts","mesh":"surface","materialLabels":[],"unit":"Millimeter",
        "maxDistance":null,"sampleSpacing":null,"materialThreshold":null,
        "coneAngleDegrees":30,"directionCount":9,"boundaryPolicy":"SourceExtentLocal",
        "maxBoundaryError":null,"evaluationBounds":null})"), [panel, send](auto id, const auto& p) {
        const auto current = panel->GetSession()->GetImageDescriptor();
        if (!current) throw std::invalid_argument("请先加载体数据");
        ThicknessInput input;
        input.source = GetText(p, "source") == "current" ? current->dataRevision : GetRef(p["source"]);
        auto& workflow = panel->GetContext().workflow;
        const auto source = workflow.getImageInput ? workflow.getImageInput(input.source) : current;
        if (!source) throw std::invalid_argument("所选体数据输入不可用");
        input.labels = GetText(p, "labels") == "parts" ? (workflow.getPartLabels ? workflow.getPartLabels() : DataRevisionRef{}) : GetRef(p["labels"]);
        if (GetText(p, "mesh") == "surface") {
            if (workflow.GetSurfaceSource() != input.source) throw std::invalid_argument("请先生成同一输入的有效测量表面");
            input.mesh = workflow.GetSurfaceMesh();
        } else input.mesh = GetRef(p["mesh"]);
        if (!GetDataRevisionRefValid(input.labels) || !GetDataRevisionRefValid(input.mesh))
            throw std::invalid_argument("请选择可用材料标签图和同一源的采样网格；输入需使用已发布的数据修订");
        for (const auto label : p["materialLabels"].toArray()) input.materialLabels.push_back(GetId(label));
        input.unit = GetEnum<ThicknessUnit>(p, "unit", {{"Millimeter", ThicknessUnit::Millimeter}, {"Meter", ThicknessUnit::Meter}});
        if (input.unit != ThicknessUnit::Millimeter) throw std::invalid_argument("Host 图像和网格使用毫米几何；此参数声明源单位，不执行显示单位换算");
        ThicknessParams params;
        params.maxDistance = GetInputNumber(p,"maxDistance",GetInputDiagonal(*source));
        params.sampleSpacing = GetInputNumber(p,"sampleSpacing",2.0 * GetVoxelSpacing(*source));
        params.materialThreshold = p["materialThreshold"].isNull()
            ? (input.source == workflow.GetSurfaceSource() && input.mesh == workflow.GetSurfaceMesh() && workflow.GetSurfaceThreshold()
                ? workflow.GetSurfaceThreshold() : std::optional<double>{GetScalarMidpoint(*source)})
            : std::optional<double>{GetNumber(p, "materialThreshold")};
        if (!params.materialThreshold) throw std::invalid_argument("请显式提供原始灰度材料阈值，或选择当前测量表面");
        params.coneAngleDegrees = GetNumber(p, "coneAngleDegrees"); params.directionCount = static_cast<std::uint32_t>(Count(p, "directionCount", 4096));
        params.boundaryPolicy = GetEnum<ThicknessBoundaryPolicy>(p, "boundaryPolicy", {
            {"Complete", ThicknessBoundaryPolicy::Complete}, {"SourceExtentLocal", ThicknessBoundaryPolicy::SourceExtentLocal}});
        params.maxBoundaryError = GetInputNumber(p,"maxBoundaryError",0.5 * GetVoxelSpacing(*source));
        if (!p["evaluationBounds"].isNull()) params.evaluationBounds = GetArray<double, 6>(p["evaluationBounds"]);
        ThicknessRequest request; request.action = ThicknessAction::Start; request.input = input; request.params = params;
        ThicknessDisplay display; display.targetViews = GetAllViews(); display.range = {0, params.maxDistance};
        display.rangeMode = ThicknessRangeMode::Result; request.display = display;
        ThicknessEvaluation evaluation; evaluation.upper = params.maxDistance; evaluation.histogramRange = display.range; request.evaluation = evaluation;
        send(id, std::move(request));
    }, TestPolicy::Compute, true);
    panel->AttachAction("Cancel", {{"targetRequestId", "0"}}, [send](auto id, const auto& p) {
        ThicknessRequest r; r.action = ThicknessAction::Cancel; r.targetRequestId = GetId(p["targetRequestId"]); send(id, r);
    }, TestPolicy::Stop);
    panel->AttachAction("Clear", {}, [send](auto id, const auto&) {
        ThicknessRequest r; r.action = ThicknessAction::Clear; send(id, r);
    });
    panel->AttachAction("SetEvaluation", GetJson(R"({"lower":0,"upper":null,"histogramRange":null,"histogramBins":32,"minRegionArea":0})"),
        [feature, send](auto id, const auto& p) {
            const auto result = feature->GetResult(feature->GetState().result);
            if (!result || !result->statistics.maximum) throw std::invalid_argument("所选结果没有可评估的有效厚度值");
            auto parameters = p;
            parameters["upper"] = GetInputNumber(p,"upper",std::max(*result->statistics.maximum,std::numeric_limits<double>::epsilon()));
            if (p["histogramRange"].isNull()) parameters["histogramRange"] = QJsonArray{0.,parameters["upper"]};
            ThicknessRequest r; r.action = ThicknessAction::SetEvaluation; r.evaluation = Evaluation(parameters); send(id,r);
        }, TestPolicy::Compute);
    panel->AttachAction("SetDisplay", GetJson(R"({"mode":"Continuous","rangeMode":"Result","range":[0,1],"palette":"InverseRainbow",
        "constantColor":null,"lowColor":null,"highColor":null,"belowColor":null,"aboveColor":null,
        "segments":[],"displayStyle":"Overlay","opacityRange":[0.15,1],
        "opacity":1,"isVisible":true,"hasLegend":true})"),
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
    panel->AttachAction("ResultEvidence",{{"result","current"}},[panel,feature,resolveResult](auto id,const auto& p) {
        const auto snapshot=feature->GetResult(resolveResult(p));
        if(!snapshot || !snapshot->samples)throw std::invalid_argument("没有壁厚采样结果");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const auto add=[&](const auto& value){hash.addData(reinterpret_cast<const char*>(&value),sizeof(value));};
        for(const auto& sample:*snapshot->samples) {
            add(sample.sourceTriangle);add(sample.barycentricCorners);
            add(sample.source);add(sample.area);add(sample.thickness);add(sample.validity);
        }
        auto result=Statistics(*snapshot);result["samplesSha256"]=QString::fromLatin1(hash.result().toHex());
        QCryptographicHash nodeHash(QCryptographicHash::Sha256);
        const auto addNode=[&](const auto& value){nodeHash.addData(reinterpret_cast<const char*>(&value),sizeof(value));};
        if(snapshot->nodes)for(const auto& node:*snapshot->nodes) {
            addNode(node.index);addNode(node.validWeight);addNode(node.totalWeight);addNode(node.thickness);
        }
        result["nodesSha256"]=QString::fromLatin1(nodeHash.result().toHex());
        panel->SetComplete(id,"Observed",result);
    },TestPolicy::Read);
    panel->onObserve = [panel, feature, visible] {
        const auto state = feature->GetState();
        QJsonObject summary{{"isBusy", state.isBusy}, {"isCurrent", state.isCurrent}, {"requestId", QString::number(state.requestId)},
            {"result", GetRefText(state.result)}, {"isDisplayReady", state.isDisplayReady}, {"isVisible", *visible}};
        const auto result = feature->GetResult(state.result); summary["hasResult"] = result.has_value();
        ParameterChoices results;
        const auto graph = panel->GetContext().workflow.getPublishedGraph ? panel->GetContext().workflow.getPublishedGraph() : QJsonObject();
        for (const auto value : graph["nodes"].toArray()) {
            const auto node = value.toObject();
            if (node["producer"] != "wall-thickness") continue;
            const auto ref = GetRef(node["ref"]);
            if (feature->GetResult(ref)) results.append({GetRefText(ref),"壁厚结果 · #" + node["order"].toString()});
        }
        summary["hasReadableResult"] = !results.isEmpty() || result.has_value();
        QJsonObject selectedResults;
        for (const auto* action : {"Result","ResultEvidence","SetActive","SelectSample"})
            if (auto* form = panel->GetParameterEditor(action))
                if (auto* field = form->GetField("result")) {
                    field->SetReferenceChoices(results);
                    const auto chosen = field->GetValue();
                    try { selectedResults[action] = feature->GetResult(chosen == "current" ? state.result : GetRef(chosen)).has_value(); }
                    catch (const std::invalid_argument&) { selectedResults[action] = false; }
                }
        summary["selectedResults"] = selectedResults;
        auto image = panel->GetSession()->GetImageDescriptor();
        const auto choice = panel->GetParameterEditor("Start")->GetField("source")->GetValue();
        if (choice != "current" && panel->GetContext().workflow.getImageInput) {
            try { image = panel->GetContext().workflow.getImageInput(GetRef(choice)); }
            catch (const std::invalid_argument&) { image.reset(); }
        }
        if (image) {
            auto* form = panel->GetParameterEditor("Start");
            auto& workflow = panel->GetContext().workflow;
            const auto meshChoice = form->GetField("mesh")->GetValue();
            bool matchedSurface = meshChoice == "surface";
            if (!matchedSurface) try { matchedSurface = GetRef(meshChoice) == workflow.GetSurfaceMesh(); }
                catch (const std::invalid_argument&) {}
            const auto threshold = matchedSurface && image->dataRevision == workflow.GetSurfaceSource() && workflow.GetSurfaceThreshold()
                ? *workflow.GetSurfaceThreshold() : GetScalarMidpoint(*image);
            form->GetField("materialThreshold")->SetDefaultValue(threshold);
            form->GetField("sampleSpacing")->SetDefaultValue(2.0 * GetVoxelSpacing(*image));
            form->GetField("maxBoundaryError")->SetDefaultValue(0.5 * GetVoxelSpacing(*image));
            form->GetField("maxDistance")->SetDefaultValue(GetInputDiagonal(*image));
        }
        if (state.displayRange) summary["displayRange"] = GetValues(*state.displayRange);
        if (result) { const auto stats = Statistics(*result); for (auto it = stats.begin(); it != stats.end(); ++it) summary[it.key()] = it.value(); }
        panel->SetState(summary);
    };
    panel->AttachAction("ExportComparison",{{"outputPath",""}},[panel,feature](auto id,const auto& p){
        const auto snapshot=feature->GetResult(feature->GetState().result);
        if(!snapshot||!snapshot->samples)throw std::runtime_error("wall result unavailable");
        QFile file(GetText(p,"outputPath"));if(!file.open(QIODevice::WriteOnly|QIODevice::Text))throw std::runtime_error("export open failed");
        // 聚合节点场没有唯一对端；比较文件只导出查询位置及对应场值。
        QTextStream out(&file);out.setRealNumberPrecision(17);out<<"sample_id,x_mm,y_mm,z_mm,thickness_mm,area_mm2,validity\n";
        std::size_t i=0;for(const auto& s:*snapshot->samples){out<<qulonglong(i++);for(auto x:s.source)out<<','<<x;out<<','<<s.thickness<<','<<s.area<<','<<int(s.validity)<<'\n';}
        out.flush();if(file.error()!=QFile::NoError)throw std::runtime_error("wall write failed");
        QJsonArray histogram,reasons,regions;const auto& s=snapshot->statistics;
        for(auto x:s.histogramAreas)histogram.append(x);
        for(std::size_t j=0;j<s.reasonCounts.size();++j)reasons.append(QJsonObject{{"reason",int(j)},{"count",QString::number(s.reasonCounts[j])},{"area",s.reasonAreas[j]}});
        for(const auto& r:snapshot->regions)regions.append(QJsonObject{{"id",QString::number(r.id)},{"area",r.area},{"minimum",r.minimum},{"maximum",r.maximum},{"bounds",GetValues(r.sampleBounds)},{"samples",QString::number(r.sampleIds.size())}});
        panel->SetComplete(id,"Exported",{{"statistics",Statistics(*snapshot)},{"histogramAreas",histogram},{"reasons",reasons},{"regions",regions},{"evaluatedArea",s.evaluatedArea},{"validArea",s.validArea}});
    },TestPolicy::Read);
    panel->onStop = [panel] { panel->SendAction("Cancel", {{"targetRequestId", "0"}}); };
    return panel;
}
}
