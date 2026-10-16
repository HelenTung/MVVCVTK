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
    auto listedReader=std::make_shared<std::shared_ptr<const GapResultReader>>();
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
        if(visible){d.hasLegend=true;if(slot>=0)d.legendPositions[id.toStdString()]={.025+.25*slot,.20};}
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
    panel->AttachAction("Defects",{{"pageOffset","0"}},[panel,feature,listRange,listedReader](auto id,const auto& p) {
        const auto state=feature->GetState();auto* widget=panel->GetStatisticsWidget();const auto offset=GetId(p["pageOffset"]);
        const auto ref=offset?GetRef(widget->property("defectResult").toString()):state.resultSet;
        const auto reader=*listedReader&&(*listedReader)->GetMetadata().result==ref?*listedReader:feature->GetResultReader(ref);
        if(!reader)throw std::invalid_argument("所选正式孔隙结果不可读，请显式刷新目录首页");
        *listedReader=reader;const auto source=reader->GetMetadata().source;
        const auto table=offset?GetRef(widget->property("defectTable").toString()):state.voidTable;
        QJsonArray rows,ids,indices,records;auto next=offset;const auto total=reader->GetMetadata().objectCount;
        if(offset>total)throw std::invalid_argument("孔隙目录偏移越界");
        while(rows.size()<128&&next<total) {
            const auto page=reader->GetObjects(next,static_cast<std::size_t>(128-rows.size()));
            if(page.error!=GapReadError::None||page.next<=next)throw std::runtime_error("孔隙对象页读取失败");
            auto row=next;
            for(const auto& object:page.objects) {
                const auto rowIndex=row++;
                if(*listRange&&(object.volumeMM3<(**listRange)[0]||object.volumeMM3>(**listRange)[1]))continue;
                rows.append(QJsonArray{QString::number(object.id),object.parentVolumeMM3,object.intersectionVolumeMM3,
                    object.isRoiPartial?"部分相交":"完整",object.volumeMM3,object.hasWholeObjectGeometry?"完整父缺陷":"ROI 片段（不是新封闭孔隙）"});
                ids.append(QString::number(object.id));indices.append(QString::number(rowIndex));
                records.append(QJsonObject{{"void-id",QString::number(object.id)},{"rowIndex",QString::number(rowIndex)},
                    {"parent-volume-mm3",object.parentVolumeMM3},{"intersection-volume-mm3",object.intersectionVolumeMM3},
                    {"volume-mm3",object.volumeMM3},{"roi-partial",object.isRoiPartial},{"whole-object-geometry",object.hasWholeObjectGeometry}});
            }
            next=page.next;
        }
        widget->SetTable({"父缺陷编号","完整体积 mm³","ROI 内体积 mm³","相交状态","本次统计体积 mm³","量值对象"},rows);widget->ShowTable();
        widget->setProperty("defectRows",indices);widget->setProperty("defectIds",ids);widget->setProperty("defectNext",QJsonValue(QString::number(next)));
        widget->setProperty("defectTotal",QJsonValue(QString::number(total)));widget->setProperty("defectTable",GetRefText(table));
        widget->setProperty("defectSource",GetRefText(source));widget->setProperty("defectResult",GetRefText(ref));
        widget->onRow=[panel,widget](int row){const auto ids=widget->property("defectIds").toJsonArray();if(row<0||row>=ids.size())return;
            panel->SendAction("SelectObject",{{"regionId",ids[row]},{"result",widget->property("defectResult").toString()}});};
        panel->SetComplete(id,"Observed",{{"resultSet",GetRefText(ref)},{"table",GetRefText(table)},
            {"page",QJsonObject{{"rows",records},{"next",QString::number(next)},{"total",QString::number(total)},{"offset",QString::number(offset)}}}});
        if(panel->onMessage)panel->onMessage(*listRange?QString("列表体积筛选 [%1, %2] mm³；算法结果未变。").arg((**listRange)[0]).arg((**listRange)[1]):"缺陷目录已更新；选行按原始编号定位，后续页保持当前结果修订。");
    },TestPolicy::Read);
    const auto getReader=[panel,feature,listedReader](const QJsonObject& p){
        const auto text=p["result"].toString();const auto ref=text=="current"?feature->GetState().resultSet:GetRef(text=="listed"?panel->GetStatisticsWidget()->property("defectResult").toString():text);
        const auto reader=*listedReader&&(*listedReader)->GetMetadata().result==ref?*listedReader:feature->GetResultReader(ref);
        if(!reader)throw std::invalid_argument("所选正式孔隙结果不可读");return reader;
    };
    const auto getRegionId=[](const QJsonObject& p){const auto id=GetId(p["regionId"]);if(!id||id>INT32_MAX)throw std::invalid_argument("请输入有效的孔隙原始编号");return static_cast<std::int32_t>(id);};
    for(const bool isolated:{false,true})panel->AttachAction(isolated?"IsolateObject":"SelectObject",{{"result","listed"},{"regionId","1"}},
        [panel,feature,getReader,getRegionId,isolated](auto id,const auto& p){
            const auto reader=getReader(p);const auto region=getRegionId(p);GapHostRequest request;
            request.action=GapHostAction::SelectObject;request.selection=GapObjectSelection{reader->GetMetadata().result,region,isolated};
            const bool accepted=feature->SendRequest(std::move(request)).status==GapAdmissionStatus::Accepted;
            panel->SetComplete(id,accepted?"Succeeded":"Rejected",{{"resultSet",GetRefText(reader->GetMetadata().result)},{"regionId",QString::number(region)},{"isIsolated",isolated}});
            if(accepted)panel->SendAction("LocateDefectById",{{"result",GetRefText(reader->GetMetadata().result)},{"regionId",QString::number(region)}});
        },TestPolicy::View);
    panel->AttachAction("ClearObjectSelection",{},[panel,feature](auto id,const auto&){
        GapHostRequest request;request.action=GapHostAction::SelectObject;request.selection=GapObjectSelection{feature->GetState().resultSet,{},false};
        panel->SetComplete(id,feature->SendRequest(std::move(request)).status==GapAdmissionStatus::Accepted?"Succeeded":"Rejected");
    },TestPolicy::View);
    panel->AttachAction("ObjectPicking",{{"isEnabled",false}},[panel,feature](auto id,const auto& p){
        GapHostRequest request;request.action=GapHostAction::SetObjectPicking;request.isObjectPickingEnabled=GetBool(p,"isEnabled");
        const bool accepted=feature->SendRequest(std::move(request)).status==GapAdmissionStatus::Accepted;
        panel->SetComplete(id,accepted?"Succeeded":"Rejected",{{"isObjectPickingEnabled",feature->GetState().isObjectPickingEnabled}});
    },TestPolicy::View);
    panel->AttachAction("LocateDefectById",{{"result","listed"},{"regionId","1"}},[panel,getReader,getRegionId](auto id,const auto& p){
        const auto reader=getReader(p);const auto geometry=reader->GetGeometry(getRegionId(p),0,0,1);
        if(geometry.error!=GapReadError::None||geometry.points.empty())throw std::invalid_argument("该孔隙没有可定位的正式几何");
        const auto world=panel->GetContext().workflow.getModelWorldPosition(reader->GetMetadata().source,geometry.points[0]);
        HostSessionSetRequest request;request.cursor=HostCursorParams{world,-1};panel->SendHost(id,std::move(request));
    },TestPolicy::View);
    panel->AttachAction("ReadDefectGeometry",{{"result","listed"},{"regionId","1"},{"pointOffset","0"},{"triangleOffset","0"},{"pageSize","128"}},[panel,getReader,getRegionId](auto id,const auto& p){
        const auto reader=getReader(p);const auto count=GetId(p["pageSize"]);if(!count||count>4096)throw std::invalid_argument("几何页大小必须为 1～4096");
        const auto geometry=reader->GetGeometry(getRegionId(p),GetId(p["pointOffset"]),GetId(p["triangleOffset"]),static_cast<std::size_t>(count));
        if(geometry.error!=GapReadError::None)throw std::invalid_argument("孔隙几何范围不可读");
        QJsonArray points,triangles;for(const auto& point:geometry.points)points.append(GetValues(point));
        for(std::size_t i=0;i<geometry.triangleIndices.size();i+=3)triangles.append(QJsonArray{QString::number(geometry.triangleIndices[i]),QString::number(geometry.triangleIndices[i+1]),QString::number(geometry.triangleIndices[i+2])});
        panel->SetComplete(id,"Observed",{{"resultSet",GetRefText(geometry.result)},{"regionId",QString::number(geometry.objectId)},
            {"firstPoint",QString::number(geometry.firstPoint)},{"firstTriangle",QString::number(geometry.firstTriangle)},
            {"nextPoint",QString::number(geometry.nextPoint)},{"nextTriangle",QString::number(geometry.nextTriangle)},
            {"totalPoints",QString::number(geometry.totalPoints)},{"totalTriangles",QString::number(geometry.totalTriangles)},{"points",points},{"triangles",triangles}});
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
        panel->GetStatisticsWidget()->SetInteractionModes({"筛选缺陷列表"});
        return true;
    };
    panel->onObserve = [panel, feature,runParameters,listRange] {
        const auto state = feature->GetState();
        auto summary = GetStatistics(state.statistics);
        summary["analysisState"] = static_cast<int>(state.analysisState);
        summary["isExitPending"] = state.isExitPending;
        summary["isViewActive"] = state.isViewActive;
        summary["isOverlayVisible"] = state.isOverlayVisible;
        summary["isDisplayComplete"]=state.isDisplayComplete;summary["hasDisplayFailure"]=state.hasDisplayFailure;
        summary["displayRequestedBatches"]=QString::number(state.displayRequestedBatches);summary["displayLoadedBatches"]=QString::number(state.displayLoadedBatches);
        summary["displayMessage"]=QString::fromStdString(state.displayMessage);
        summary["hasDisplayGeometry"]=state.displayLoadedBatches>0;
        summary["selectionResult"]=GetRefText(state.selectionResult);
        summary["selectedObject"]=state.selectedObject?QJsonValue(QString::number(*state.selectedObject)):QJsonValue();
        summary["isSelectionIsolated"]=state.isSelectionIsolated;summary["isObjectPickingEnabled"]=state.isObjectPickingEnabled;
        panel->GetParameterEditor("ObjectPicking")->GetField("isEnabled")->SetAppliedBoolean(state.isObjectPickingEnabled,GetRefText(state.resultSet));
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
    panel->onSessionClosing=[listedReader] {listedReader->reset();};
    return panel;
}
}
