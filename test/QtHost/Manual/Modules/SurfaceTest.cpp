// 测试用途：通过表面页面测试四种确定方法、结果显示、阈值复制与有界采样。
#include "ModuleFactories.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Support/ParameterEditor.h"
#include <QPointer>
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
    if (snapshot->isoEstimate) summary["isoEstimate"] = QJsonObject{{"iso", snapshot->isoEstimate->isoValue},
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
    panel->SetNotice("自动 ISO50 仅估计阈值；全局等值面预览点不能直接用于测量。对齐请使用局部自适应或梯度峰值方法的有效点，定位稳定性不等于计量不确定度。");
    const auto defaults = GetJson(R"({"componentSelection":"Largest","initialIsoValue":null,"seedModelPoint":null,"roiModelBounds":null,"profileHalfLengthModel":null,"profileSampleStepModel":null,"maximumOffsetModel":null,"profileSmoothingSigmaModel":null,"minimumObjectVoxels":"1","minimumContrast":0})");
    for (const auto& method : std::vector<std::pair<QString, SurfaceDeterminationMethod>>{
        {"AutomaticIso50", SurfaceDeterminationMethod::AutomaticIso50}, {"GlobalIsoPreview", SurfaceDeterminationMethod::GlobalIsoPreview},
        {"LocalAdaptiveIso50", SurfaceDeterminationMethod::LocalAdaptiveIso50}, {"GradientPeak", SurfaceDeterminationMethod::GradientPeak}}) {
        auto parameters = defaults;
        if (method.second == SurfaceDeterminationMethod::AutomaticIso50) parameters = {{"roiModelBounds", QJsonValue()}};
        else if (method.second == SurfaceDeterminationMethod::GlobalIsoPreview) {
            for (const auto* key : {"profileHalfLengthModel", "profileSampleStepModel", "maximumOffsetModel", "profileSmoothingSigmaModel", "minimumContrast"}) parameters.remove(key);
        }
        panel->AttachAction(method.first, parameters, [panel, feature, method](auto id, const auto& params) {
            SurfaceDeterminationStartParams start;
            start.method = method.second; start.targetViews = GetMainViews();
            start.modelUnit = "mm";
            if (const auto source = panel->GetSession()->GetImageDescriptor()) start.sourceVolume = source->dataRevision;
            if (params.contains("componentSelection")) start.componentSelection = GetEnum<SurfaceComponentSelection>(params, "componentSelection", {
                {"Largest", SurfaceComponentSelection::Largest}, {"Seeded", SurfaceComponentSelection::Seeded}, {"All", SurfaceComponentSelection::All}});
            if (params.contains("initialIsoValue") && !params["initialIsoValue"].isNull()) start.initialIsoValue = GetNumber(params, "initialIsoValue");
            if (start.componentSelection == SurfaceComponentSelection::Seeded && !params["seedModelPoint"].isNull()) start.seedModelPoint = GetArray<double, 3>(params["seedModelPoint"]);
            if (!params["roiModelBounds"].isNull()) start.roiModelBounds = GetArray<double, 6>(params["roiModelBounds"]);
            if (params.contains("profileHalfLengthModel") && !params["profileHalfLengthModel"].isNull()) start.profileHalfLengthModel = GetNumber(params, "profileHalfLengthModel");
            if (params.contains("profileSampleStepModel") && !params["profileSampleStepModel"].isNull()) start.profileSampleStepModel = GetNumber(params, "profileSampleStepModel");
            if (params.contains("maximumOffsetModel") && !params["maximumOffsetModel"].isNull()) start.maximumOffsetModel = GetNumber(params, "maximumOffsetModel");
            if (params.contains("profileSmoothingSigmaModel") && !params["profileSmoothingSigmaModel"].isNull()) start.profileSmoothingSigmaModel = GetNumber(params, "profileSmoothingSigmaModel");
            if (params.contains("minimumObjectVoxels")) start.minimumObjectVoxels = GetId(params["minimumObjectVoxels"]);
            if (params.contains("minimumContrast")) start.minimumContrast = GetNumber(params, "minimumContrast");
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
                    owner->GetContext().workflow.SetSurfaceInput(snapshot->sourceRevision, snapshot->meshRevision);
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
        auto snapshot = feature->GetSurfaceSnapshot();
        if (!snapshot) snapshot = feature->GetPreviewSnapshot();
        if (!snapshot || !snapshot->points) throw std::invalid_argument("没有网格点");
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
#if defined(MANUAL_ALIGNMENT)
    panel->AttachAction("OpenAlignment", {}, [panel](auto id, const auto&) {
        if (panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("Alignment", "ImportReference", {});
        panel->SetComplete(id, "ParametersCopied", {{"message", "已进入计量对齐，请导入与当前测量表面对应的名义参考"}});
    }, TestPolicy::Read);
#endif
    panel->observeInBackground = true;
    panel->onObserve = [panel, feature] {
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
        if (measured) panel->GetContext().workflow.SetSurfaceInput(snapshot->sourceRevision, snapshot->meshRevision);
        else panel->GetContext().workflow.SetSurfaceInput({}, {});
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Stop", {{"targetRequestId", "0"}}); };
    return panel;
}
}
