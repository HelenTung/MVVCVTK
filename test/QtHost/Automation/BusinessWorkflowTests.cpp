// 文档中的阶段转换及统计来源回归，配合实际 Qt 业务操作检查。
#include "BusinessWorkflowTests.h"
#include "TestCaseRunner.h"
#include "App/TestWindow.h"
#include "Modules/ModulePanel.h"
#include "Support/JsonInput.h"
#include "Support/BusinessStep.h"
#include "Support/BusinessStatistics.h"
#include "Support/ReferenceDataSource.h"
#if defined(MANUAL_ALIGNMENT)
#include "Modules/AlignmentInput.h"
#endif
#include "Support/SceneGraph.h"
#if defined(MANUAL_CROP)
#include "Modules/ModuleFactories.h"
#include "Host/CropHostFeature.h"
#include <QVTKOpenGLNativeWidget.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <QGridLayout>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <future>
#include <iostream>
#endif
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
void CheckCropStartReadiness()
{
#if defined(MANUAL_CROP)
    // future 可先被 owner 消费，worker 的纯完成门铃随后才到达；不伪造场景或就绪状态。
    constexpr int gateTimeoutMs=5000;
    struct CompletionGate final {
        std::atomic<bool> armed{false},entered{false},returned{false},expired{false},released{false};
        std::promise<void> release;
        std::shared_future<void> ready=release.get_future().share();
        void Release(){if(!released.exchange(true))release.set_value();}
    };
    const auto gate=std::make_shared<CompletionGate>();
    QWidget views;
    QGridLayout layout(&views);
    TestSession runtime;
    TestWorkflow workflow;
    TestRecordWriter records;
    QtHostPump pump;
    std::vector<vtkSmartPointer<vtkGenericOpenGLRenderWindow>> windows;
    std::unique_ptr<ModulePanel> panel;
    const std::vector<std::string> ids{"primary-3d","slice-top-down","slice-front-back","slice-left-right"};
    const HostRenderViewRole roles[]{HostRenderViewRole::Primary3D,HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,HostRenderViewRole::LeftRightSlice};
    const HostRenderMode modes[]{HostRenderMode::CompositeVolume,HostRenderMode::SliceTopDown,
        HostRenderMode::SliceFrontBack,HostRenderMode::SliceLeftRight};
    HostSessionConfig config;
    for(std::size_t index=0;index<ids.size();++index) {
        auto* widget=new QVTKOpenGLNativeWidget(&views);
        const auto window=vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        widget->setRenderWindow(window);layout.addWidget(widget,int(index/2),int(index%2));windows.push_back(window);
        HostRenderViewConfig view;view.id=ids[index];view.role=roles[index];view.renderWindow=window;
        view.window.viewInit.viewMode=modes[index];config.renderViews.push_back(std::move(view));
    }
    pump.SetConfig(config);
    const auto owner=QThread::currentThreadId();
    config.onWorkAvailable=[gate,notify=config.onWorkAvailable,owner,gateTimeoutMs] {
        const bool held=QThread::currentThreadId()!=owner && gate->armed.exchange(false);
        if(held) {
            gate->entered.store(true);
            if(gate->ready.wait_for(std::chrono::milliseconds(gateTimeoutMs))!=std::future_status::ready)
                gate->expired.store(true);
        }
        notify();
        if(held)gate->returned.store(true);
    };
    views.resize(640,480);views.show();
    try {
        Require(runtime.BuildSession(std::move(config)),"crop work-notice fixture builds the real four-view Host");
        const auto session=runtime.GetSession();pump.SetSession(session);
        const auto feature=std::make_shared<CropHostFeature>();
        Require(runtime.AttachFeature(feature),"crop work-notice fixture attaches the actual Crop feature");
        workflow.getRenderPending=[&pump](const auto& id){return pump.GetIsRenderPending(id);};
        workflow.getViewRenderPending=[&pump](const auto& id){return pump.GetIsViewPending(id);};
        panel.reset(CreateCropTest({runtime,workflow,records},feature,nullptr));panel->resize(420,720);panel->show();
        const QPointer<ModulePanel> observed(panel.get());
        pump.onUpdated=[observed]{if(observed)observed->Observe();};
        pump.onRendered=[observed]{if(observed)observed->Observe();};
        pump.getVisible=[](const auto&){return true;};
        const auto idle=[&] {
            return panel->GetObservedState()["framesReady"].toBool()
                && std::none_of(ids.begin(),ids.end(),[&](const auto& id){return pump.GetIsRenderPending(id);});
        };
        HostReloadRequest load;load.geometry={{16,16,16},{1.,1.,1.},{}};
        load.metadata.identity.datasetId="crop-start-work-notice";load.metadata.source.kind=ImageSourceKind::Memory;
        load.metadata.source.uri="memory://crop-start-work-notice";load.voxels.resize(16*16*16);
        for(int z=0;z<16;++z)for(int y=0;y<16;++y)for(int x=0;x<16;++x)
            load.voxels[x+16*(y+16*z)]=(x>=4&&x<12&&y>=4&&y<12&&z>=4&&z<12)?100.f:0.f;
        const auto loaded=std::make_shared<bool>(false);
        Require(session->SendRequest(std::move(load),[loaded](const auto& result){*loaded=result.isSucceeded;}).status==HostAdmissionStatus::Accepted
            && Await([&]{return *loaded&&idle();}),"crop work-notice fixture presents its known input through real updates and renders");
        auto* primary=panel->findChild<QPushButton*>("businessPrimary");
        Require(primary && primary->isVisible() && primary->isEnabled() && primary->property("businessAction")=="Start",
            "ready actual crop input exposes Start");

        // 真实重置产生绘制需求；暂停窗口的既有 ready 门控，确保不能把它当作普通完成门铃。
        for(const auto& window:windows)window->SetReadyForRendering(false);
        HostViewResetRequest reset;reset.targetView.viewId=ids.front();
        Require(session->SendRequest(std::move(reset)).status==HostAdmissionStatus::Accepted,"real view reset is admitted");
        pump.SendUpdates();
        Require(std::any_of(ids.begin(),ids.end(),[&](const auto& id){return pump.GetIsViewPending(id);})
            && !primary->isEnabled() && !primary->toolTip().isEmpty(),"actual pending rendering disables visible Crop.Start with a reason");
        const auto blocked=panel->SendAction("Start",{{"shape","Box"},{"removalMode","KeepInside"}});
        Require(records.GetRecord(blocked)["status"]=="Rejected" && records.GetRecord(blocked)["completeCount"].toInt()==1
            && !feature->GetState().isActive,"actual pending rendering rejects Start before changing Crop");
        for(const auto& window:windows)window->SetReadyForRendering(true);
        pump.SetVisible();
        Require(Await([&]{return idle()&&primary->isEnabled();}),"natural render completion restores visible Crop.Start");

        QTemporaryDir output;Require(output.isValid(),"actual export has an isolated output directory");
        HostDataExportRequest exportData;exportData.outputPath=output.path().toStdString();exportData.format=HostDataExportFormat::Raw;
        const auto exported=std::make_shared<bool>(false);
        gate->armed.store(true);
        Require(session->SendRequest(std::move(exportData),[exported](const auto& result){*exported=result.isSucceeded;}).status==HostAdmissionStatus::Accepted,
            "real background export is admitted");
        Require(Await([&]{return gate->entered.load();}),"a real worker reaches its completed-task notification gate");
        // 正常 HostDriven 更新已经可以消费 ready future，门铃本身不拥有该结果。
        pump.SendUpdates();
        Require(Await([&]{return *exported&&idle()&&primary->isEnabled();}) && !gate->expired.load(),
            "owner consumes completed export and presents all views before its delayed notice is released");
        const auto snapshot=[&] {
            QJsonArray values;
            for(const auto& scene:session->GetSceneViewStates())values.append(QJsonObject{{"id",QString::fromStdString(scene.id)},
                {"available",scene.isAvailable},{"epoch",QString::number(scene.sceneEpoch)},{"rendered",QString::number(scene.renderedEpoch)},
                {"presentation",QString::number(scene.presentationRevision)},
                {"data",scene.presentation?GetRefText(scene.presentation->dataRevision):QString()},
                {"interacting",scene.presentation&&scene.presentation->isInteracting}});
            return values;
        };
        const auto before=snapshot();
        gate->Release();
        // 不驱动 owner 事件：确定性保留“晚到通知已排队，真实视图未改变”的合法窗口。
        QElapsedTimer returned;returned.start();
        while(!gate->returned.load() && returned.elapsed()<gateTimeoutMs)QThread::yieldCurrentThread();
        Require(gate->returned.load() && !gate->expired.load(),"delayed real completion notice is delivered within the fixture guard");
        Require(snapshot()==before && std::all_of(ids.begin(),ids.end(),[&](const auto& id){return !pump.GetIsViewPending(id)&&pump.GetIsRenderPending(id);})
            && primary->isEnabled(),"only the general work notice is queued while exact view snapshots and visible Start remain ready");
        const auto countBefore=records.GetRecords()["records"].toArray().size();
        bool noticeConsumed=false;int earlyDispatches=0;
        const auto update=pump.onUpdated;
        pump.onUpdated=[&] {
            if(!noticeConsumed) {
                noticeConsumed=true;earlyDispatches=records.GetRecords()["records"].toArray().size()-countBefore;
            }
            update();
        };
        const auto next=ClickPreparedAction(panel.get(),"Start",gateTimeoutMs);
        Require(Await([&]{return records.GetRecord(next)["isTerminal"].toBool();}),"late-notice Start reaches one terminal result");
        const auto result=records.GetRecord(next);
        std::cout<<"CROP_LATE_WORK status="<<result["status"].toString().toStdString()
            <<" message="<<result["result"].toObject()["message"].toString().toStdString()
            <<" completeCount="<<result["completeCount"].toInt()<<" sceneUnchangedBeforeClick=true viewPending=false completedExport=true"
            <<" noticeConsumed="<<noticeConsumed<<" earlyDispatches="<<(noticeConsumed?earlyDispatches:records.GetRecords()["records"].toArray().size()-countBefore)<<std::endl;
        Require(noticeConsumed && earlyDispatches==0 && result["status"]=="Succeeded" && result["completeCount"].toInt()==1
            && result["parameters"].toObject()==QJsonObject{{"shape","Box"},{"removalMode","KeepInside"}}
            && records.GetRecords()["records"].toArray().size()==int(next),
            "actual Click waits for the late notice and sends Crop.Start exactly once successfully");
        pump.onUpdated=update;

        // 已接纳但尚未进入 Pump 的真实展示工作，旧 scene 仍可能看起来就绪。
        Require(Await(idle),"the successful Start settles before the uncollected reset control");
        const auto editing=feature->GetState();const auto history=feature->GetHistory();
        Require(editing.isActive && !editing.history.isDragging && !editing.history.pendingRequestCount
            && history.appliedHead==history.rootNodeId,"first Start leaves an idle active Root that legally supports resuming Start");
        // Start 清理 BuildResult 草稿会切走参数卡；在触发重置之前一次性准备实际入口。
        panel->SetParameterPatch("Start",{{"shape","Box"},{"removalMode","KeepInside"}});
        const auto* startButton=panel->findChild<QPushButton*>("action_Start");
        Require(panel->GetParameterAction()=="Start" && startButton && startButton->isVisible() && startButton->isEnabled()
            && panel->GetActionParameters("Start")==QJsonObject{{"shape","Box"},{"removalMode","KeepInside"}},
            "uncollected reset control starts from the exact visible enabled Start form");
        for(const auto& window:windows)window->SetReadyForRendering(false);
        const auto resetCount=records.GetRecords()["records"].toArray().size();
        bool resetCollected=false;int resetEarlyDispatches=0;
        HostViewResetRequest queuedReset;queuedReset.targetView.viewId=ids.front();
        Require(session->SendRequest(std::move(queuedReset)).status==HostAdmissionStatus::Accepted
            && pump.GetIsRenderPending(ids.front()) && !pump.GetIsViewPending(ids.front()),
            "real reset has queued work before Pump has collected a render request");
        pump.onUpdated=[&] {
            update();
            if(!resetCollected) {
                resetCollected=true;resetEarlyDispatches=records.GetRecords()["records"].toArray().size()-resetCount;
                for(const auto& window:windows)window->SetReadyForRendering(true);
            }
        };
        const auto resumed=ClickPreparedAction(panel.get(),"Start",gateTimeoutMs);
        Require(Await([&]{return records.GetRecord(resumed)["isTerminal"].toBool();}) && resetCollected && resetEarlyDispatches==0
            && records.GetRecord(resumed)["status"]=="Succeeded" && records.GetRecord(resumed)["completeCount"].toInt()==1
            && records.GetRecords()["records"].toArray().size()==resetCount+1,
            "actual Click sends nothing before a queued real reset is collected and rendered, then sends exactly one successful Start");
        std::cout<<"CROP_QUEUED_RESET collected="<<resetCollected<<" earlyDispatches="<<resetEarlyDispatches
            <<" status="<<records.GetRecord(resumed)["status"].toString().toStdString()<<" completeCount="<<records.GetRecord(resumed)["completeCount"].toInt()<<std::endl;
        pump.onUpdated=update;
        Require(Await([&]{return runtime.Stop();}),"crop work-notice fixture stops the actual Host naturally");pump.Stop();
        std::cout<<"CROP_START_FIXTURE_STOP stopped=true"<<std::endl;
    } catch(...) {
        gate->Release();pump.onUpdated={};pump.onRendered={};
        const bool stopped=Await([&]{return runtime.Stop();});pump.Stop();
        if(!stopped)std::cerr<<"CROP_FIXTURE_CLEANUP Stop remained pending"<<std::endl;
        throw;
    }
#endif
}
void CheckBusinessWorkflow(TestWindow& window)
{
    CheckPrimaryDispatch();
    CheckCropStartReadiness();
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
    Require(!window.findChild<QWidget*>("diagnosticBar") && window.findChild<QWidget*>("parameterPages")->isAncestorOf(window.findChild<QLabel*>("currentInputIdentity")),"actual adopted input identity stays in the tool property header");
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
