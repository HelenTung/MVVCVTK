// 测试用途：以当前计量公共接口执行独立选区检测、偏差、模型姿态与冻结报告。
#include "ModuleFactories.h"
#include "AlignmentInput.h"
#include "Support/ParameterEditor.h"
#include "Support/RoiSelection.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include <QPointer>
namespace Manual {
namespace {
QString Verdict(AlignmentInspectionVerdict value)
{
    switch (value) {
    case AlignmentInspectionVerdict::NotEvaluated: return "NotEvaluated";
    case AlignmentInspectionVerdict::Pass: return "Pass";
    case AlignmentInspectionVerdict::Fail: return "Fail";
    case AlignmentInspectionVerdict::Indeterminate: return "Indeterminate";
    }
    throw std::invalid_argument("检测判定无效");
}
QJsonValue Value(const std::optional<double>& value) { return value ? QJsonValue(*value) : QJsonValue(); }
InspectionMetric GetInspectionMetric(const QJsonObject& p)
{
    return GetEnum<InspectionMetric>(p,"measurementMetric",{{"Radius",InspectionMetric::Radius},{"Diameter",InspectionMetric::Diameter},{"CenterX",InspectionMetric::CenterX},{"CenterY",InspectionMetric::CenterY},{"CenterZ",InspectionMetric::CenterZ},{"CenterDistance",InspectionMetric::CenterDistance},{"OrientedDirectionAngleDegrees",InspectionMetric::OrientedDirectionAngleDegrees},{"SignedParallelPlaneDistance",InspectionMetric::SignedParallelPlaneDistance},{"FitRms",InspectionMetric::FitRms},{"FitMaximum",InspectionMetric::FitMaximum}});
}
AlignmentGeometryKind GetInspectionKind(const QJsonObject& p)
{
    return GetEnum<AlignmentGeometryKind>(p,"kind",{{"Point",AlignmentGeometryKind::Point},{"Line",AlignmentGeometryKind::Line},{"Plane",AlignmentGeometryKind::Plane},{"Circle",AlignmentGeometryKind::Circle},{"Sphere",AlignmentGeometryKind::Sphere},{"Cylinder",AlignmentGeometryKind::Cylinder}});
}
QStringList MeasurementChoices(const QString& first,const QString& second={},bool section=false)
{
    const auto center=[section](const QString& kind){return QStringList{"Point","Circle","Sphere"}.contains(kind) || (section && QStringList{"Line","Cylinder"}.contains(kind));};
    const auto direction=[](const QString& kind){return QStringList{"Line","Cylinder","Plane","Circle"}.contains(kind);};
    if(!second.isEmpty()) {QStringList result;if(center(first) && center(second))result<<"CenterDistance";if(direction(first) && direction(second))result<<"OrientedDirectionAngleDegrees";if(first=="Plane" && second=="Plane")result<<"SignedParallelPlaneDistance";return result;}
    QStringList result{"FitRms","FitMaximum"};if(QStringList{"Circle","Sphere","Cylinder"}.contains(first))result<<"Radius"<<"Diameter";if(center(first))result<<"CenterX"<<"CenterY"<<"CenterZ";return result;
}
std::vector<std::uint64_t> Ids(const QJsonValue& value)
{
    if (!value.isArray()) throw std::invalid_argument("编号必须为数组");
    std::vector<std::uint64_t> result;
    for (const auto id : value.toArray()) result.push_back(GetId(id));
    return result;
}
QJsonObject Statistics(const AlignmentDeviationField& field)
{
    const auto& s = field.statistics;
    QJsonArray counts;
    for (auto count : s.counts) counts.append(QString::number(count));
    return {{"counts", counts}, {"hasValues", s.hasValues}, {"selectedArea", s.selectedArea},
        {"evaluatedArea", s.evaluatedArea}, {"outOfToleranceArea", s.outOfToleranceArea},
        {"minimum", s.hasValues ? QJsonValue(s.minimum) : QJsonValue()},
        {"maximum", s.hasValues ? QJsonValue(s.maximum) : QJsonValue()},
        {"mean", s.hasValues ? QJsonValue(s.mean) : QJsonValue()},
        {"rms", s.hasValues ? QJsonValue(s.rms) : QJsonValue()},
        {"absoluteP95", s.hasValues ? QJsonValue(s.absoluteP95) : QJsonValue()}};
}
}
void AttachInspectionActions(ModulePanel* panel, std::shared_ptr<MetrologyAlignmentHostFeature> feature,
    std::function<DataRevisionRef()> currentAlignment)
{
    struct Selection { DataRevisionRef inspection, deviation, voxelVolume; QMap<QString,QJsonObject> successfulParameters; };
    auto selected = std::make_shared<Selection>();
    const auto alignment = [feature, currentAlignment](const QJsonObject& p) {
        const auto ref = p["result"] == "current" ? currentAlignment() : GetRef(p["result"]);
        if (!feature->GetResult(ref)) throw std::invalid_argument("请选择已发布的对齐结果");
        return ref;
    };
    const auto send = [owner = QPointer<ModulePanel>(panel), feature, selected](std::uint64_t id, AlignmentRequest request) {
        const auto admission = feature->SendRequest(std::move(request), [owner, selected, id](AlignmentResult result) {
            if (!owner) return;
            if (GetDataRevisionRefValid(result.inspectionResult)) selected->inspection = result.inspectionResult;
            if (GetDataRevisionRefValid(result.deviationResult)) selected->deviation = result.deviationResult;
            if (GetDataRevisionRefValid(result.voxelVolumeResult)) selected->voxelVolume = result.voxelVolumeResult;
            if(result.status==AlignmentStatus::FullyDetermined) {const auto record=owner->GetContext().records.GetRecord(id);selected->successfulParameters[record["action"].toString()]=record["parameters"].toObject();}
            auto json = GetAlignmentResult(result);
            json["inspection"] = GetRefText(result.inspectionResult);
            json["deviation"] = GetRefText(result.deviationResult);
            json["voxelVolume"] = GetRefText(result.voxelVolumeResult);
            json["isModelPoseApplied"] = result.isModelPoseApplied;
            owner->SetComplete(id, result.status == AlignmentStatus::FullyDetermined ? "FullyDetermined"
                : result.status == AlignmentStatus::Cancelled ? "Cancelled" : result.status == AlignmentStatus::Unavailable ? "Unavailable" : "Failed", json);
        });
        if (owner) owner->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
            {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
    };
    panel->AttachAction("EvaluateSurface", GetJson(R"({"result":"current","computeBackend":"Cpu","metric":"UnsignedDistance","sourceTriangles":[],"referenceTriangles":[],"maxDistance":1,"minNormalCosine":null,"tolerance":null,"reverseCoverage":true})"),
        [alignment, send](auto id, const auto& p) {
            AlignmentDeviationRequest evaluation; evaluation.alignmentResult = alignment(p);
            evaluation.computeBackend = GetEnum<AlignmentComputeBackend>(p, "computeBackend", {{"Cpu", AlignmentComputeBackend::Cpu}, {"Cuda", AlignmentComputeBackend::Cuda}});
            evaluation.metric = GetEnum<AlignmentDeviationMetric>(p, "metric", {{"UnsignedDistance", AlignmentDeviationMetric::UnsignedDistance}, {"NominalNormalProjection", AlignmentDeviationMetric::NominalNormalProjection}});
            evaluation.sourceTriangles = Ids(p["sourceTriangles"]); evaluation.referenceTriangles = Ids(p["referenceTriangles"]);
            evaluation.maxDistance = GetNumber(p, "maxDistance"); evaluation.reverseCoverage = GetBool(p, "reverseCoverage");
            if (!p["minNormalCosine"].isNull()) evaluation.minNormalCosine = GetNumber(p, "minNormalCosine");
            if (!p["tolerance"].isNull()) evaluation.tolerance = GetArray<double, 2>(p["tolerance"]);
            AlignmentRequest request; request.action = AlignmentAction::EvaluateSurface; request.deviation = std::move(evaluation); send(id, std::move(request));
        }, TestPolicy::Compute, true);
    for (const auto section : {false, true}) {
        auto defaults = GetJson(R"({"result":"current","id":"inspection","nominal":null,"nominalSource":"","deviationLimits":null})");
        if (section) {
            defaults["planePoint"] = QJsonArray{0,0,0}; defaults["planeNormal"] = QJsonArray{0,0,1};
            defaults["triangles"] = QJsonArray{}; defaults["sampleSpacing"] = .1; defaults["planeTolerance"] = 1e-8;
            defaults["selectionPoint"] = QJsonValue(); defaults["maxSelectionDistance"] = 1.;
        } else {
            defaults["kind"] = "Cylinder"; defaults["vertexIds"] = QJsonArray{}; defaults["targetBounds"] = QJsonValue();
            defaults["inputRoi"]="";
            defaults["targetDirection"] = QJsonArray{0,0,1};
            defaults["useSectionPlane"]=false;defaults["sectionPlaneVertexIds"]=QJsonArray{};defaults["sectionPlaneBounds"]=QJsonValue();defaults["sectionPlaneNormal"]=QJsonArray{0,0,1};
        }
        defaults["measurementMetric"]="Diameter";
        panel->AttachAction(section ? "InspectSection" : "InspectGeometry", defaults,
            [panel, feature, alignment, send, section](auto id, const auto& p) {
                InspectionPlan plan; plan.id = GetText(p, "id").toStdString(); plan.alignmentResult = alignment(p);
                const auto snapshot = feature->GetResult(plan.alignmentResult);
                if(!snapshot)throw std::invalid_argument("所选对齐结果不可读");
                InspectionGeometrySpec geometry; geometry.fit.id = "measured";
                if (section) {
                    geometry.fit.kind = AlignmentGeometryKind::Circle;
                    InspectionSection cut; cut.pinnedMesh = snapshot->input.mesh;
                    cut.planePoint = GetArray<double, 3>(p["planePoint"]); cut.planeNormal = GetArray<double, 3>(p["planeNormal"]);
                    cut.triangles = Ids(p["triangles"]); cut.sampleSpacing = GetNumber(p, "sampleSpacing"); cut.planeTolerance = GetNumber(p, "planeTolerance");
                    if (!p["selectionPoint"].isNull()) cut.selectionPoint = GetArray<double, 3>(p["selectionPoint"]);
                    cut.maxSelectionDistance = GetNumber(p, "maxSelectionDistance"); geometry.section = std::move(cut);
                } else {
                    geometry.fit.kind = GetInspectionKind(p);
                    geometry.fit.region.pinnedMesh = snapshot->input.mesh; geometry.fit.region.vertexIds = Ids(p["vertexIds"]);
                    if (!p["targetBounds"].isNull()) geometry.fit.region.targetBounds = GetArray<double, 6>(p["targetBounds"]);
                    if(const auto roi=GetSelectedRoi(*panel->GetSession(),p["inputRoi"],snapshot->input.source)) {
                        const auto vertices=panel->GetContext().workflow.getRoiVertices(*roi,snapshot->input.source,snapshot->input.mesh);
                        SetRegionRoi(geometry.fit.region,vertices,snapshot->input.mesh);
                    }
                    geometry.fit.targetDirection = GetArray<double, 3>(p["targetDirection"]);
                }
                plan.geometries.push_back(std::move(geometry));
                InspectionItem diameter; diameter.id = "measurement"; diameter.name = GetText(p,"measurementMetric").toStdString();
                diameter.measured.metric = GetInspectionMetric(p);
                if(!section && p["useSectionPlane"].toBool() && QStringList{"Line","Cylinder"}.contains(p["kind"].toString()) && QStringList{"CenterX","CenterY","CenterZ"}.contains(p["measurementMetric"].toString())) {
                    InspectionGeometrySpec plane;plane.fit.id="axis-section-plane";plane.fit.kind=AlignmentGeometryKind::Plane;plane.fit.region.pinnedMesh=snapshot->input.mesh;plane.fit.region.vertexIds=Ids(p["sectionPlaneVertexIds"]);if(!p["sectionPlaneBounds"].isNull())plane.fit.region.targetBounds=GetArray<double,6>(p["sectionPlaneBounds"]);plane.fit.targetDirection=GetArray<double,3>(p["sectionPlaneNormal"]);plan.geometries.push_back(std::move(plane));diameter.measured.first.sectionPlane=1;
                }
                if (!p["nominal"].isNull()) diameter.nominal = GetNumber(p, "nominal");
                diameter.nominalSource = GetText(p, "nominalSource").toStdString();
                if (!p["deviationLimits"].isNull()) diameter.deviationLimits = GetArray<double, 2>(p["deviationLimits"]);
                plan.items.push_back(diameter);
                InspectionItem rms; rms.id = "fit-rms"; rms.name = "拟合残差 RMS"; rms.measured.metric = InspectionMetric::FitRms; plan.items.push_back(rms);
                AlignmentRequest request; request.action = AlignmentAction::Inspect; request.inspection = std::move(plan); send(id, std::move(request));
            }, TestPolicy::Compute, true);
    }
    const auto pairGeometry=GetJson(R"({"kind":"Plane","vertexIds":[],"targetBounds":null,"targetDirection":[0,0,1]})");
    QJsonObject pair{{"result","current"},{"id","geometry-pair"},{"first",pairGeometry},{"second",pairGeometry},{"measurementMetric","SignedParallelPlaneDistance"},{"maxParallelAngleDegrees",.001},{"useSectionPlane",false},{"sectionPlaneVertexIds",QJsonArray{}},{"sectionPlaneBounds",QJsonValue()},{"sectionPlaneNormal",QJsonArray{0,0,1}},{"nominal",QJsonValue()},{"nominalSource",""},{"deviationLimits",QJsonValue()}};
    panel->AttachAction("InspectPair",pair,[feature,alignment,send](auto id,const auto& p) {
        InspectionPlan plan;plan.id=GetText(p,"id").toStdString();plan.alignmentResult=alignment(p);const auto snapshot=feature->GetResult(plan.alignmentResult);
        for(const auto* key:{"first","second"}) {
            const auto values=p[key].toObject();InspectionGeometrySpec geometry;geometry.fit.id=key;geometry.fit.kind=GetInspectionKind(values);
            geometry.fit.region.pinnedMesh=snapshot->input.mesh;geometry.fit.region.vertexIds=Ids(values["vertexIds"]);if(!values["targetBounds"].isNull())geometry.fit.region.targetBounds=GetArray<double,6>(values["targetBounds"]);
            geometry.fit.targetDirection=GetArray<double,3>(values["targetDirection"]);plan.geometries.push_back(std::move(geometry));
        }
        InspectionItem item;item.id="pair-measurement";item.name=GetText(p,"measurementMetric").toStdString();item.measured.metric=GetInspectionMetric(p);item.measured.first.geometry=0;item.measured.second=InspectionOperand{1,{}};item.measured.maxParallelAngleDegrees=GetNumber(p,"maxParallelAngleDegrees");
        if(p["useSectionPlane"].toBool() && p["measurementMetric"]=="CenterDistance") {
            InspectionGeometrySpec plane;plane.fit.id="axis-section-plane";plane.fit.kind=AlignmentGeometryKind::Plane;plane.fit.region.pinnedMesh=snapshot->input.mesh;plane.fit.region.vertexIds=Ids(p["sectionPlaneVertexIds"]);
            if(!p["sectionPlaneBounds"].isNull())plane.fit.region.targetBounds=GetArray<double,6>(p["sectionPlaneBounds"]);plane.fit.targetDirection=GetArray<double,3>(p["sectionPlaneNormal"]);plan.geometries.push_back(std::move(plane));
            if(plan.geometries[0].fit.kind==AlignmentGeometryKind::Line || plan.geometries[0].fit.kind==AlignmentGeometryKind::Cylinder)item.measured.first.sectionPlane=2;
            if(plan.geometries[1].fit.kind==AlignmentGeometryKind::Line || plan.geometries[1].fit.kind==AlignmentGeometryKind::Cylinder)item.measured.second->sectionPlane=2;
        }
        if(!p["nominal"].isNull())item.nominal=GetNumber(p,"nominal");item.nominalSource=GetText(p,"nominalSource").toStdString();if(!p["deviationLimits"].isNull())item.deviationLimits=GetArray<double,2>(p["deviationLimits"]);plan.items.push_back(std::move(item));
        AlignmentRequest request;request.action=AlignmentAction::Inspect;request.inspection=std::move(plan);send(id,std::move(request));
    },TestPolicy::Compute,true);
    panel->AttachAction("InspectionResult", {{"inspection", "current"}}, [panel, feature, selected](auto id, const auto& p) {
        const auto ref = p["inspection"] == "current" ? selected->inspection : GetRef(p["inspection"]);
        const auto snapshot = feature->GetInspection(ref);
        if (!snapshot || !snapshot->report) throw std::invalid_argument("检测结果不存在");
        QJsonArray items, geometries;
        for (const auto& item : snapshot->report->items) items.append(QJsonObject{{"id", QString::fromStdString(item.id)},
            {"measured", Value(item.measured)}, {"nominal", Value(item.nominal)}, {"deviation", Value(item.deviation)},
            {"verdict", Verdict(item.verdict)}, {"message", QString::fromStdString(item.message)}});
        for (const auto& g : snapshot->report->geometries) {
            QJsonObject json{{"status", static_cast<int>(g.status)}, {"center", GetValues(g.alignedCenter)},
                {"direction", GetValues(g.alignedDirection)}, {"radius", g.fitted.radius}, {"fitRms", g.fitted.fitRms}, {"fitMax", g.fitted.fitMax},
                {"sampleCount", QString::number(g.fitted.sampleCount)}, {"message", QString::fromStdString(g.message)}};
            if (g.section) json["section"] = QJsonObject{{"closedLoops", QString::number(g.section->closedLoops)},
                {"samples", QString::number(g.section->samples.size())}, {"perimeter", g.section->perimeter}, {"maxPlaneCorrection", g.section->maxPlaneCorrection}};
            geometries.append(json);
        }
        panel->SetComplete(id, "Observed", {{"inspection", GetRefText(ref)}, {"alignmentResult", GetRefText(snapshot->alignmentResult)},
            {"isCurrent", snapshot->isCurrent}, {"unit", static_cast<int>(snapshot->unit)}, {"verdict", Verdict(snapshot->report->verdict)}, {"items", items}, {"geometries", geometries}});
    }, TestPolicy::Read);
    panel->AttachAction("DeviationResult", {{"deviation", "current"}}, [panel, feature, selected](auto id, const auto& p) {
        const auto ref = p["deviation"] == "current" ? selected->deviation : GetRef(p["deviation"]);
        const auto snapshot = feature->GetDeviation(ref);
        if (!snapshot || !snapshot->forward) throw std::invalid_argument("偏差结果不存在");
        panel->SetComplete(id, "Observed", {{"deviation", GetRefText(ref)}, {"isCurrent", snapshot->isCurrent},
            {"unit", static_cast<int>(snapshot->unit)}, {"verdict", Verdict(snapshot->verdict)},
            {"forward", Statistics(*snapshot->forward)}, {"reverse", snapshot->reverse ? QJsonValue(Statistics(*snapshot->reverse)) : QJsonValue()}});
    }, TestPolicy::Read);
    panel->AttachAction("SetDeviationDisplay", GetJson(R"({"deviation":"current","range":null,"useToleranceColors":false,"isVisible":true,"selectedVertex":null})"),
        [selected, send](auto id, const auto& p) {
            AlignmentDeviationDisplay display; display.result = p["deviation"] == "current" ? selected->deviation : GetRef(p["deviation"]);
            if (!p["range"].isNull()) display.range = GetArray<double, 2>(p["range"]);
            display.useToleranceColors = GetBool(p, "useToleranceColors"); display.isVisible = GetBool(p, "isVisible");
            if (!p["selectedVertex"].isNull()) display.selectedVertex = GetId(p["selectedVertex"]);
            AlignmentRequest request; request.action = AlignmentAction::SetDeviationDisplay; request.deviationDisplay = display; send(id, std::move(request));
        }, TestPolicy::View);
    panel->AttachAction("ApplyModelPose", {{"result", "current"}}, [alignment, send](auto id, const auto& p) {
        AlignmentRequest request; request.action = AlignmentAction::ApplyModelPose; request.resultRef = alignment(p); send(id, std::move(request));
    }, TestPolicy::View, true);
    panel->AttachAction("EvaluateVoxelVolume", GetJson(R"({"id":"voxel-volume","source":"current","extent":null,"storedValueRange":[0,1],"requireEnclosedSelection":false})"),
        [panel, send](auto id, const auto& p) {
            const auto image = panel->GetSession()->GetImageDescriptor();
            if (!image) throw std::invalid_argument("需要源体数据");
            VoxelVolumeRequest volume; volume.id = GetText(p, "id").toStdString();
            volume.source = p["source"] == "current" ? image->dataRevision : GetRef(p["source"]);
            if (!p["extent"].isNull()) volume.extent = GetArray<int, 6>(p["extent"]);
            volume.storedValueRange = GetArray<double, 2>(p["storedValueRange"]);
            volume.requireEnclosedSelection = GetBool(p, "requireEnclosedSelection");
            AlignmentRequest request; request.action = AlignmentAction::EvaluateVoxelVolume; request.voxelVolume = volume; send(id, std::move(request));
        }, TestPolicy::Compute, true);
    panel->AttachAction("VoxelVolumeResult", {{"voxelVolume", "current"}}, [panel, feature, selected](auto id, const auto& p) {
        const auto ref = p["voxelVolume"] == "current" ? selected->voxelVolume : GetRef(p["voxelVolume"]);
        const auto snapshot = feature->GetVoxelVolume(ref);
        if (!snapshot) throw std::invalid_argument("体素体积结果不存在");
        const auto& s = snapshot->statistics;
        panel->SetComplete(id, "Observed", {{"voxelVolume", GetRefText(ref)}, {"source", GetRefText(snapshot->source)},
            {"isCurrent", snapshot->isCurrent}, {"selected", QString::number(s.selected)}, {"total", QString::number(s.total)},
            {"volumeMm3", Value(s.volumeMm3)}, {"observedVolumeMm3", s.observedVolumeMm3},
            {"cellVolumeMm3", s.cellVolumeMm3}, {"validity", static_cast<int>(s.validity)}, {"touchesRegionBoundary", s.touchesRegionBoundary}});
    }, TestPolicy::Read);
    panel->AttachAction("SetSceneDisplay", GetJson(R"({"result":"current","inspection":null,"isVisible":true,"showMeasured":true,"showReference":true,"selectedItem":null,"roiMarkerLimit":"256"})"),
        [alignment, selected, send](auto id, const auto& p) {
            AlignmentSceneDisplay display; display.anchorAlignment = alignment(p); display.isVisible = GetBool(p, "isVisible");
            display.roiMarkerLimit = GetId(p["roiMarkerLimit"]);
            AlignmentScenePart part; part.id = "manual-part"; part.alignmentResult = display.anchorAlignment;
            part.showMeasured = GetBool(p, "showMeasured"); part.showReference = GetBool(p, "showReference");
            if (!p["inspection"].isNull()) {
                const auto ref = p["inspection"] == "current" ? selected->inspection : GetRef(p["inspection"]);
                if (GetDataRevisionRefValid(ref)) part.inspection = ref;
                else throw std::invalid_argument("需要有效检测修订；不显示检测时取消指定");
            }
            if (!p["selectedItem"].isNull()) part.selectedItem = GetText(p, "selectedItem").toStdString();
            display.parts.push_back(std::move(part));
            AlignmentRequest request; request.action = AlignmentAction::SetSceneDisplay; request.sceneDisplay = std::move(display); send(id, std::move(request));
        }, TestPolicy::View);
    panel->AttachAction("ExportReport", GetJson(R"({"outputPath":"","format":"Json","result":"current","inspection":"current","deviation":"current","voxelVolume":"current","captureCurrentDisplay":true,"includeDeviationSamples":false})"),
        [panel, feature, selected, currentAlignment](auto id, const auto& p) {
            MetrologyReportRequest request; request.id = "manual-report"; request.title = "手动测试计量报告";
            const auto append = [&](const char* key, DataRevisionRef current, auto& refs) {
                if (p[key].isNull()) return;
                const auto ref = p[key] == "current" ? current : GetRef(p[key]);
                if (GetDataRevisionRefValid(ref)) refs.push_back(ref);
            };
            const auto chosen=[panel](const char* action,const char* key,DataRevisionRef fallback){const QJsonValue value=panel->GetActionParameters(action)[key];return value=="current"?fallback:GetRef(value);};
            append("result", chosen("Result","result",currentAlignment()), request.alignments); append("inspection", chosen("InspectionResult","inspection",selected->inspection), request.inspections);
            append("deviation", chosen("DeviationResult","deviation",selected->deviation), request.deviations); append("voxelVolume", chosen("VoxelVolumeResult","voxelVolume",selected->voxelVolume), request.voxelVolumes);
            request.captureCurrentDisplay = GetBool(p, "captureCurrentDisplay");
            const auto report = feature->GetReport(request);
            if (!report.report) throw std::invalid_argument(report.message);
            MetrologyReportWriteOptions options;
            options.format = GetEnum<MetrologyReportFormat>(p, "format", {{"Json", MetrologyReportFormat::Json}, {"Csv", MetrologyReportFormat::Csv}});
            options.includeDeviationSamples = GetBool(p, "includeDeviationSamples");
            const auto write = WriteMetrologyReportFile(*report.report, GetText(p, "outputPath").toUtf8().toStdString(), options);
            panel->SetComplete(id, write.isCommitted ? "ReportExported" : "Failed", {{"isCommitted", write.isCommitted},
                {"bytesWritten", QString::number(write.write.bytesWritten)}, {"status", static_cast<int>(write.write.status)},
                {"message", QString::fromStdString(write.write.message)}, {"capturedCommit", QString::number(report.report->capturedCommit)}});
        }, TestPolicy::Read);
    const auto observeAlignment = panel->onObserve;
    panel->onStatistics=[panel,feature,selected] {
        if(QStringList{"EvaluateVoxelVolume","VoxelVolumeResult"}.contains(panel->GetCurrentAction())) {
            const QJsonValue choice=panel->GetActionParameters("VoxelVolumeResult")["voxelVolume"];
            const auto ref=choice=="current" ? selected->voxelVolume : GetRef(choice);
            if(const auto volume=feature->GetVoxelVolume(ref)) {
                const auto& s=volume->statistics;
                panel->GetStatisticsWidget()->SetMessage("独立体素体积结果","结果 "+GetRefText(ref)+" · 来源 "+GetRefText(volume->source)+" · 原生体素网格，独立于对齐；存储值区间未自动标定");
                panel->GetStatisticsWidget()->SetTable({"量值","结果","单位"},QJsonArray{
                    QJsonArray{"可评价体积",Value(s.volumeMm3),"mm³"},QJsonArray{"观察选中体积",s.observedVolumeMm3,"mm³"},
                    QJsonArray{"单元体积",s.cellVolumeMm3,"mm³"},QJsonArray{"选中 / 总体素",QString::number(s.selected)+" / "+QString::number(s.total),"体素"}});
                return true;
            }
        }
        const QJsonValue choice=panel->GetActionParameters("DeviationResult")["deviation"];
        const auto ref=choice=="current" ? selected->deviation : GetRef(choice);
        const auto snapshot=feature->GetDeviation(ref);
        const bool geometryPage=QStringList{"InspectGeometry","InspectSection","InspectPair","InspectionResult"}.contains(panel->GetCurrentAction());
        if(!geometryPage && snapshot && snapshot->forward) {
            const auto field=snapshot->forward;
            const auto metric=snapshot->parameters->metric;
            const QString unit=snapshot->unit==AlignmentUnit::Millimeter ? "mm" : snapshot->unit==AlignmentUnit::Meter ? "m" : "模型单位";
            panel->GetStatisticsWidget()->StartHistogram(GetRefText(ref),metric==AlignmentDeviationMetric::UnsignedDistance ? "实测到名义的无符号距离分布" : "名义法向投影分布",
                (metric==AlignmentDeviationMetric::UnsignedDistance ? "无符号距离 / " : "法向投影 / ")+unit,"有效面积 / "+unit+"²",
                [field,ref,verdict=snapshot->verdict](std::size_t offset) {
                    StatisticsSamples result;result.total=field->samples.size();result.next=std::min(result.total,offset+4096);
                    result.description="偏差修订 "+GetRefText(ref)+" · 实测 → 名义 · 全量面积加权 · 判定 "+Verdict(verdict)
                        +QString(" · 有效覆盖 %1 / %2 · 另列未评估面积；反向覆盖为独立无符号距离").arg(field->statistics.evaluatedArea).arg(field->statistics.selectedArea);
                    for(std::size_t i=offset;i<result.next;++i){const auto& sample=field->samples[i];
                        if(sample.state==AlignmentDeviationState::Valid && std::isfinite(sample.value) && std::isfinite(sample.area) && sample.area>0){result.values.push_back(sample.value);result.weights.push_back(sample.area);}else ++result.excluded;}
                    return result;
                });
            panel->GetStatisticsWidget()->onInterval=[panel,feature,ref](double low,double high) {
                const auto result=feature->GetDeviation(ref);if(!result || !result->forward)return;const auto& samples=result->forward->samples;
                for(std::size_t i=0;i<samples.size();++i)if(samples[i].state==AlignmentDeviationState::Valid && panel->GetStatisticsWidget()->ContainsInterval(samples[i].value,low,high)) {
                    auto p=panel->GetActionParameters("SetDeviationDisplay");p["deviation"]=GetRefText(ref);p["selectedVertex"]=QString::number(i);panel->SendAction("SetDeviationDisplay",p);return;
                }
            };
            QJsonArray markers{QJsonObject{{"label","零偏差"},{"value",0.}}};if(snapshot->parameters->tolerance){markers.append(QJsonObject{{"label","公差下限"},{"value",(*snapshot->parameters->tolerance)[0]}});markers.append(QJsonObject{{"label","公差上限"},{"value",(*snapshot->parameters->tolerance)[1]}});}panel->GetStatisticsWidget()->SetMarkers(markers);
            return true;
        }
        const QJsonValue inspectedChoice=panel->GetActionParameters("InspectionResult")["inspection"];const auto inspectedRef=inspectedChoice=="current"?selected->inspection:GetRef(inspectedChoice);
        const auto inspection=feature->GetInspection(inspectedRef);
        if(inspection && inspection->report) {
            QJsonArray rows;std::vector<double> rms;
            for(const auto& geometry:inspection->report->geometries){
                const bool valid=geometry.status==AlignmentStatus::FullyDetermined && geometry.fitted.sampleCount>0;
                if(valid)rms.push_back(geometry.fitted.fitRms);
                rows.append(QJsonArray{QString::fromStdString(geometry.fitted.id),valid ? QJsonValue(geometry.fitted.fitRms) : QJsonValue(),valid ? QJsonValue(geometry.fitted.fitMax) : QJsonValue(),QString::number(geometry.fitted.sampleCount)});
            }
            panel->GetStatisticsWidget()->SetHistogram(BuildHistogram(rms,{},"逐特征几何拟合残差 RMS",
                "检测修订 "+GetRefText(inspectedRef)+" · 逐特征汇总；拟合残差与配准 RMS、偏差场 RMS 分别命名","拟合 RMS（结果长度单位）","特征数量"));
            panel->GetStatisticsWidget()->SetTable({"特征","拟合 RMS","拟合 Max","样本数"},rows);return true;
        }
        return false;
    };
    panel->onObserve = [panel, feature, observeAlignment,selected,currentAlignment] {
        if (observeAlignment) observeAlignment();
        ParameterChoices inspections, deviations, volumes;
        const auto graph = panel->GetContext().workflow.getPublishedGraph ? panel->GetContext().workflow.getPublishedGraph() : QJsonObject{};
        for (const auto value : graph["nodes"].toArray()) {
            const auto node = value.toObject(); if (node["producer"] != "metrology-alignment") continue;
            const auto ref = GetRef(node["ref"]); const auto text = GetRefText(ref);
            if (feature->GetInspection(ref)) inspections.append({text, "尺寸检测 · #" + node["order"].toString()});
            if (feature->GetDeviation(ref)) deviations.append({text, "表面偏差 · #" + node["order"].toString()});
            if (feature->GetVoxelVolume(ref)) volumes.append({text, "体素体积 · #" + node["order"].toString()});
        }
        const auto choices = [&](const char* key, const ParameterChoices& values, std::initializer_list<const char*> actions) {
            for (const auto* action : actions) if (auto* form = panel->GetParameterEditor(action))
                if (auto* field = form->GetField(key)) field->SetReferenceChoices(values);
        };
        choices("inspection", inspections, {"InspectionResult", "SetSceneDisplay", "ExportReport"});
        choices("deviation", deviations, {"DeviationResult", "SetDeviationDisplay", "ExportReport"});
        choices("voxelVolume", volumes, {"VoxelVolumeResult", "ExportReport"});
        auto summary=panel->GetObservedState();
        const auto matches=[panel,selected](const QString& action){try{return selected->successfulParameters.contains(action) && selected->successfulParameters[action]==panel->GetActionParameters(action);}catch(const std::exception&){return false;}};
        const auto action=panel->GetParameterAction();
        const auto bindingMatches=[panel,currentAlignment](const char* action,DataRevisionRef binding){try{const QJsonValue value=panel->GetActionParameters(action)["result"];if(value=="current" && !panel->GetObservedState()["isReferenceCurrent"].toBool())return false;return binding==(value=="current"?currentAlignment():GetRef(value));}catch(const std::exception&){return false;}};
        const auto inspection=feature->GetInspection(selected->inspection);const auto deviation=feature->GetDeviation(selected->deviation);
        summary["inspectionContext"]=QJsonObject{{"action",action},{"isReadable",inspection.has_value()},{"isCurrent",inspection && inspection->isCurrent},{"parametersMatch",matches(action)},{"bindingMatch",inspection && bindingMatches(action.toUtf8().constData(),inspection->alignmentResult)}};
        summary["inspectionMatchesDraft"]=(action=="InspectGeometry" || action=="InspectSection" || action=="InspectPair") && inspection && inspection->isCurrent && matches(action) && bindingMatches(action.toUtf8().constData(),inspection->alignmentResult);
        summary["deviationMatchesDraft"]=deviation && deviation->isCurrent && matches("EvaluateSurface") && bindingMatches("EvaluateSurface",deviation->alignmentResult);
        const auto volume=feature->GetVoxelVolume(selected->voxelVolume);const auto image=panel->GetSession()->GetImageDescriptor();DataRevisionRef volumeSource;try{const QJsonValue value=panel->GetActionParameters("EvaluateVoxelVolume")["source"];volumeSource=value=="current" && image?image->dataRevision:GetRef(value);}catch(const std::exception&){}
        summary["voxelMatchesDraft"]=volume && volume->source==volumeSource && matches("EvaluateVoxelVolume");
        if(QStringList{"InspectGeometry","InspectSection","InspectPair"}.contains(action))try {
            const auto p=panel->GetActionParameters(action);const auto allowed=MeasurementChoices(action=="InspectSection"?"Circle":action=="InspectPair"?p["first"].toObject()["kind"].toString():p["kind"].toString(),action=="InspectPair"?p["second"].toObject()["kind"].toString():QString(),action!="InspectSection" && GetBool(p,"useSectionPlane"));
            ParameterChoices choices;for(const auto& entry:GetParameterChoices("Alignment","measurementMetric"))if(allowed.contains(entry.first))choices.append(entry);
            panel->GetParameterEditor(action)->GetField("measurementMetric")->SetAllowedChoices(choices);summary["inspectionProblem"]=allowed.contains(p["measurementMetric"].toString())?QString():QString("当前几何组合不支持此量值；请选择列表中的适用量值。轴心位置须提供截面平面。");
        }catch(const std::exception& error){summary["inspectionProblem"]=QString::fromUtf8(error.what());}
        summary["latestInspection"]=GetDataRevisionRefValid(selected->inspection)?GetRefText(selected->inspection):QString();summary["latestDeviation"]=GetDataRevisionRefValid(selected->deviation)?GetRefText(selected->deviation):QString();summary["latestVoxelVolume"]=GetDataRevisionRefValid(selected->voxelVolume)?GetRefText(selected->voxelVolume):QString();panel->SetState(summary);
    };
}
}
