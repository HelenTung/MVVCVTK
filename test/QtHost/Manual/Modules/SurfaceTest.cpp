// 测试用途：材料等值面、结果用途、阈值复制与有界采样。
#include "ModuleFactories.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Support/ParameterEditor.h"
#include "Support/ReferenceDataSource.h"
#include "Support/RoiSelection.h"
#include <QPointer>
#include <QFile>
#include <QTextStream>
#include <cmath>
namespace Manual {
namespace {
QJsonObject GetSurface(const std::shared_ptr<SurfaceDeterminationHostFeature>& feature,QJsonObject& formalStatistics)
{
    const auto state = feature->GetState();
    QJsonObject summary{{"stage", static_cast<int>(state.stage)}, {"failureReason", static_cast<int>(state.failureReason)},
        {"requestId", QString::number(state.requestId)}, {"source", GetRefText(state.sourceRevision)},
        {"resultRevision", QString::number(state.resultRevision)}, {"progress", state.progress01},
        {"points", QString::number(state.pointCount)}, {"acceptedPoints", QString::number(state.acceptedPointCount)},
        {"lowContrastPoints", QString::number(state.lowContrastPointCount)}, {"rejectedPoints", QString::number(state.rejectedPointCount)},
        {"truncatedPoints", QString::number(state.truncatedPointCount)}, {"error", QString::fromStdString(state.errorMessage)}};
    summary["purpose"] = static_cast<int>(state.purpose);
    summary["taskDiagnostics"]=QJsonObject{{"points",summary["points"]},{"acceptedPoints",summary["acceptedPoints"]},{"rejectedPoints",summary["rejectedPoints"]},{"purpose",summary["purpose"]}};
    const auto describeEstimate=[](const SurfaceIsoEstimate& value) {
        QJsonObject result{{"iso",value.isoValue},{"samples",QString::number(value.sampleCount)},{"isAutomatic",value.sampleCount>0}};
        if(value.sampleCount){result["background"]=value.backgroundValue;result["material"]=value.materialValue;}
        else result["providedRange"]=QJsonArray{value.backgroundValue,value.materialValue};
        return result;
    };
    if (state.isoEstimate) summary["isoEstimate"] = describeEstimate(*state.isoEstimate);
    const auto preview = feature->GetPreviewSnapshot();
    summary["previewRequestId"] = preview ? QString::number(preview->requestId) : QString();
    summary["previewPoints"]=preview && preview->points?QString::number(preview->points->size()):QString();
    // 只在当前调用范围持有正式网格；预览和估计不替换正式结果身份。
    const auto snapshot = feature->GetSurfaceSnapshot();
    if (!snapshot) return summary;
    summary["mesh"] = GetRefText(snapshot->meshRevision);
    summary["generation"]=GetRefText(snapshot->dataRevision);summary["selectedAreaMM2"]=snapshot->selectedAreaMM2;summary["selectionToleranceMM"]=snapshot->selectionToleranceMM;summary["selectionMesh"]=GetRefText(snapshot->selectionMeshRevision);
    summary["formalSource"]=GetRefText(snapshot->sourceRevision);
    summary["points"]=snapshot->points?QString::number(snapshot->points->size()):QString();
    if(state.purpose==SurfaceTaskPurpose::Determine && state.requestId==snapshot->requestId)
        formalStatistics={{"mesh",summary["mesh"]},{"acceptedPoints",QString::number(state.acceptedPointCount)},{"lowContrastPoints",QString::number(state.lowContrastPointCount)},{"rejectedPoints",QString::number(state.rejectedPointCount)},{"truncatedPoints",QString::number(state.truncatedPointCount)}};
    for(const auto* key:{"acceptedPoints","lowContrastPoints","rejectedPoints","truncatedPoints"})summary[key]=formalStatistics["mesh"]==summary["mesh"]?formalStatistics[key]:QJsonValue();
    summary["method"] = static_cast<int>(snapshot->method);
    if (!state.isoEstimate && snapshot->isoEstimate) summary["isoEstimate"] = describeEstimate(*snapshot->isoEstimate);
    QJsonArray objects;
    if (snapshot->objects) for (const auto& object : *snapshot->objects) {
        objects.append(QJsonObject{{"objectIndex", static_cast<int>(object.objectIndex)}, {"closed", object.isClosed},
            {"manifold", object.isManifold}, {"truncated", object.isTruncated}, {"orientationValid", object.isOrientationValid},
            {"areaValidity", static_cast<int>(object.areaValidity)}, {"volumeValidity", static_cast<int>(object.volumeValidity)},
            {"area", object.areaModelUnit2 ? QJsonValue(*object.areaModelUnit2) : QJsonValue()},
            {"volume", object.volumeModelUnit3 ? QJsonValue(*object.volumeModelUnit3) : QJsonValue()}});
        if (objects.size() >= 256) break;
    }
    summary["objects"] = objects;
    summary["objectCount"] = snapshot->objects?static_cast<int>(snapshot->objects->size()):0;
    summary["objectSummaryLimit"] = 256;
    return summary;
}
}
ModulePanel* CreateSurfaceTest(TestContext context, std::shared_ptr<SurfaceDeterminationHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Surface", parent);
    panel->SetNotice("选择体数据后，阈值留空时由当前表面算法估计背景/材料峰；显式阈值或材料范围直接按所填值处理。继续执行真实表面定位和质量检查，定位稳定性不等于计量不确定度。");
    const auto parameters = GetJson(R"({"componentSelection":"All","initialIsoValue":null,"materialRange":null,"seedModelPoint":null,"analysisRoi":"","parentGeneration":"","minimumObjectVoxels":"0"})");
    auto requestedParameters=std::make_shared<QJsonObject>();
    auto formalStatistics=std::make_shared<QJsonObject>();
    {
        const auto send=[panel, feature,requestedParameters,formalStatistics](std::uint64_t id, const QJsonObject& params, SurfaceTaskPurpose purpose) {
            SurfaceDeterminationStartParams start;
            start.purpose=purpose;
            start.targetViews = GetAllViews();
            start.modelUnit = "mm";
            const auto source = panel->GetSession()->GetImageDescriptor();
            if (!source) throw std::invalid_argument("请先选择体数据输入");
            start.sourceVolume = source->dataRevision;
            if (params.contains("componentSelection")) start.componentSelection = GetEnum<SurfaceComponentSelection>(params, "componentSelection", {
                {"Largest", SurfaceComponentSelection::Largest}, {"Seeded", SurfaceComponentSelection::Seeded}, {"All", SurfaceComponentSelection::All}});
            if (params.contains("materialRange") && !params["materialRange"].isNull()) start.materialRange = GetArray<double, 2>(params["materialRange"]);
            if (!params["initialIsoValue"].isNull()) start.initialIsoValue = GetNumber(params, "initialIsoValue");
            if (start.componentSelection == SurfaceComponentSelection::Seeded && !params["seedModelPoint"].isNull()) start.seedModelPoint = GetArray<double, 3>(params["seedModelPoint"]);
            if(!params["parentGeneration"].toString().isEmpty())start.parentGeneration=GetRef(params["parentGeneration"]);
            start.analysisRoi = GetSelectedRoi(*panel->GetSession(), params["analysisRoi"], source->dataRevision);
            start.minimumObjectVoxels = GetId(params["minimumObjectVoxels"]);
            SurfaceStartRequest request{start};
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(std::move(request), [owner, feature, id,requestedParameters,params,purpose,formalStatistics](SurfaceDeterminationResult result) {
                if (!owner) return;
                if(result.status==SurfaceResultStatus::Succeeded && purpose==SurfaceTaskPurpose::Determine)*requestedParameters=params;
                auto summary = GetSurface(feature,*formalStatistics);
                summary["status"] = static_cast<int>(result.status);
                summary["message"] = QString::fromStdString(result.message);
                owner->SetComplete(id, result.status == SurfaceResultStatus::Succeeded ? "Succeeded"
                    : result.status == SurfaceResultStatus::Cancelled ? "Cancelled" : "Failed", summary);
            });
            panel->SetAdmission(id, admission.status == SurfaceAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        };
        panel->AttachAction("GlobalAutomatic",parameters,[send](auto id,const auto& p){send(id,p,SurfaceTaskPurpose::Determine);},TestPolicy::Compute,true);
        for(const auto& action:std::vector<std::pair<QString,SurfaceTaskPurpose>>{{"Preview",SurfaceTaskPurpose::Preview},{"Determine",SurfaceTaskPurpose::Determine}})
            panel->AttachAction(action.first,{},[panel,send,action](auto id,const auto& p){
                send(id,p.isEmpty() ? panel->GetActionParameters("GlobalAutomatic") : p,action.second);
            },TestPolicy::Compute,true);
    }
    for (const QString action : {QString("Stop"), QString("Visibility"), QString("Clear"), QString("ClearPreview")}) {
        QJsonObject defaults;
        if (action == "Visibility") defaults = {{"isVisible", true}};
        if (action == "Stop") defaults = {{"targetRequestId", "0"}};
        panel->AttachAction(action, defaults, [panel, feature, action](auto id, const auto& params) {
            SurfaceDeterminationRequest request = SurfaceClearRequest{};
            if (action == "Visibility") request = SurfaceVisibilityRequest{GetBool(params, "isVisible")};
            else if (action == "Stop") request = SurfaceStopRequest{GetId(params["targetRequestId"])};
            else if (action == "ClearPreview") request = SurfaceClearPreviewRequest{};
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(std::move(request), [owner, id](SurfaceDeterminationResult result) {
                if (owner) owner->SetComplete(id, result.status == SurfaceResultStatus::Succeeded ? "Succeeded"
                    : result.status == SurfaceResultStatus::Cancelled ? "Cancelled" : "Failed",
                    {{"requestId", QString::number(result.requestId)}, {"message", QString::fromStdString(result.message)}});
            });
            panel->SetAdmission(id, admission.status == SurfaceAdmissionStatus::Accepted,
                {{"requestId", QString::number(admission.requestId)}, {"admission", static_cast<int>(admission.status)}});
        }, action == "Stop" ? TestPolicy::Stop : TestPolicy::Change);
    }
    for (const QString destination : {QString("Display"), QString("Gap"), QString("Part")})
        panel->AttachAction("CopyIsoTo" + destination, {}, [panel, feature, destination](auto id, const auto&) {
            const auto state = feature->GetState();
            const auto current = panel->GetSession()->GetImageDescriptor();
            if (!state.isoEstimate || !current || state.sourceRevision != current->dataRevision)
                throw std::invalid_argument("当前输入没有有效 ISO 估计");
            const auto iso = state.isoEstimate->isoValue;
            if (destination == "Display") {
                const auto view = panel->GetSession()->GetRenderViewState({"primary-3d"});
                if (!view || (view->viewMode != HostRenderMode::IsoSurface && view->viewMode != HostRenderMode::CompositeIsoSurface))
                    throw std::invalid_argument("主三维当前不是等值面模式，请先切换显示模式");
                HostViewSetRequest request; request.targetView.viewId = "primary-3d"; request.iso = iso;
                panel->SendHost(id, std::move(request));
            } else {
                const auto copy = panel->GetContext().workflow.onCopyParameters;
                if (!copy) throw std::invalid_argument("参数目标不可用");
                copy(destination, "Start", destination == "Gap" ? QJsonObject{{"isoMode", "AbsoluteValue"}, {"absoluteIsoValue", iso}} : QJsonObject{{"threshold", iso}});
                panel->SetComplete(id, "ParametersCopied", {{"iso", iso}});
            }
        });
    panel->AttachAction("SamplePoints", {{"maxPoints", "128"}}, [panel, feature](auto id, const auto& p) {
        const auto limit = GetId(p["maxPoints"]);
        if (limit == 0 || limit > 4096) throw std::invalid_argument("最大采样点数必须为 1～4096");
        const auto input = panel->GetSession()->GetImageDescriptor();
        auto snapshot = feature->GetSurfaceSnapshot();
        if (!snapshot || !input || snapshot->sourceRevision != input->dataRevision)
            snapshot = feature->GetPreviewSnapshot();
        if (!snapshot || !snapshot->points || !input || snapshot->sourceRevision != input->dataRevision)
            throw std::invalid_argument("当前输入没有网格点");
        const auto& points = *snapshot->points;
        const auto stride = std::max<std::size_t>(1, (points.size() + limit - 1) / limit);
        QJsonArray samples;
        for (std::size_t index = 0; index < points.size(); index += stride)
            samples.append(QJsonObject{{"vertexId", QString::number(index)}, {"sourcePoint", GetValues(points[index].positionModel)},
                {"normal", GetValues(points[index].normalModel)}, {"flags", static_cast<int>(points[index].flags)},
                {"validSupportRatio", points[index].validSupportRatio}, {"fitResidual", points[index].fitResidual},
                {"localizationSigma", points[index].estimatedLocalizationSigma}});
        panel->SetComplete(id, "Observed", {{"mesh", GetRefText(snapshot->meshRevision)}, {"source", GetRefText(snapshot->sourceRevision)},
            {"samples", samples}, {"totalPoints", QString::number(points.size())}});
    }, TestPolicy::Read);
    panel->AttachAction("MeasurementSelection", {{"minSupportRatio", .5}, {"maxLocalizationSigma", .5}, {"maxSurfaceFitResidual", 1.}},
        [panel, feature](auto id, const auto& p) {
            const auto snapshot = feature->GetSurfaceSnapshot();
            const auto input = panel->GetSession()->GetImageDescriptor();
            if (!snapshot || !input || snapshot->sourceRevision != input->dataRevision
                || !snapshot->points || !snapshot->triangleIndices || !snapshot->triangleValidity)
                throw std::invalid_argument("当前输入没有完整的测量网格与三角形质量信息");
            const auto& points = *snapshot->points;
            const auto& triangles = *snapshot->triangleIndices;
            if (triangles.size()%3 || snapshot->triangleValidity->size()!=triangles.size()/3)
                throw std::invalid_argument("三角形质量信息与拓扑不一致");
            const auto support = GetNumber(p, "minSupportRatio"), sigma = GetNumber(p, "maxLocalizationSigma"), residual = GetNumber(p, "maxSurfaceFitResidual");
            if (support<0 || support>1 || sigma<0 || residual<0) throw std::invalid_argument("测量质量范围无效");
            QJsonArray vertices, selectedTriangles;
            std::vector<bool> valid(points.size(), false), used(points.size(), false);
            for (std::size_t i=0; i<points.size(); ++i) {
                const auto& point=points[i];
                valid[i]=static_cast<int>(point.flags)==0 && point.validSupportRatio>=support
                    && point.estimatedLocalizationSigma<=sigma && point.fitResidual<=residual;
            }
            std::size_t degenerate=0, rejectedQuality=0;
            for (std::size_t i=0; i<triangles.size()/3; ++i) {
                const auto a=triangles[3*i], b=triangles[3*i+1], c=triangles[3*i+2];
                if (a>=points.size() || b>=points.size() || c>=points.size()) throw std::invalid_argument("测量网格索引越界");
                const auto& x=points[a].positionModel; const auto& y=points[b].positionModel; const auto& z=points[c].positionModel;
                const std::array<double,3> u{y[0]-x[0],y[1]-x[1],y[2]-x[2]}, v{z[0]-x[0],z[1]-x[1],z[2]-x[2]};
                const std::array<double,3> cross{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
                const auto areaSquared=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2];
                if (!std::isfinite(areaSquared) || areaSquared<=0) { ++degenerate; continue; }
                if (!valid[a] || !valid[b] || !valid[c] || !(*snapshot->triangleValidity)[i]) { ++rejectedQuality; continue; }
                selectedTriangles.append(QString::number(i));
                used[a]=used[b]=used[c]=true;
            }
            for (std::size_t i=0; i<used.size(); ++i) if (used[i]) vertices.append(QString::number(i));
            panel->SetComplete(id, "Observed", {{"source", GetRefText(snapshot->sourceRevision)}, {"mesh", GetRefText(snapshot->meshRevision)},
                {"vertexIds", vertices}, {"triangleIds", selectedTriangles}, {"totalVertices", QString::number(points.size())},
                {"selectedVertices", QString::number(vertices.size())}, {"totalTriangles", QString::number(triangles.size()/3)},
                {"selectedTriangles", QString::number(selectedTriangles.size())}, {"degenerateTriangles", QString::number(degenerate)},
                {"rejectedQualityTriangles", QString::number(rejectedQuality)}, {"criteria", p}});
        }, TestPolicy::Read);
    panel->AttachAction("ExportComparison", {{"outputPath",""}}, [panel,feature](auto id,const auto& p){
        auto snapshot=feature->GetSurfaceSnapshot();
        if(!snapshot)snapshot=feature->GetPreviewSnapshot();
        if(!snapshot||!snapshot->points)throw std::runtime_error("surface unavailable");
        QFile file(GetText(p,"outputPath"));if(!file.open(QIODevice::WriteOnly|QIODevice::Text))throw std::runtime_error("export open failed");
        QTextStream out(&file);out.setRealNumberPrecision(17);
        out<<"vertex_id,x_mm,y_mm,z_mm,nx,ny,nz,flags,support_ratio,fit_residual,localization_sigma_mm\n";
        std::size_t i=0;for(const auto& v:*snapshot->points){out<<qulonglong(i++);for(auto x:v.positionModel)out<<','<<x;for(auto x:v.normalModel)out<<','<<x;out<<','<<int(v.flags)<<','<<v.validSupportRatio<<','<<v.fitResidual<<','<<v.estimatedLocalizationSigma<<'\n';}
        out.flush();if(file.error()!=QFile::NoError)throw std::runtime_error("surface write failed");
        QJsonArray objects;if(snapshot->objects)for(const auto& o:*snapshot->objects)objects.append(QJsonObject{{"objectIndex",int(o.objectIndex)},{"area",o.areaModelUnit2?QJsonValue(*o.areaModelUnit2):QJsonValue()},{"volume",o.volumeModelUnit3?QJsonValue(*o.volumeModelUnit3):QJsonValue()},{"closed",o.isClosed},{"manifold",o.isManifold}});
        panel->SetComplete(id,"Exported",{{"points",QString::number(i)},{"triangles",QString::number(snapshot->triangleIndices?snapshot->triangleIndices->size()/3:0)},{"canonicalParameters",QString::fromStdString(snapshot->canonicalParameters)},{"objects",objects},{"coordinateFrame",QString::fromStdString(snapshot->coordinateFrame)}});
    },TestPolicy::Read);
#if defined(MANUAL_ALIGNMENT)
    panel->AttachAction("OpenAlignment", {}, [panel,feature](auto id, const auto&) {
        const auto surface=feature->GetSurfaceSnapshot();if(!surface)throw std::invalid_argument("正式测量表面不可用");
        if (panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("Alignment", "ImportReference", {{"source",GetRefText(surface->sourceRevision)},{"mesh",GetRefText(surface->meshRevision)}});
        panel->SetComplete(id, "ParametersCopied", {{"source",GetRefText(surface->sourceRevision)},{"mesh",GetRefText(surface->meshRevision)},{"message", "已带入正式网格；请准备该目标的名义参考"}});
    }, TestPolicy::Read);
#endif
#if defined(MANUAL_WALL)
    panel->AttachAction("OpenWall",{},[panel,feature](auto id,const auto&) {
        const auto surface=feature->GetSurfaceSnapshot();
        if(!surface) throw std::invalid_argument("正式测量表面不可用");
        const auto labels=panel->GetContext().workflow.getPartLabels ? panel->GetContext().workflow.getPartLabels() : DataRevisionRef{};
        QJsonObject patch{{"source",GetRefText(surface->sourceRevision)},{"mesh",GetRefText(surface->meshRevision)}};
        if(GetDataRevisionRefValid(labels)) patch["labels"]=GetRefText(labels);
        if(surface->resolvedParams.initialIsoValue) patch["materialThreshold"]=*surface->resolvedParams.initialIsoValue;
        if(panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("Wall","Start",patch);
        panel->SetComplete(id,"ParametersCopied",{{"message",GetDataRevisionRefValid(labels) ? "已带入同源测量表面，请核对材料标签。" : "已带入测量表面，仍需生成同源零件标签。"}});
    },TestPolicy::Read);
#endif
    panel->observeInBackground = true;
    panel->onObserve = [panel, feature,requestedParameters,formalStatistics] {
        const auto image = panel->GetSession()->GetImageDescriptor();
        if (image) {
            auto* form = panel->GetParameterEditor("GlobalAutomatic");
            auto range = image->scalarRange;
            try {
                const auto requested = form->GetField("materialRange")->GetValue();
                if (!requested.isNull()) range = GetArray<double,2>(requested);
            } catch (const std::exception&) { /* 未完成的手工输入留给提交校验。 */ }
            form->GetField("initialIsoValue")->SetDefaultValue(range[0] * 0.5 + range[1] * 0.5);
            form->GetField("materialRange")->SetDefaultValue(GetValues(image->scalarRange));
        }
        auto summary = GetSurface(feature,*formalStatistics);
        const auto state = feature->GetState();
        const auto snapshot = feature->GetSurfaceSnapshot();
        const auto input = panel->GetSession()->GetImageDescriptor();
        const auto preview = feature->GetPreviewSnapshot();
        const bool current = input && snapshot && snapshot->sourceRevision == input->dataRevision;
        const bool hasPreview = input && preview && preview->sourceRevision == input->dataRevision;
        const bool measured = current && GetDataRevisionRefValid(snapshot->meshRevision)
            && snapshot->purpose == SurfaceTaskPurpose::Determine && feature->GetResultValidity(snapshot->dataRevision).canMeasure;
        summary["hasIso"] = input && state.isoEstimate && state.sourceRevision == input->dataRevision;
        const auto display = panel->GetSession()->GetRenderViewState({"primary-3d"});
        summary["canApplyIsoToDisplay"] = display && (display->viewMode == HostRenderMode::IsoSurface || display->viewMode == HostRenderMode::CompositeIsoSurface);
        summary["hasPreview"] = hasPreview;
        summary["hasResult"] = current || hasPreview || summary["hasIso"].toBool();
        summary["hasMesh"] = (current && GetDataRevisionRefValid(snapshot->meshRevision)) || hasPreview;
        summary["isOverlayVisible"] = state.isOverlayVisible;
        const auto displayKey = hasPreview ? QString::number(preview->requestId) : summary["mesh"].toString();
        panel->GetParameterEditor("Visibility")->GetField("isVisible")->SetAppliedBoolean(summary["hasMesh"].toBool() ? QJsonValue(state.isOverlayVisible) : QJsonValue(), displayKey, summary["hasMesh"].toBool() ? QString() : "当前没有可用表面");
        summary["hasMeasurement"] = measured;
        summary["isFormalCurrent"]=current;
        try {
            const auto draft=panel->GetActionParameters("GlobalAutomatic");
            QJsonObject formal=*requestedParameters;
            if(snapshot) {
                const auto& p=snapshot->requestedParams;
                formal["componentSelection"]=p.componentSelection==SurfaceComponentSelection::Largest?"Largest":p.componentSelection==SurfaceComponentSelection::Seeded?"Seeded":"All";
                formal["initialIsoValue"]=p.initialIsoValue?QJsonValue(*p.initialIsoValue):QJsonValue();
                formal["materialRange"]=p.materialRange?QJsonValue(GetValues(*p.materialRange)):QJsonValue();
                if(p.componentSelection==SurfaceComponentSelection::Seeded)formal["seedModelPoint"]=p.seedModelPoint?QJsonValue(GetValues(*p.seedModelPoint)):QJsonValue();
                else formal.remove("seedModelPoint");
                formal["minimumObjectVoxels"]=QString::number(p.minimumObjectVoxels);
                formal["analysisRoi"]=p.analysisRoi?GetRefText(*p.analysisRoi):QString();
                summary["formalMesh"]=GetRefText(snapshot->meshRevision);summary["formalParameters"]=formal;
                summary["formalParameterFingerprint"]=QString::number(snapshot->parameterFingerprint);
            }
            summary["parametersChanged"]=current && draft!=formal;
        }
        catch(const std::exception&){summary["parametersChanged"]=true;}
        summary["isBusy"] = state.stage == SurfaceDeterminationStage::Preparing || state.stage == SurfaceDeterminationStage::ThresholdEstimation
            || state.stage == SurfaceDeterminationStage::SeedExtraction || state.stage == SurfaceDeterminationStage::SubvoxelRefinement
            || state.stage == SurfaceDeterminationStage::TopologyValidation || state.stage == SurfaceDeterminationStage::Committing || state.stage == SurfaceDeterminationStage::Stopping;
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Stop", {{"targetRequestId", "0"}}); };
    panel->onStatistics=[panel,feature] {
        const auto state=feature->GetState();
        auto estimate=state.isoEstimate;auto source=state.sourceRevision;
        if(!estimate)if(const auto snapshot=feature->GetSurfaceSnapshot()){estimate=snapshot->isoEstimate;source=snapshot->sourceRevision;}
        if(!estimate || estimate->sampleCount==0)return false;
        QJsonArray counts,edges;for(auto value:estimate->histogramCounts)counts.append(static_cast<double>(value));for(auto value:estimate->histogramEdges)edges.append(value);
        const auto description="来源 "+GetRefText(source)+QString(" · 原算法估计范围 · 512 桶最近中心分桶 · 采样步长 %1/%2/%3 · 有效 %4 · 排除 %5")
            .arg(estimate->histogramSampleSteps[0]).arg(estimate->histogramSampleSteps[1]).arg(estimate->histogramSampleSteps[2]).arg(estimate->sampleCount).arg(estimate->excludedSampleCount);
        panel->GetStatisticsWidget()->SetHistogram({{"title","表面初始灰度估计分布"},{"description",description},{"counts",counts},{"edges",edges},
            {"xUnit","原始灰度"},{"yUnit","算法估计样本次数"},{"markers",QJsonArray{
                QJsonObject{{"label","BG"},{"value",estimate->backgroundValue}},QJsonObject{{"label","M"},{"value",estimate->materialValue}},QJsonObject{{"label","ISO"},{"value",estimate->isoValue}}}}});
        const auto parameters=panel->GetActionParameters("GlobalAutomatic");
        const auto* sourceChoice=panel->findChild<QComboBox*>("surfaceParameterSource");
        const bool manual=sourceChoice && sourceChoice->currentData().toBool();
        auto* statistics=panel->GetStatisticsWidget();statistics->SetInteractionModes(manual?QStringList{"编辑手动 ISO"}:QStringList{});
        const auto iso=parameters["initialIsoValue"].isDouble()?parameters["initialIsoValue"].toDouble():estimate->isoValue;
        statistics->SetMarkers(QJsonArray{QJsonObject{{"label","BG"},{"value",estimate->backgroundValue}},QJsonObject{{"label","M"},{"value",estimate->materialValue}},
            QJsonObject{{"label",manual?"草稿 ISO":"ISO"},{"key","initialIsoValue"},{"editable",manual},{"value",iso}}});
        if(manual) {
            statistics->onInterval=[panel](double low,double high){panel->SetParameterPatch("GlobalAutomatic",{{"initialIsoValue",(low+high)*0.5}});if(panel->onMessage)panel->onMessage("已修改手动 ISO 草稿；BG/M 保持原估计，正式结果未改变。");};
            statistics->onMarker=[panel](const QString& key,double value){panel->SetParameterPatch("GlobalAutomatic",{{key,value}});};
        }
        return true;
    };
    return panel;
}
}
