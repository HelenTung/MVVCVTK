// 文档中的阶段转换及统计来源回归，配合实际 Qt 业务操作检查。
#include "BusinessWorkflowTests.h"
#include "App/TestWindow.h"
#include "Modules/ModulePanel.h"
#include "Support/BusinessStep.h"
#include "Support/BusinessStatistics.h"
#include "Support/ReferenceDataSource.h"
#if defined(MANUAL_ALIGNMENT)
#include "Modules/AlignmentInput.h"
#endif
#include "Support/SceneGraph.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QMouseEvent>
#include <QTreeWidget>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <limits>
namespace Manual {
namespace {
void Require(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
bool Await(const std::function<bool()>& ready){QElapsedTimer timer;timer.start();while(timer.elapsed()<3000){QCoreApplication::processEvents(QEventLoop::AllEvents,10);if(ready())return true;}return false;}
double Sum(const QJsonObject& data){double sum=0;for(const auto value:data["counts"].toArray())sum+=value.toDouble();return sum;}
void CheckPrimaryDispatch()
{
    for(const bool changesAction:{false,true}) {
        TestSession runtime;TestWorkflow workflow;TestRecordWriter records;
        ModulePanel panel({runtime,workflow,records},"Data");
        int executions=0;
        const auto action=[&](std::uint64_t id,const QJsonObject&) {++executions;panel.SetComplete(id,"Succeeded");};
        panel.AttachAction("Load",{},action);panel.AttachAction("ActivateAccepted",{},action);
        bool changesOnRefresh=false;
        panel.onObserve=[&] {
            panel.SetState(!changesOnRefresh?QJsonObject{}:changesAction
                ?QJsonObject{{"activationFailed",true},{"acceptedRevision","new-input"}}
                :QJsonObject{{"isBusy",true}});
        };
        panel.Observe();
        auto* primary=panel.findChild<QPushButton*>("businessPrimary");
        Require(primary && primary->isEnabled() && primary->property("businessAction")=="Load",
            "primary dispatch fixture starts with an enabled exact action");
        // 点击处理内的刷新模拟异步业务状态刚好变化；点击意图仍应留有原动作的终态记录。
        changesOnRefresh=true;primary->click();
        const auto record=records.GetRecord(1);
        Require(records.GetRecords()["records"].toArray().size()==1 && record["action"]=="Load",
            "primary refresh cannot silently drop a click or replace its requested action");
        Require(record["status"]=="Rejected" && record["isTerminal"].toBool()
            && record["completeCount"].toInt()==1 && executions==0,
            "captured primary action retains fresh session rejection and exactly one completion");
    }
}
}
void CheckBusinessWorkflow(TestWindow& window)
{
    CheckPrimaryDispatch();
#if defined(MANUAL_ALIGNMENT)
    AlignmentRegion selection;selection.vertexIds={1,3,8};
    DataRevisionRef mesh;mesh.entityId.bytes[0]=1;mesh.generation=1;
    DataRevisionRef roi;roi.entityId.bytes[0]=2;roi.generation=1;
    SetRegionRoi(selection,roi,mesh);
    Require(selection.pinnedMesh==mesh && selection.roi==roi && selection.vertexIds==std::vector<std::uint64_t>{1,3,8},
        "shared spatial ROI stays an exact reference alongside the explicit vertex selection");
    bool rejected=false;try{SetRegionRoi(selection,{},mesh);}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected && selection.roi==roi && selection.vertexIds==std::vector<std::uint64_t>{1,3,8},
        "empty ROI selection cannot become the complete mesh or overwrite the prior selection");
#endif
    // 与完整真实 CT 相同的网格周期；无需分配十余 GB 就能复现抽样列偏置。
    const std::array<int,3> dimensions{1536,1536,1536};
    const std::size_t total=std::size_t(1536)*1536*1536;
    const auto stride=ReferenceDataSource::GetGraySampleStride(total,dimensions);
    std::array<bool,3> low{},high{};std::size_t central=0,samples=0;
    for(std::size_t voxel=0;voxel<total;voxel+=stride){
        const std::array<std::size_t,3> point{voxel%1536,(voxel/1536)%1536,voxel/(std::size_t(1536)*1536)};
        bool inside=true;for(int axis=0;axis<3;++axis){low[axis]=low[axis] || point[axis]<384;high[axis]=high[axis] || point[axis]>=1152;inside=inside && point[axis]>=384 && point[axis]<1152;}
        central+=inside;++samples;
    }
    Require(samples<=262144 && central>10000 && std::all_of(low.begin(),low.end(),[](bool value){return value;}) && std::all_of(high.begin(),high.end(),[](bool value){return value;}),"full CT statistics sampling spans every axis and its central ROI rather than repeated X columns");
    const QJsonObject input{{"available",true},{"dataRevision","source-A"}};
    Require(GetBusinessStep("Crop",{{"output","00000000000000000000000000000000:0"}},input,"Start").action=="Start","an empty crop revision cannot masquerade as a published result before the first crop");
    Require(GetBusinessStep("Data",{{"acceptedRevision","B"},{"activeRevision","A"}},input,"Load").action=="Load","adopting derived data is not an activation failure");
    Require(GetBusinessStep("Data",{{"acceptedRevision","B"},{"activationFailed",true}},input,"Load").action=="ActivateAccepted","a real failed adoption offers activation retry");
    const auto confirmed=GetBusinessStep("Crop",{{"isConfirmed",true}},input,"Start");
    Require(confirmed.action=="BuildResult" && confirmed.usesOutput,"confirmed crop offers generation followed by exact adoption");
    Require(GetBusinessStep("Crop",{{"isConfirmed",false},{"nodeCount","3"}},input,"BuildResult").action=="FinishEditing","changed active path returns to confirmation instead of publishing");
    Require(GetBusinessStep("Crop",{{"output","crop-B"},{"outputMatchesPreview",true}},input,"Start").action=="SelectOutput","matching published crop offers adoption retry");
    Require(GetBusinessStep("Crop",{{"output","old-O1"},{"hasCurrentPreview",true}},input,"Start").action=="FinishEditing","old output cannot hijack a new preview");
    const auto adjusting=GetBusinessStep("Crop",{{"isActive",true},{"hasCurrentPreview",false}},input,"Start");
    Require(adjusting.action.isEmpty() && adjusting.next.contains("拖动"),"a started crop with no edit node asks for widget adjustment before confirmation");
    const auto retained=GetBusinessStep("Crop",{{"output","old-O1"},{"viewingOutput",true},{"blockerCount",1}},input,"RestoreSource");
    Require(retained.action=="Start" && retained.stage.contains("下游"),"downstream consumers retain crop output and offer a new crop instead of implying successful release");
    Require(GetBusinessStep("Crop",{{"operationCount","3"},{"nodeCount","0"}},input,"BuildResult").action=="Start","branches outside the root path are not an active crop");
    Require(GetBusinessStep("Crop",{{"output","source-A"}},input,"Start").title.contains("继续裁剪"),"adopted crop continues from current input");
    Require(GetBusinessStep("Artifact",{{"hasCandidate",true},{"isCandidateCurrent",true}},input,"Ring").usesOutput,"artifact candidate offers publish and adoption");
    Require(GetBusinessStep("Artifact",{{"canSelectOutput",true}},input,"Ring").action=="SelectOutput","published artifact does not recompute for adoption retry");
    Require(GetBusinessStep("Roi",{{"hasDraft",true}},input,"Begin").action=="Commit","ROI draft offers formal save");
    Require(GetBusinessStep("Part",{{"hasPreview",true}},input,"Paint").action=="Commit","part editing candidate remains distinct from formal labels");
    Require(GetBusinessStep("Surface",{{"hasPreview",true}},input,"GlobalAutomatic").action=="Determine","surface preview must precede formal measurement qualification");
    Require(GetBusinessStep("Alignment",{},input,"EvaluateVoxelVolume").action=="EvaluateVoxelVolume","voxel volume remains independent of alignment preparation");
    Require(GetBusinessStep("Gap",{{"hasResult",true},{"isCurrent",false}},input,"Start").action=="Start","old input defect results cannot appear as current analysis");
    Require(GetBusinessStep("Gap",{{"hasResult",true},{"isCurrent",true},{"parametersChanged",true}},input,"SetDisplay").action=="Start","algorithm filter edits require analysis rather than list refresh");
    Require(GetBusinessStep("Gap",{{"hasResult",true},{"isCurrent",true}},input,"Start").action=="Defects","current defect result opens its catalog");
    for(const auto* name:{"Data","View","Rotation","Artifact","Roi","Crop","Part","Surface","Alignment","Wall","Gap"}) {
        auto* page=window.GetModule(name);Require(page!=nullptr,"all eleven business pages are present");
        auto* primary=page->findChild<QPushButton*>("businessPrimary");auto* stage=page->findChild<QLabel*>("businessStage");auto* scroll=page->findChild<QScrollArea*>("operationScroll");
        Require(primary && stage && scroll && !scroll->isAncestorOf(primary),"every business has a fixed stage primary outside parameter scrolling");
    }
    Require(!window.findChild<QWidget*>("diagnosticBar") && window.findChild<QWidget*>("workbenchInput")->isAncestorOf(window.findChild<QLabel*>("currentInputIdentity")),"actual adopted input identity stays in the fixed input strip");
    const auto weighted=BuildHistogram({-1,0,1},{2,3,5},"area","fixed result","mm","mm2");
    Require(Sum(weighted)==10,"area histogram sums area rather than sample count");
    Require(BuildHistogram({std::numeric_limits<double>::quiet_NaN()},{},"invalid","invalid","gray","count")["counts"].toArray().isEmpty(),"invalid samples are not inserted in zero bin");
    BusinessStatistics statistics;
    const auto reader=[](std::size_t){StatisticsSamples samples;samples.values={0,0,1,1};samples.total=4;samples.next=4;samples.description="revision R1";return samples;};
    statistics.StartHistogram("R1","gray","gray","count",reader);
    Require(Await([&]{return !statistics.GetHistogram()["counts"].toArray().isEmpty();}) && Sum(statistics.GetHistogram())==4,"bounded histogram conserves the known samples");
    statistics.SetHistogram(weighted);
    statistics.onInterval=[](double,double){};statistics.onRow=[](int){};statistics.SetTable({"old"},QJsonArray{QJsonArray{"old row"}});
    statistics.SetHistogram(weighted);
    Require(!statistics.onInterval && !statistics.onRow && statistics.findChild<QTreeWidget*>("businessResultTable")->topLevelItemCount()==0,"equal chart data still replaces callbacks and table context");
    const auto edges=weighted["edges"].toArray();const auto boundary=edges[1].toDouble();
    Require(!statistics.ContainsInterval(boundary,edges[0].toDouble(),boundary) && statistics.ContainsInterval(boundary,boundary,edges[2].toDouble()) && statistics.ContainsInterval(edges.last().toDouble(),edges[edges.size()-2].toDouble(),edges.last().toDouble()),"bins are half-open with a closed final endpoint");
    statistics.resize(500,300);statistics.show();statistics.SetInteractionModes({"选择区间"});statistics.findChild<QComboBox*>("histogramInteraction")->setCurrentIndex(1);QCoreApplication::processEvents();int selected=0;statistics.onInterval=[&](double,double){++selected;};auto* plot=statistics.findChild<QWidget*>("businessHistogramPlot");
    const auto press=[&](const QPoint& pos,Qt::MouseButton button){QMouseEvent event(QEvent::MouseButtonPress,pos,button,button,Qt::NoModifier);QCoreApplication::sendEvent(plot,&event);QMouseEvent release(QEvent::MouseButtonRelease,pos,button,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&release);};
    press({80,3},Qt::LeftButton);press({80,30},Qt::RightButton);Require(selected==0,"chart margins and right click never select an interval");press({80,30},Qt::LeftButton);Require(selected==1,"left click within the plot selects an interval");statistics.hide();
    statistics.StartHistogram("R1","gray","gray","count",reader);
    Require(Await([&]{return statistics.GetHistogram()["title"]=="gray" && !statistics.GetHistogram()["counts"].toArray().isEmpty();}) && Sum(statistics.GetHistogram())==4,"static chart cannot impersonate a cached histogram from the same revision");
    statistics.StartHistogram("zero","defects","mm3","count",[](std::size_t){StatisticsSamples samples;samples.description="published empty defect table";return samples;});
    Require(Await([&]{return statistics.GetHistogram()["description"].toString().contains("有效结果：0");}),"valid empty defect table is distinct from invalid samples");
    bool oldRead=false;
    statistics.StartHistogram("old","old","gray","count",[&](std::size_t){oldRead=true;return StatisticsSamples{};});
    statistics.StartHistogram("new","new","gray","count",reader);
    Require(Await([&]{return !statistics.GetHistogram()["counts"].toArray().isEmpty();}) && !oldRead && statistics.GetHistogram()["cacheKey"]=="new","replaced histogram job cannot publish a late old-source curve");
    statistics.StartHistogram("retry","retry","gray","count",[](std::size_t)->StatisticsSamples{throw std::runtime_error("first read fails");});
    Require(Await([&]{return statistics.GetHistogram()["status"]=="Failed";}),"failed histogram exposes a failed state");
    statistics.StartHistogram("retry","retry","gray","count",reader);Require(Await([&]{return statistics.GetHistogram()["status"]=="Ready";}),"same-key histogram retries after failure");
    statistics.StartHistogram("reentrant","reentrant","gray","count",[&](std::size_t){statistics.StartHistogram("replacement","replacement","gray","count",reader);StatisticsSamples old;old.values={999};old.total=1;old.next=1;return old;});
    Require(Await([&]{return statistics.GetHistogram()["cacheKey"]=="replacement" && statistics.GetHistogram()["status"]=="Ready";}) && Sum(statistics.GetHistogram())==4,"reentrant reader cannot publish over its replacement");
    const QJsonObject root{{"id","root"},{"parents",QJsonArray{}}},a{{"id","a"},{"parents",QJsonArray{"root"}}},b{{"id","b"},{"parents",QJsonArray{"root"}}},merge{{"id","merge"},{"parents",QJsonArray{"a","b"}}};
    const auto checkEdges=[&](const QJsonArray& nodes){const auto layout=GetGraphLayout(nodes);Require(layout.edges.size()==4,"all true parent edges survive list ordering");for(const auto& pair:QJsonArray{QJsonObject{{"child","a"},{"parent","root"}},QJsonObject{{"child","b"},{"parent","root"}},QJsonObject{{"child","merge"},{"parent","a"}},QJsonObject{{"child","merge"},{"parent","b"}}})Require(layout.edges.contains(pair),"branch and merge endpoints are exact");};
    checkEdges({root,a,b,merge});checkEdges({merge,b,a,root});
}
}
