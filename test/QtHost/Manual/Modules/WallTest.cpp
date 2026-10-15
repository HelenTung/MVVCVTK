// 测试用途：通过公开 Feature 请求测试壁厚计算、评估、显示、结果激活与采样定位。
#include "ModuleFactories.h"
#include "Support/ColorSegmentInput.h"
#include "Support/ParameterEditor.h"
#include "Support/RoiSelection.h"
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
    QJsonObject reasonCounts, reasonAreas;
    const char* reasons[] = {"Valid", "LowSurfaceQuality", "AmbiguousOpposite", "SharpFeature", "TruncatedSupport",
        "SearchLimitReached", "InvalidMaterialPath", "OutsideEvaluation", "NoValidSource"};
    for (std::size_t index = 0; index < static_cast<std::size_t>(ThicknessValidity::Count); ++index) {
        reasonCounts[reasons[index]] = QString::number(s.reasonCounts[index]);
        reasonAreas[reasons[index]] = s.reasonAreas[index];
    }
    return {{"result", GetRefText(result.result)}, {"isCurrent", result.isCurrent},
        {"source",GetRefText(result.archive.input.source)},{"labels",GetRefText(result.archive.input.labels)},
        {"mesh",GetRefText(result.archive.input.mesh)},
        {"evaluationRoi",result.archive.params.evaluationRoi?QJsonValue(GetRefText(*result.archive.params.evaluationRoi)):QJsonValue()},
        {"sampleCount", QString::number(s.sampleCount)}, {"validCount", QString::number(s.validCount)},
        {"unmeasuredCount",QString::number(s.sampleCount-s.validCount)},
        {"algorithmVersion",QString::fromStdString(result.archive.algorithmVersion)},
        {"method","局部灰度射线共同法线测量"},{"hasPartialCoverage",s.coverage<1.0},
        {"coverage", s.coverage}, {"minimum", s.minimum ? QJsonValue(*s.minimum) : QJsonValue()},
        {"minimumSampleIndex", s.minimumSample ? QJsonValue(QString::number(*s.minimumSample)) : QJsonValue()},
        {"maximum", s.maximum ? QJsonValue(*s.maximum) : QJsonValue()}, {"mean", s.mean ? QJsonValue(*s.mean) : QJsonValue()},
        {"regionCount", QString::number(result.regions.size())}, {"reasonCounts", reasonCounts}, {"reasonAreas", reasonAreas}};
}
ThicknessEvaluation Evaluation(const QJsonObject& p)
{
    ThicknessEvaluation value; value.lower = GetNumber(p, "lower"); value.upper = GetNumber(p, "upper");
    value.histogramRange = GetArray<double, 2>(p["histogramRange"]);
    value.histogramBins = Count(p, "histogramBins", 65536); value.minRegionArea = GetNumber(p, "minRegionArea");
    return value;
}
QJsonObject EvaluationJson(const ThicknessEvaluation& v)
{
    return {{"lower",v.lower},{"upper",v.upper},{"histogramRange",GetValues(v.histogramRange)},{"histogramBins",double(v.histogramBins)},{"minRegionArea",v.minRegionArea}};
}
QJsonObject MeasurementJson(const ThicknessInput& input,const ThicknessParams& p)
{
    QJsonArray labels;for(auto id:input.materialLabels)labels.append(QString::number(id));
    return {{"source",GetRefText(input.source)},{"labels",GetRefText(input.labels)},{"mesh",GetRefText(input.mesh)},{"materialLabels",labels},{"unit",int(input.unit)},
        {"maxDistance",p.maxDistance},{"sampleSpacing",p.sampleSpacing},{"materialThreshold",p.materialThreshold?QJsonValue(*p.materialThreshold):QJsonValue()},
        {"coneAngleDegrees",p.coneAngleDegrees},{"directionCount",int(p.directionCount)},{"boundaryPolicy",int(p.boundaryPolicy)},{"maxBoundaryError",p.maxBoundaryError},
        {"evaluationBounds",p.evaluationBounds?QJsonValue(GetValues(*p.evaluationBounds)):QJsonValue()},
        {"evaluationRoi",p.evaluationRoi?QJsonValue(GetRefText(*p.evaluationRoi)):QJsonValue()}};
}
ThicknessDisplay Display(const QJsonObject& p)
{
    ThicknessDisplay value; value.targetViews = GetAllViews();
    value.mode = GetEnum<ThicknessDisplayMode>(p, "mode", {{"Continuous", ThicknessDisplayMode::Continuous}, {"Tolerance", ThicknessDisplayMode::Tolerance}});
    if (p.contains("range")) value.range = GetArray<double, 2>(p["range"]);
    value.legendPosition=GetArray<double,2>(p["legendPosition"]);
    value.opacity = GetNumber(p, "opacity");
    if(p.contains("isReversed"))value.colorBand.isReversed=GetBool(p,"isReversed");
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
    value.toleranceBelowColor=GetArray<double,3>(p["toleranceBelowColor"]);
    value.toleranceWithinColor=GetArray<double,3>(p["toleranceWithinColor"]);
    value.toleranceAboveColor=GetArray<double,3>(p["toleranceAboveColor"]);
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
    const auto viewed=[panel,resolveResult]{return resolveResult(panel->GetActionParameters("Result"));};
    auto appliedDisplays=std::make_shared<QMap<QString,QJsonObject>>();
    struct ViewLease {
        std::uint64_t action=0, presentationCommand=0;
        HostRenderMode original=HostRenderMode::Volume;
        DataRevisionRef source;
        bool preparing=true, cancelled=false;
    };
    auto lease=std::make_shared<std::shared_ptr<ViewLease>>();
    const QPointer<ModulePanel> wallOwner(panel);
    const auto afterUpdates=[wallOwner](std::function<void()> next) {
        // Host 写入回调与已提交的场景快照不是同一时钟；退出当前回调栈后推进展示事务。
        if(wallOwner)QMetaObject::invokeMethod(wallOwner,[wallOwner,next]{
            if(!wallOwner)return;
            if(const auto session=wallOwner->GetSession())(void)session->SendUpdates();
            next();
        },Qt::QueuedConnection);
    };
    const auto restore=[wallOwner,lease,afterUpdates](std::uint64_t id,std::function<void(bool)> done) {
        if(!wallOwner || !*lease || (*lease)->action!=id) {done(true);return;}
        const auto saved=*lease;
        const auto finish=[lease,saved,done](bool succeeded){if(*lease==saved)*lease={};done(succeeded);};
        afterUpdates([wallOwner,saved,finish,afterUpdates]{
        if(!wallOwner) {finish(false);return;}
        const auto session=wallOwner->GetSession();
        if(!session) {finish(false);return;}
        const auto scenes=session->GetSceneViewStates();
        const auto current=std::find_if(scenes.begin(),scenes.end(),[](const auto& view){return view.id=="primary-3d";});
        // 用户改变展示或输入后保留其选择；只撤销本次测量取得的显示资源租约。
        const auto preparedMode=saved->original==HostRenderMode::Volume?HostRenderMode::IsoSurface:HostRenderMode::CompositeIsoSurface;
        if(current==scenes.end() || wallOwner->GetContext().workflow.primaryPresentationCommand!=saved->presentationCommand
            || !current->presentation || current->presentation->dataRevision!=saved->source
            || current->presentation->viewMode!=preparedMode) {finish(true);return;}
        HostViewSetRequest request;request.targetView.viewId="primary-3d";request.mode=saved->original;
        const auto admission=session->SendRequest(std::move(request),[finish,afterUpdates](HostResult result){
            afterUpdates([finish,result]{finish(result.isSucceeded);});
        });
        if(admission.status!=HostAdmissionStatus::Accepted)finish(false);
        });
    };
    const auto send = [panel, feature, visible,appliedDisplays,restore](std::uint64_t id, ThicknessRequest request) {
        std::optional<ThicknessDisplay> display;
        if (const auto* value = std::get_if<ThicknessStartRequest>(&request)) display = value->display;
        else if (const auto* value = std::get_if<ThicknessDisplayRequest>(&request)) display = value->display;
        else if (const auto* value = std::get_if<ThicknessActivationRequest>(&request)) display = value->display;
        const QPointer<ModulePanel> owner(panel);
        const auto admission = feature->SendRequest(std::move(request), [owner, feature, visible, display, id,appliedDisplays,restore](const ThicknessResult& result) {
            if (!owner) return;
            if (display && result.status == ThicknessStatus::Succeeded) *visible = display->isVisible;
            const auto record=owner->GetContext().records.GetRecord(id);
            if(result.status==ThicknessStatus::Succeeded && record["action"]=="SetDisplay")(*appliedDisplays)[GetRefText(feature->GetState().result)]=record["parameters"].toObject();
            if(result.status==ThicknessStatus::Succeeded && record["action"]=="SetEvaluation")owner->SetParameterPatch("Result",{{"result",GetRefText(result.result)}});
            QJsonObject info{{"requestId", QString::number(result.requestId)}, {"result", GetRefText(result.result)},
                {"statusCode", static_cast<int>(result.status)}, {"message", QString::fromStdString(result.message)},
                {"isActivated", result.isActivated}, {"isDisplayReady", result.isDisplayReady}};
            if (const auto range = feature->GetState().displayRange) info["displayRange"] = GetValues(*range);
            if (const auto snapshot = feature->GetResult(result.result)) {
                const auto stats = Statistics(*snapshot); for (auto it = stats.begin(); it != stats.end(); ++it) info[it.key()] = it.value();
            }
            restore(id,[owner,id,status=Status(result.status),info](bool restored) mutable {
                info["viewRestoreSucceeded"]=restored;
                if(!restored && status=="Succeeded")status="SucceededWithDisplayFailure";
                if(owner)owner->SetComplete(id,status,info);
            });
        });
        panel->SetAdmission(id, admission.status == ThicknessAdmissionStatus::Accepted,
            {{"requestId", QString::number(admission.requestId)}, {"admissionStatus", static_cast<int>(admission.status)}});
        if(admission.status!=ThicknessAdmissionStatus::Accepted)restore(id,[](bool){});
    };
    const auto build=[panel](const QJsonObject& p) {
        ThicknessInputSelection selection;
        if (GetText(p, "source") != "current") selection.source = GetRef(p["source"]);
        if (GetText(p, "labels") != "parts") selection.labels = GetRef(p["labels"]);
        if (GetText(p, "mesh") != "surface") selection.mesh = GetRef(p["mesh"]);
        auto& workflow = panel->GetContext().workflow;
        const auto source = selection.source ? panel->GetSession()->GetImageDescriptor(*selection.source)
            : panel->GetSession()->GetImageDescriptor();
        if (!source) throw std::invalid_argument("所选壁厚体输入不可读");
        for (const auto label : p["materialLabels"].toArray()) selection.materialLabels.push_back(GetId(label));
        selection.unit = GetEnum<ThicknessUnit>(p, "unit", {{"Millimeter", ThicknessUnit::Millimeter}, {"Meter", ThicknessUnit::Meter}});
        if (selection.unit != ThicknessUnit::Millimeter)
            throw std::invalid_argument("Host 图像输入使用毫米几何；此测试入口不执行单位转换");
        ThicknessParams params;
        params.maxDistance = GetInputNumber(p,"maxDistance",GetInputDiagonal(*source));
        params.sampleSpacing = GetInputNumber(p,"sampleSpacing",2.0 * GetVoxelSpacing(*source));
        const auto surface = workflow.GetMeasurementSurface();
        params.materialThreshold = p["materialThreshold"].isNull()
            ? (surface && surface->source == source->dataRevision
                && (!selection.mesh || *selection.mesh == surface->mesh) && surface->threshold
                ? surface->threshold : std::optional<double>{GetScalarMidpoint(*source)})
            : std::optional<double>{GetNumber(p, "materialThreshold")};
        params.coneAngleDegrees = GetNumber(p, "coneAngleDegrees"); params.directionCount = static_cast<std::uint32_t>(Count(p, "directionCount", 4096));
        params.boundaryPolicy = GetEnum<ThicknessBoundaryPolicy>(p, "boundaryPolicy", {
            {"Complete", ThicknessBoundaryPolicy::Complete}, {"SourceExtentLocal", ThicknessBoundaryPolicy::SourceExtentLocal}});
        params.maxBoundaryError = GetInputNumber(p,"maxBoundaryError",0.5 * GetVoxelSpacing(*source));
        params.evaluationRoi=GetSelectedRoi(*panel->GetSession(),p["evaluationRoi"],source->dataRevision);
        auto request = workflow.BuildThicknessRequest(*panel->GetSession(), selection, std::move(params));
        ThicknessDisplay display; display.targetViews = GetAllViews(); display.range = {0, request.params.maxDistance};
        display.rangeMode = ThicknessRangeMode::Result; request.display = display;
        ThicknessEvaluation evaluation; evaluation.upper = request.params.maxDistance; evaluation.histogramRange = display.range; request.evaluation = evaluation;
        return request;
    };
    panel->AttachAction("Start", GetJson(R"({"source":"current","labels":"parts","mesh":"surface","materialLabels":[],"unit":"Millimeter",
        "maxDistance":null,"sampleSpacing":null,"materialThreshold":null,
        "coneAngleDegrees":30,"directionCount":9,"boundaryPolicy":"SourceExtentLocal",
        "maxBoundaryError":null,"evaluationRoi":""})"), [panel,feature,build,send,lease,restore,wallOwner,afterUpdates](auto id,const auto& p){
        auto request=build(p);
        if(*lease || feature->GetState().isBusy) {panel->SetAdmission(id,false,{{"message","壁厚测量正在进行"}});return;}
        auto saved=std::make_shared<ViewLease>();saved->action=id;*lease=saved;
        afterUpdates([wallOwner,lease,saved,request,send,restore,afterUpdates]() mutable {
        if(!wallOwner || *lease!=saved)return;
        if(saved->cancelled) {*lease={};wallOwner->SetComplete(saved->action,"Cancelled");return;}
        const auto session=wallOwner->GetSession();
        if(!session){*lease={};wallOwner->SetComplete(saved->action,"Failed");return;}
        const auto view=session->GetRenderViewState({"primary-3d"});
        if(!view || (view->viewMode!=HostRenderMode::Volume && view->viewMode!=HostRenderMode::CompositeVolume)) {*lease={};send(saved->action,std::move(request));return;}
        saved->original=view->viewMode;saved->source=view->dataRevision;
        saved->presentationCommand=wallOwner->GetContext().workflow.primaryPresentationCommand;
        HostViewSetRequest display;display.targetView.viewId="primary-3d";
        display.mode=view->viewMode==HostRenderMode::Volume?HostRenderMode::IsoSurface:HostRenderMode::CompositeIsoSurface;
        // 原始灰度 CUDA 输入与体渲染纹理分时使用显存；不裁剪、降采样或替换测量输入。
        const auto admission=session->SendRequest(std::move(display),[wallOwner,lease,saved,request,send,restore,afterUpdates](HostResult result) mutable {
            afterUpdates([wallOwner,lease,saved,request,send,restore,result]() mutable {
            if(!wallOwner || *lease!=saved)return;
            if(saved->cancelled) {
                restore(saved->action,[wallOwner,saved](bool restored){if(wallOwner)wallOwner->SetComplete(saved->action,"Cancelled",{{"viewRestoreSucceeded",restored}});});
                return;
            }
            if(!result.isSucceeded) {
                restore(saved->action,[wallOwner,saved,result](bool restored){if(wallOwner)wallOwner->SetComplete(saved->action,"Failed",{{"message",QString::fromStdString(result.message)},{"viewRestoreSucceeded",restored}});});
                return;
            }
            const auto view=wallOwner->GetSession()->GetRenderViewState({"primary-3d"});
            const auto preparedMode=saved->original==HostRenderMode::Volume?HostRenderMode::IsoSurface:HostRenderMode::CompositeIsoSurface;
            if(!view || view->viewMode!=preparedMode || view->dataRevision!=saved->source) {
                restore(saved->action,[wallOwner,saved](bool restored){if(wallOwner)wallOwner->SetComplete(saved->action,"Failed",
                    {{"message","壁厚准备期间三维展示或输入已改变，未启动测量。"},{"viewRestoreSucceeded",restored}});});
                return;
            }
            saved->preparing=false;send(saved->action,std::move(request));
            });
        });
        if(admission.status!=HostAdmissionStatus::Accepted) {*lease={};wallOwner->SetComplete(saved->action,"Rejected",{{"message",QString::fromStdString(admission.message)}});}
        });
    }, TestPolicy::Compute, true);
    panel->AttachAction("Cancel", {{"targetRequestId", "0"}}, [panel,send,lease](auto id, const auto& p) {
        if(*lease && (*lease)->preparing && GetId(p["targetRequestId"])==0) {
            (*lease)->cancelled=true;panel->SetAdmission(id,true);panel->SetComplete(id,"Succeeded");
            return;
        }
        ThicknessRequest r; r = ThicknessCancelRequest{}; std::get<ThicknessCancelRequest>(r).targetRequestId = GetId(p["targetRequestId"]); send(id, r);
    }, TestPolicy::Stop);
    panel->AttachAction("Clear", {}, [send](auto id, const auto&) {
        ThicknessRequest r; r = ThicknessClearRequest{}; send(id, r);
    });
    panel->AttachAction("SetEvaluation", GetJson(R"({"lower":0,"upper":null,"histogramRange":null,"histogramBins":32,"minRegionArea":0})"),
        [feature, send,viewed](auto id, const auto& p) {
            const auto result = feature->GetResult(viewed());
            if (!result || !result->statistics.maximum) throw std::invalid_argument("所选结果没有可评估的有效厚度值");
            auto parameters = p;
            parameters["upper"] = GetInputNumber(p,"upper",std::max(*result->statistics.maximum,std::numeric_limits<double>::epsilon()));
            if (p["histogramRange"].isNull()) parameters["histogramRange"] = QJsonArray{0.,parameters["upper"]};
            send(id, ThicknessEvaluationRequest{Evaluation(parameters)});
        }, TestPolicy::Compute);
    panel->AttachAction("SetDisplay", GetJson(R"({"result":"current","isReversed":false,"mode":"Continuous","rangeMode":"Result","range":[0,1],"palette":"InverseRainbow",
        "toleranceBelowColor":[0.8156862745098039,0.34509803921568627,0.34901960784313724],"toleranceWithinColor":[0.3803921568627451,0.7019607843137254,0.44313725490196076],"toleranceAboveColor":[0.3254901960784314,0.4470588235294118,0.7372549019607844],"constantColor":null,"lowColor":null,"highColor":null,"belowColor":null,"aboveColor":null,
        "segments":[],"displayStyle":"Overlay","opacityRange":[0.15,1],
        "legendPosition":[0.025,0.20],"opacity":1,"isVisible":true,"hasLegend":true})"),
        [send,resolveResult,feature](auto id,const auto& p) {
            auto d=Display(p);const auto ref=resolveResult(p);const auto snapshot=feature->GetResult(ref);
            if(snapshot && snapshot->appliedDisplay){d.hiddenLegendViews=snapshot->appliedDisplay->hiddenLegendViews;d.legendPositions=snapshot->appliedDisplay->legendPositions;}
            if(!p["legendView"].toString().isEmpty())d.legendPositions[p["legendView"].toString().toStdString()]=d.legendPosition;
            send(id,ThicknessDisplayRequest{d,ref});
        }, TestPolicy::View);
    panel->getColorParameters=[panel,feature,appliedDisplays] {
        const auto ref=feature->GetState().result;if(!GetDataRevisionRefValid(ref))return QJsonObject{};
        auto p=panel->GetActionParameters("SetDisplay");
        const auto snapshot=feature->GetResult(ref);const auto d=snapshot && snapshot->appliedDisplay ? *snapshot->appliedDisplay:ThicknessDisplay{};
        const auto& band=d.colorBand;const auto colors=GetColorBandJson(band.mode,band.constantColor,band.lowColor,band.highColor,band.belowColor,band.aboveColor,band.segments,band.isReversed);
        for(auto i=colors.begin();i!=colors.end();++i)p[i.key()]=i.value();
        const char* ranges[]{"Manual","Result","Histogram"};const char* styles[]{"Overlay","Constant","Inclined","InverseInclined"};
        p["rangeMode"]=ranges[static_cast<unsigned>(d.rangeMode)];p["range"]=GetValues(d.range);p["displayStyle"]=styles[static_cast<unsigned>(d.style)];
        p["toleranceBelowColor"]=GetValues(d.toleranceBelowColor);p["toleranceWithinColor"]=GetValues(d.toleranceWithinColor);p["toleranceAboveColor"]=GetValues(d.toleranceAboveColor);
        p["mode"]=d.mode==ThicknessDisplayMode::Continuous ? "Continuous":"Tolerance";p["opacity"]=d.opacity;p["hasLegend"]=d.hasLegend;p["isVisible"]=d.isVisible;p["opacityRange"]=GetValues(d.opacityRange);
        p["legendPosition"]=GetValues(d.legendPosition);p["result"]=GetRefText(ref);return p;
    };
    panel->getColorPosition=[feature](const QString& id) {
        const auto d=feature->GetState().appliedDisplay.value_or(ThicknessDisplay{});const auto i=d.legendPositions.find(id.toStdString());const auto p=i==d.legendPositions.end() ? d.legendPosition:i->second;return QPointF(p[0],p[1]);
    };
    panel->getColorSurface=[feature] { const auto state=feature->GetState();const auto result=feature->GetResult(state.result);return result ? GetRefText(result->archive.input.mesh):QString(); };
    panel->setColorVisible=[feature](bool visible){const auto state=feature->GetState();if(!state.appliedDisplay)return;auto d=*state.appliedDisplay;d.isVisible=visible;feature->SendRequest(ThicknessDisplayRequest{d,state.result});};
    panel->setViewLegend=[feature](const QString& id,bool visible,int slot) {
        const auto state=feature->GetState();if(!state.appliedDisplay)return;
        auto d=*state.appliedDisplay;const auto view=id.toStdString();
        if(visible){d.hasLegend=true;d.legendPositions[id.toStdString()]={.025+.25*slot,.20};}
        auto& hidden=d.hiddenLegendViews;hidden.erase(std::remove(hidden.begin(),hidden.end(),view),hidden.end());if(!visible)hidden.push_back(view);
        feature->SendRequest(ThicknessDisplayRequest{d,state.result});
    };
    panel->getColorDescription=[feature] {
        const auto state=feature->GetState();QString text="壁厚 · "+GetRefText(state.result);
        if(state.displayRange)text+=QString("\n生效范围：%1 ～ %2").arg((*state.displayRange)[0],0,'g',8).arg((*state.displayRange)[1],0,'g',8);
        return text+(state.isDisplayReady ? "\n显示就绪":"\n结果已就绪，显示未就绪／隐藏");
    };
    panel->AttachAction("SetActive", {{"result", "current"}}, [panel,send, resolveResult](auto id, const auto& p) {
        const auto ref=resolveResult(p);panel->SetParameterPatch("Result",{{"result",GetRefText(ref)}});
        ThicknessRequest r; r = ThicknessActivationRequest{}; std::get<ThicknessActivationRequest>(r).resultRevision = ref; send(id, r);
    });
    panel->AttachAction("ActivateViewed",{},[send,viewed](auto id,const auto&){ThicknessRequest r=ThicknessActivationRequest{};std::get<ThicknessActivationRequest>(r).resultRevision=viewed();send(id,r);});
    panel->AttachAction("SelectSample", {{"result", "current"}, {"sampleIndex", "0"}}, [send, resolveResult](auto id, const auto& p) {
        ThicknessRequest r; r = ThicknessSampleRequest{}; std::get<ThicknessSampleRequest>(r).resultRevision = resolveResult(p); std::get<ThicknessSampleRequest>(r).sampleIndex = GetId(p["sampleIndex"]); send(id, r);
    }, TestPolicy::View);
    panel->AttachAction("LocateMinimum",{},[feature,send,viewed](auto id,const auto&) {
        const auto result=feature->GetResult(viewed());
        if(!result || !result->statistics.minimumSample)throw std::invalid_argument("该结果没有有效最薄样本，未测区域不会当作零厚度。");
        send(id,ThicknessSampleRequest{result->result,*result->statistics.minimumSample});
    },TestPolicy::View);
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
    panel->onObserve = [panel, feature, visible,viewed,build,appliedDisplays,lease] {
        const auto state = feature->GetState();
        QJsonObject summary{{"isBusy", state.isBusy || bool(*lease)}, {"isCurrent", state.isCurrent}, {"requestId", QString::number(state.requestId)},
            {"result", GetRefText(state.result)}, {"isDisplayReady", state.isDisplayReady}, {"isVisible", *visible}};
        const auto result = feature->GetResult(viewed()); summary["hasResult"] = result.has_value();
        summary["activeResult"]=GetDataRevisionRefValid(state.result)?GetRefText(state.result):QString();
        summary["viewedResult"]=GetDataRevisionRefValid(viewed())?GetRefText(viewed()):QString();summary["viewedIsActive"]=viewed()==state.result;
        if(result){summary["viewedSource"]=GetRefText(result->archive.input.source);summary["viewedIsCurrent"]=result->isCurrent;summary["isCurrent"]=result->isCurrent;}
        const auto action=panel->GetParameterAction();
        try {
            if(result && action=="Start"){const auto draft=build(panel->GetActionParameters("Start"));summary["measurementParametersChanged"]=MeasurementJson(draft.input,draft.params)!=MeasurementJson(result->archive.input,result->archive.params);}
            if(result && action=="SetEvaluation"){auto p=panel->GetActionParameters("SetEvaluation");p["upper"]=GetInputNumber(p,"upper",result->statistics.maximum?std::max(*result->statistics.maximum,std::numeric_limits<double>::epsilon()):1.);if(p["histogramRange"].isNull())p["histogramRange"]=QJsonArray{0.,p["upper"]};summary["evaluationParametersChanged"]=EvaluationJson(Evaluation(p))!=EvaluationJson(result->archive.evaluation);}
            summary["displayParametersChanged"]=result && action=="SetDisplay" && appliedDisplays->value(GetRefText(result->result))!=panel->GetActionParameters("SetDisplay");
        }catch(const std::exception&){summary["measurementParametersChanged"]=action=="Start";}
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
            summary["source"]=GetRefText(image->dataRevision);
            auto* form = panel->GetParameterEditor("Start");
            auto& workflow = panel->GetContext().workflow;
            const auto surface = workflow.GetMeasurementSurface();
            const auto meshChoice = form->GetField("mesh")->GetValue();
            bool matchedSurface = meshChoice == "surface";
            if (!matchedSurface) try { matchedSurface = surface && GetRef(meshChoice) == surface->mesh; }
                catch (const std::invalid_argument&) {}
            const auto threshold = matchedSurface && surface && image->dataRevision == surface->source && surface->threshold
                ? *surface->threshold : GetScalarMidpoint(*image);
            form->GetField("materialThreshold")->SetDefaultValue(threshold);
            form->GetField("sampleSpacing")->SetDefaultValue(2.0 * GetVoxelSpacing(*image));
            form->GetField("maxBoundaryError")->SetDefaultValue(0.5 * GetVoxelSpacing(*image));
            form->GetField("maxDistance")->SetDefaultValue(GetInputDiagonal(*image));
            QString preparation;
            const auto labelChoice=form->GetField("labels")->GetValue();
            DataRevisionRef labels;
            try {labels=labelChoice=="parts" ? workflow.getPartLabels ? workflow.getPartLabels() : DataRevisionRef{} : GetRef(labelChoice);}catch(const std::exception&){}
            if(!GetDataRevisionRefValid(labels))preparation="缺少标签：请先分割所选源体数据，生成同源材料标签。";
            else {
                bool matched=false;for(const auto& descriptor:panel->GetSession()->GetLabelMapDescriptors())
                    if(descriptor.dataRevision==labels)matched=descriptor.sourceRevision==image->dataRevision;
                if(!matched)for(const auto value:graph["nodes"].toArray())if(value.toObject()["ref"]==GetRefText(labels))
                    matched=value.toObject()["parents"].toArray().contains(GetRefText(image->dataRevision));
                if(!matched)preparation="标签来源与所选体数据不一致，请采用正确源数据并生成同源标签。";
            }
            if(meshChoice=="surface" && (!surface || surface->source!=image->dataRevision))preparation+=(preparation.isEmpty()?"":"\n")+QString("缺少同源正式测量表面：请进入表面测定。");
            summary["preparationProblem"]=preparation;
        }
        if (state.displayRange) summary["displayRange"] = GetValues(*state.displayRange);
        if (result) { summary["source"]=GetRefText(result->archive.input.source);const auto stats = Statistics(*result); for (auto it = stats.begin(); it != stats.end(); ++it) summary[it.key()] = it.value(); }
        panel->SetState(summary);
    };
    panel->AttachAction("ExportComparison",{{"outputPath",""}},[panel,feature,viewed](auto id,const auto& p){
        const auto snapshot=feature->GetResult(viewed());
        if(!snapshot||!snapshot->samples)throw std::runtime_error("wall result unavailable");
        // 与窗口共用精确结果的业务表，无效厚度留空，单位来自结果契约。
        auto record=panel->GetContext().records.GetRecord(id);record["result"]=QJsonObject{{"result",GetRefText(snapshot->result)}};
        const auto reports=panel->GetContext().workflow.readReports.at("Wall")(record,{});
        if(reports.isEmpty())throw std::runtime_error("wall report unavailable");
        ExportReport(GetText(p,"outputPath"),reports.first());
        QJsonArray histogram,reasons,regions;const auto& s=snapshot->statistics;
        for(auto x:s.histogramAreas)histogram.append(x);
        for(std::size_t j=0;j<s.reasonCounts.size();++j)reasons.append(QJsonObject{{"reason",int(j)},{"count",QString::number(s.reasonCounts[j])},{"area",s.reasonAreas[j]}});
        for(const auto& r:snapshot->regions)regions.append(QJsonObject{{"id",QString::number(r.id)},{"area",r.area},{"minimum",r.minimum},{"maximum",r.maximum},{"bounds",GetValues(r.sampleBounds)},{"samples",QString::number(r.sampleIds.size())}});
        panel->SetComplete(id,"Exported",{{"statistics",Statistics(*snapshot)},{"histogramAreas",histogram},{"reasons",reasons},{"regions",regions},{"evaluatedArea",s.evaluatedArea},{"validArea",s.validArea}});
    },TestPolicy::Read);
    panel->onStop = [panel] { panel->SendAction("Cancel", {{"targetRequestId", "0"}}); };
    panel->onStatistics=[panel,feature] {
        const auto selected=panel->GetActionParameters("Result");
        const auto ref=selected["result"]=="current" ? feature->GetState().result : GetRef(selected["result"]);
        const auto result=feature->GetResult(ref);if(!result)return false;
        const auto& s=result->statistics;const auto& e=result->archive.evaluation;
        QJsonArray counts,edges;for(auto value:s.histogramAreas)counts.append(value);
        for(std::size_t i=0;i<=s.histogramAreas.size();++i)edges.append(e.histogramRange[0]+(e.histogramRange[1]-e.histogramRange[0])*i/std::max<std::size_t>(1,s.histogramAreas.size()));
        panel->GetStatisticsWidget()->SetHistogram({{"title","壁厚有效面积分布"},{"description","结果 "+GetRefText(ref)+QString(" · 来源 %1 · 已计算面积桶 · 有效面积 %2 mm² · 覆盖 %3% · 范围外面积 %4 / %5 mm²；未测不进入零桶")
            .arg(GetRefText(result->archive.input.source)).arg(s.validArea).arg(s.coverage*100).arg(s.histogramBelowArea).arg(s.histogramAboveArea)},
            {"counts",counts},{"edges",edges},{"xUnit","壁厚 / mm"},{"yUnit","有效面积 / mm²"},{"markers",QJsonArray{QJsonObject{{"label","评价下限"},{"value",e.lower}},QJsonObject{{"label","评价上限"},{"value",e.upper}}}}});
        QJsonArray reasons;for(std::size_t i=0;i<s.reasonCounts.size();++i)if(s.reasonCounts[i])reasons.append(QJsonArray{QString::number(i),QString::number(s.reasonCounts[i]),s.reasonAreas[i]});
        panel->GetStatisticsWidget()->SetTable({"有效性原因编号","样本数","面积 mm²"},reasons);
        panel->GetStatisticsWidget()->onInterval=[panel,feature,ref](double low,double high) {
            if(panel->GetStatisticsWidget()->GetInteractionMode()=="编辑评价区间") {
                panel->SetParameterPatch("SetEvaluation",{{"lower",low},{"upper",high}});return;
            }
            const auto snapshot=feature->GetResult(ref);if(!snapshot || !snapshot->samples)return;
            for(std::size_t i=0;i<snapshot->samples->size();++i){const auto& sample=(*snapshot->samples)[i];
                if(sample.validity==ThicknessValidity::Valid && panel->GetStatisticsWidget()->ContainsInterval(sample.thickness,low,high)) {
                    panel->SendAction("SelectSample",{{"result",GetRefText(ref)},{"sampleIndex",QString::number(i)}});return;}}
            if(panel->onMessage)panel->onMessage("所选区间没有可定位的有效壁厚样本。");
        };
        panel->GetStatisticsWidget()->SetInteractionModes({"定位有效壁厚","编辑评价区间"});
        const auto evaluationDraft=panel->GetActionParameters("SetEvaluation");
        panel->GetStatisticsWidget()->SetMarkers(QJsonArray{
            QJsonObject{{"label","评价下限"},{"key","lower"},{"editable",true},{"value",GetInputNumber(evaluationDraft,"lower",e.lower)}},
            QJsonObject{{"label","评价上限"},{"key","upper"},{"editable",true},{"value",GetInputNumber(evaluationDraft,"upper",e.upper)}}});
        panel->GetStatisticsWidget()->onMarker=[panel](const QString& key,double value){if(panel->GetStatisticsWidget()->GetInteractionMode()=="编辑评价区间")panel->SetParameterPatch("SetEvaluation",{{key,value}});};
        return true;
    };
    return panel;
}
}
