// 测试用途：通过孔隙页面测试分析请求、统计结果、叠加显示与退出收口。
#include "ModuleFactories.h"
#include "Support/ColorSegmentInput.h"
#include "Support/ParameterEditor.h"
#include "Support/RoiSelection.h"
#include "Host/GapHostFeature.h"
#include <QPointer>
#include <limits>
namespace Manual {
namespace {
QJsonObject GetStatistics(const GapStatistics& value)
{
    return {{"objectVoxels", QString::number(value.objectVoxelCount)}, {"voidVoxels", QString::number(value.voidVoxelCount)},
        {"objectVolumeMM3", value.objectVolumeMM3}, {"voidVolumeMM3", value.voidVolumeMM3}, {"porosityRatio", value.isPorosityAvailable?QJsonValue(value.porosityRatio):QJsonValue()},
        {"porosityAvailable",value.isPorosityAvailable},{"evaluationVoxels",QString::number(value.evaluationVoxelCount)},{"actualSupportVoxels",QString::number(value.actualSupportVoxelCount)}};
}
}
ModulePanel* CreateGapTest(TestContext context, std::shared_ptr<GapHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Gap", parent);
    auto runParameters=std::make_shared<QJsonObject>();
    auto listRange=std::make_shared<std::optional<std::array<double,2>>>();
    panel->SetNotice("切换结果叠加显示可隐藏或显示分析结果；退出分析需等待当前处理结束。结果区显示孔隙体积、孔隙率等统计与数据修订。");
    for(const auto& action:QStringList{"Start","Evaluate"})panel->AttachAction(action, GetJson(R"({"isoMode":"DataRangeRatio","dataRangeRatio":0.5,"absoluteIsoValue":null,"backgroundMean":null,"materialMean":null,"filter":false,"minVolumeMM3":0,"parentResult":"","evaluationRoi":"","detectionRoi":"","roiPolicy":"WholeIfIntersects"})"),
        [panel, feature,runParameters,action](auto id, const auto& params) {
            const auto input = panel->GetSession()->GetImageDescriptor();
            if (!input) throw std::invalid_argument("请先选择体数据输入");
            GapHostStartParams start;
            start.targetViews = GetAllViews();
            start.surface.isoMode = GetEnum<GapIsoMode>(params, "isoMode", {{"AbsoluteValue", GapIsoMode::AbsoluteValue}, {"DataRangeRatio", GapIsoMode::DataRangeRatio}});
            start.surface.absoluteIsoValue = GetInputNumber(params, "absoluteIsoValue", GetScalarMidpoint(*input));
            start.surface.dataRangeRatio = GetNumber(params, "dataRangeRatio");
            start.surface.backgroundMean = static_cast<float>(GetInputNumber(params, "backgroundMean", input->scalarRange[0]));
            start.surface.materialMean = static_cast<float>(GetInputNumber(params, "materialMean", input->scalarRange[1]));
            const auto requestedSurface = start.surface;
            start.voidParams.isFilterEnabled = GetBool(params, "filter");
            start.voidParams.minVolumeMM3 = GetNumber(params, "minVolumeMM3");
            start.evaluationRoi=GetSelectedRoi(*panel->GetSession(),params["evaluationRoi"],input->dataRevision);
            start.detectionRoi=GetSelectedRoi(*panel->GetSession(),params["detectionRoi"],input->dataRevision);
            if(!params["parentResult"].toString().isEmpty())start.parentResult=GetRef(params["parentResult"]);
            start.roiPolicy=GetEnum<GapRoiPolicy>(params,"roiPolicy",{{"InsideOnly",GapRoiPolicy::InsideOnly},{"WholeIfIntersects",GapRoiPolicy::WholeIfIntersects}});
            GapHostRequest request; request.action = action=="Evaluate"?GapHostAction::Evaluate:GapHostAction::Start; request.start = start;
            const QPointer<ModulePanel> owner(panel);
            const auto accepted = (feature->SendRequest(std::move(request), [owner, id, requestedSurface,runParameters,params](GapHostResult result) {
                if (!owner) return;
                if(result.status==GapResultStatus::Succeeded || result.status==GapResultStatus::SucceededWithDisplayFailure)*runParameters=params;
                auto summary = GetStatistics(result.statistics);
                summary["isoMode"] = requestedSurface.isoMode == GapIsoMode::AbsoluteValue ? "AbsoluteValue" : "DataRangeRatio";
                if (requestedSurface.isoMode == GapIsoMode::AbsoluteValue) summary["absoluteIsoValue"] = requestedSurface.absoluteIsoValue;
                else summary["dataRangeRatio"] = requestedSurface.dataRangeRatio;
                summary["status"] = static_cast<int>(result.status);
                summary["message"] = QString::fromStdString(result.message);
                summary["source"] = GetRefText(result.sourceRevision);
                summary["voidTable"] = GetRefText(result.voidTable);
                summary["resultSet"] = GetRefText(result.resultSet);
                summary["commitId"] = QString::number(result.commitId);
                const auto status = result.status == GapResultStatus::Succeeded ? "Succeeded"
                    : result.status == GapResultStatus::SucceededWithDisplayFailure ? "SucceededWithDisplayFailure"
                    : result.status == GapResultStatus::SourceChanged ? "SourceChanged" : "Failed";
                owner->SetComplete(id, status, summary);
            }).status == GapAdmissionStatus::Accepted);
            panel->SetAdmission(id, accepted);
        }, TestPolicy::Compute, true);
    panel->AttachAction("SetDisplay", GetJson(R"({"result":"current","isReversed":false,"hasLegend":true,"legendPosition":[0.025,0.20],"opacity":1,"palette":"Rainbow","rangeMode":"Result","range":[0,1],
        "constantColor":null,"lowColor":null,"highColor":null,"belowColor":null,"aboveColor":null,
        "segments":[],"displayStyle":"Constant","opacityRange":[0.15,1]})"),
        [panel, feature](auto id, const auto& p) {
            GapDisplayParams display;
            display.legendPosition=GetArray<double,2>(p["legendPosition"]);
            display.hasLegend=GetBool(p,"hasLegend");display.isReversed=GetBool(p,"isReversed");display.opacity=GetNumber(p,"opacity");
            if(p.contains("palette"))display.mode = GetEnum<GapColorMode>(p, "palette", {{"Constant", GapColorMode::Constant},
                {"Gradient", GapColorMode::Gradient}, {"Rainbow", GapColorMode::Rainbow},
                {"InverseRainbow", GapColorMode::InverseRainbow}, {"HueLoop", GapColorMode::HueLoop}});
            display.rangeMode = GetEnum<GapRangeMode>(p, "rangeMode", {
                {"Result", GapRangeMode::Result}, {"SelectedInterval", GapRangeMode::SelectedInterval}});
            if (p.contains("range")) display.range = GetArray<double, 2>(p["range"]);
            if (p.contains("constantColor") && !p["constantColor"].isNull()) display.constantColor = GetArray<double, 3>(p["constantColor"]);
            if (p.contains("lowColor") && !p["lowColor"].isNull()) display.lowColor = GetArray<double, 3>(p["lowColor"]);
            if (p.contains("highColor") && !p["highColor"].isNull()) display.highColor = GetArray<double, 3>(p["highColor"]);
            if (p.contains("belowColor") && !p["belowColor"].isNull()) display.belowColor = GetArray<double, 3>(p["belowColor"]);
            if (p.contains("aboveColor") && !p["aboveColor"].isNull()) display.aboveColor = GetArray<double, 3>(p["aboveColor"]);
            display.segments=GetColorSegments<GapColorSegment,GapColorMode>(p,{
                {"Constant",GapColorMode::Constant},{"Gradient",GapColorMode::Gradient},{"Rainbow",GapColorMode::Rainbow},
                {"InverseRainbow",GapColorMode::InverseRainbow},{"HueLoop",GapColorMode::HueLoop}});
            if(p.contains("displayStyle"))display.style=GetEnum<GapDisplayStyle>(p,"displayStyle",{
                {"Constant",GapDisplayStyle::Constant},{"Inclined",GapDisplayStyle::Inclined},{"InverseInclined",GapDisplayStyle::InverseInclined}});
            if(p.contains("opacityRange"))display.opacityRange=GetArray<double,2>(p["opacityRange"]);
            display.hiddenLegendViews=feature->GetState().display.hiddenLegendViews;display.legendPositions=feature->GetState().display.legendPositions;
            if(!p["legendView"].toString().isEmpty())display.legendPositions[p["legendView"].toString().toStdString()]=display.legendPosition;
            GapHostRequest request; request.action = GapHostAction::SetDisplay; request.display = display;
            request.displayResult=p["result"]=="current" ? feature->GetState().resultSet:GetRef(p["result"]);
            const bool isAccepted = (feature->SendRequest(std::move(request)).status == GapAdmissionStatus::Accepted);
            const auto state = feature->GetState();
            panel->SetComplete(id, isAccepted ? "Succeeded" : "Rejected",
                {{"isOverlayVisible", state.isOverlayVisible}, {"range", state.displayRange ? QJsonValue(GetValues(*state.displayRange)):QJsonValue()},
                    {"palette", static_cast<int>(state.display.mode)}, {"displayStyle",static_cast<int>(state.display.style)},
                    {"segmentCount",static_cast<int>(state.display.segments.size())},{"resultSet", GetRefText(state.resultSet)}});
        }, TestPolicy::View);
    panel->getColorParameters=[panel,feature] {
        const auto ref=feature->GetState().resultSet;if(!GetDataRevisionRefValid(ref))return QJsonObject{};
        auto p=panel->GetActionParameters("SetDisplay");const auto d=feature->GetState().display;
        const auto colors=GetColorBandJson(d.mode,d.constantColor,d.lowColor,d.highColor,d.belowColor,d.aboveColor,d.segments,d.isReversed);
        for(auto i=colors.begin();i!=colors.end();++i)p[i.key()]=i.value();
        p["rangeMode"]=d.rangeMode==GapRangeMode::Result ? "Result":"SelectedInterval";p["range"]=GetValues(d.range);
        const char* styles[]{"Constant","Inclined","InverseInclined"};p["displayStyle"]=styles[static_cast<unsigned>(d.style)];
        p["hasLegend"]=d.hasLegend;p["opacity"]=d.opacity;p["opacityRange"]=GetValues(d.opacityRange);p["legendPosition"]=GetValues(d.legendPosition);p["result"]=GetRefText(ref);return p;
    };
    panel->getColorPosition=[feature](const QString& id) {
        const auto d=feature->GetState().display;const auto i=d.legendPositions.find(id.toStdString());const auto p=i==d.legendPositions.end() ? d.legendPosition:i->second;return QPointF(p[0],p[1]);
    };
    panel->getColorSurface=[feature] { return QString(); };
    panel->setViewLegend=[feature](const QString& id,bool visible,int slot) {
        const auto state=feature->GetState();if(!GetDataRevisionRefValid(state.resultSet))return;
        auto d=state.display;const auto view=id.toStdString();
        if(visible){d.hasLegend=true;d.legendPositions[id.toStdString()]={.025+.25*slot,.20};}
        auto& hidden=d.hiddenLegendViews;hidden.erase(std::remove(hidden.begin(),hidden.end(),view),hidden.end());if(!visible)hidden.push_back(view);
        GapHostRequest request;request.action=GapHostAction::SetDisplay;request.display=d;request.displayResult=state.resultSet;feature->SendRequest(request);
    };
    panel->getColorDescription=[feature] {
        const auto state=feature->GetState();QString text="孔隙 · "+GetRefText(state.resultSet)+"\n缺陷体积 / mm³";
        return state.displayRange ? text+QString("\n生效范围：%1 ～ %2").arg((*state.displayRange)[0],0,'g',8).arg((*state.displayRange)[1],0,'g',8):text+"\n暂无有效测量值";
    };
    for (const bool exit : {false, true}) panel->AttachAction(exit ? "Exit" : "Overlay", {},
        [panel, feature, exit](auto id, const auto&) {
            GapHostRequest request; request.action = exit ? GapHostAction::Exit : GapHostAction::Overlay;
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(std::move(request), [owner, id, exit](GapHostResult result) {
                if (owner) owner->SetComplete(id, result.status == GapResultStatus::Succeeded
                    ? (exit ? "Exited" : "Succeeded") : "Failed",
                    {{"requestId", QString::number(result.requestId)}, {"message", QString::fromStdString(result.message)}});
            });
            panel->SetAdmission(id, admission.status == GapAdmissionStatus::Accepted,
                {{"requestId", QString::number(admission.requestId)}, {"admission", static_cast<int>(admission.status)}});
        }, exit ? TestPolicy::Stop : TestPolicy::View);
    panel->onStop = [panel] { panel->SendAction("Exit", {}); };
    panel->AttachAction("Defects",{{"pageOffset","0"}},[panel,feature,listRange](auto id,const auto& p) {
        const auto state=feature->GetState();if(!GetDataRevisionRefValid(state.voidTable))throw std::invalid_argument("尚无已发布缺陷表");
        const auto page=panel->GetContext().workflow.readRecordPage(state.voidTable,GetId(p["pageOffset"]),128,*listRange);
        QJsonArray rows,indices;for(const auto value:page["rows"].toArray()) {
            const auto row=value.toObject();rows.append(QJsonArray{row["void-id"],row["volume-mm3"],row["equivalent-diameter-mm"]});indices.append(row["rowIndex"]);
        }
        auto* widget=panel->GetStatisticsWidget();widget->SetTable({"缺陷编号","体积 mm³","等效直径 mm"},rows);
        widget->setProperty("defectRows",indices);widget->setProperty("defectNext",page["next"]);widget->setProperty("defectTotal",page["total"]);widget->setProperty("defectTable",GetRefText(state.voidTable));widget->setProperty("defectSource",GetRefText(state.sourceRevision));
        widget->onRow=[panel,widget](int row) {
            const auto indices=widget->property("defectRows").toJsonArray();if(row<0 || row>=indices.size())return;
            panel->SendAction("LocateDefect",{{"rowIndex",indices[row]},{"table",widget->property("defectTable").toString()},{"source",widget->property("defectSource").toString()}});
        };
        panel->SetComplete(id,"Observed",{{"table",GetRefText(state.voidTable)},{"page",page}});
        if(panel->onMessage)panel->onMessage(*listRange ? QString("列表体积筛选 [%1, %2] mm³；算法结果与全量直方图未变。").arg((**listRange)[0]).arg((**listRange)[1]) : "缺陷目录已更新；在“统计与结果”中选行可定位，翻页不重新运行算法。");
    },TestPolicy::Read);
    panel->AttachAction("ResetDefectFilter",{},[panel,listRange](auto id,const auto&){listRange->reset();panel->SendAction("Defects",{{"pageOffset","0"}});panel->SetComplete(id,"Observed");},TestPolicy::Read);
    panel->AttachAction("NextDefectPage",{},[panel](auto id,const auto&) {
        const auto offset=panel->GetStatisticsWidget()->property("defectNext").toJsonValue();
        panel->SetParameterPatch("Defects",{{"pageOffset",offset.isString()?offset:QJsonValue("0")}});
        panel->SendAction("Defects",panel->GetActionParameters("Defects"));panel->SetComplete(id,"Observed");
    },TestPolicy::Read);
    panel->AttachAction("LocateDefect",{{"rowIndex","0"},{"table","current"},{"source","current"}},[panel,feature](auto id,const auto& p) {
        const auto state=feature->GetState();const auto table=p["table"]=="current" ? state.voidTable : GetRef(p["table"]);
        const auto source=p["source"]=="current" ? state.sourceRevision : GetRef(p["source"]);
        const auto world=panel->GetContext().workflow.getRecordLocation(table,GetId(p["rowIndex"]),source);
        HostSessionSetRequest request;request.cursor=HostCursorParams{world,-1};panel->SendHost(id,std::move(request));
    },TestPolicy::View);
    panel->onStatistics=[panel,feature,listRange] {
        if(panel->GetObservedState()["parametersChanged"].toBool())return false;
        const auto state=feature->GetState();if(!GetDataRevisionRefValid(state.voidTable))return false;
        const auto ref=state.voidTable;const auto read=panel->GetContext().workflow.readRecordSamples;
        panel->GetStatisticsWidget()->StartHistogram(GetRefText(ref),"缺陷体积分布","缺陷体积 / mm³","缺陷数量",
            [owner=QPointer<ModulePanel>(panel),read,ref](auto offset){if(!owner || owner->GetContext().workflow.GetIsClosing())throw std::runtime_error("统计已停止");return read(ref,"volume-mm3",offset);});
        panel->GetStatisticsWidget()->onInterval=[panel,feature,listRange,ref](double low,double high){
            if(feature->GetState().voidTable!=ref){if(panel->onMessage)panel->onMessage("所显示分布的结果已变化，请刷新后再筛选。");return;}
            const auto edges=panel->GetStatisticsWidget()->GetHistogram()["edges"].toArray();
            const auto upper=!edges.isEmpty() && high<edges.last().toDouble() ? std::nextafter(high,-std::numeric_limits<double>::infinity()) : high;
            *listRange=std::array<double,2>{low,upper};panel->SendAction("Defects",{{"pageOffset","0"}});
        };
        return true;
    };
    panel->onObserve = [panel, feature,runParameters,listRange] {
        const auto state = feature->GetState();
        auto summary = GetStatistics(state.statistics);
        summary["analysisState"] = static_cast<int>(state.analysisState);
        summary["isExitPending"] = state.isExitPending;
        summary["isViewActive"] = state.isViewActive;
        summary["isOverlayVisible"] = state.isOverlayVisible;
        summary["range"] = GetValues(state.display.range);
        summary["palette"] = static_cast<int>(state.display.mode);
        summary["source"] = GetRefText(state.sourceRevision);
        summary["resultSet"] = GetRefText(state.resultSet);
        summary["voidTable"] = GetRefText(state.voidTable);
        summary["isListFiltered"]=listRange->has_value();
        summary["hasNextDefectPage"]=panel->GetStatisticsWidget()->property("defectNext").toJsonValue().toString().toULongLong()<panel->GetStatisticsWidget()->property("defectTotal").toJsonValue().toString().toULongLong();
        try {summary["parametersChanged"]=!runParameters->isEmpty() && panel->GetActionParameters("Start")!=*runParameters;}
        catch(const std::exception&){summary["parametersChanged"]=true;}
        const auto input = panel->GetSession()->GetImageDescriptor();
        if (input) {
            auto* form = panel->GetParameterEditor("Start");
            form->GetField("absoluteIsoValue")->SetDefaultValue(GetScalarMidpoint(*input));
            form->GetField("backgroundMean")->SetDefaultValue(input->scalarRange[0]);
            form->GetField("materialMean")->SetDefaultValue(input->scalarRange[1]);
        }
        summary["isBusy"] = state.analysisState == GapAnalysisState::Running || state.isExitPending;
        summary["hasResult"] = GetDataRevisionRefValid(state.resultSet);
        summary["isCurrent"] = input && input->dataRevision == state.sourceRevision && state.analysisState == GapAnalysisState::Succeeded;
        panel->SetState(summary);
    };
    return panel;
}
}
