// 测试用途：材料等值面、结果用途、阈值复制与有界采样。
#include "ModuleFactories.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Support/ParameterEditor.h"
#include "Support/ReferenceDataSource.h"
#include <QPointer>
#include <QFile>
#include <QTextStream>
namespace Manual {
namespace {
QJsonObject GetSurface(const std::shared_ptr<SurfaceDeterminationHostFeature>& feature)
{
    const auto state = feature->GetState();
    QJsonObject summary{{"stage", static_cast<int>(state.stage)}, {"failureReason", static_cast<int>(state.failureReason)},
        {"requestId", QString::number(state.requestId)}, {"source", GetRefText(state.sourceRevision)},
        {"resultRevision", QString::number(state.resultRevision)}, {"progress", state.progress01},
        {"points", QString::number(state.pointCount)}, {"acceptedPoints", QString::number(state.acceptedPointCount)},
        {"lowContrastPoints", QString::number(state.lowContrastPointCount)}, {"rejectedPoints", QString::number(state.rejectedPointCount)},
        {"truncatedPoints", QString::number(state.truncatedPointCount)}, {"error", QString::fromStdString(state.errorMessage)}};
    summary["purpose"] = static_cast<int>(state.purpose);
    if (state.isoEstimate) summary["isoEstimate"] = QJsonObject{{"iso", state.isoEstimate->isoValue},
        {"background", state.isoEstimate->backgroundValue}, {"material", state.isoEstimate->materialValue},
        {"samples", QString::number(state.isoEstimate->sampleCount)}};
    const auto preview = feature->GetPreviewSnapshot();
    summary["previewRequestId"] = preview ? QString::number(preview->requestId) : QString();
    // 只在当前调用范围持有正式网格；预览和估计不替换正式结果身份。
    const auto snapshot = feature->GetSurfaceSnapshot();
    if (!snapshot) return summary;
    summary["mesh"] = GetRefText(snapshot->meshRevision);
    summary["method"] = static_cast<int>(snapshot->method);
    if (!state.isoEstimate && snapshot->isoEstimate) summary["isoEstimate"] = QJsonObject{{"iso", snapshot->isoEstimate->isoValue},
        {"background", snapshot->isoEstimate->backgroundValue}, {"material", snapshot->isoEstimate->materialValue},
        {"samples", QString::number(snapshot->isoEstimate->sampleCount)}};
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
    summary["objectCount"] = static_cast<int>(state.objectCount);
    summary["objectSummaryLimit"] = 256;
    return summary;
}
}
ModulePanel* CreateSurfaceTest(TestContext context, std::shared_ptr<SurfaceDeterminationHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Surface", parent);
    panel->SetNotice("选择体数据后，未提供初始阈值时采用当前灰度范围中值；背景/材料灰度也可显式指定。业务继续执行真实的表面定位和质量检查，定位稳定性不等于计量不确定度。");
    const auto parameters = GetJson(R"({"componentSelection":"All","initialIsoValue":null,"materialRange":null,"seedModelPoint":null,"roiModelBounds":null,"minimumObjectVoxels":"0"})");
    {
        panel->AttachAction("GlobalAutomatic", parameters, [panel, feature](auto id, const auto& params) {
            SurfaceDeterminationStartParams start;
            start.targetViews = GetAllViews();
            start.modelUnit = "mm";
            const auto source = panel->GetSession()->GetImageDescriptor();
            if (!source) throw std::invalid_argument("请先选择体数据输入");
            start.sourceVolume = source->dataRevision;
            if (params.contains("componentSelection")) start.componentSelection = GetEnum<SurfaceComponentSelection>(params, "componentSelection", {
                {"Largest", SurfaceComponentSelection::Largest}, {"Seeded", SurfaceComponentSelection::Seeded}, {"All", SurfaceComponentSelection::All}});
            if (params.contains("materialRange") && !params["materialRange"].isNull()) start.materialRange = GetArray<double, 2>(params["materialRange"]);
            const auto range = start.materialRange.value_or(source->scalarRange);
            start.initialIsoValue = GetInputNumber(params, "initialIsoValue", range[0] * 0.5 + range[1] * 0.5);
            if (start.componentSelection == SurfaceComponentSelection::Seeded && !params["seedModelPoint"].isNull()) start.seedModelPoint = GetArray<double, 3>(params["seedModelPoint"]);
            if (!params["roiModelBounds"].isNull()) {
                const auto source = panel->GetSession()->GetImageDescriptor();
                if (!source) throw std::invalid_argument("表面范围需要源图像");
                start.analysisRoi = CreateInputRoi(*panel->GetSession(), source->dataRevision,
                    params["roiModelBounds"], QJsonValue(), "Surface analysis region", true);
            }
            start.minimumObjectVoxels = GetId(params["minimumObjectVoxels"]);
            SurfaceDeterminationRequest request; request.action = SurfaceDeterminationAction::Start; request.start = start;
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(std::move(request), [owner, feature, id](SurfaceDeterminationResult result) {
                if (!owner) return;
                auto summary = GetSurface(feature);
                summary["status"] = static_cast<int>(result.status);
                summary["message"] = QString::fromStdString(result.message);
                const auto snapshot = feature->GetSurfaceSnapshot();
                if (result.status == SurfaceResultStatus::Succeeded && snapshot && GetDataRevisionRefValid(snapshot->meshRevision)
                    && snapshot->purpose == SurfaceTaskPurpose::Determine && feature->GetResultValidity(snapshot->dataRevision).canMeasure)
                    owner->GetContext().workflow.SetSurfaceInput(snapshot->sourceRevision, snapshot->meshRevision, snapshot->resolvedParams.initialIsoValue);
                else owner->GetContext().workflow.SetSurfaceInput({}, {});
                owner->SetComplete(id, result.status == SurfaceResultStatus::Succeeded ? "Succeeded"
                    : result.status == SurfaceResultStatus::Cancelled ? "Cancelled" : "Failed", summary);
            });
            panel->SetAdmission(id, admission.status == SurfaceAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        }, TestPolicy::Compute, true);
    }
    for (const auto& action : std::vector<std::pair<QString, SurfaceDeterminationAction>>{
        {"Stop", SurfaceDeterminationAction::Stop}, {"Visibility", SurfaceDeterminationAction::SetVisibility}, {"Clear", SurfaceDeterminationAction::Clear}, {"ClearPreview", SurfaceDeterminationAction::ClearPreview}}) {
        QJsonObject defaults;
        if (action.second == SurfaceDeterminationAction::SetVisibility) defaults = {{"isVisible", true}};
        if (action.second == SurfaceDeterminationAction::Stop) defaults = {{"targetRequestId", "0"}};
        panel->AttachAction(action.first, defaults, [panel, feature, action](auto id, const auto& params) {
            SurfaceDeterminationRequest request; request.action = action.second;
            if (request.action == SurfaceDeterminationAction::SetVisibility) request.isVisible = GetBool(params, "isVisible");
            if (request.action == SurfaceDeterminationAction::Stop) request.targetRequestId = GetId(params["targetRequestId"]);
            const auto admission = feature->SendRequest(std::move(request));
            if (action.second == SurfaceDeterminationAction::Clear && admission.status == SurfaceAdmissionStatus::Accepted)
                panel->GetContext().workflow.SetSurfaceInput({}, {});
            panel->SetComplete(id, admission.status == SurfaceAdmissionStatus::Accepted ? "Accepted" : "Rejected",
                {{"admission", static_cast<int>(admission.status)}});
        }, action.second == SurfaceDeterminationAction::Stop ? TestPolicy::Stop : TestPolicy::Change);
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
    panel->AttachAction("OpenAlignment", {}, [panel](auto id, const auto&) {
        if (panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("Alignment", "ImportReference", {});
        panel->SetComplete(id, "ParametersCopied", {{"message", "已进入计量对齐，请导入与当前测量表面对应的名义参考"}});
    }, TestPolicy::Read);
#endif
    panel->observeInBackground = true;
    panel->onObserve = [panel, feature] {
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
        auto summary = GetSurface(feature);
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
        summary["isBusy"] = state.stage == SurfaceDeterminationStage::Preparing || state.stage == SurfaceDeterminationStage::ThresholdEstimation
            || state.stage == SurfaceDeterminationStage::SeedExtraction || state.stage == SurfaceDeterminationStage::SubvoxelRefinement
            || state.stage == SurfaceDeterminationStage::TopologyValidation || state.stage == SurfaceDeterminationStage::Committing || state.stage == SurfaceDeterminationStage::Stopping;
        if (measured) panel->GetContext().workflow.SetSurfaceInput(snapshot->sourceRevision, snapshot->meshRevision, snapshot->resolvedParams.initialIsoValue);
        else panel->GetContext().workflow.SetSurfaceInput({}, {});
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Stop", {{"targetRequestId", "0"}}); };
    return panel;
}
}
