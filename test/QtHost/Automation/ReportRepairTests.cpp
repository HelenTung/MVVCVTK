#include "ReportWorkflowTests.h"
#include "TestCaseRunner.h"
#include "App/TestWindow.h"
#include "Support/ResultReport.h"
#include "Support/ReportComparison.h"
#include "Support/ReportStorage.h"
#include "Support/WorkbenchStyle.h"
#if defined(MANUAL_ALIGNMENT)
#include "Support/InspectionReport.h"
#include "Support/AlignmentReportParameters.h"
#include "Support/JsonInput.h"
#endif
#include <QApplication>
#include <QBuffer>
#include <QAction>
#include <QIODevice>
#include <QDoubleSpinBox>
#include <QLocale>
#include <QProgressBar>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QIdentityProxyModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QMenu>
#include <QThread>
#include <condition_variable>
#include <mutex>
#include <QTreeWidget>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <atomic>
#include <limits>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace Manual {
class ReportTaskTestAccess final {
public:
    static void BeforeStart(ResultReport& view,std::function<void()> hook){view.m_beforeWorkerStart=std::move(hook);}
    static void ChooseExportPath(ResultReport& view,std::function<QString(const QString&)> choose){view.m_chooseExportPath=std::move(choose);}
    static void Start(ResultReport& view,std::function<ReportDocument(const ReportCheckpoint&)> work){view.StartTask(std::move(work));}
    static bool HasTask(const ResultReport& view){return bool(view.m_task);}
    static QTimer* GetBuildTimer(ResultReport& view){return view.m_buildTimer;}
    static bool HasScan(const ResultReport& view){return bool(view.m_rowScan);}
    static bool HasPendingScan(const ResultReport& view){return bool(view.m_pendingRowScan);}
    static void Generate(ResultReport& view){view.ContinueGeneration();}
    static void ContinueTask(ResultReport& view){view.ContinueTask();}
    static void Compare(ResultReport& view){view.CompareSelected();}
    static ReportRows Rows(const ResultReport& view){return view.m_rows;}
    static void SetRows(ResultReport& view,ReportRows rows){view.m_rows=std::move(rows);}
};
namespace {
constexpr int reportWaitTimeoutMs=30000;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
// Qt 5.14 的 QList 等号在当前 MSVC 中引用已移除的 stdext；逐格比较保持同一精确判据。
bool SameCells(const QStringList& a,const QStringList& b){return a.size()==b.size()&&std::equal(a.cbegin(),a.cend(),b.cbegin());}
template<class F> void Wait(F pending) {
    QElapsedTimer elapsed;elapsed.start();
    do {QCoreApplication::processEvents(QEventLoop::AllEvents,10);}while(pending() && elapsed.elapsed()<reportWaitTimeoutMs);
    Require(!pending(),"report repair test timed out");
}
ReportDocument Read(QByteArray bytes) {
    QBuffer input(&bytes);input.open(QIODevice::ReadOnly);return ReadReportCsv(input,"外部报告");
}
ReportDocument Document(const QString& id,const QString& quantity="表面对象体积",int count=2) {
    ReportDocument doc;doc.id=id;doc.title=id;doc.category="本次会话结果";doc.module="Surface";
    doc.source="data-a";doc.result=id+"-result";doc.relatedResults=QStringList{id+"-generation"};
    doc.columns={{"object_id",{},{},false},{"surface_object_volume_mm3",quantity,"mm3",true}};
    doc.identityColumn=0;doc.valueColumn=1;doc.table=std::make_shared<ReportTable>();
    doc.conditions={{"阈值",42},{"方法","正式表面"}};
    for(int i=0;i<count;++i)doc.table->Append({QString("point-%1").arg(i),QString::number(i+1)});
    return doc;
}
ReportComparisonOptions Options(const ReportDocument& a,const ReportDocument& b) {
    ReportComparisonOptions o;o.baselineColumn=o.currentColumn=1;
    o.baselineQuantity=a.columns[1].quantity;o.currentQuantity=b.columns[1].quantity;
    o.baselineUnit=a.columns[1].unit;o.currentUnit=b.columns[1].unit;
    o.baselineIdentity=o.currentIdentity=0;o.match=ReportMatch::Identity;o.identitiesConfirmed=true;return o;
}
bool Visible(ResultReport& view,const QString& id) {
    const auto tree=view.findChild<QTreeWidget*>("reportList");
    for(int i=0;i<tree->topLevelItemCount();++i)for(int j=0;j<tree->topLevelItem(i)->childCount();++j){const auto item=tree->topLevelItem(i)->child(j);
        if(item->data(0,Qt::UserRole).toString()==id)return !item->isHidden();}
    return false;
}
void CheckSemanticRoundtrip() {
    const auto hashOf=[](const QStringList& cells){ReportTable table;table.Append(cells);return table.GetContentHash();};
    Require(hashOf({"a,b","c"})!=hashOf({"a","b,c"}) && hashOf({"a"})!=hashOf({"a",""})
        && hashOf({"a","b"})!=hashOf({"b","a"}),"normalized hash lost cell boundaries, missing values or column order");
    ReportTable ordered,reversed;ordered.Append({"a"});ordered.Append({"b"});reversed.Append({"b"});reversed.Append({"a"});
    Require(ordered.GetContentHash()!=reversed.GetContentHash(),"normalized hash lost native row order");
    auto quoted=Read("item_id,thickness_mm\n\"中文,\"\"引号\"\"\n换行\",\n");
    QByteArray quotedBytes;QBuffer quotedOutput(&quotedBytes);quotedOutput.open(QIODevice::WriteOnly);WriteReportCsv(quotedOutput,quoted);
    Require(quoted.table->GetContentHash()==Read(quotedBytes).table->GetContentHash(),"CSV escaping changed normalized row hash");
    int reads=0;auto callback=Document("hash-callback");
    callback.table=std::make_shared<ReportTable>(3,[&](ReportRowId row){++reads;return QStringList{QString::number(row),"1"};});
    QByteArray callbackBytes;QBuffer callbackOutput(&callbackBytes);callbackOutput.open(QIODevice::WriteOnly);WriteReportCsv(callbackOutput,callback);
    const auto exportedReads=reads;
    Require(callback.table->GetContentHash()==Read(callbackBytes).table->GetContentHash() && reads==exportedReads,
        "full callback export did not retain its complete hash without another row traversal");

    for(const bool filtered:{false,true}) {
        int partialReads=0;auto partial=Document("partial-hash");
        partial.table=std::make_shared<ReportTable>(3,[&](ReportRowId row){++partialReads;return QStringList{QString::number(row),"1"};});
        QByteArray partialBytes;QBuffer partialOutput(&partialBytes);partialOutput.open(QIODevice::WriteOnly);
        auto rows=GetReportRowRange(1);bool cancelled=false;
        // 固定在读取第一行后取消整表；筛选单行正常写完，两个分支都不能发布整表哈希。
        try{WriteReportCsv(partialOutput,partial,filtered?&rows:nullptr,[&]{if(!filtered&&partialReads==1)throw std::runtime_error("cancel");});}
        catch(const std::runtime_error&){cancelled=true;Require(!filtered,"filtered hash fixture unexpectedly failed");}
        Require(partialReads==1 && cancelled==!filtered,"partial hash fixture did not retain its exact one-row read and cancellation boundary");
        const auto before=partialReads;
        Require(partial.table->GetContentHash()==callback.table->GetContentHash() && partialReads==before+3,
            "partial or cancelled export published an incomplete full-table hash");
    }

    for(const auto& quantity:QStringList{"表面对象体积","选区单元体积和","本次统计体积","半径相对名义偏差"}) {
        auto native=Document(quantity,quantity);
        QByteArray bytes;QBuffer output(&bytes);output.open(QIODevice::WriteOnly);WriteReportCsv(output,native);output.close();
        auto copy=Read(bytes);
        Require(native.table->GetContentHash()==copy.table->GetContentHash(),"CSV roundtrip changed full normalized content hash");
        Require(copy.columns[1].quantity==quantity && copy.columns[1].unit=="mm3","CSV roundtrip lost business semantics");
        const auto original=native.table->Read(1),restored=copy.table->Read(1);
        Require(copy.columns[1].name==native.columns[1].name && original.size()==restored.size() && std::equal(original.cbegin(),original.cend(),restored.cbegin()),"CSV roundtrip changed headings or exact values");
        const auto compared=CompareReports(native,copy,Options(native,copy));
        Require(compared.conditions["有效配对数"]==2 && compared.table->Read(0)[4]=="0" && compared.table->Read(1)[4]=="0","automatic imported mapping fails native comparison");
    }
    const auto generic=Read("item_id,volume_mm3\nv,1\n");
    Require(generic.columns[1].quantity=="体积","generic external volume must not claim gap semantics");
    auto source=Document("source"),other=Document("other","选区单元体积和");
    bool rejected=false;try{CompareReports(source,other,Options(source,other));}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"different native volume quantities were compared as one metric");
}
void CheckAssociationsAndClearing() {
    ResultReport view;auto a=Document("a"),b=Document("b");a.table->PrepareRead();b.table->PrepareRead();
    auto result=CompareReports(a,b,Options(a,b));
    Require(result.source=="data-a" && result.relatedResults.contains(a.result) && result.relatedResults.contains(b.relatedResults.first()),"comparison lost same-source or result associations");
    view.AddReport(a);view.AddReport(b);view.AddReport(result);view.SelectReport(a.id);
    auto* conditions=view.findChild<QPlainTextEdit*>("reportConditions");Require(conditions->toPlainText().contains("阈值"),"fixture did not display calculation conditions");
    view.ShowReports("data-b");
    Require(!view.GetSelectedReport().table && conditions->toPlainText().isEmpty() && view.findChild<QLineEdit*>("reportSearch")->text().isEmpty(),"source switch kept old conditions, details or filters");
    Require(!view.findChild<QAction*>("locateReportRow")->isEnabled(),"cleared selection can still locate an old row");
    view.ShowReports("data-a");Require(Visible(view,result.id),"same-source comparison hidden from data entry");
    view.ShowReports({},b.relatedResults.first());Require(Visible(view,result.id),"comparison hidden from original related result entry");
    b.source="data-b";auto cross=CompareReports(a,b,Options(a,b));view.AddReport(cross);
    Require(cross.source.isEmpty() && cross.relatedSources.contains("data-a") && cross.relatedSources.contains("data-b"),"cross-source comparison was assigned to one side");
    view.ShowReports("data-a");Require(Visible(view,cross.id),"cross-source report missing from A");
    view.ShowReports("data-b");Require(Visible(view,cross.id),"cross-source report missing from B");
    b.module="Wall";const auto crossModule=CompareReports(a,b,Options(a,b));view.AddReport(crossModule);
    view.ShowReports({},b.result,"Wall");Require(Visible(view,crossModule.id),"comparison missing from the other business module entry");
    auto imported=Read("item_id,volume_mm3\nexternal,7\n");view.AddReport(imported);view.SelectReport(imported.id);
    view.getSources=[]{return QMap<QString,QString>{{"data-b","测试体数据"}};};
    DrivePopupInteraction([&]{view.findChild<QAction*>("associateReportSource")->trigger();},[&]{
        auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if(!dialog || dialog->parentWidget()!=&view)return false;
        dialog->findChild<QComboBox*>()->setCurrentIndex(1);dialog->accept();return true;
    },"source association chooser missing");
    view.ShowReports("data-b");Require(Visible(view,imported.id),"explicit imported report association was not used by filtering");
    view.SetReportSource(imported.id,{});view.ShowReports("data-b");Require(!Visible(view,imported.id),"removing association left stale source membership");
    auto removable=Document("remove");std::weak_ptr<ReportTable> storage=removable.table;view.AddReport(removable);view.SelectReport(removable.id);
    view.findChild<QAction*>("setReportBaseline")->trigger();removable.table.reset();view.RemoveReport(removable.id);
    Require(storage.expired() && !view.GetReportIds().contains("remove") && !view.GetSelectedReport().table,"removed report retained page, scan or document storage");
}
void CheckSpatialEvidence() {
    auto a=Read("point_label,x_mm,y_mm,z_mm,thickness_mm\na,0,0,0,1\n");
    auto b=Read("point_label,x_mm,y_mm,z_mm,thickness_mm\nb,0.003,0.004,0,2\nmissing,3,0,0,9\n");
    auto o=Options(a,b);o.baselineColumn=o.currentColumn=4;o.baselineQuantity=o.currentQuantity=a.columns[4].quantity;
    o.baselineUnit=o.currentUnit="mm";o.match=ReportMatch::Position;o.coordinatesConfirmed=true;
    o.baselineCoordinateUnit=o.currentCoordinateUnit="mm";o.distanceTolerance=.01;
    const auto result=CompareReports(a,b,o);const auto pair=result.table->Read(0),unmatched=result.table->Read(1);
    Require(pair[0]=="a" && pair[7]=="b" && std::abs(pair[8].toDouble()-.005)<1e-12,"spatial report lost A/B IDs or physical pairing distance");
    Require(unmatched[0].isEmpty() && unmatched[7]=="missing" && unmatched[8].isEmpty() && unmatched[4].isEmpty(),"unmatched B point fabricated baseline identity, distance or delta");
    auto coincident=Read("point_label,x_mm,y_mm,z_mm,thickness_mm\na,0,0,0,1\nb,0,0,0,2\nc,1,0,0,3\n");
    const auto duplicate=CompareReports(coincident,coincident,o);
    Require(duplicate.conditions["有效配对数"]==1 && duplicate.conditions["歧义基准项数"]==2 && duplicate.table->GetCount()==5
        && duplicate.table->Read(0)[7].isEmpty() && duplicate.table->Read(3)[0].isEmpty(),"coincident positions were arbitrarily paired or lost unmatched evidence");
    auto far=Read("point_label,x_mm,y_mm,z_mm,thickness_mm\nfar,1e20,0,0,3\nnear,0,0,0,4\n");
    o.distanceTolerance=1e-9;const auto fallback=CompareReports(far,far,o);
    Require(fallback.conditions["有效配对数"]==2 && fallback.table->GetCount()==2 && fallback.table->Read(0)[8]=="0", "spatial acceleration range rejected valid finite coordinates instead of completing a full scan");
}
void CheckWorkerStartupAndRecovery() {
    auto document=Document("launch-input");QTemporaryDir directory;const auto path=directory.filePath("input.csv");ExportReport(path,document);
    ResultReport view;auto queued=Document("queued-after-launch-failure");
    view.onReadReports=[queued](const QJsonObject&,const QSet<QString>&){return QVector<ReportDocument>{queued};};
    view.QueueReport({{"operationId","queued-launch"},{"module","Surface"},{"result",QJsonObject{{"generation","queued-generation"}}}});view.show();
    int starts=0;ReportTaskTestAccess::BeforeStart(view,[&]{if(++starts==1)throw std::runtime_error("injected worker startup failure");});
    bool rejected=false;try{view.ImportReport(path);}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&!ReportTaskTestAccess::HasTask(view)&&!view.property("reportTaskBusy").toBool(),"failed worker startup retained a task placeholder");
    Wait([&]{return !view.GetReportIds().contains(queued.id);});
    view.ImportReport(path);Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(view.property("reportTaskError").toString().isEmpty()&&view.GetSelectedReport().table->GetCount()==2,"worker startup failure prevented retry or paused native generation permanently");
    auto release=std::make_shared<std::promise<void>>();const auto ready=release->get_future().share();auto entered=std::make_shared<std::atomic<bool>>(false);
    struct ReleaseOnExit final {std::shared_ptr<std::promise<void>> promise;~ReleaseOnExit(){try{promise->set_value();}catch(...){}}} cleanup{release};
    ReportTaskTestAccess::Start(view,[entered,ready](const ReportCheckpoint&)->ReportDocument{entered->store(true);if(ready.wait_for(std::chrono::seconds(30))!=std::future_status::ready)throw std::runtime_error("IO fixture release guard");throw std::runtime_error("injected IO failure");});
    Wait([&]{return !entered->load();});view.CancelTasks();release->set_value();Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(view.property("reportTaskError").toString()=="injected IO failure","cancellation hid a real worker failure");
    view.ImportReport(path);Wait([&]{return view.property("reportTaskBusy").toBool();});Require(view.property("reportTaskError").toString().isEmpty(),"worker failure prevented a subsequent request");

}
void CheckReopenedGeneration(bool closeWindow) {
    ResultReport view;const auto document=Document("reopened-native");int attempts=0;
    auto release=std::make_shared<std::promise<void>>();const auto ready=release->get_future().share();
    auto entered=std::make_shared<std::atomic<bool>>(false);
    struct ReleaseOnExit final {std::shared_ptr<std::promise<void>> promise;~ReleaseOnExit(){try{promise->set_value();}catch(...){}}} cleanup{release};
    view.onPrepareReports=[&](const QJsonObject&,const QSet<QString>&)->ReportPreparation {
        const bool first=++attempts==1;
        return [first,ready,entered,document](const ReportCheckpoint& check) {
            if(first){entered->store(true);if(ready.wait_for(std::chrono::seconds(30))!=std::future_status::ready)throw std::runtime_error("reopen fixture release guard");}
            check();return QVector<ReportDocument>{document};
        };
    };
    view.QueueReport({{"operationId","reopen-native"},{"module","Surface"},{"result",QJsonObject{{"generation","reopen-generation"}}}});view.show();
    Wait([&]{return !entered->load();});
    if(closeWindow)view.close();else view.hide();
    view.show();
    // 确定重开后的生成轮询先于旧 worker 完成，不依靠线程调度碰运气。
    ReportTaskTestAccess::Generate(view);
    Require(ReportTaskTestAccess::HasTask(view)&&attempts==1,"reopening launched another native worker before cancellation completed");
    const auto* timer=ReportTaskTestAccess::GetBuildTimer(view);
    Require(timer->isActive()&&timer->timerType()==Qt::CoarseTimer&&timer->interval()==20,
        "reopening must wait for canceled report work with an active Coarse 20ms timer");
    release->set_value();Wait([&]{return !view.GetReportIds().contains(document.id);});
    Require(attempts==2&&!view.property("reportTaskError").toString().size(),"reopening lost the pending native report queue");
    Wait([&]{return view.GetGenerationPending();});
    Require(view.GetReportIds().size()==1,"reopening must publish the pending native report exactly once");
    std::cout<<"PASS: report "<<(closeWindow?"close":"hide")<<" / reopen waits with Coarse 20ms and publishes once after cancellation"<<std::endl;
}
void CheckComparisonGenerationAdmission() {
    for(int outcome=0;outcome<3;++outcome) {
        ResultReport view;auto baseline=Document("mapping-baseline"),current=Document("mapping-current");
        const auto native=Document("mapping-pending-native");int preparations=0,launches=0;
        view.AddReport(baseline);Require(view.SelectReport(baseline.id),"comparison baseline fixture selection failed");
        view.findChild<QAction*>("setReportBaseline")->trigger();view.AddReport(current);
        Require(view.SelectReport(current.id)&&view.GetSelectedReport().id!=baseline.id,"comparison current fixture selection failed");view.show();
        view.onPrepareReports=[&](const QJsonObject&,const QSet<QString>&)->ReportPreparation {
            ++preparations;return [native](const ReportCheckpoint& check){check();return QVector<ReportDocument>{native};};
        };
        if(outcome==2)ReportTaskTestAccess::BeforeStart(view,[&]{if(++launches==1)throw std::runtime_error("injected comparison startup failure");});
        const auto before=view.GetReportIds();bool mappingShown=false,generatedDuringMapping=false;
        const auto mapping=[&]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if(!dialog || dialog->parentWidget()!=&view || dialog->windowTitle()!="核对比较指标、单位与匹配依据")return false;
            mappingShown=true;
            view.QueueReport({{"operationId","mapping-native"},{"module","Surface"},{"result",QJsonObject{{"generation",native.result}}}});
            // 在映射窗口的嵌套事件循环内主动触发生成，避免依赖计时器和线程调度顺序。
            ReportTaskTestAccess::Generate(view);generatedDuringMapping=preparations!=0;
            if(outcome==1)dialog->reject();else dialog->accept();
            return true;
        };
        QString failure;try{DrivePopupInteraction([&]{ReportTaskTestAccess::Compare(view);},mapping,"comparison mapping dialog was not opened");}
        catch(const std::exception& error){failure=QString::fromUtf8(error.what());}
        Require(mappingShown,"comparison mapping dialog was not opened");
        Require(!generatedDuringMapping,"native report generation stole comparison admission during mapping dialog");
        if(outcome==0) {
            Require(failure.isEmpty()&&ReportTaskTestAccess::HasTask(view)&&preparations==0,"accepted comparison lost its reserved task admission");
            Wait([&]{return !view.GetReportIds().contains(native.id)||view.property("reportTaskBusy").toBool();});
            Require(view.GetReportIds().size()==before.size()+2,"comparison or deferred native report was lost after mapping acceptance");
        } else {
            Require((outcome==1?failure.isEmpty():failure=="injected comparison startup failure")&&!ReportTaskTestAccess::HasTask(view)
                &&view.GetReportIds()==before,"rejected or failed comparison published a task or partial report");
            Wait([&]{return !view.GetReportIds().contains(native.id)||view.property("reportTaskBusy").toBool();});
            Require(view.GetReportIds().size()==before.size()+1,"rejected or failed comparison lost its deferred native report");
        }
        Require(preparations==1&&view.property("reportTaskError").toString().isEmpty(),"deferred native generation did not recover exactly once");
        view.hide();
    }
    std::cout<<"PASS: comparison mapping reserves admission and restores native generation after acceptance, rejection and startup failure"<<std::endl;
}
void CheckNonblockingRowLifecycle() {
    struct Gate final {
        std::mutex mutex;std::condition_variable changed;bool released=false;
        std::atomic<bool> enabled{true},entered{false},exited{false},guarded{false};
        void Hold(){if(!enabled.exchange(false))return;entered=true;std::unique_lock<std::mutex> lock(mutex);
            // 只用于测试死锁时释放 fixture；正确性断言检查回调返回时 worker 尚未退出，不比较耗时。
            if(!changed.wait_for(lock,std::chrono::seconds(30),[&]{return released;}))guarded=true;exited=true;}
        void Release(){{std::lock_guard<std::mutex> lock(mutex);released=true;}changed.notify_all();}
        void Reset(){std::lock_guard<std::mutex> lock(mutex);released=false;entered=false;exited=false;guarded=false;enabled=true;}
    };
    auto gate=std::make_shared<Gate>();auto destroyed=std::make_shared<std::atomic<bool>>(false);auto view=std::make_unique<ResultReport>();
    QObject::connect(view.get(),&QObject::destroyed,[destroyed]{destroyed->store(true);});
    struct ReleaseOnExit final {std::shared_ptr<Gate> gate;~ReleaseOnExit(){gate->Release();}} release{gate};
    auto doc=Document("row-lifecycle","表面对象体积",0);auto* owner=QThread::currentThread();
    doc.table=std::make_shared<ReportTable>(5000,[gate,owner,destroyed](ReportRowId row){if(QThread::currentThread()!=owner){gate->Hold();Require(!destroyed->load(),"report reader callback outlived the destroyed window");}return QStringList{QString::number(row),"1"};});
    view->AddReport(doc);view->show();auto* search=view->findChild<QLineEdit*>("reportSearch");
    int launches=0;ReportTaskTestAccess::BeforeStart(*view,[&]{if(++launches==1)throw std::runtime_error("injected row startup failure");});
    search->setText("123");Wait([&]{return view->property("reportRowsBusy").toBool();});
    Require(!view->property("reportRowsError").toString().isEmpty()&&!ReportTaskTestAccess::HasScan(*view)&&!ReportTaskTestAccess::HasPendingScan(*view),"failed scan startup retained a placeholder");
    search->setText("234");Wait([&]{return !gate->entered.load();});
    search->setText("345");search->setText("456");search->setText("4999");
    Require(!gate->exited.load()&&!gate->guarded.load()&&launches==2&&ReportTaskTestAccess::HasPendingScan(*view),"changing filters waited for the old worker or launched intermediate requests");
    gate->Release();Wait([&]{return view->property("reportRowsBusy").toBool();});
    Require(launches==3&&view->GetFilteredCount()==1&&view->CreateFilteredReader()(0)==4999,"only the latest pending scan must be launched/published");
    gate->Reset();search->setText("__cancel_blocked_worker__");Wait([&]{return !gate->entered.load();});
    view->findChild<QPushButton*>("cancelReportRows")->click();
    Require(!gate->exited.load()&&!gate->guarded.load()&&!view->property("reportRowsBusy").toBool()&&view->GetFilteredCount()==0,
        "cancel callback waited for its running worker or exposed partial rows");
    gate->Release();Wait([&]{return ReportTaskTestAccess::HasScan(*view);});
    gate->Reset();search->setText("__close_blocked_worker__");Wait([&]{return !gate->entered.load();});
    view->close();Require(!gate->exited.load()&&!gate->guarded.load(),"closing report UI synchronously waited for its row worker");
    bool rejected=false;try{view->ImportReport("unused.csv");}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"closed report window admitted a new worker");
    view->show();search->setText("4999");Require(!gate->exited.load()&&ReportTaskTestAccess::HasPendingScan(*view),"reopening waited for the canceled worker");
    gate->Release();Wait([&]{return view->property("reportRowsBusy").toBool();});
    Require(view->GetFilteredCount()==1&&view->CreateFilteredReader()(0)==4999,"old canceled result overwrote the reopened request");
    gate->Reset();search->setText("__destroy_blocked_worker__");Wait([&]{return !gate->entered.load();});view->close();
    // 删除时才等待收尾，随后处理已排队事件，检查没有窗口存活期之外的回调。
    auto completion=std::async(std::launch::async,[gate]{gate->Release();});view.reset();completion.get();
    Require(gate->exited.load()&&!gate->guarded.load(),"report dependencies were released before the worker exited");QCoreApplication::processEvents();
}
void CheckGenerationAndTasks() {
    ResultReport view;int prepared=0,readRows=0;std::weak_ptr<int> lease;
    const QJsonObject record{{"operationId","lazy"},{"module","Wall"},{"source",QJsonObject{{"dataRevision","data-a"}}},{"result",QJsonObject{{"result","lazy-result"},{"mesh","input-mesh"}}}};
    view.onReadReports=[&](const QJsonObject&,const QSet<QString>&) {
        ++prepared;auto doc=Document("lazy","聚合节点厚度场值",0);doc.module="Wall";
        auto held=std::make_shared<int>(1);lease=held;
        doc.table->SetRows(50000,[&,held](ReportRowId row){++readRows;return QStringList{QString::number(row),QString::number(row+1)};});
        return QVector<ReportDocument>{doc};
    };
    view.QueueReport(record);QCoreApplication::processEvents();
    Require(prepared==0 && view.GetReportAvailable("Wall","lazy-result"),"closed report window eagerly reads native data or hides the pending report entry");
    Require(!view.GetReportAvailable("Wall","input-mesh"),"pending report availability mistakes the input mesh for its output result");
    view.ShowReports({},"lazy-result","Wall");view.show();
    Wait([&]{return !view.GetReportIds().contains("lazy");});
    Require(readRows<50000 && !lease.expired() && view.GetReportIds().contains("lazy"),"native preview eagerly materialized all rows or lost its precise row source");
    const auto lazy=view.GetSelectedReport();Require(lazy.table && lazy.table->GetCount()==50000 && !lazy.table->GetIsComplete(),"lazy report lost its full count or eagerly completed");
    const auto last=lazy.table->Read(49999);Require(last[0]=="49999" && readRows<50000,"random native page required a full-prefix scan");
    QTemporaryDir directory;const auto path=directory.filePath("original.csv");
    view.ExportReportFile(path);QTimer::singleShot(0,&view,[&]{view.CancelTasks();});Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(!QFile::exists(path) && view.GetReportIds().contains("lazy"),"cancelled export committed partial data or removed the native report");
    view.ExportReportFile(path);Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(lease.expired() && lazy.table->GetIsComplete() && lazy.table->GetCount()==50000 && QFile::exists(path),"full export failed to materialize all rows or release its native source");
    const auto before=view.GetReportIds();view.ImportReport(path);Require(view.property("reportTaskBusy").toBool(),"import did not return after admission");
    bool heartbeat=false;QTimer::singleShot(0,&view,[&]{heartbeat=true;view.findChild<QPushButton*>("cancelReportTask")->click();});
    Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(heartbeat && view.GetReportIds()==before && QFile::exists(path),"cancelled import blocked events, published partial data or removed source CSV");
    view.ImportReport(path);Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(view.property("reportTaskError").toString().isEmpty() && view.GetSelectedReport().table->GetCount()==50000,"retry import lost rows");
    const auto importedId=view.GetSelectedReport().id;view.RemoveReport(importedId);Require(QFile::exists(path),"removing report deleted original CSV");
    view.ImportReport(path);view.ShowReports("data-b");
    Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(!view.GetSelectedReport().table,"late import stole a newer source-filtered selection");
    view.ShowReports();
    view.SelectReport("lazy");view.findChild<QAction*>("setReportBaseline")->trigger();
    auto current=Document("current","聚合节点厚度场值",50000);view.AddReport(current);view.SelectReport(current.id);
    DrivePopupInteraction([&]{view.findChild<QAction*>("compareReports")->trigger();},[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog || dialog->parentWidget()!=&view || !dialog->findChild<QComboBox*>("reportMatchMode"))return false;
        dialog->accept();return true;
    },"comparison mapping dialog missing");
    Require(view.property("reportTaskBusy").toBool(),"comparison blocked its UI admission");
    const auto comparisonIds=view.GetReportIds();QTimer::singleShot(0,&view,[&]{view.findChild<QPushButton*>("cancelReportTask")->click();});
    Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(view.GetReportIds()==comparisonIds && view.GetSelectedReport().id==current.id,"cancelled comparison published a partial result or changed selection");
    bool fail=true;ResultReport retry;
    retry.onReadReports=[&](const QJsonObject&,const QSet<QString>&){if(fail)throw std::runtime_error("暂时不可读取");return QVector<ReportDocument>{Document("retry")};};
    retry.QueueReport(record);retry.show();QCoreApplication::processEvents();
    Require(retry.GetReportAvailable("Wall","lazy-result") && retry.GetReportIds().isEmpty(),"read failure permanently discarded the retryable operation");
    fail=false;retry.findChild<QPushButton*>("retryFailedReport")->click();Wait([&]{return !retry.GetReportIds().contains("retry");});retry.hide();
    ResultReport failedPreparation;std::atomic<int> attempts{0};
    failedPreparation.onPrepareReports=[&](const QJsonObject&,const QSet<QString>&)->ReportPreparation {
        return [&](const ReportCheckpoint&){++attempts;throw std::runtime_error("持续准备失败");return QVector<ReportDocument>{};};
    };
    failedPreparation.QueueReport(record);failedPreparation.show();Wait([&]{return attempts==0 || failedPreparation.property("reportTaskBusy").toBool();});
    for(int i=0;i<20;++i)QCoreApplication::processEvents(QEventLoop::AllEvents,10);
    Require(attempts==1 && !failedPreparation.property("reportTaskError").toString().isEmpty() && failedPreparation.GetReportAvailable("Wall","lazy-result"),"failed preparation immediately retried forever or discarded the source");
    failedPreparation.hide();
    ResultReport parentView;
    parentView.onReadReports=[](const QJsonObject&,const QSet<QString>&){
        auto doc=Document("parent-inspection");doc.module="Alignment";doc.source="data-a";doc.result="child-inspection";
        doc.relatedResults=QStringList{"parent-alignment"};return QVector<ReportDocument>{doc};
    };
    parentView.QueueReport({{"operationId","parent-op"},{"module","Alignment"},
        {"source",QJsonObject{{"dataRevision","data-b"}}},{"result",QJsonObject{{"inspection","child-inspection"}}}});
    parentView.ShowReports("data-a","parent-alignment","Alignment");parentView.show();
    Wait([&]{return parentView.GetSelectedReport().id!="parent-inspection";});
    Require(parentView.GetSelectedReport().source=="data-a","lazy parent entry used the current input instead of the frozen report source");
    parentView.hide();
    view.hide();
}
void CheckFailureIsolation() {
    ResultReport view;int attempts=0,partialAttempts=0;bool unreadable=true,writeFailure=true;
    view.onReadReports=[&](const QJsonObject& record,const QSet<QString>&){const auto id=record["operationId"].toString();
        if(id=="1"){++attempts;if(unreadable)throw std::runtime_error("结果表修订不可读");}
        auto doc=Document("isolated-"+id, "厚度",0);
        if(id=="2"){++partialAttempts;doc.table->SetRows(3,[&](ReportRowId index){if(writeFailure&&index==1)throw std::runtime_error("报告文件写入失败");return QStringList{QString::number(index),QString::number(index)};});}
        else doc.table->Append({"item","12.5"});return QVector<ReportDocument>{doc};};
    for(const auto id:{"1","2","3"})view.QueueReport({{"operationId",id},{"module","Wall"},{"result",QJsonObject{{"result",QString("result-")+id}}}});
    view.show();Wait([&]{return !view.GetReportIds().contains("isolated-3");});
    Require(view.property("reportFailureCount").toInt()==2 && attempts==1 && partialAttempts==1,"one failed report blocked others or retried itself automatically");
    auto* errors=view.findChild<QComboBox*>("failedReportOperations");Require(errors->count()==2 && errors->itemText(0).contains("结果表修订不可读"),"individual report failure state is missing");
    view.hide();view.ShowReports();view.show();QCoreApplication::processEvents();Require(attempts==1&&partialAttempts==1,"reopening retried quarantined failures");
    unreadable=false;view.ShowReports("other-data");errors->setCurrentIndex(errors->findData("1"));view.findChild<QPushButton*>("retryFailedReport")->click();Wait([&]{return !view.GetReportIds().contains("isolated-1");});
    Require(attempts==2 && partialAttempts==1 && view.property("reportFailureCount").toInt()==1,"explicit retry affected another failed operation");
    writeFailure=false;errors->setCurrentIndex(errors->findData("2"));view.findChild<QPushButton*>("retryFailedReport")->click();Wait([&]{return !view.GetReportIds().contains("isolated-2");});
    Require(partialAttempts==2 && view.property("reportFailureCount").toInt()==0 && !errors->isVisible(),"retried report did not clear its own error state");view.hide();
}
void CheckVolumeCompleteness() {
    for(const auto state:{"1","2","3"}) {
        auto native=Document("volume","选区单元体积和",0);
        native.columns={{"item_id",{},{},false},{"selected_cell_volume_mm3","选区单元体积和","mm3",true},
            {"observed_volume_mm3","有效选中部分体积","mm3",true},{"selection_completeness",{},{},false}};
        native.table->Append({"volume",{},QString(state)=="3"?"0":"12.5",state});QByteArray bytes;QBuffer file(&bytes);file.open(QIODevice::WriteOnly);WriteReportCsv(file,native);file.close();auto copy=Read(bytes);
        Require(copy.validityColumn==-1,"selection completeness was inferred as measurement validity");
        for(int column=1;column<=2;++column) {
            ReportComparisonOptions o;o.match=ReportMatch::Distribution;o.baselineColumn=o.currentColumn=column;
            o.baselineQuantity=native.columns[column].quantity;o.currentQuantity=copy.columns[column].quantity;
            o.baselineUnit=native.columns[column].unit;o.currentUnit=copy.columns[column].unit;
            const auto result=CompareReports(native,copy,o);const auto a=result.table->Read(0),b=result.table->Read(1);
            Require(a[2]==b[2] && a[3]==b[3] && a[2]==(column==1?"0":"1"),"incomplete volume roundtrip changed per-metric availability");
        }
    }
    const auto wall=Read("sample_id,thickness_mm,validity\n0,12.5,1\n");
    ReportComparisonOptions o;o.match=ReportMatch::Distribution;o.baselineColumn=o.currentColumn=1;o.baselineQuantity=o.currentQuantity=wall.columns[1].quantity;o.baselineUnit=o.currentUnit="mm";
    Require(CompareReports(wall,wall,o).table->Read(0)[2]=="0","completeness repair made invalid wall samples valid");
}
void CheckConcurrentReportReads() {
    auto native=Document("concurrent-read","体积",50000);native.table->PrepareRead();
    const auto start=[native]{return std::async(std::launch::async,[native]{QByteArray bytes;QBuffer file(&bytes);file.open(QIODevice::WriteOnly);
        WriteReportCsv(file,native,nullptr,[]{});return bytes;});};
    auto first=start(),second=start();int ownerReads=0;bool overlapped=false;
    Wait([&]{const bool pending=first.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready || second.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;
        overlapped=overlapped||pending;const int index=ownerReads++%50000;const auto row=native.table->Read(index);
        Require(row[0]==QString("point-%1").arg(index)&&row[1]==QString::number(index+1),"owner reads changed while independent exports were running");
        return pending;});
    Require(overlapped,"concurrent export did not overlap owner reads");
    for(const auto bytes:{first.get(),second.get()}){const auto copy=Read(bytes);Require(copy.table->GetCount()==50000,"concurrent full export changed its frozen row count");
        for(int index=0;index<50000;++index){const auto row=copy.table->Read(index);Require(row[0]==QString("point-%1").arg(index)&&row[1]==QString::number(index+1),"concurrent export changed a frozen row");}}
    std::cout<<"PASS: frozen report concurrent owner reads and independent full exports"<<std::endl;
}
void CheckWideRowAddressing() {
    // 虚拟来源只验证类型边界与分页寻址，不冒充真实吞吐或二十亿行物化测试。
    const ReportRowId count=static_cast<ReportRowId>(std::numeric_limits<int>::max())+213;
    Require(GetReportCount(QJsonValue(2147483860.))==count && GetReportCount(QJsonValue(QString("9007199254740993")))==9007199254740993LL
        && GetReportCountValue(9007199254740993LL).isString(),"report metadata narrows a 64-bit count or loses JSON integer precision");
    ReportDocument doc;doc.id="wide-row-contract";doc.title="64 位分页契约";doc.category="本次会话结果";doc.module="Gap";
    doc.columns={{"void_id",{},{},false},{"value","体积","mm3",true}};doc.table=std::make_shared<ReportTable>(count,[](ReportRowId row){return QStringList{QString::number(row),QString::number(row+1)};});
    Require(doc.table->GetCount()==count && doc.table->Read(count-1)[0]==QString::number(count-1),"report source truncates global row IDs to int");
    ResultReport view;ReportRowId located=-1;view.onLocate=[&](const ReportDocument&,ReportRowId row){located=row;};view.AddReport(doc);view.show();
    view.findChild<QPushButton*>("reportLast")->click();QCoreApplication::processEvents();auto* table=view.findChild<QTableView*>("reportTable");
    const int local=table->model()->rowCount()-1;Require(local>=0 && table->model()->data(table->model()->index(local,0)).toString()==QString::number(count-1),"last report page truncates a 64-bit row");
    table->selectRow(local);view.findChild<QAction*>("locateReportRow")->trigger();Require(located==count-1,"locate callback truncates a 64-bit global row");
    view.findChild<QPushButton*>("reportFirst")->click();Require(table->model()->data(table->model()->index(0,0)).toString()=="0","first report page lost original order");view.hide();
}
void CheckDiskIndexPages() {
    ReportTable table;ReportIndex indices;ReportDiskArray<qint64> mapped(2053);
    for(ReportRowId row=0;row<2053;++row){table.Append({QString::number(row),QString(static_cast<int>(row%73),'x')});indices.Append(row*1000000123LL);}
    table.PrepareRead();indices.PrepareRead();const auto read=table.CreateReader();const auto index=indices.CreateReader();
    for(int pass=0;pass<2;++pass)for(ReportRowId i=0;i<2053;++i){const auto row=pass?2052-i:i;
        Require(table.Read(row)[0]==QString::number(row) && read(row)[0]==QString::number(row),"owner or independent report reader corrupted a binary offset page boundary");
        Require(indices.Read(row)==row*1000000123LL && index(row)==indices.Read(row),"64-bit row index corrupted a page boundary or tail page");}
    for(ReportRowId row=0;row<2053;++row){Require(mapped[row]==0,"disk-backed state initialization missed a block");mapped[row]=row*1000000123LL;}
    for(ReportRowId row=2052;row>=0;--row)Require(mapped[row]==row*1000000123LL,"disk-backed state changed a 64-bit record");
}
// 单个测试使用独立临时目录，直接破坏真实工作文件，不增加生产故障注入接口。
class IndexFaultFiles final {
public:
    QTemporaryDir directory;
    QByteArray previous=qgetenv("TMP");bool hadPrevious=qEnvironmentVariableIsSet("TMP");
    IndexFaultFiles(){Require(directory.isValid(),"index fault directory unavailable");qputenv("TMP",directory.path().toLocal8Bit());Require(QDir::tempPath()==directory.path(),"temporary directory override was not applied");}
    ~IndexFaultFiles(){if(hadPrevious)qputenv("TMP",previous);else qunsetenv("TMP");}
    QString GetIndexPath() const {
        const auto names=QDir(directory.path()).entryList(QDir::Files);
        Require(names.size()==1,"expected the sole retained report index in the isolated directory");return directory.filePath(names.first());
    }
};
void CheckIndexReadRecovery() {
    IndexFaultFiles files;ReportIndex index;
    for(ReportRowId i=0;i<6149;++i)index.Append(10000+i);
    index.PrepareRead();const auto read=index.CreateReader();
    Require(index.Read(0)==10000 && read(0)==10000,"index fault fixture did not cache its old page");
    QFile disk(files.GetIndexPath());Require(disk.open(QIODevice::ReadWrite),"index fault file cannot be opened");const auto saved=disk.readAll();
    Require(disk.resize(1024*sizeof(ReportRowId))&&disk.flush(),"index fault truncation failed");
    for(const auto& access:std::vector<std::function<ReportRowId(ReportRowId)>>{[&](ReportRowId row){return index.Read(row);},read}){
        // 越过 QFile 的预读缓冲；连续重试同一失败页，中间不切回旧页以免掩盖缓存污染。
        for(int attempt=0;attempt<2;++attempt){bool failed=false;try{access(4096);}catch(const std::runtime_error&){failed=true;}
            Require(failed,"failed page retry exposed old data under the new page identity");}
        Require(access(0)==10000,"index read failure corrupted its previously committed page");
    }
    Require(disk.seek(0)&&disk.write(saved)==saved.size()&&disk.flush(),"index fault restoration failed");
    Require(index.Read(4096)==14096 && read(4096)==14096 && index.Read(6148)==16148 && read(6148)==16148,"repaired index did not retry the correct page and tail");
}
void CheckPageReadFailures() {
    QTemporaryDir directory;std::atomic<ReportRowId> failRow{-1};std::atomic<int> reads{0};ResultReport view;
    auto doc=Document("page-io","体积",0);doc.module="Gap";
    doc.table=std::make_shared<ReportTable>(2053,[&](ReportRowId row){++reads;if(row==failRow.load())throw std::runtime_error("正文短读");return QStringList{QString("point-%1").arg(row),QString::number(row+1)};});
    ReportRowId located=-1;view.onLocate=[&](const ReportDocument&,ReportRowId row){located=row;};
    view.AddReport(doc);auto* table=view.findChild<QTableView*>("reportTable");
    Require(reads<=400 && table->model()->rowCount()==200,"opening a page scanned the complete report");
    auto* search=view.findChild<QLineEdit*>("reportSearch");search->setText("point-");Wait([&]{return view.property("reportRowsBusy").toBool();});
    Require(view.property("reportRowsError").toString().isEmpty()&&view.GetFilteredCount()==2053,"filtered IO fixture failed");
    // 小索引可驻留小段，大索引由文件后备；页面边界通过同一只读接口验证失败隔离。
    // 真实 ReportIndex 文件的连续短读与恢复已由 CheckIndexReadRecovery 独立覆盖。
    class FaultingRows final : public MeshValueSource<ReportRowId> {
    public:
        ReportRows original;std::atomic<bool> fail{true};
        std::size_t GetCount()const noexcept override{return original.size();}
        std::size_t GetResidentBytes()const noexcept override{return original.GetResidentBytes();}
        void ReadValues(std::size_t offset,std::size_t count,ReportRowId* target)const override {
            if(fail.load() && offset+count>1024)throw std::runtime_error("索引短读");
            for(std::size_t i=0;i<count;++i)target[i]=original[offset+i];
        }
    };
    auto index=std::make_shared<FaultingRows>();index->original=ReportTaskTestAccess::Rows(view);ReportTaskTestAccess::SetRows(view,ReportRows(index));
    view.findChild<QPushButton*>("reportLast")->click();
    Require(table->model()->rowCount()==0&&!view.property("reportRowsError").toString().isEmpty(),"failed page showed stale or partial data");
    Require(!view.findChild<QPushButton*>("exportSelectedReport")->isEnabled(),"failed page retained selected export using invalid row mapping");
    for(const auto* action:{"locateReportRow","exportFilteredReport"})Require(!view.findChild<QAction*>(action)->isEnabled(),"failed page retained an action using invalid row mapping");
    bool rejected=false;try{view.ExportReportFile(directory.filePath("invalid.csv"),true);}catch(const std::runtime_error&){rejected=true;}
    Require(rejected&&!QFile::exists(directory.filePath("invalid.csv"))&&located==-1&&view.GetSelectedReport().table==doc.table,"page failure changed formal report or allowed filtered export");
    index->fail=false;
    view.findChild<QPushButton*>("reportFirst")->click();view.findChild<QPushButton*>("reportLast")->click();
    Require(view.property("reportRowsError").toString().isEmpty()&&table->model()->data(table->model()->index(52,0)).toString()=="point-2052","repaired filtered index did not recover exact original rows");
    view.findChild<QPushButton*>("reportFirst")->click();failRow=250;const auto before=reads.load();view.findChild<QPushButton*>("reportNext")->click();
    Require(table->model()->rowCount()==0&&reads-before<=200&&view.property("reportRowsError").toString()=="正文短读","body failure escaped the page boundary or scanned beyond the current page");
    failRow=-1;view.findChild<QPushButton*>("reportFirst")->click();view.findChild<QPushButton*>("reportNext")->click();
    Require(table->model()->data(table->model()->index(0,0)).toString()=="point-200","body read retry mixed page identities");
    table->selectionModel()->select(table->model()->index(0,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
    const auto failurePath=directory.filePath("failure.csv");
    ReportTaskTestAccess::ChooseExportPath(view,[&](const QString&){return failurePath;});
    auto* exportSelected=view.findChild<QPushButton*>("exportSelectedReport");
    Require(exportSelected && exportSelected->isEnabled() && table->selectionModel()->selectedRows().size()==2
        && table->model()->data(table->model()->index(0,0)).toString()=="point-200"
        && table->model()->data(table->model()->index(1,0)).toString()=="point-201","selected export probe did not retain the two selected rows on the second page");
    const auto exportReads=reads.load();failRow=201;exportSelected->click();
    Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(reads-exportReads==2 && view.property("reportRowsError").toString()=="正文短读","selected export did not read both selected rows and reach injected row 201");
    const auto taskError=view.property("reportTaskError").toString();
    Require(taskError=="正文短读" && view.findChild<QLabel*>("reportTaskStatus")->text().contains(taskError),"selected export read failure reason is not visible");
    Require(!QFile::exists(failurePath)&&table->model()->rowCount()==0&&!view.findChild<QAction*>("locateReportRow")->isEnabled(),"failed selected export committed a partial file or left a usable stale page");
    Require(!exportSelected->isEnabled()&&!view.findChild<QAction*>("exportFilteredReport")->isEnabled(),"failed selected export left actions enabled on the invalidated page");
    QDir residue(directory.path());Require(residue.entryList({"*failure.csv*"},QDir::Files|QDir::Hidden).isEmpty(),"failed selected export left a QSaveFile temporary artifact");
    Require(!view.findChild<QPushButton*>("copyReportCsv")&&!table->findChild<QAction*>("copyReportCsvAction"),"clipboard copy action remains available");
    std::cout<<"PASS: selected export failure reached row 201 after exactly 2 selected-row reads; target absent and page invalidated"<<std::endl;
    failRow=-1;view.SelectReport(doc.id);Require(view.property("reportRowsError").toString().isEmpty()&&table->model()->rowCount()==200,"reselecting a recovered report did not restore its page");
}
void CheckSlowScanCancellation() {
    ResultReport view;auto doc=Document("slow-scan","体积",0);auto lease=std::make_shared<int>(1);std::weak_ptr<int> retained=lease;
    struct Gate final {
        std::promise<void> resume;std::shared_future<void> release=resume.get_future().share();
        std::atomic<bool> entered{false},opened{false};std::atomic<int> starts{0};
        void Open(){if(!opened.exchange(true))resume.set_value();}
        ~Gate(){Open();}
    };
    const auto gate=std::make_shared<Gate>();
    struct ReleaseGate {std::shared_ptr<Gate> value;~ReleaseGate(){value->Open();}} releaseGate{gate};
    auto* owner=QThread::currentThread();
    doc.table=std::make_shared<ReportTable>(500,[gate,held=lease,owner](ReportRowId row){
        if(QThread::currentThread()!=owner&&row==0){++gate->starts;if(!gate->opened){gate->entered=true;gate->release.wait();}}
        return QStringList{QString("point-%1").arg(row),QString::number(row)};
    });lease.reset();view.AddReport(doc);auto* search=view.findChild<QLineEdit*>("reportSearch");search->setText("point-");
    Wait([&]{return !gate->entered.load();});
    std::promise<void> returned;auto completed=returned.get_future();
    // 看门狗只避免错误实现在测试里死等；判据是 UI 操作先返回，再由测试释放慢读。
    auto watchdog=std::async(std::launch::async,[&]{const bool responsive=completed.wait_for(std::chrono::seconds(5))==std::future_status::ready;gate->Open();return responsive;});
    view.findChild<QPushButton*>("cancelReportRows")->click();
    Require(!view.property("reportRowsBusy").toBool()&&!view.findChild<QPushButton*>("exportSelectedReport")->isEnabled(),"cancelled scan still exposes partial results");
    search->setText("point-1");search->setText("point-2");search->setText("point-499");
    Require(gate->starts==1,"query replacement started an additional worker while the old scan was blocked");
    returned.set_value();Require(watchdog.get(),"ordinary cancellation or query replacement waited for the blocked reader");
    Wait([&]{return view.property("reportRowsBusy").toBool();});
    Require(view.property("reportRowsError").toString().isEmpty()&&view.GetFilteredCount()==1&&view.CreateFilteredReader()(0)==499&&gate->starts==2,"coalesced latest query was lost or superseded by cancelled rows");
    const auto closingGate=std::make_shared<Gate>();ReleaseGate closingRelease{closingGate};
    auto closing=Document("retiring-scan","体积",0);auto held=std::make_shared<int>(1);std::weak_ptr<int> retiring=held;
    closing.table=std::make_shared<ReportTable>(500,[closingGate,held,owner](ReportRowId row){
        if(QThread::currentThread()!=owner&&row==0){closingGate->entered=true;closingGate->release.wait();}
        return QStringList{QString("point-%1").arg(row),QString::number(row)};
    });held.reset();view.AddReport(closing);view.SelectReport(closing.id);search->setText("point-");Wait([&]{return !closingGate->entered.load();});
    view.SelectReport(doc.id);view.RemoveReport(closing.id);closing.table.reset();
    Require(!retiring.expired()&&view.GetSelectedReport().id==doc.id&&view.GetFilteredCount()==500,"removal released a running reader or invalidated the newer selection");
    closingGate->Open();Wait([&]{return !retiring.expired();});
    Require(view.GetSelectedReport().id==doc.id&&view.findChild<QTableView*>("reportTable")->model()->rowCount()==200,"retired scan completion replaced or cleared the newer page");
    search->setText("point-");view.CancelTasks(true);
    Require(!view.property("reportRowsBusy").toBool(),"final task shutdown left a pending scan");
    view.RemoveReport(doc.id);doc.table.reset();
    Require(retained.expired(),"completed cancelled scan retained its immutable source owner");
}
#if defined(MANUAL_ALIGNMENT)
void CheckFrozenAlignmentParameters() {
    DataRevisionRef ref;ref.entityId.bytes[0]=83;ref.generation=1;
    // 第一个不能精确表示为 IEEE double 的正整数，验证编号不经浮点转换。
    constexpr std::uint64_t large=9007199254740993ULL;const auto decimal=QStringLiteral("9007199254740993");
    InspectionPlan plan;plan.id="frozen-plan";plan.alignmentResult=ref;plan.requiresMeasurementQuality=true;
    InspectionGeometrySpec geometry;geometry.fit.kind=AlignmentGeometryKind::Circle;geometry.fit.region.pinnedMesh=ref;
    geometry.fit.region.vertexIds={large};geometry.fit.region.roi=ref;
    InspectionSection section;section.pinnedMesh=ref;section.triangles={large};section.selectionPoint=AlignmentPoint{1,2,3};geometry.section=section;
    plan.geometries.push_back(geometry);
    InspectionItem item;item.id="diameter";item.name="冻结直径";item.measured.metric=InspectionMetric::Diameter;item.nominal=3.75;item.deviationLimits=std::array<double,2>{-.1,.2};plan.items.push_back(item);
    const auto saved=GetAlignmentReportParameters(plan);const auto savedGeometry=saved["geometries"].toArray().first().toObject();
    const auto savedRegion=savedGeometry["fit"].toObject()["region"].toObject();const auto savedItem=saved["items"].toArray().first().toObject();
    Require(saved["id"]=="frozen-plan" && saved["alignmentResult"]==GetRefText(ref) && saved["requiresMeasurementQuality"].toBool(),"inspection conditions lost frozen plan identity or quality policy");
    Require(savedRegion["vertexIds"].toArray().first()==decimal && savedRegion["roi"]==GetRefText(ref)
        && savedGeometry["section"].toObject()["triangles"].toArray().first()==decimal,"inspection condition indices lost 64-bit precision");
    Require(savedItem["name"]=="冻结直径" && savedItem["nominal"]==3.75 && savedItem["measured"].toObject()["metric"]=="Diameter"
        && savedItem["deviationLimits"].toArray()==QJsonArray({-.1,.2}),"inspection conditions lost item definition or tolerance");
    AlignmentDeviationRequest deviation;deviation.alignmentResult=ref;deviation.evaluationRoi=ref;deviation.sourceTriangles={large};
    deviation.metric=AlignmentDeviationMetric::NominalNormalProjection;deviation.computeBackend=AlignmentComputeBackend::Cuda;deviation.reverseCoverage=false;
    const auto deviationJson=GetAlignmentReportParameters(deviation);
    Require(deviationJson["evaluationRoi"]==GetRefText(ref) && deviationJson["sourceTriangles"].toArray().first()==decimal
        && deviationJson["computeBackend"]=="Cuda" && deviationJson["metric"]=="NominalNormalProjection"
        && deviationJson["tolerance"].isNull() && !deviationJson["reverseCoverage"].toBool(),"deviation conditions replaced frozen backend, selection, metric or absent tolerance");
    VoxelVolumeRequest volume;volume.source=ref;volume.method=VoxelVolumeMethod::Labels;volume.labels={0,large};volume.validityImage=ref;volume.requireEnclosedSelection=true;
    const auto volumeJson=GetAlignmentReportParameters(volume);
    Require(volumeJson["source"]==GetRefText(ref) && volumeJson["validityImage"]==GetRefText(ref) && volumeJson["method"]=="Labels"
        && volumeJson["labels"].toArray()==QJsonArray({QStringLiteral("0"),decimal}) && volumeJson["extent"].isNull()
        && volumeJson["storedValueRange"].isNull() && volumeJson["requireEnclosedSelection"].toBool(),"volume conditions lost exact label selection, validity input or completeness policy");
    int checkpoints=0;bool cancelled=false;
    try {GetAlignmentReportParameters(plan,[&]{if(++checkpoints>1)throw std::runtime_error("parameter-json-cancelled");});}
    catch(const std::runtime_error& error){cancelled=std::string(error.what())=="parameter-json-cancelled";}
    Require(cancelled && checkpoints>1,"frozen parameter traversal ignored cancellation inside a collection");
}
void CheckInspectionPreparation() {
    ResultReport view;std::weak_ptr<const InspectionPlan> planLease;std::weak_ptr<const InspectionReport> reportLease;
    DataRevisionRef ref;ref.entityId.bytes[0]=31;ref.generation=1;const auto text=GetRefText(ref);
    const QJsonObject record{{"operationId","inspection-preparation"},{"module","Alignment"},{"result",QJsonObject{{"inspection",text}}}};
    view.onReadReports=[&](const QJsonObject& input,const QSet<QString>&) {
        auto plan=std::make_shared<InspectionPlan>();auto report=std::make_shared<InspectionReport>();
        for(int i=0;i<50000;++i){InspectionItem spec;spec.id=std::to_string(i);spec.name="name-"+spec.id;
            spec.measured.metric=i%2?InspectionMetric::OrientedDirectionAngleDegrees:InspectionMetric::Radius;plan->items.push_back(spec);}
        for(int i=49999;i>=0;--i){InspectionItemResult row;row.id=std::to_string(i);row.nominal=i;row.measured=i+.5;row.deviation=.5;row.message="original";report->items.push_back(row);}
        auto repeated=plan->items.front();repeated.name="wrong duplicate";plan->items.push_back(repeated);
        InspectionItemResult unknown;unknown.id="unknown";report->items.push_back(unknown);
        InspectionSnapshot snapshot;snapshot.result=ref;snapshot.alignmentResult=ref;snapshot.sourceMesh=ref;snapshot.unit=AlignmentUnit::Millimeter;
        snapshot.plan=plan;snapshot.report=report;planLease=plan;reportLease=report;
        auto doc=CreateInspectionReport(input,snapshot,"data-a");Require(!doc.table && bool(doc.prepareReports),"inspection provider eagerly materialized its plan or rows");
        return QVector<ReportDocument>{std::move(doc)};
    };
    view.QueueReport(record);view.ShowReports({},text,"Alignment");view.show();
    Wait([&]{return view.property("reportGenerationProgress").toULongLong()<256;});
    Require(view.property("reportGenerationBusy").toBool()&&!planLease.expired(),"inspection preparation failed to yield before finishing its index");
    view.CancelTasks();Require(planLease.expired()&&reportLease.expired()&&view.GetReportIds().isEmpty(),"cancelled inspection preparation retained snapshots or published a group");
    view.ShowReports({},text,"Alignment");Wait([&]{return view.property("reportGenerationPhase").toString()!="rows";});
    Require(view.property("reportGenerationBusy").toBool()&&view.GetReportIds().isEmpty(),"inspection row writing failed to yield before publication");
    view.CancelTasks();Require(planLease.expired()&&reportLease.expired(),"cancelled inspection row writing retained snapshots");
    view.ShowReports({},text,"Alignment");Wait([&]{return view.GetReports("Alignment",{text}).size()!=2 || view.property("reportGenerationBusy").toBool();});
    const auto groups=view.GetReports("Alignment",{text});Require(planLease.expired()&&reportLease.expired(),"completed inspection groups retained the native plan or results");
    int total=0;for(const auto& doc:groups){Require(doc.table->GetCount()==25000,"inspection grouping changed its complete population");const bool angle=doc.columns[3].unit=="deg";
        Require(angle||doc.columns[3].unit=="mm","inspection grouping lost units");
        for(int i=0;i<doc.table->GetCount();++i){const int original=(angle?49999:49998)-2*i;const auto row=doc.table->Read(i);
            Require(row[0]==QString::number(original)&&row[1]=="name-"+row[0]&&row[2]==QString::number(original)&&row[3]==QString::number(original+.5,'g',17)&&row[4]=="0.5"&&row[6]=="original","inspection preparation changed IDs, order, names, values or original reason");}total+=doc.table->GetCount();}
    Require(total==50000,"inspection preparation lost or duplicated report items");view.hide();
}
#endif
}
class ShortWriteDevice final : public QIODevice {
public:
    explicit ShortWriteDevice(qint64 limit):m_limit(limit){open(QIODevice::WriteOnly);}
    QByteArray bytes;
protected:
    qint64 readData(char*,qint64) override{return -1;}
    qint64 writeData(const char* data,qint64 size) override {
        if(bytes.size()>=m_limit){setErrorString("injected short write");return -1;}
        const auto accepted=std::min(size,m_limit-bytes.size());bytes.append(data,static_cast<int>(accepted));return accepted;
    }
private:qint64 m_limit;
};
void CheckFileExports() {
    QTemporaryDir directory;Require(directory.isValid(),"export temp directory unavailable");
    auto doc=Document("file-export","原始数量",4);doc.identityResult=QString(32,'a')+":1";
    doc.columns={{"object_id",{},{},true},{"surface_object_volume_mm3","原始数量","mm3",true},{"boundary_status",{},{},false}};doc.identityColumn=0;
    doc.table=std::make_shared<ReportTable>();
    const QStringList exactId{"9007199254740993","id,\"quoted\"\n中文","", "last"};
    const QStringList exactValue{"1,2","3\"4\n5","","0"};const QStringList exactStatus{"Interior","BoundaryUncertain","Missing","Interior"};
    for(int i=0;i<exactId.size();++i)doc.table->Append({exactId[i],exactValue[i],exactStatus[i]});
    ResultReport view;view.AddReport(doc);view.SelectReport(doc.id);
    auto* table=view.findChild<QTableView*>("reportTable");Require(table&&table->model()->rowCount()==4,"export fixture page did not load");
    Require(!view.findChild<QPushButton*>("copyReportCsv")&&table->actions().size()==1
        &&table->actions().first()->objectName()=="exportSelectedReportAction","report context menu retained clipboard or whole-report actions");
    auto* tools=view.findChild<QToolButton*>("reportActions");Require(tools&&tools->menu(),"report command menu is missing");view.resize(1000,300);view.show();QCoreApplication::processEvents();
    Require(table->viewport()->height()>=table->rowHeight(0),"narrow report clips its first data row");
    bool menuSeen=false;DrivePopupInteraction([&]{tools->showMenu();},[&]{
        auto* menu=tools->menu();if(!menu->isVisible())return false;
        for(const auto* name:{"setReportBaseline","compareReports","exportFilteredReport","associateReportSource","saveReportConditions","removeReport"}) {
            auto* action=view.findChild<QAction*>(name);Require(action&&action->isVisible()&&menu->actions().contains(action),"secondary report action is not reachable through the visible menu");
        }
        view.findChild<QAction*>("setReportBaseline")->trigger();menuSeen=true;menu->close();return true;
    },"report command menu did not open");Require(menuSeen,"report command menu was not inspected");
    const auto allPath=directory.filePath("all.csv");ReportTaskTestAccess::ChooseExportPath(view,[&](const QString&){return allPath;});
    view.findChild<QPushButton*>("exportReportCsv")->click();Require(view.property("reportTaskBusy").toBool(),"all-row export did not admit asynchronously");Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(view.findChild<QLabel*>("reportTaskStatus")->text()=="已导出 4 行 · all.csv","completed export row count is not displayed as exact decimal digits");
    QCoreApplication::processEvents();
    auto* status=view.findChild<QLabel*>("reportTaskStatus");
    Require(status&&status->isVisible()&&view.rect().contains(QRect(status->mapTo(&view,QPoint()),status->size()))
        &&table->viewport()->height()>=table->rowHeight(0),"completed export feedback or first table row leaves the narrow report bounds");view.hide();
    QFile allFile(allPath);Require(allFile.open(QIODevice::ReadOnly),"all-row export target unreadable");const auto allBytes=allFile.readAll();
    Require(allBytes.startsWith(QByteArray("\xef\xbb\xbf"))&&allBytes.contains("\r\n")
        &&allBytes.contains(("mvvcvtk-result="+doc.identityResult).toUtf8()),"export lost UTF-8 BOM, CRLF or result identity header annotation");
    QBuffer allBuffer;allBuffer.setData(allBytes);Require(allBuffer.open(QIODevice::ReadOnly),"all-row export buffer open failed");const auto all=ReadReportCsv(allBuffer,"all export");
    Require(all.columns.size()==3&&all.columns[0].name=="object_id"&&all.identityResult==doc.identityResult&&all.columns[1].name=="surface_object_volume_mm3"
        &&all.columns[1].quantity=="原始数量"&&all.columns[1].unit=="mm3"&&all.columns[2].name=="boundary_status","export header lost ID annotation, original quantity/unit or status column");
    Require(all.table->GetCount()==4,"all-row export count/order changed");
    for(ReportRowId row=0;row<4;++row){const auto actual=all.table->Read(row);Require(actual.size()==3&&actual[0]==exactId[static_cast<int>(row)]&&actual[1]==exactValue[static_cast<int>(row)]&&actual[2]==exactStatus[static_cast<int>(row)],"all-row export changed exact cell value, quoting, row order, missing value or status");}
    table->selectRow(1);table->selectionModel()->select(table->model()->index(3,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
    const auto selectedPath=directory.filePath("selected.csv");ReportTaskTestAccess::ChooseExportPath(view,[&](const QString&){return selectedPath;});
    view.findChild<QPushButton*>("exportSelectedReport")->click();Wait([&]{return view.property("reportTaskBusy").toBool();});
    QFile selectedFile(selectedPath);Require(selectedFile.open(QIODevice::ReadOnly),"selected-row export target unreadable");QBuffer selectedBuffer;selectedBuffer.setData(selectedFile.readAll());selectedBuffer.open(QIODevice::ReadOnly);
    const auto selected=ReadReportCsv(selectedBuffer,"selected export");Require(selected.table->GetCount()==2,"selected export row count changed");
    Require(SameCells(selected.table->Read(0),QStringList{exactId[1],exactValue[1],exactStatus[1]})&&SameCells(selected.table->Read(1),QStringList{exactId[3],exactValue[3],exactStatus[3]}),"selected export changed selected row order or exact values");
    {
        ResultReport keyboard;keyboard.AddReport(doc);auto* grid=keyboard.findChild<QTableView*>("reportTable");
        class ReadProbe final : public QIdentityProxyModel {
        public:
            mutable int displayReads=0;
            QVariant data(const QModelIndex& index,int role) const override {if(role==Qt::DisplayRole)++displayReads;return QIdentityProxyModel::data(index,role);}
        } probe;
        probe.setSourceModel(grid->model());grid->setModel(&probe);grid->setCurrentIndex(probe.index(0,0));
        for(const auto key:{Qt::Key_C,Qt::Key_Insert})for(const auto type:{QEvent::ShortcutOverride,QEvent::KeyPress}){
            probe.displayReads=0;QKeyEvent event(type,key,Qt::ControlModifier);QCoreApplication::sendEvent(grid,&event);
            // Qt 默认 Copy 会读当前单元格后写系统剪贴板；零读取证明在此之前已截断，测试不访问剪贴板。
            Require(event.isAccepted()&&probe.displayReads==0,"report retained the Qt item-view clipboard shortcut");
        }
        QKeyEvent down(QEvent::KeyPress,Qt::Key_Down,Qt::NoModifier);QCoreApplication::sendEvent(grid,&down);
        Require(grid->currentIndex().row()==1,"blocking Copy disabled ordinary table keyboard navigation");
        grid->setModel(probe.sourceModel());
    }
    ReportTaskTestAccess::ChooseExportPath(view,[](const QString&){return QString();});view.findChild<QPushButton*>("exportReportCsv")->click();
    Require(!view.property("reportTaskBusy").toBool()&&view.property("reportExportStatus").toString()!="writing","cancelled save dialog started an export task");
    ShortWriteDevice shortWrite(19);bool writeFailed=false;try{WriteReportCsv(shortWrite,doc);}catch(const std::runtime_error& e){writeFailed=std::string(e.what()).find("write")!=std::string::npos||std::string(e.what()).find("写")!=std::string::npos;}
    Require(writeFailed&&!shortWrite.bytes.isEmpty(),"WriteReportCsv did not report a short/failed QIODevice write");
    const auto badPath=directory.filePath("existing-directory");Require(QDir().mkpath(badPath),"failed-target directory fixture could not be created");
    ReportTaskTestAccess::ChooseExportPath(view,[&](const QString&){return badPath;});view.findChild<QPushButton*>("exportReportCsv")->click();
    Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(!view.property("reportTaskBusy").toBool()&&!view.property("reportTaskError").toString().isEmpty(),"directory output path did not expose a write failure");
    QDir badResidue(badPath);Require(badResidue.entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty(),"failed directory export left a temporary file");
#ifdef Q_OS_WIN
    const auto lockedPath=directory.filePath("locked.csv");{QFile original(lockedPath);Require(original.open(QIODevice::WriteOnly)&&original.write("original-bytes")==14,"locked target seed write failed");}
    HANDLE locked=CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Require(locked!=INVALID_HANDLE_VALUE,"cannot acquire no-delete-share handle for commit-failure fixture");
    struct HandleGuard{HANDLE h;~HandleGuard(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}} handle{locked};
    ReportTaskTestAccess::ChooseExportPath(view,[&](const QString&){return lockedPath;});view.findChild<QPushButton*>("exportReportCsv")->click();Wait([&]{return view.property("reportTaskBusy").toBool();});
    QFile stillOriginal(lockedPath);Require(stillOriginal.open(QIODevice::ReadOnly)&&stillOriginal.readAll()=="original-bytes","commit failure changed locked target bytes");
    Require(view.property("reportExportStatus").toString()=="failed"&&view.property("reportTaskError").toString().startsWith("报告保存失败")
        &&view.property("reportExportRows").toLongLong()==4,"locked target did not reach and report QSaveFile commit failure");
    QDir directoryCheck(directory.path());Require(SameCells(directoryCheck.entryList({"*locked.csv*"},QDir::Files|QDir::Hidden),QStringList{"locked.csv"}),"commit failure left QSaveFile temporary artifact");
#endif
    struct ExportGate final {
        std::promise<void> release;std::shared_future<void> future=release.get_future().share();std::atomic<bool> entered{false},opened{false};
        void Open(){if(!opened.exchange(true))release.set_value();}
    };
    struct ReleaseExportGate final {std::shared_ptr<ExportGate> gate;~ReleaseExportGate(){gate->Open();}};
    for(const bool existing:{false,true}) {
        ResultReport cancelView;const auto gate=std::make_shared<ExportGate>();ReleaseExportGate release{gate};
        auto cancelDoc=Document("cancel-export","quantity",0);
        cancelDoc.table=std::make_shared<ReportTable>(1000,[gate](ReportRowId row){if(row==300){gate->entered=true;if(gate->future.wait_for(std::chrono::milliseconds(reportWaitTimeoutMs))!=std::future_status::ready)throw std::runtime_error("export cancellation fixture guard");}return QStringList{QString::number(row),QString::number(row+1)};});
        cancelView.AddReport(cancelDoc);cancelView.SelectReport(cancelDoc.id);
        const auto name=existing?QString("cancel-existing.csv"):QString("cancel-new.csv");const auto cancelPath=directory.filePath(name);
        if(existing){QFile original(cancelPath);Require(original.open(QIODevice::WriteOnly)&&original.write("preserve-me")==11,"cancel target seed write failed");}
        ReportTaskTestAccess::ChooseExportPath(cancelView,[&](const QString&){return cancelPath;});cancelView.findChild<QPushButton*>("exportReportCsv")->click();
        Wait([&]{return !gate->entered.load();});
        std::promise<void> cancelReturned;auto cancelReturnedFuture=cancelReturned.get_future();
        auto watchdog=std::async(std::launch::async,[&]{const bool responsive=cancelReturnedFuture.wait_for(std::chrono::seconds(5))==std::future_status::ready;gate->Open();return responsive;});
        cancelView.findChild<QPushButton*>("cancelReportTask")->click();cancelReturned.set_value();Require(watchdog.get(),"cancel button blocked waiting for the export worker");
        Wait([&]{return cancelView.property("reportTaskBusy").toBool();});
        if(existing){QFile preserved(cancelPath);Require(preserved.open(QIODevice::ReadOnly)&&preserved.readAll()=="preserve-me","cancelled export changed an existing target");}
        else Require(!QFile::exists(cancelPath),"cancelled export published a partial target");
        Require(cancelView.property("reportExportStatus").toString()=="cancelled"&&cancelView.property("reportTaskError").toString().isEmpty(),"cooperative cancellation was reported as a worker error");
        const auto expected=existing?QStringList{name}:QStringList{};QDir cancelResidue(directory.path());
        Require(SameCells(cancelResidue.entryList({"*"+name+"*"},QDir::Files|QDir::Hidden),expected),"cancelled export left a QSaveFile temporary artifact");
    }
    {
        ResultReport lateView;const auto gate=std::make_shared<ExportGate>();ReleaseExportGate release{gate};
        auto late=Document("late-cancel-export","quantity",0);auto* owner=QThread::currentThread();
        late.table=std::make_shared<ReportTable>(4,[gate,owner](ReportRowId row){
            if(row==0&&QThread::currentThread()!=owner){gate->entered=true;if(gate->future.wait_for(std::chrono::milliseconds(reportWaitTimeoutMs))!=std::future_status::ready)throw std::runtime_error("late cancellation fixture guard");}
            return QStringList{QString::number(row),QString::number(row+1)};
        });
        lateView.AddReport(late);const auto path=directory.filePath("committed-before-owner.csv");lateView.ExportReportFile(path);
        ReportTaskTestAccess::ContinueTask(lateView);Wait([&]{return !gate->entered.load();});gate->Open();
        // 此区间不派发 owner 事件：文件存在证明 commit 已发生，任务占位尚未回收。
        QElapsedTimer guard;guard.start();while(!QFile::exists(path)&&guard.elapsed()<reportWaitTimeoutMs)QThread::yieldCurrentThread();
        Require(QFile::exists(path)&&ReportTaskTestAccess::HasTask(lateView),"late cancellation fixture did not hold owner completion after commit");
        lateView.CancelTasks();Wait([&]{return lateView.property("reportTaskBusy").toBool();});
        Require(lateView.property("reportExportStatus").toString()=="completed"&&lateView.property("reportTaskError").toString().isEmpty()
            &&lateView.property("reportExportPath").toString()==path,"late cancellation misreported a committed file as cancelled");
        QFile input(path);Require(input.open(QIODevice::ReadOnly),"late-cancel committed file unreadable");const auto copy=ReadReportCsv(input,"late cancellation");
        Require(copy.table->GetCount()==4,"late cancellation altered the committed file");
        for(ReportRowId row=0;row<4;++row)Require(SameCells(copy.table->Read(row),QStringList{QString::number(row),QString::number(row+1)}),"late cancellation changed a committed cell");
    }
    const auto stressRows=[](){const auto text=qEnvironmentVariable("MVVCVTK_REPORT_STRESS_ROWS");if(text.isEmpty())return ReportRowId(200003);bool ok=false;const auto value=text.toLongLong(&ok);Require(ok&&value>0&&value<=(std::numeric_limits<ReportRowId>::max()-7)/3,"invalid report export stress row count");return value;}();
    ResultReport stress;auto large=Document("large-export","stress quantity",0);large.columns={{"row_id",{},{},false},{"value",{}, {},false}};
    auto* owner=QThread::currentThread();struct ReadMetrics final {std::atomic<bool> wrongThread{false};std::atomic<ReportRowId> reads{0};};const auto metrics=std::make_shared<ReadMetrics>();
    large.table=std::make_shared<ReportTable>(stressRows,[metrics,owner](ReportRowId row){if(QThread::currentThread()==owner)metrics->wrongThread=true;++metrics->reads;return QStringList{QString::number(row),QString::number(row*3+7)};});
    stress.AddReport(large);stress.SelectReport(large.id);
    const auto stressPath=directory.filePath("stress.csv");ReportTaskTestAccess::ChooseExportPath(stress,[&](const QString&){return stressPath;});
    QString evidence=qEnvironmentVariable("MVVCVTK_REPORT_EXPORT_EVIDENCE");
    if(!evidence.isEmpty()){
        Require(QDir().mkpath(evidence),"export evidence directory cannot be created");stress.resize(1280,780);stress.setStyleSheet(GetWorkbenchStyle());stress.show();QCoreApplication::processEvents();
        const auto shot=QDir(evidence).filePath("report-export-before.png");Require(!QFileInfo::exists(shot)&&stress.grab().save(shot),"cannot save report export before screenshot");
    }
    metrics->reads=0;metrics->wrongThread=false;
    auto* pulse=new QTimer(&stress);pulse->setInterval(20);pulse->setTimerType(Qt::CoarseTimer);QVector<qint64> gaps;QElapsedTimer pulseClock;pulseClock.start();
    QObject::connect(pulse,&QTimer::timeout,&stress,[&]{gaps.append(pulseClock.restart());});pulse->start();
    QElapsedTimer admission;admission.start();stress.findChild<QPushButton*>("exportReportCsv")->click();const auto admissionMs=admission.elapsed();
    Require(stress.property("reportTaskBusy").toBool(),"large export failed asynchronous admission");
    Require(stress.findChild<QLabel*>("reportTaskStatus")->text()==QString("正在导出 · 0 / %1 行").arg(stressRows),"admitted export row count is not displayed as exact decimal digits");
    bool writingShot=false;
    QElapsedTimer run;run.start();while(stress.property("reportTaskBusy").toBool()&&run.elapsed()<120000){QCoreApplication::processEvents(QEventLoop::AllEvents,10);if(!writingShot&&!evidence.isEmpty()&&stress.property("reportExportStatus").toString()=="writing"){
        const auto shot=QDir(evidence).filePath("report-export-writing.png");Require(!QFileInfo::exists(shot)&&stress.grab().save(shot),"cannot save report export writing screenshot");writingShot=true;}}
    Require(!stress.property("reportTaskBusy").toBool()&&stress.property("reportExportStatus").toString()=="completed","large export did not commit successfully");pulse->stop();
    Require(stress.findChild<QLabel*>("reportTaskStatus")->text()==QString("已导出 %1 行 · stress.csv").arg(stressRows),"large export completion lost its exact decimal row count");
    const auto exportMs=admissionMs+run.elapsed();
    QFile stressFile(stressPath);Require(stressFile.open(QIODevice::ReadOnly),"large export file unreadable");QBuffer stressBuffer;stressBuffer.setData(stressFile.readAll());stressBuffer.open(QIODevice::ReadOnly);const auto stressCopy=ReadReportCsv(stressBuffer,"large export");
    Require(stressCopy.table->GetCount()==stressRows&&metrics->reads==stressRows&&!metrics->wrongThread.load()&&!gaps.isEmpty(),"large export row count, owner heartbeat or worker read-thread identity incorrect");
    for(ReportRowId row=0;row<stressRows;++row){const auto cells=stressCopy.table->Read(row);Require(SameCells(cells,QStringList{QString::number(row),QString::number(row*3+7)}),"large CSV row failed independent exact-value formula check");}
    std::sort(gaps.begin(),gaps.end());const auto percentile=[&](double p){return gaps.isEmpty()?qint64(0):gaps[static_cast<int>(p*(gaps.size()-1))];};
    qint64 over100=0,over1000=0,maxGap=0;for(const auto gap:gaps){if(gap>100)++over100;if(gap>1000)++over1000;maxGap=std::max(maxGap,gap);}
    std::cout<<"REPORT_EXPORT_STRESS rows="<<stressRows<<" admissionMs="<<admissionMs<<" totalMs="<<exportMs<<" ownerEvents="<<gaps.size()<<" maxMs="<<maxGap<<" p95Ms="<<percentile(.95)<<" over100ms="<<over100<<" over1s="<<over1000<<" workerReadThread=true"<<std::endl;
    if(!evidence.isEmpty()){const auto shot=QDir(evidence).filePath("report-export-complete.png");Require(!QFileInfo::exists(shot)&&stress.grab().save(shot),"cannot save report export completion screenshot");}
    if(stress.isVisible())stress.close();
}
void CheckExportLifetimes() {
    QTemporaryDir directory;Require(directory.isValid(),"export lifetime temp directory unavailable");
    struct Gate final {
        std::promise<void> release;std::shared_future<void> ready=release.get_future().share();
        std::atomic<bool> entered{false},exited{false},opened{false};
        void Open(){if(!opened.exchange(true))release.set_value();}
    };
    struct ReleaseOnExit final {std::shared_ptr<Gate> gate;~ReleaseOnExit(){gate->Open();}};
    for(const bool closeWindow:{false,true}) {
        ResultReport view;const auto gate=std::make_shared<Gate>();ReleaseOnExit release{gate};
        auto original=Document("export-original","原始数量",0);auto* owner=QThread::currentThread();
        auto source=std::make_shared<const int>(42);const std::weak_ptr<const int> sourceLease=source;
        original.table=std::make_shared<ReportTable>(1000,[source,gate,owner](ReportRowId row){
            if(row==300){
                Require(QThread::currentThread()!=owner,"export body read on the owner thread");gate->entered=true;
                if(gate->ready.wait_for(std::chrono::milliseconds(reportWaitTimeoutMs))!=std::future_status::ready)throw std::runtime_error("export lifetime fixture guard");
                gate->exited=true;
            }
            return QStringList{QString::number(row),QString::number(*source+row)};
        });
        view.AddReport(original);view.show();const auto path=directory.filePath(closeWindow?"close.csv":"selected.csv");
        view.ExportReportFile(path);original.table.reset();source.reset();Wait([&]{return !gate->entered.load();});
        auto replacement=Document("export-replacement");replacement.source="data-b";
        if(closeWindow) {
            view.close();Require(!gate->exited.load(),"closing waited for the blocked export worker");
            bool rejected=false;try{view.ExportReportFile(directory.filePath("closed.csv"));}catch(const std::runtime_error&){rejected=true;}
            Require(rejected,"closed report window accepted another export");
            view.show();view.RemoveReport(original.id);
        }
        view.AddReport(replacement);Require(view.SelectReport(replacement.id),"replacement report not selected");view.ShowReports("data-b");
        Require(!sourceLease.expired()&&!gate->exited.load(),"selection or removal released the source while its export reader was running");
        gate->Open();Wait([&]{return view.property("reportTaskBusy").toBool();});
        Require(view.GetSelectedReport().id==replacement.id,"old export completion replaced a newer report selection");
        Require(view.property("reportTaskError").toString().isEmpty(),"export lifetime transition reported a worker error");
        if(closeWindow) {
            Require(view.property("reportExportStatus").toString()=="cancelled"&&!QFile::exists(path)&&sourceLease.expired(),
                "close/reopen published a partial export or retained the removed report source");
            const auto retry=directory.filePath("reopened.csv");view.ExportReportFile(retry);Wait([&]{return view.property("reportTaskBusy").toBool();});
            QFile file(retry);Require(file.open(QIODevice::ReadOnly),"reopened report export did not commit");const auto copy=ReadReportCsv(file,"reopened");
            Require(copy.table->GetCount()==2&&SameCells(copy.table->Read(1),QStringList{"point-1","2"}),"old task corrupted the reopened report export");
        } else {
            QFile file(path);Require(file.open(QIODevice::ReadOnly),"frozen original export did not commit");const auto copy=ReadReportCsv(file,"original");
            Require(copy.table->GetCount()==1000&&SameCells(copy.table->Read(0),QStringList{"0","42"})
                &&SameCells(copy.table->Read(999),QStringList{"999","1041"}),"selection change replaced the frozen export source");
            view.RemoveReport(original.id);Require(sourceLease.expired(),"completed export retained the removed source");
        }
        view.CancelTasks(true);QCoreApplication::processEvents();
    }
    // worker 尚在等 owner 物化时直接析构；取消必须唤醒 gate，且不能再调用原生来源。
    std::weak_ptr<const int> sourceLease;auto reads=std::make_shared<int>(0);const auto path=directory.filePath("destroyed.csv");
    {
        auto view=std::make_unique<ResultReport>();auto doc=Document("export-destroy-before-prepare","原始数量",0);
        auto source=std::make_shared<const int>(7);sourceLease=source;doc.table=std::make_shared<ReportTable>();
        doc.table->SetRows(1000,[source,reads](ReportRowId row){++*reads;return QStringList{QString::number(row),QString::number(*source)};});
        view->AddReport(doc);*reads=0;view->ExportReportFile(path);doc.table.reset();source.reset();
        Require(view->property("reportTaskBusy").toBool(),"destruction fixture did not reserve the export task");view.reset();
    }
    Require(*reads==0&&sourceLease.expired()&&!QFile::exists(path),"destroying an unprepared export retained or read its source or published a file");
    QCoreApplication::processEvents();
    std::cout<<"PASS: report export freezes its source across selection, close/reopen, removal and destruction before preparation"<<std::endl;
}
void CheckSelectedUnmaterializedExports() {
    QTemporaryDir directory;Require(directory.isValid(),"selected lazy export temp directory unavailable");
    for(const bool failUnselected:{false,true}) {
        auto doc=Document(failUnselected?"selected-lazy-fault":"selected-lazy","Original quantity",0);
        doc.columns={{"source_id",{},{},false},{"native_measurement","Original quantity","mm3",true}};
        doc.table=std::make_shared<ReportTable>();std::atomic<ReportRowId> sourceReads{0},unselectedReads{0},row500Reads{0},outOfRangeReads{0};
        doc.table->SetRows(1000,[&,failUnselected](ReportRowId row){
            ++sourceReads;if(row<0||row>=1000){++outOfRangeReads;throw std::out_of_range("selected export source row out of range");}
            if(row>=2)++unselectedReads;if(row==500){++row500Reads;if(failUnselected)throw std::runtime_error("unselected row 500 unreadable");}
            return QStringList{QString("id-%1").arg(row),QString("value-%1").arg(row*5+1)};
        });
        ResultReport view;view.AddReport(doc);Require(view.SelectReport(doc.id),"lazy selected export report selection failed");
        auto* table=view.findChild<QTableView*>("reportTable");Require(table&&table->model()->rowCount()==200&&!doc.table->GetIsComplete(),"lazy export preview unexpectedly materialized its source");
        table->selectionModel()->select(table->model()->index(0,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        table->selectionModel()->select(table->model()->index(1,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        sourceReads=0;unselectedReads=0;row500Reads=0;outOfRangeReads=0;
        const auto path=directory.filePath(failUnselected?"selected-lazy-fault.csv":"selected-lazy.csv");
        ReportTaskTestAccess::ChooseExportPath(view,[&](const QString&){return path;});view.findChild<QPushButton*>("exportSelectedReport")->click();
        Wait([&]{return view.property("reportTaskBusy").toBool();});
        Require(sourceReads==2&&unselectedReads==0&&row500Reads==0&&outOfRangeReads==0,
            "selected lazy export read rows outside selected source rows");
        Require(!doc.table->GetIsComplete()&&view.property("reportExportStatus").toString()=="completed"&&view.property("reportTaskError").toString().isEmpty(),
            "selected lazy export materialized the source or failed its task status");
        QFile file(path);Require(file.open(QIODevice::ReadOnly),"selected lazy export output unreadable");QBuffer buffer;buffer.setData(file.readAll());Require(buffer.open(QIODevice::ReadOnly),"selected lazy export buffer open failed");
        const auto exported=ReadReportCsv(buffer,"selected lazy export");Require(exported.table->GetCount()==2&&exported.columns.size()==2
            &&exported.columns[1].name=="native_measurement"&&exported.columns[1].quantity=="Original quantity"&&exported.columns[1].unit=="mm3",
            "selected lazy export lost row count or original unit header");
        Require(SameCells(exported.table->Read(0),QStringList{"id-0","value-1"})&&SameCells(exported.table->Read(1),QStringList{"id-1","value-6"}),
            "selected lazy export changed exact original row values or order");
    }
    auto filtered=Document("filtered-lazy","Filtered quantity",0);filtered.columns={{"name",{},{},false},{"measurement","Filtered quantity","mm3",true}};
    auto* owner=QThread::currentThread();std::atomic<ReportRowId> filterReads{0},ownerReads{0},unmatchedExportReads{0};
    filtered.table=std::make_shared<ReportTable>(12,[&](ReportRowId row){if(QThread::currentThread()==owner)++ownerReads;else ++filterReads;const bool match=row==2||row==7||row==11;if(!match)++unmatchedExportReads;return QStringList{match?QString("match-%1").arg(row):QString("skip-%1").arg(row),QString::number(row*2)};});
    ResultReport filterView;filterView.AddReport(filtered);filterView.SelectReport(filtered.id);filterReads=0;ownerReads=0;filterView.findChild<QLineEdit*>("reportSearch")->setText("match-");
    Wait([&]{return filterView.property("reportRowsBusy").toBool();});
    Require(filterView.property("reportRowsError").toString().isEmpty()&&filterView.GetFilteredCount()==3&&filterReads==12,
        "filtered export fixture did not complete the full-table filter scan");
    const auto hits=filterView.GetFilteredRows();Require(hits.size()==3&&hits[0]==2&&hits[1]==7&&hits[2]==11,"filter scan did not retain original matching row identities");
    filterReads=0;ownerReads=0;unmatchedExportReads=0;const auto filteredPath=directory.filePath("filtered-lazy.csv");ReportTaskTestAccess::ChooseExportPath(filterView,[&](const QString&){return filteredPath;});
    filterView.findChild<QAction*>("exportFilteredReport")->trigger();Wait([&]{return filterView.property("reportTaskBusy").toBool();});
    Require(filterReads==3&&ownerReads==0&&unmatchedExportReads==0,"filtered export reread unselected source rows after the full filter scan");
    QFile filteredFile(filteredPath);Require(filteredFile.open(QIODevice::ReadOnly),"filtered lazy output unreadable");QBuffer filteredBuffer;filteredBuffer.setData(filteredFile.readAll());filteredBuffer.open(QIODevice::ReadOnly);
    const auto filteredCopy=ReadReportCsv(filteredBuffer,"filtered lazy export");Require(filteredCopy.table->GetCount()==3,"filtered lazy export row count changed");
    for(ReportRowId i=0;i<3;++i){const auto original=hits[static_cast<int>(i)];Require(SameCells(filteredCopy.table->Read(i),QStringList{QString("match-%1").arg(original),QString::number(original*2)}),"filtered lazy export changed hit order or value");}
}
void CheckReportNumericLocales() {
    const QLocale system=QLocale::system(),current=QLocale();const auto zero=current.zeroDigit();
    std::cout<<"REPORT_LOCALE system="<<system.name().toStdString()<<" default="<<current.name().toStdString()
        <<" zeroDigitU+"<<QString::number(zero.unicode(),16).toStdString()<<" zeroDigitIsDigit="<<zero.isDigit()<<std::endl;
    QLocale safe=current;const bool fallback=!safe.zeroDigit().isDigit();if(fallback)safe=QLocale::c();safe.setNumberOptions(QLocale::OmitGroupSeparator);
    if(fallback)std::cout<<"REPORT_LOCALE_FALLBACK applied=C because system zeroDigit is not a digit"<<std::endl;
    ResultReport progressView;auto* progress=progressView.findChild<QProgressBar*>("reportExportProgress");
    Require(progress!=nullptr,"report export progress bar is missing");progress->setRange(0,1000);progress->setValue(500);
    Require(progress->locale().name()==QLocale::c().name()&&progress->locale().zeroDigit()==QLocale::c().zeroDigit()&&progress->text()=="50%",
        "export progress bar did not use ASCII-digit C locale text");

    auto baseline=Document("locale-baseline","metric",1),currentDoc=Document("locale-current","metric",1);
    baseline.title="Baseline";currentDoc.title="Current";
    ResultReport compareView;compareView.AddReport(baseline);compareView.AddReport(currentDoc);
    Require(compareView.SelectReport(baseline.id),"locale comparison baseline selection failed");compareView.findChild<QAction*>("setReportBaseline")->trigger();
    Require(compareView.SelectReport(currentDoc.id),"locale comparison current selection failed");compareView.show();
    bool popupSeen=false;DrivePopupInteraction([&]{ReportTaskTestAccess::Compare(compareView);},[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog||dialog->parentWidget()!=&compareView||dialog->windowTitle()!="核对比较指标、单位与匹配依据")return false;
        popupSeen=true;auto* distance=dialog->findChild<QDoubleSpinBox*>("reportDistanceTolerance");auto* tolerance=dialog->findChild<QDoubleSpinBox*>("reportValueTolerance");
        Require(distance&&tolerance,"comparison numeric tolerance fields are missing or unnamed");
        const auto localeMatches=[&](QDoubleSpinBox* box){return box->locale().name()==safe.name()&&box->locale().zeroDigit()==safe.zeroDigit();};
        Require(localeMatches(distance)&&localeMatches(tolerance),"comparison tolerance fields do not follow the ParameterEditor safe-locale fallback");
        distance->setValue(.01);tolerance->setValue(.125);
        Require(distance->value()==.01&&tolerance->value()==.125,"setting comparison tolerances lost exact values");
        Require(distance->text()==safe.toString(.01,'f',9)&&tolerance->text()==safe.toString(.125,'f',9),"comparison tolerance text does not use exact safe-locale nine-decimal formatting");
        std::cout<<"REPORT_NUMERIC_CONTROLS progress="<<progress->text().toStdString()<<" distance="<<distance->text().toStdString()
            <<" tolerance="<<tolerance->text().toStdString()<<" safeLocale="<<safe.name().toStdString()<<std::endl;
        dialog->reject();return true;
    },"comparison locale popup was not inspected");
    Require(popupSeen&&!ReportTaskTestAccess::HasTask(compareView)&&!compareView.property("reportTaskBusy").toBool(),"rejected locale comparison admitted a task");
    Require(QLocale()==current&&QLocale::system().name()==system.name(),"locale regression changed process default or system locale");
    compareView.hide();
}
void CheckBoundedReportStop() {
    for(const bool scan:{false,true}) {
    ResultReport view;
    auto release=std::make_shared<std::promise<void>>();const auto ready=release->get_future().share();
    auto entered=std::make_shared<std::atomic<bool>>(false);
    if(scan) {
        auto doc=Document("stop-scan","metric",0);auto* owner=QThread::currentThread();
        doc.table=std::make_shared<ReportTable>(500,[entered,ready,owner](ReportRowId row){
            if(row==0&&QThread::currentThread()!=owner){entered->store(true);
                if(ready.wait_for(std::chrono::milliseconds(reportWaitTimeoutMs))!=std::future_status::ready)throw std::runtime_error("stop scan fixture guard");}
            return QStringList{QString::number(row),"1"};
        });view.AddReport(doc);view.findChild<QLineEdit*>("reportSearch")->setText("1");
        Wait([&]{return !entered->load();});
        // 同时保留一个等待旧扫描的 task；停止不能为两个 future 分别等待。
        ReportTaskTestAccess::Start(view,[](const ReportCheckpoint& check)->ReportDocument{check();return {};});
    } else {
    ReportTaskTestAccess::Start(view,[entered,ready](const ReportCheckpoint& check)->ReportDocument {
        entered->store(true);
        if(ready.wait_for(std::chrono::milliseconds(reportWaitTimeoutMs))!=std::future_status::ready)throw std::runtime_error("stop fixture release guard");
        check();return {};
    });
    Wait([&]{return !entered->load();});
    }
    std::promise<void> returned;auto completed=returned.get_future();
    auto watchdog=std::async(std::launch::async,[&]{
        const bool beforeRelease=completed.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        release->set_value();return beforeRelease;
    });
    QElapsedTimer elapsed;elapsed.start();const bool stopped=view.CancelTasks(true);const auto stopMs=elapsed.elapsed();returned.set_value();
    const bool beforeRelease=watchdog.get();
    std::cout<<"REPORT_STOP blockedWorker=true scan="<<scan<<" returnedBeforeRelease="<<beforeRelease<<" elapsedMs="<<stopMs<<std::endl;
    Wait([&]{return ReportTaskTestAccess::HasTask(view)||ReportTaskTestAccess::HasScan(view);});
    Require(beforeRelease,"final report stop waited for a blocked reader instead of retaining it for retry");
    Require(!stopped&&view.CancelTasks(true),"pending report stop reported success or could not be retried");
    view.show();bool rejected=false;
    try{ReportTaskTestAccess::Start(view,[](const ReportCheckpoint&)->ReportDocument{return {};});}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"showing a finally stopped report reopened task admission");
    }
}
void CheckReportStop(TestWindow& window) {
    auto* view=dynamic_cast<ResultReport*>(window.findChild<QWidget*>("resultReport"));
    Require(view!=nullptr,"host report stop fixture has no report owner");
    window.GetWorkflow().onShowReports({},{});QCoreApplication::processEvents();
    Require(view->isVisible(),"host report reopen fixture must exercise a visible widget's showEvent");
    Wait([&]{return view->GetGenerationPending();});
    std::promise<void> release;const auto ready=release.get_future().share();
    struct ReleaseOnExit final {std::promise<void>& release;bool opened=false;~ReleaseOnExit(){if(!opened)release.set_value();}} unblock{release};
    auto entered=std::make_shared<std::atomic<bool>>(false);auto source=std::make_shared<const int>(42);std::weak_ptr<const int> lease=source;
    ReportTaskTestAccess::Start(*view,[entered,ready,source](const ReportCheckpoint& check)->ReportDocument {
        entered->store(true);
        if(ready.wait_for(std::chrono::milliseconds(reportWaitTimeoutMs))!=std::future_status::ready)throw std::runtime_error("host stop fixture release guard");
        check();return {};
    });source.reset();Wait([&]{return !entered->load();});
    const auto session=window.GetSession();const auto reports=view->GetReportIds();
    QElapsedTimer clock;clock.start();
    Require(!window.StopSession()&&!window.StopSession(),"host stop reported completion while its report reader was blocked");
    const auto stopMs=clock.elapsed();
    Require(!window.close()&&window.isVisible(),"host close destroyed or hid the owner of an unfinished report reader");
    QCoreApplication::processEvents();
    Require(window.GetSession()==session&&window.GetIsReady()&&view->GetReportIds()==reports&&!lease.expired(),
        "pending report stop released Host, reports or the reader source");
    view->hide();window.GetWorkflow().onShowReports({},{});window.GetWorkflow().onShowBusinessReport({},{});
    Require(!view->isVisible(),"host stopping callbacks reopened its report dock");
    view->show();bool rejected=false;
    try{ReportTaskTestAccess::Start(*view,[](const ReportCheckpoint&)->ReportDocument{return {};});}catch(const std::runtime_error&){rejected=true;}
    Require(rejected,"showing a report during host stop reopened task admission");
    release.set_value();unblock.opened=true;Wait([&]{return ReportTaskTestAccess::HasTask(*view);});
    Require(lease.expired()&&view->CancelTasks(true),"completed host report task retained its source or could not stop");
    std::cout<<"PASS: host report Stop/close return pending before reader release, retain owner and source, and reject reopening; twoStopCallsMs="<<stopMs<<std::endl;
}
void CheckReportRepairs() {
    CheckBoundedReportStop();
    CheckSemanticRoundtrip();CheckWorkerStartupAndRecovery();CheckReopenedGeneration(false);CheckReopenedGeneration(true);CheckComparisonGenerationAdmission();CheckNonblockingRowLifecycle();CheckAssociationsAndClearing();CheckSpatialEvidence();CheckGenerationAndTasks();CheckFailureIsolation();CheckVolumeCompleteness();CheckConcurrentReportReads();CheckWideRowAddressing();CheckDiskIndexPages();CheckIndexReadRecovery();CheckPageReadFailures();CheckSlowScanCancellation();CheckFileExports();CheckExportLifetimes();CheckSelectedUnmaterializedExports();CheckReportNumericLocales();
#if defined(MANUAL_ALIGNMENT)
    CheckFrozenAlignmentParameters();CheckInspectionPreparation();
#endif
    std::cout<<"PASS: report semantics, associations, spatial evidence, lazy generation, cancellation and resource release"<<std::endl;
}
}
