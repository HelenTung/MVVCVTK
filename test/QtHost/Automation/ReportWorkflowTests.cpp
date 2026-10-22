#include "ReportWorkflowTests.h"
#include "TestCaseRunner.h"
#include "ReportLargeTests.h"
#include "App/TestWindow.h"
#include "Modules/ModulePanel.h"
#include "Support/ResultReport.h"
#include "Support/ReportComparison.h"
#include "Support/ReportStorage.h"
#include "Support/JsonInput.h"
#if defined(MANUAL_SURFACE)
#include "Support/SurfaceRead.h"
#endif
#include <QBuffer>
#include <QMouseEvent>
#include <vector>
#include <QFile>
#include <QTemporaryDir>
#include <QTableView>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QTreeWidget>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QHeaderView>
#include <QTimer>
#include <QDialog>
#include <QDockWidget>
#include <QComboBox>
#include <QApplication>
#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QScrollArea>
#include <QScrollBar>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTreeWidgetItemIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <array>
#include "Support/ProcessMeasurements.h"
#include <future>
#include <atomic>
#include <cmath>
namespace Manual {
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool SameRow(const QStringList& a,const QStringList& b){return a.size()==b.size() && std::equal(a.cbegin(),a.cend(),b.cbegin());}
bool SameIndices(const ReportRows& rows,std::initializer_list<int> expected){return std::size_t(rows.size())==expected.size() && std::equal(rows.begin(),rows.end(),expected.begin());}
ReportDocument Read(QByteArray bytes){QBuffer input(&bytes);input.open(QIODevice::ReadOnly);return ReadReportCsv(input,"测试报告");}
template<class F> void Reject(F action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}Require(rejected,message);}
std::array<std::uint64_t,4> GetGapReportCounts(const QJsonObject& record,const QJsonValue& history,const ReportDocument& document)
{
    const auto require=[](bool value,const QString& message){Require(value,message.toUtf8().constData());};
    const auto readRef=[](const QJsonValue& value,const QString& field) {
        try{return GetRef(value);}
        catch(const std::invalid_argument& error){throw std::runtime_error(("Gap report "+field+": "+QString::fromUtf8(error.what())).toStdString());}
    };
    const auto readId=[](const QJsonValue& value,const QString& field) {
        try{return GetId(value);}
        catch(const std::invalid_argument& error){throw std::runtime_error(("Gap report "+field+": "+QString::fromUtf8(error.what())).toStdString());}
    };
    Require(document.module=="Gap" && document.id.startsWith("gap:"),"Gap report requires an exact native gap document");
    const auto table=readRef(document.id.mid(4),"document.table");
    const auto resultSet=readRef(document.result,"document.resultSet"),source=readRef(document.source,"document.source");
    require(readRef(document.identityResult,"document.identityResult")==resultSet,"Gap report document resultSet and reader identity disagree");
    const auto sameRef=[](const QJsonValue& value,const DataRevisionRef& expected) {
        try{return GetRef(value)==expected;}catch(const std::invalid_argument&){return false;}
    };
    require(std::any_of(document.relatedResults.cbegin(),document.relatedResults.cend(),[&](const auto& value){return sameRef(value,table);}),
        "Gap report document does not retain its exact native table");
    Require(record["module"]=="Gap" && record["result"].isObject(),"Gap report action has no result object");
    Require(history.isArray(),"Gap report completion history is unavailable or is not an array");
    const auto action=record["action"].toString();const bool direct=action=="Start" || action=="Evaluate";
    const auto result=record["result"].toObject();
    if(!direct) {
        require(readRef(result["resultSet"],"action.resultSet")==resultSet,"Gap report action and frozen document resultSet disagree");
        if(action=="Defects" || result.contains("table"))require(readRef(result["table"],"action.table")==table,"Gap report action and frozen document table disagree");
        if(result.contains("source"))require(readRef(result["source"],"action.source")==source,"Gap report action and frozen document source disagree");
    }
    // 查询没有 core 统计；只接受同一冻结三元组的成功完成事实，不能借当前 UI 或目录重算期望。
    auto operations=history.toArray();if(direct)operations.prepend(record);
    QJsonObject matchedOperations;std::array<std::uint64_t,4> counts{};bool found=false;
    const std::array<const char*,4> keys{"voidCount","voidVoxels","boundaryUncertainCount","boundaryUncertainVoxels"};
    const auto completion=[](const QJsonObject& value) {
        return QJsonObject{{"module",value["module"]},{"action",value["action"]},{"status",value["status"]},
            {"isTerminal",value["isTerminal"]},{"completeCount",value["completeCount"]},{"result",value["result"]}};
    };
    for(int index=0;index<operations.size();++index) {
        const auto candidate=operations[index].toObject();const auto candidateAction=candidate["action"].toString();
        const bool successful=candidate["status"]=="Succeeded" || candidate["status"]=="SucceededWithDisplayFailure";
        const bool original=direct && index==0;
        if(!original && (candidate["module"]!="Gap" || (candidateAction!="Start" && candidateAction!="Evaluate") || !successful))continue;
        const auto core=candidate["result"].toObject();
        if(!original && !sameRef(core["voidTable"],table) && !sameRef(core["resultSet"],resultSet))continue;
        const auto operation=candidate["operationId"].toString();
        require(successful && candidate["isTerminal"].isBool() && candidate["isTerminal"].toBool()
            && candidate["completeCount"].isDouble() && candidate["completeCount"].toDouble()==1.,
            "Gap report producer "+operation+" must be a successful terminal completion exactly once");
        require(readRef(core["voidTable"],"producer "+operation+".voidTable")==table
            && readRef(core["resultSet"],"producer "+operation+".resultSet")==resultSet
            && readRef(core["source"],"producer "+operation+".source")==source,
            "Gap report producer "+operation+" conflicts with the frozen table/resultSet/source");
        const auto id=readId(candidate["operationId"],"producer.operationId");require(id!=0,"Gap report producer operationId must be nonzero");
        std::array<std::uint64_t,4> parsed;
        for(std::size_t i=0;i<keys.size();++i)parsed[i]=readId(core.value(keys[i]),"producer "+operation+"."+keys[i]);
        require(!found || parsed==counts,"Gap report has conflicting statistics for the same table/resultSet/source at operation "+operation);
        counts=parsed;found=true;
        const auto key=QString::number(id);const auto value=completion(candidate);
        require(!matchedOperations.contains(key) || matchedOperations.value(key)==QJsonValue(value),"Gap report has contradictory snapshots for producer operation "+key);
        matchedOperations.insert(key,value);
    }
    // 同一 operation 的另一份矛盾快照即使声称不同结果，也不能作为“无关新结果”漏掉。
    for(const auto value:operations) {
        const auto candidate=value.toObject();QString key;
        try{key=QString::number(GetId(candidate["operationId"]));}catch(const std::invalid_argument&){continue;}
        if(matchedOperations.contains(key))require(matchedOperations.value(key)==QJsonValue(completion(candidate)),
            "Gap report has contradictory snapshots for producer operation "+key);
    }
    require(found,"Gap report has no successful Start/Evaluate producer for "+document.id+" / "+document.result+" / "+document.source);
    return counts;
}
void CheckGapReportCounts()
{
    const QString source="00000000000000000000000000000001:1",otherSource="00000000000000000000000000000006:1";
    const QString tableA="00000000000000000000000000000002:1",resultA="00000000000000000000000000000003:1";
    const QString tableB="00000000000000000000000000000004:1",resultB="00000000000000000000000000000005:1";
    const QJsonObject coreA{{"voidTable",tableA},{"resultSet",resultA},{"source",source},
        {"voidCount","2"},{"voidVoxels","16"},{"boundaryUncertainCount","1"},{"boundaryUncertainVoxels","27"}};
    auto coreB=coreA;coreB["voidTable"]=tableB;coreB["resultSet"]=resultB;coreB["voidCount"]="0";coreB["voidVoxels"]="0";
    const QJsonObject a{{"module","Gap"},{"action","Start"},{"operationId","1"},{"status","Succeeded"},
        {"isTerminal",true},{"completeCount",1},{"result",coreA}};
    auto b=a;b["operationId"]="2";b["action"]="Evaluate";b["result"]=coreB;
    const QJsonObject query{{"module","Gap"},{"action","Defects"},{"operationId","3"},{"status","Observed"},
        {"isTerminal",true},{"completeCount",1},{"result",QJsonObject{{"table",tableA},{"resultSet",resultA}}}};
    ReportDocument doc;doc.module="Gap";doc.id="gap:"+tableA;doc.result=doc.identityResult=resultA;doc.source=source;doc.relatedResults=QStringList{tableA};
    const std::array<std::uint64_t,4> expectedA{2,16,1,27},expectedB{0,0,1,27};
    Require(GetGapReportCounts(query,QJsonArray{a,b},doc)==expectedA && GetGapReportCounts(query,QJsonArray{b,a},doc)==expectedA,
        "old Gap report statistics are independent of the current result and history order");
    auto newQuery=query;newQuery["result"]=QJsonObject{{"table",tableB},{"resultSet",resultB}};
    auto newDoc=doc;newDoc.id="gap:"+tableB;newDoc.result=newDoc.identityResult=resultB;newDoc.relatedResults=QStringList{tableB};
    Require(GetGapReportCounts(newQuery,QJsonArray{a,b},newDoc)==expectedB,"new Gap report retains its distinct producer statistics");
    Require(GetGapReportCounts(b,QJsonArray{a,b},newDoc)==expectedB,"direct Evaluate keeps its own successful statistics");
    auto repeated=a;repeated["operationId"]="4";repeated["action"]="Evaluate";repeated["status"]="SucceededWithDisplayFailure";
    Require(GetGapReportCounts(a,QJsonArray{a,a,repeated,b},doc)==expectedA,"identical direct/history copies and consistent producers may agree without double counting");
    auto maximum=coreA;
    const QStringList fields{"voidCount","voidVoxels","boundaryUncertainCount","boundaryUncertainVoxels"};
    for(const auto& field:fields)maximum[field]="18446744073709551615";
    auto maximumProducer=a;maximumProducer["result"]=maximum;
    const auto max=std::numeric_limits<std::uint64_t>::max();
    Require(GetGapReportCounts(query,QJsonArray{maximumProducer},doc)==std::array<std::uint64_t,4>{max,max,max,max},"Gap counts preserve the maximum uint64 decimal string");
    Reject([&]{GetGapReportCounts(query,QJsonValue(),doc);},"missing Gap history must reject");
    Reject([&]{GetGapReportCounts(query,QJsonArray{},doc);},"missing exact Gap producer must reject");
    Reject([&]{GetGapReportCounts(query,QJsonArray{b},doc);},"unrelated new Gap producer must not replace old history");
    auto badQuery=query;badQuery["result"]=QJsonObject{{"table",tableB},{"resultSet",resultA}};
    Reject([&]{GetGapReportCounts(badQuery,QJsonArray{a},doc);},"Gap query table mismatch must reject");
    badQuery["result"]=QJsonObject{{"table",tableA},{"resultSet",resultB}};
    Reject([&]{GetGapReportCounts(badQuery,QJsonArray{a},doc);},"Gap query resultSet mismatch must reject");
    badQuery["result"]=QJsonObject{{"resultSet",resultA}};
    Reject([&]{GetGapReportCounts(badQuery,QJsonArray{a},doc);},"Gap query missing table must reject");
    badQuery["result"]=QJsonObject{{"table",tableA}};
    Reject([&]{GetGapReportCounts(badQuery,QJsonArray{a},doc);},"Gap query missing resultSet must reject");
    auto badDoc=doc;badDoc.source.clear();
    Reject([&]{GetGapReportCounts(query,QJsonArray{a},badDoc);},"Gap document missing source must reject");
    badDoc=doc;badDoc.id="gap:not-a-ref";
    Reject([&]{GetGapReportCounts(query,QJsonArray{a},badDoc);},"Gap document malformed table reference must reject");
    badDoc=doc;badDoc.identityResult=resultB;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a},badDoc);},"Gap document reader identity mismatch must reject");
    auto bad=a;bad["status"]="Failed";
    Reject([&]{GetGapReportCounts(query,QJsonArray{bad},doc);},"failed Gap operation is not a statistics producer");
    bad["operationId"]="5";
    Require(GetGapReportCounts(query,QJsonArray{bad,a},doc)==expectedA,"unrelated failed attempts do not replace a successful Gap producer");
    bad=a;bad["isTerminal"]=false;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"nonterminal matching Gap producer must reject");
    bad=a;bad["completeCount"]=2;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"duplicate Gap completion must reject even beside a valid copy");
    auto conflict=coreA;conflict["source"]=otherSource;bad=a;bad["result"]=conflict;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"same Gap table/resultSet cannot claim another source");
    conflict=coreA;conflict.remove("source");bad["result"]=conflict;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"matching Gap producer missing source must reject");
    conflict=coreA;conflict["resultSet"]=resultB;bad["result"]=conflict;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"same Gap table cannot claim another resultSet");
    conflict=coreA;conflict["voidTable"]=tableB;bad["result"]=conflict;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"same Gap resultSet cannot claim another table");
    bad=b;bad["operationId"]="1";
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"same operation with another result is contradictory rather than unrelated");
    conflict=coreA;conflict["voidCount"]="3";bad=repeated;bad["result"]=conflict;
    Reject([&]{GetGapReportCounts(query,QJsonArray{a,bad},doc);},"conflicting counts for the same Gap identity must reject");
    const std::array<QJsonValue,6> invalidValues{QJsonValue(QJsonValue::Undefined),QJsonValue(QJsonValue::Null),QJsonValue(2),QJsonValue(true),
        QJsonValue(QString("not-a-u64")),QJsonValue(QString("18446744073709551616"))};
    for(const auto& field:fields)for(const auto invalid:invalidValues) {
        auto malformed=coreA;if(invalid.isUndefined())malformed.remove(field);else malformed[field]=invalid;
        auto producer=repeated;producer["result"]=malformed;
        Reject([&]{GetGapReportCounts(query,QJsonArray{producer},doc);},"malformed Gap statistics must reject without relying on a conflicting good copy");
        Reject([&]{GetGapReportCounts(query,QJsonArray{a,producer},doc);},"every malformed Gap statistics field must reject even beside a valid producer");
        producer["operationId"]="1";producer["action"]="Start";
        Reject([&]{GetGapReportCounts(producer,QJsonArray{a},doc);},"direct Gap producer must not borrow history to hide a malformed statistics field");
    }
    bad=a;bad["result"]=coreB;
    Reject([&]{GetGapReportCounts(bad,QJsonArray{a},doc);},"direct Gap producer must not borrow history to hide an identity mismatch");
}
ReportComparisonOptions Options(){ReportComparisonOptions o;o.baselineColumn=o.currentColumn=1;o.baselineIdentity=o.currentIdentity=0;o.baselineQuantity=o.currentQuantity="厚度";o.baselineUnit=o.currentUnit="mm";o.match=ReportMatch::Identity;o.identitiesConfirmed=true;return o;}
void WaitRows(ResultReport& view,int timeoutMs=0) {
    QElapsedTimer elapsed;elapsed.start();
    while(view.property("reportRowsBusy").toBool() && (timeoutMs<=0 || elapsed.elapsed()<timeoutMs))QCoreApplication::processEvents(QEventLoop::AllEvents,20);
    Require(!view.property("reportRowsBusy").toBool(),"report row processing did not finish within test deadline");
    if(!view.property("reportRowsError").toString().isEmpty())throw std::runtime_error(view.property("reportRowsError").toString().toStdString());
}
void WaitTasks(ResultReport& view,int timeoutMs=0,bool success=true) {
    QElapsedTimer elapsed;elapsed.start();
    while(view.property("reportTaskBusy").toBool() && (timeoutMs==0 || elapsed.elapsed()<timeoutMs))QCoreApplication::processEvents(QEventLoop::AllEvents,20);
    Require(!view.property("reportTaskBusy").toBool() && (!success || view.property("reportTaskError").toString().isEmpty()),"report task did not complete successfully");
}
void CheckVolumeColumns(ResultReport& view,const ReportDocument& native,const ReportDocument& copy,int timeoutMs) {
    Require(native.validityColumn==-1 && copy.validityColumn==-1,"volume completeness became a whole-row validity filter");
    for(int column=0;column<native.columns.size();++column)if(native.columns[column].numeric) {
        view.SelectReport(native.id);view.findChild<QAction*>("setReportBaseline")->trigger();view.SelectReport(copy.id);
        DrivePopupInteraction([&]{view.findChild<QAction*>("compareReports")->trigger();},[column,&view]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if(!dialog || dialog->parentWidget()!=&view || !dialog->findChild<QComboBox*>("reportMatchMode"))return false;
            dialog->findChild<QComboBox*>("reportBaselineValueColumn")->setCurrentIndex(column+1);
            dialog->findChild<QComboBox*>("reportCurrentValueColumn")->setCurrentIndex(column+1);dialog->accept();return true;
        },"volume column mapping dialog missing");
        WaitTasks(view,timeoutMs);
        auto compared=view.GetSelectedReport();const auto a=compared.table->Read(0),b=compared.table->Read(1);
        int valid=0;for(ReportRowId row=0;row<native.table->GetCount();++row){double value;if(GetReportNumber(GetReportCell(native.table->Read(row),column),value))++valid;}
        Require(a[2]==QString::number(valid) && b[2]==a[2] && b[3]==a[3] && b[4]==a[4] && b[5]==a[5],"real volume per-column count or statistics changed on automatic CSV mapping");
        view.RemoveReport(compared.id);
        std::cout<<"PASS: real volume column "<<native.columns[column].name.toStdString()<<" valid="<<valid<<std::endl;
    }
}
void SortRows(ResultReport& view,int column,int timeoutMs=0) {
    auto* table=view.findChild<QTableView*>("reportTable");
    QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Q_ARG(int,column));WaitRows(view,timeoutMs);
}
void CheckReadTaskCoordinator() {
    TestSession runtime;TestWorkflow workflow;TestRecordWriter records;
    ModulePanel panel({runtime,workflow,records},"Surface");
    const auto wait=[&]{QElapsedTimer time;time.start();while(panel.property("readTasksBusy").toBool()&&time.elapsed()<5000)QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        Require(!panel.property("readTasksBusy").toBool(),"read worker did not finish");};
    std::promise<void> started,resume;auto release=resume.get_future().share();bool published=false;
    const auto id=records.StartRecord("Surface","read",{},{});
    panel.StartReadTask(id,"Observed",[&](const ReadTaskContext&){started.set_value();release.wait();return QJsonObject{};},[&]{published=true;});
    const auto reached=started.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
    const auto accepted=panel.CancelReadTasks();resume.set_value();wait();
    Require(reached&&accepted==1&&!published&&records.GetRecord(id)["status"]=="Cancelled","accepted cancellation published a late read result");
    std::promise<void> committing,finish;auto finishCommit=finish.get_future().share();published=false;
    const auto exportId=records.StartRecord("Surface","export",{},{});
    panel.StartReadTask(exportId,"Exported",[&](const ReadTaskContext& context){
        context.commit([&]{committing.set_value();finishCommit.wait();});return QJsonObject{};
    },[&]{published=true;});
    const auto began=committing.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready;
    const auto late=panel.CancelReadTasks();finish.set_value();wait();
    Require(began&&late==0&&published&&records.GetRecord(exportId)["status"]=="Exported","commit boundary accepted late cancellation or misreported completed export");
    const auto failId=records.StartRecord("Surface","failed-read",{},{});
    panel.StartReadTask(failId,"Observed",[](const ReadTaskContext&)->QJsonObject{throw std::runtime_error("read IO failure");});wait();
    Require(records.GetRecord(failId)["status"]=="Failed","read failure reported as success");
}
void CheckSurfaceReportTarget() {
    TestSession runtime;TestWorkflow workflow;TestRecordWriter records;
    const QString mesh="11111111111111111111111111111111:1",first="22222222222222222222222222222222:1",second="33333333333333333333333333333333:1";
    QSet<QString> available{mesh,first,second};QString opened;
    workflow.readReports["Surface"]=[](const auto&,const auto&){return QVector<ReportDocument>{};};
    workflow.getReportAvailable=[&](const auto& module,const auto& ref){return module=="Surface" && available.contains(ref);};
    workflow.onShowBusinessReport=[&](const auto&,const auto& ref){opened=ref;};
    ModulePanel panel({runtime,workflow,records},"Surface");
    auto* button=panel.findChild<QPushButton*>("viewBusinessReport");Require(button!=nullptr,"surface report button exists");
    const auto complete=[&](const QString& generation){const auto id=records.StartRecord("Surface","ReadObjects",{},{});panel.SetComplete(id,"Observed",{{"mesh",mesh},{"generation",generation}});};
    complete(second);button->click();
    Require(opened==second && button->property("reportResult")==second,"shared mesh surface report button targets the exact new generation");
    complete(first);button->click();
    Require(opened==first && button->property("reportResult")==first,"shared mesh surface report button can return to an old generation");
    available.remove(first);complete(first);
    Require(button->isHidden() && button->property("reportResult").toString().isEmpty(),"unavailable exact surface report must not fall back to another generation on the shared mesh");
}
void CheckReportGenerationSelection() {
    ResultReport view;const QString result="11111111111111111111111111111111:1";
    ReportDocument first;first.id="generation-first";first.result=result;first.module="Surface";first.category="本次会话结果";first.columns={{"value",{},{},true}};
    first.table=std::make_shared<ReportTable>(1,[](quint64){return QStringList{"1"};});
    auto second=first;second.id="generation-second";
    view.AddReport(first);view.AddReport(second);view.ShowReports({},result,"Surface");view.show();
    int calls=0;
    const auto wait=[&](const auto& done){QElapsedTimer elapsed;elapsed.start();while(!done()&&elapsed.elapsed()<30000)QCoreApplication::processEvents(QEventLoop::AllEvents,20);Require(done(),"report generation selection regression did not finish");};
    view.onReadReports=[&](const auto&,const auto&){++calls;return QVector<ReportDocument>{};};
    const auto operation=[&](const QString& id){return QJsonObject{{"operationId",id},{"module","Surface"},{"result",QJsonObject{{"mesh",result}}}};};
    view.QueueReport(operation("empty-generation"));view.SelectReport(second.id);
    wait([&]{return calls>0&&!view.property("reportGenerationBusy").toBool();});
    Require(view.GetSelectedReport().id==second.id,"empty queued generation overwrote the selected frozen report");
    auto* follow=view.findChild<QCheckBox*>("followLatestReport");Require(follow!=nullptr,"report follow control is available");
    follow->setChecked(true);view.SelectReport(first.id);
    bool picked=false;int rows=0;auto late=first;late.id="generation-late";late.table=std::make_shared<ReportTable>(50000,[&](quint64 row){++rows;if(row==0)QTimer::singleShot(0,&view,[&]{picked=true;view.SelectReport(second.id);});return QStringList{QString::number(row)};});
    view.onReadReports=[late](const auto&,const auto&){return QVector<ReportDocument>{late};};
    view.QueueReport(operation("late-generation"));
    wait([&]{return picked&&!view.property("reportGenerationBusy").toBool();});
    Require(rows>0&&rows<50000,"opening a native report materialized the complete table instead of the visible page");
    QTemporaryDir exportDirectory;view.SelectReport(late.id);view.ExportReportFile(exportDirectory.filePath("native.csv"));view.SelectReport(second.id);WaitTasks(view);
    Require(late.table->GetIsComplete()&&late.table->GetCount()==50000,"explicit export did not prepare the complete native report");
    Require(view.GetSelectedReport().id==second.id&&view.GetReportIds().contains(late.id),"late generation lost the full document or replaced a newer user selection");
    for(const bool enabled:{false,true}) {
        follow->setChecked(enabled);auto next=first;next.id=enabled?"generation-follow":"generation-preserve";
        view.onReadReports=[next](const auto&,const auto&){return QVector<ReportDocument>{next};};view.QueueReport(operation(next.id));
        wait([&]{return view.GetReportIds().contains(next.id)&&!view.property("reportGenerationBusy").toBool();});
        Require(view.GetSelectedReport().id==(enabled?next.id:second.id),"generation ignored the explicit report follow setting");
    }
    auto lazy=first;lazy.id="generation-lazy-native";lazy.table=std::make_shared<ReportTable>();
    const auto lazyReads=std::make_shared<std::size_t>(0);
    lazy.table->SetRows(50000,[lazyReads](ReportRowId row){++*lazyReads;return QStringList{QString::number(row)};});
    view.onReadReports=[lazy](const auto&,const auto&){return QVector<ReportDocument>{lazy};};
    CheckNativeReportCancellation(view,[&]{view.QueueReport(operation("lazy-native-generation"));},0);
    Require(view.GetReportIds().contains(lazy.id)&&!lazy.table->GetIsComplete()&&*lazyReads>0&&*lazyReads<50000,
        "native cancellation audit forced full materialization or rejected lazy publication");
    Require(SameRow(lazy.table->Read(49999),QStringList{"49999"})&&!lazy.table->GetIsComplete(),
        "lazy published native table lost its tail or materialized its full prefix");
}
#if defined(MANUAL_SURFACE)
void CheckSurfaceReads() {
    SurfaceGenerationSnapshot snapshot;snapshot.dataRevision=GetRef("00000000000000000000000000000001:17");
    auto points=std::make_shared<DataArray<SurfacePointRecord>>(4);
    (*points)[0].positionModel={0,0,0};(*points)[1].positionModel={1,0,0};(*points)[2].positionModel={0,1,0};(*points)[3].positionModel={0,0,1};
    for(auto& point:*points){point.validSupportRatio=1;point.estimatedLocalizationSigma=.1;point.fitResidual=.2;}
    (*points)[3].validSupportRatio=.1;snapshot.points=std::make_shared<const MeshReadArray<SurfacePointRecord>>(std::move(*points));
    snapshot.triangleIndices=std::make_shared<const MeshReadArray<std::uint32_t>>(DataArray<std::uint32_t>{0,1,2,0,1,3,0,0,1});
    snapshot.triangleValidity=std::make_shared<const MeshReadArray<std::uint8_t>>(DataArray<std::uint8_t>{1,1,1});
    const auto selection=ReadSurfaceMeasurement(snapshot,{});
    Require(selection->generation==snapshot.dataRevision&&selection->vertices==DataArray<std::uint64_t>({0,1,2})
        &&selection->triangles==DataArray<std::uint64_t>({0})&&selection->rejectedQuality==1&&selection->degenerate==1,"measurement selection changed quality semantics or original identities");
    Reject([&]{ReadSurfaceMeasurement(snapshot,{},[]{throw std::runtime_error("cancel");});},"measurement cancellation ignored");
    QTemporaryDir directory;const auto path=directory.filePath("surface.csv");
    ExportSurfaceComparison(path,snapshot);QFile input(path);Require(input.open(QIODevice::ReadOnly),"surface export unreadable");
    const auto original=input.readAll();input.close();
    Require(original.contains("point_id [mvvcvtk-result=00000000000000000000000000000001:17]")&&original.count('\n')==5,"surface export lost fixed revision or complete rows");
    int checks=0;Reject([&]{ExportSurfaceComparison(path,snapshot,[&]{if(++checks==3)throw std::runtime_error("cancel before commit");});},"surface export cancellation ignored");
    Require(input.open(QIODevice::ReadOnly)&&input.readAll()==original,"cancelled export replaced complete file");input.close();
    Reject([&]{ExportSurfaceComparison(directory.path(),snapshot);},"surface export write failure reported success");
    const std::array<double,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Require(ExportSurfaceFixture(directory.filePath("surface.stl"),snapshot,identity,false)==3,"fixture export omitted formal triangles");
}
#endif
void CheckMixedSortAndCancellation() {
    ResultReport view;
    QStringList values{"2","10","1x"};
    // 六种输入顺序都应得到同一数值/文本顺序，直接覆盖旧比较器的三项循环。
    std::sort(values.begin(),values.end());int sequence=0;
    do {
        ReportDocument document;document.id=QString("mixed-%1").arg(sequence++);document.title="混合键";document.category="导入报告";
        document.columns={{"value",{},{},true}};document.table=std::make_shared<ReportTable>();
        for(const auto& value:values)document.table->Append({value});view.AddReport(document);view.SelectReport(document.id);
        SortRows(view,0);auto rows=view.GetFilteredRows();QStringList sorted;for(const auto row:rows)sorted.append(document.table->Read(row).front());
        Require(SameRow(sorted,QStringList{"2","10","1x"}),"mixed numeric/text ascending order depends on input permutation");
        SortRows(view,0);sorted.clear();for(const auto row:view.GetFilteredRows())sorted.append(document.table->Read(row).front());
        Require(SameRow(sorted,QStringList{"1x","10","2"}),"mixed numeric/text descending order is inconsistent");
    } while(std::next_permutation(values.begin(),values.end()));
    auto ties=Read("value\n2\n02\n2\na\nA\nnan\ninf\n");
    // 空值用有明确定界的 CSV 行，避免把空行误当记录。
    ties.table->Append({""});view.AddReport(ties);view.SelectReport(ties.id);SortRows(view,0);
    Require(SameIndices(view.GetFilteredRows(),{1,0,2,7,3,4,6,5}),"numeric ties, text ties or nonfinite keys changed stable order");
    SortRows(view,0);Require(SameIndices(view.GetFilteredRows(),{5,6,3,4,7,0,2,1}),"descending equal keys did not retain original rows");

    ReportDocument large;large.id="responsive";large.title="响应性";large.category="本次会话结果";large.module="Gap";
    large.columns={{"id",{},{},false},{"value",{},{},true}};large.table=std::make_shared<ReportTable>();
    for(int i=0;i<50000;++i)large.table->Append({QString("row-%1").arg(i),QString::number(50000-i)});
    int located=-1;view.onLocate=[&](const ReportDocument& document,ReportRowId row){Require(document.id==large.id,"locate changed selected report identity");located=row;};
    view.AddReport(large);view.SelectReport(large.id);auto* table=view.findChild<QTableView*>("reportTable");
    QElapsedTimer elapsed;elapsed.start();QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Q_ARG(int,1));
    Require(view.property("reportRowsBusy").toBool() && view.GetFilteredRows().empty(),"sorting blocks admission or exposes partial rows");
    bool yielded=false;QTimer::singleShot(0,&view,[&]{yielded=view.property("reportRowsBusy").toBool();});WaitRows(view);
    Require(yielded && view.GetFilteredRows().size()==50000 && table->model()->data(table->model()->index(0,0)).toString()=="row-49999","complete sorting did not yield to owner events or lost rows");
    table->selectRow(0);view.findChild<QAction*>("locateReportRow")->trigger();Require(located==49999,"sorted page location did not map to original row");
    std::cout<<"Reports: 50000-row sort elapsedMs="<<elapsed.elapsed()<<std::endl;
    auto* search=view.findChild<QLineEdit*>("reportSearch");search->setText("row-");search->setText("row-49999");WaitRows(view);
    Require(SameIndices(view.GetFilteredRows(),{49999}),"superseded search committed stale rows");
    QTemporaryDir directory;const auto path=directory.filePath("filtered.csv");const auto filteredRows=view.GetFilteredRows();ExportReport(path,large,&filteredRows);
    QFile filteredInput(path);Require(filteredInput.open(QIODevice::ReadOnly),"filtered export unreadable");const auto filtered=ReadReportCsv(filteredInput,"筛选结果");
    Require(filtered.table->GetCount()==1 && SameRow(filtered.table->Read(0),large.table->Read(49999)),"filtered export lost original row identity or values");
    const auto asyncPath=directory.filePath("filtered-async.csv");view.ExportReportFile(asyncPath,true);view.SelectReport(ties.id);WaitTasks(view);
    QFile asyncInput(asyncPath);Require(asyncInput.open(QIODevice::ReadOnly),"asynchronous filtered export unreadable");const auto asyncCopy=ReadReportCsv(asyncInput,"筛选导出");
    Require(asyncCopy.table->GetCount()==1 && SameRow(asyncCopy.table->Read(0),large.table->Read(49999)),"selection change released or altered the disk-backed export index");
    view.SelectReport(large.id);
    search->setText("row-");bool cancelledWhileBusy=false;
    QTimer::singleShot(0,&view,[&]{cancelledWhileBusy=view.property("reportRowsBusy").toBool();view.findChild<QPushButton*>("cancelReportRows")->click();});WaitRows(view);
    Require(cancelledWhileBusy && view.GetFilteredRows().empty(),"cancelled scan published partial or late results");
    Require(!view.findChild<QAction*>("exportFilteredReport")->isEnabled() && !view.findChild<QPushButton*>("exportSelectedReport")->isEnabled(),"cancelled scan is presented as an exportable complete result");
    search->setText("row-4");view.SelectReport(ties.id);QCoreApplication::processEvents();
    Require(view.GetSelectedReport().id==ties.id && view.GetFilteredRows().size()==8,"old report scan overwrote a new selection");
    ReportDocument virtualRows;virtualRows.id="64-bit-native";virtualRows.title="64 位原生页";virtualRows.category="本次会话结果";
    virtualRows.columns={{"id",{},{},false}};virtualRows.table=std::make_shared<ReportTable>();
    const ReportRowId beyondInt=ReportRowId(std::numeric_limits<int>::max())+19;
    int reads=0;virtualRows.table->SetRows(static_cast<std::size_t>(beyondInt),[&](ReportRowId row){++reads;return QStringList{QString::number(row)};});
    view.AddReport(virtualRows);view.SelectReport(virtualRows.id);
    const auto implicit=view.GetFilteredRows();Require(implicit.size()==static_cast<std::size_t>(beyondInt) && implicit.back()==beyondInt-1 && reads<1000,"natural 64-bit range was capped or fully expanded");
    Require(virtualRows.table->Read(beyondInt-1).front()==QString::number(beyondInt-1),"native row callback narrowed the requested tail index");
    view.SelectReport(ties.id);
    {ResultReport pending;pending.AddReport(large);auto* pendingTable=pending.findChild<QTableView*>("reportTable");QMetaObject::invokeMethod(pendingTable->horizontalHeader(),"sectionClicked",Q_ARG(int,1));}
    QCoreApplication::processEvents();
}
void CheckExternalReportRuns() {
    bool supplied=false;const auto configured=qEnvironmentVariable("MVVCVTK_REPORT_STRESS_ROWS").toLongLong(&supplied);
    const ReportRowId count=supplied && configured>0?configured:120003;
    ReportDocument a;a.id="external-a";a.title=a.id;a.category="导入报告";a.columns={{"id",{},{},false},{"value",{},{},true}};a.table=std::make_shared<ReportTable>();
    auto b=a;b.id="external-b";b.title=b.id;b.table=std::make_shared<ReportTable>();
    QElapsedTimer elapsed;elapsed.start();
    for(ReportRowId row=0;row<count;++row){a.table->Append({QString::number(row),QString::number(count-row)});b.table->Append({QString::number(count-row-1),QString::number(row+1+.25,'g',17)});}
    ResultReport view;view.AddReport(a);view.SelectReport(a.id);SortRows(view,1,0);
    const auto rows=view.GetFilteredRows();Require(rows.size()==static_cast<std::size_t>(count) && rows.front()==count-1 && rows.back()==0,"external merge lost rows or changed numeric order");
    std::cout<<"Reports: external sort complete rows="<<count<<" elapsedMs="<<elapsed.elapsed()<<std::endl;
    const auto comparison=CompareReports(a,b,Options());
    Require(comparison.table->GetCount()==count && comparison.conditions["有效配对数"].toDouble()==double(count),"disk identity index lost uniquely matched rows");
    for(const auto row:std::array<ReportRowId,3>{0,count/2,count-1}) {
        const auto actual=comparison.table->Read(row);Require(actual[0]==QString::number(row) && actual[4]=="0.25","external ID merge changed exact value or original baseline order");
    }
    std::cout<<"Reports: external matching complete elapsedMs="<<elapsed.elapsed()<<std::endl;
    auto repeated=b;repeated.table=std::make_shared<ReportTable>();
    for(ReportRowId row=0;row<count;++row)repeated.table->Append({QString::number(count-row-1),QString::number(row+1+.25,'g',17)});
    repeated.table->Append({QString::number(count/2),"999"});const auto duplicate=CompareReports(a,repeated,Options());
    Require(duplicate.table->GetCount()==count+2 && duplicate.conditions["有效配对数"].toDouble()==double(count-1) && duplicate.table->Read(count/2)[4].isEmpty(),"disk duplicate group silently chose one current row");
    std::cout<<"Reports: external rows="<<count<<" elapsedMs="<<elapsed.elapsed()<<" memory="<<QJsonDocument(GetProcessMeasurements()).toJson(QJsonDocument::Compact).constData()<<std::endl;
}

}
void CheckReportWorkflow()
{
    CheckGapReportCounts();
    Require(GetReportCount(QJsonValue(4294967419.0))==4294967419ULL&&GetReportCount(QJsonValue(QString("18446744073709551615")))==UINT64_MAX,"native report counts preserve wide numeric and uint64 string forms");
    Reject([]{GetReportCount(QJsonValue(9007199254740992.0));},"unsafe JSON numeric count must not be rounded into an ID");
    std::cout<<"Reports: CSV parsing"<<std::endl;
    const auto special=Read(QByteArray("\xef\xbb\xbf")+"id,value,note\r\n9007199254740993,2.5,\"comma, quote\"\" and\r\nline\"\r\nempty,,\r\n");
    Require(special.table->GetCount()==2 && special.table->Read(0)[0]=="9007199254740993" && special.table->Read(0)[2]=="comma, quote\" and\r\nline" && special.table->Read(1)[1].isEmpty(),"CSV quoting, UTF-8 BOM, large IDs and missing values");
    QByteArray bytes;QBuffer output(&bytes);output.open(QIODevice::WriteOnly);WriteReportCsv(output,special);output.close();const auto roundtrip=Read(bytes);
    Require(SameRow(roundtrip.table->Read(0),special.table->Read(0)) && SameRow(roundtrip.table->Read(1),special.table->Read(1)),"CSV exact roundtrip");
    const auto longText=QString(65530,'a')+QString::fromUtf8("中文\"\r\n尾部");
    auto longDoc=special;longDoc.table=std::make_shared<ReportTable>();longDoc.table->Append({"long","1",longText});bytes.clear();output.open(QIODevice::WriteOnly);WriteReportCsv(output,longDoc);output.close();
    Require(Read(bytes).table->Read(0)[2]==longText,"CSV chunks preserve UTF-8, escaped quotes and embedded newlines");
    for(const auto& invalid:QVector<QByteArray>{QByteArray(),"\n","a,b\n1\n","a\n\"unterminated","a\n\"x\"bad","a\n\xff","a\n\xe4\xb8"})Reject([&]{Read(invalid);},"malformed CSV must reject");
    std::cout<<"Reports: matching and units"<<std::endl;
    auto a=Read("id,value,validity\na,1,0\nb,2,0\nc,,0\nd,9,2\ne,8,0\n");a.validityColumn=2;
    auto b=Read("id,value,validity\nb,0.003,0\na,0.002,0\nc,0.004,0\nd,0.005,0\nf,0.006,0\n");b.validityColumn=2;
    auto o=Options();o.currentUnit="m";const auto compared=CompareReports(a,b,o);
    Require(compared.conditions["有效配对数"]==2 && compared.conditions["无效/缺测配对数"]==2 && compared.conditions["基准未匹配数"]==1 && compared.conditions["当前未匹配数"]==1,"comparison counts reconcile");
    Require(compared.table->Read(0)[4]=="1" && compared.table->Read(2)[2].isEmpty() && compared.table->Read(2)[4].isEmpty() && compared.table->Read(3)[2].isEmpty(),"unit conversion and invalid values never become zero");
    auto shuffled=Read("id,value,validity\nf,0.006,0\nd,0.005,0\nc,0.004,0\na,0.002,0\nb,0.003,0\n");shuffled.validityColumn=2;
    const auto stable=CompareReports(a,shuffled,o);for(int i=0;i<compared.table->GetCount();++i)Require(SameRow(stable.table->Read(i),compared.table->Read(i)),"ID comparison independent of row order");
    auto duplicate=Read("id,value\na,2\na,4\n");const auto ambiguous=CompareReports(Read("id,value\na,1\n"),duplicate,Options());
    Require(ambiguous.conditions["有效配对数"]==0 && ambiguous.table->Read(0)[4].isEmpty() && ambiguous.table->GetCount()==3,"duplicates retain both sides and never pick first");
    const auto bom=QString(QChar(0xfeff));const auto longId=QString(255,'x')+QString::fromUtf8("𠮷")+"tail";
    const auto unicodeA=Read(("id,value\n"+bom+"a,1\na,2\n"+longId+",3\n").toUtf8());
    const auto unicodeB=Read(("id,value\n"+longId+",4\na,3\n"+bom+"a,2\n").toUtf8());
    const auto unicode=CompareReports(unicodeA,unicodeB,Options());
    Require(unicode.conditions["有效配对数"]==3 && unicode.table->Read(0)[0]==bom+"a" && unicode.table->Read(0)[4]=="1" && unicode.table->Read(2)[0]==longId,"disk keys changed BOM or cross-block UTF-16 identities");
    auto noTrust=Options();noTrust.identitiesConfirmed=false;Reject([&]{CompareReports(a,b,noTrust);},"local IDs require explicit stability confirmation");
    const auto objectRows=Read("object_id [mvvcvtk-result=11111111111111111111111111111111:1],volume_mm3\n0,1\n1,2\n");
    Require(objectRows.identityColumn==0 && objectRows.identityResult=="11111111111111111111111111111111:1","object CSV retains result-scoped identity");
    const auto pointRows=Read("object_id,point_id [mvvcvtk-result=11111111111111111111111111111111:1],x_mm\n0,0,1\n0,1,2\n");
    Require(pointRows.identityColumn==1 && pointRows.table->GetCount()==2,"point identity takes precedence over repeated object IDs");
    auto otherResult=a;otherResult.identityResult="result-b";auto originalResult=a;originalResult.identityResult="result-a";
    Reject([&]{CompareReports(originalResult,otherResult,Options());},"equal local IDs from different formal results never imply the same object");
    Reject([&]{CompareReports(originalResult,a,Options());},"missing identity context cannot match result-local IDs by coincidence");
    bytes.clear();output.open(QIODevice::WriteOnly);WriteReportCsv(output,objectRows);output.close();
    const auto scopedRoundtrip=Read(bytes);
    Require(scopedRoundtrip.identityResult==objectRows.identityResult&&SameRow(scopedRoundtrip.table->Read(1),objectRows.table->Read(1)),"result context and object IDs roundtrip together");
    const auto literalHeader=Read("#mvvcvtk_identity_result,11111111111111111111111111111111:1\n0,2\n");
    Require(literalHeader.identityResult.isEmpty()&&literalHeader.columns[0].name=="#mvvcvtk_identity_result", "ordinary CSV header is not consumed as native result metadata");
    Reject([&]{Read("object_id,volume_mm3 [mvvcvtk-result=11111111111111111111111111111111:1]\n0,1\n");},"scope annotation on a quantity cannot authenticate an unrelated identity column");
    auto largeNatural=GetReportRowRange((ReportRowId(1)<<32)+17);
    const auto wideChunk=largeNatural.GetChunk(std::size_t(1)<<32,3);const auto& widePage=wideChunk.values;
    Require(widePage.size()==3&&widePage[0]==(quint64(1)<<32)&&widePage[1]==(quint64(1)<<32)+1&&widePage[2]==(quint64(1)<<32)+2,"global report row positions exceed Qt page int without materializing all rows");
    {
        ReportDocument sorted;sorted.table=std::make_shared<ReportTable>();
        constexpr ReportRowId count=8192*16+1;
        for(ReportRowId i=0;i<count;++i)sorted.table->Append({QString::number(i),QString::number(i%33)});
        sorted.id="wide-sort";sorted.title="完整外排";sorted.category="导入报告";sorted.columns={{"id",{},{},false},{"value",{},{},true}};
        ResultReport sortedView;sortedView.AddReport(sorted);sortedView.SelectReport(sorted.id);SortRows(sortedView,1);auto rows=sortedView.GetFilteredRows();
        bool valid=rows.size()==static_cast<std::size_t>(count);ReportRowId previous=0;
        for(ReportRowId i=0;i<count;++i){const auto row=rows[static_cast<std::size_t>(i)];valid=valid&&row<count&&(!i||row%33>previous%33||(row%33==previous%33&&row>previous));previous=row;}
        Require(valid,"multi-pass external sorting preserves all rows, numeric order and stable equal-key order");
    }
    auto wrong=Options();wrong.currentQuantity="ROI 内相交体积";Reject([&]{CompareReports(a,b,wrong);},"semantic mismatch rejects");
    wrong=Options();wrong.currentUnit="deg";Reject([&]{CompareReports(a,b,wrong);},"angle/length mismatch rejects");
    const auto repeatedColumns=Read("sample_id,thickness_mm,validity,validity\na,1,2,0\n");
    Require(repeatedColumns.mappingRequired && repeatedColumns.validityColumn<0,"duplicate column mapping is never chosen silently");
    Reject([&]{CompareReports(repeatedColumns,repeatedColumns,Options());},"duplicate columns require explicit mapping confirmation");
    auto pa=Read("sample_id,x_mm,y_mm,z_mm,thickness_mm\n0,0,0,0,1\n1,2,0,0,2\n");
    auto pb=Read("sample_id,x_mm,y_mm,z_mm,thickness_mm\n0,2.001,0,0,3\n1,0.001,0,0,4\n");
    auto spatial=Options();spatial.baselineColumn=spatial.currentColumn=4;spatial.match=ReportMatch::Position;spatial.coordinatesConfirmed=true;spatial.baselineCoordinateUnit=spatial.currentCoordinateUnit="mm";spatial.distanceTolerance=.01;
    const auto space=CompareReports(pa,pb,spatial);Require(space.table->Read(0)[4]=="3" && space.table->Read(1)[4]=="1","spatial pairing uses position rather than sample order");
    auto multi=Read("sample_id,x_mm,y_mm,z_mm,thickness_mm\n0,0,0,0,2\n1,0.001,0,0,3\n");const auto multiple=CompareReports(pa,multi,spatial);Require(multiple.table->Read(0)[4].isEmpty(),"spatial ambiguity rejects delta");
    auto distribution=Options();distribution.match=ReportMatch::Distribution;const auto stats=CompareReports(a,a,distribution);Require(stats.table->GetCount()==2 && stats.table->Read(0)[2]=="3","distribution excludes missing/invalid measurements without fake pairs");
    std::cout<<"Reports: table selection and paging"<<std::endl;
    {
        quint64 largestRead=0;
        ReportDocument lazy;lazy.id="large-native-directory";lazy.title="按行读取目录";lazy.category="目录";
        lazy.columns={{"id",{},{},false},{"value","数值","1",true}};
        lazy.table=std::make_shared<ReportTable>((quint64(1)<<32)+7,[&](quint64 row){largestRead=std::max(largestRead,row);return QStringList{QString::number(row),"1"};});
        ResultReport lazyView;lazyView.AddReport(lazy);
        Require(lazyView.GetFilteredCount()==(quint64(1)<<32)+7&&largestRead<200,"opening a large native report reads only the current page without materializing all objects");
        lazyView.findChild<QPushButton*>("reportNext")->click();
        auto* lazyTable=lazyView.findChild<QTableView*>("reportTable");
        Require(lazyTable->model()->data(lazyTable->model()->index(0,0)).toString()=="200"&&largestRead>=200&&largestRead<400,"natural next page reads only its global row range");
        Require(lazy.table->Read(quint64(1)<<32)[0]==QString::number(quint64(1)<<32),"native report row addressing is not truncated to Qt int");
        lazyView.findChild<QPushButton*>("reportLast")->click();
        const auto lastOffset=((lazy.table->GetCount()-1)/200)*200;
        Require(lazyTable->model()->data(lazyTable->model()->index(0,0)).toString()==QString::number(lastOffset),"last page retains global 64-bit row indices");
        Reject([&]{lazy.table->Append({"new","2"});},"published native directory is immutable");
    }
    ResultReport view;ReportDocument large;large.id="large";large.title="大报告";large.category="导入报告";large.source="data-a";large.result="result-a";
    large.module="Wall";large.relatedResults.append("alignment-a");large.columns={{"id",{},{},false},{"value","数值","1",true}};large.table=std::make_shared<ReportTable>();
    for(int i=0;i<1203;++i)large.table->Append({QString::number(i),QString::number(1203-i)});
    std::cout<<"Reports: table stored"<<std::endl;
    view.AddReport(large);auto newer=large;newer.id="newer";newer.source="data-b";newer.result="result-b";view.AddReport(newer);
    Require(view.GetSelectedReport().id=="large","new report preserves explicit selection");
    Require(view.GetReports("Wall",{"result-a"}).size()==1 && view.GetReports("Wall",{"alignment-a"}).size()==2
        && view.GetReports("Wall",{"data-a"}).isEmpty() && view.GetReports("Gap",{"result-a"}).isEmpty()
        && view.GetReports("Wall",{}).isEmpty(),"report lookup freezes exact result and module identities");
    ResultReport scoped;auto foreign=large;foreign.id="foreign";foreign.module="Gap";scoped.AddReport(foreign);scoped.AddReport(large);
    scoped.ShowReports({},"result-a","Wall");Require(scoped.GetSelectedReport().id=="large","business report entry excludes another module with the same related result");
    std::cout<<"Reports: initial selection"<<std::endl;
    view.findChild<QCheckBox*>("followLatestReport")->setChecked(true);auto latest=large;latest.id="latest";view.AddReport(latest);Require(view.GetSelectedReport().id=="latest","follow latest is opt-in");
    view.findChild<QCheckBox*>("followLatestReport")->setChecked(false);view.SelectReport("large");view.show();QCoreApplication::processEvents();
    std::cout<<"Reports: table shown"<<std::endl;
    auto* table=view.findChild<QTableView*>("reportTable");Require(table && table->model()->rowCount()==200,"report uses bounded page model");
    Require(!table->horizontalHeader()->isSortIndicatorShown(),"native row order does not display a fake sorting arrow");
    for(int i=0;i<6;++i)view.findChild<QPushButton*>("reportNext")->click();
    Require(table->model()->rowCount()==3 && table->model()->data(table->model()->index(2,0)).toString()=="1202","complete report browses beyond 1000 rows");
    std::cout<<"Reports: paging complete"<<std::endl;
    SortRows(view,1);
    const auto sortedFirst=table->model()->data(table->model()->index(0,0)).toString();
    if(sortedFirst!="1202")throw std::runtime_error("numeric sorting covers the complete table, not the current page: actual="+sortedFirst.toStdString()+" count="+std::to_string(view.GetFilteredCount()));
    Require(table->horizontalHeader()->isSortIndicatorShown()&&table->horizontalHeader()->sortIndicatorSection()==1,"sorting indicator follows the actual selected sort column");
    view.findChild<QLineEdit*>("reportSearch")->setText("1202");WaitRows(view);Require(view.GetFilteredRows().size()==2,"search covers all rows, including unloaded pages");
    std::cout<<"Reports: sorting and search complete"<<std::endl;
    QTemporaryDir directory;const auto path=directory.filePath("报告.csv");ExportReport(path,large);std::cout<<"Reports: full export complete"<<std::endl;view.ImportReport(path);
    WaitTasks(view);std::cout<<"Reports: independent import complete"<<std::endl;
    Require(view.GetSelectedReport().table->GetCount()==1203,"independent import/export preserves entire report");
    Require(!table->horizontalHeader()->isSortIndicatorShown(),"selecting another report clears the old sort indicator");
    view.ShowReports("data-a","missing-result");Require(!view.GetSelectedReport().table,"missing exact result never leaves unrelated old details visible");
    std::cout<<"Reports: missing result cleared"<<std::endl;
    view.ShowReports("data-a","result-a");Require(view.GetSelectedReport().result=="result-a","data/result context selects exact report");
    view.ShowReports({},"alignment-a");Require(view.GetSelectedReport().relatedResults.contains("alignment-a"),"parent result context selects explicitly associated reports");
    std::cout<<"Reports: comparison dialog"<<std::endl;
    ResultReport compareView;
    auto nativeA=Read("sample_id,thickness_mm,validity\nA,1,0\nB,2,0\n"),nativeB=Read("sample_id,thickness_mm,validity\nB,3,0\nA,4,0\n");
    compareView.AddReport(nativeA);compareView.findChild<QAction*>("setReportBaseline")->trigger();compareView.AddReport(nativeB);compareView.SelectReport(nativeB.id);
    DrivePopupInteraction([&]{compareView.findChild<QAction*>("compareReports")->trigger();},[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog || dialog->parentWidget()!=&compareView || !dialog->findChild<QComboBox*>("reportMatchMode"))return false;
        dialog->findChild<QComboBox*>("reportMatchMode")->setCurrentIndex(1);dialog->findChild<QCheckBox*>("reportMatchConfirmed")->setChecked(true);dialog->accept();return true;
    },"comparison dialog opens");
    WaitTasks(compareView);
    Require(compareView.GetSelectedReport().category=="对比结果" && compareView.GetSelectedReport().table->Read(0)[4]=="3","actual report UI creates and selects comparison without input data");
    compareView.resize(1180,720);compareView.show();QCoreApplication::processEvents();compareView.grab().save(QDir::current().filePath("report-workflow.png"));compareView.hide();
    view.hide();CheckMixedSortAndCancellation();CheckExternalReportRuns();CheckReportRepairs();CheckReadTaskCoordinator();CheckSurfaceReportTarget();CheckReportGenerationSelection();
#if defined(MANUAL_SURFACE)
    CheckSurfaceReads();
#endif
    std::cout<<"PASS: Report CSV, units, matching, selection, full-table paging and responsive sorting"<<std::endl;
}
void CheckBusinessReport(TestWindow& window,const QJsonObject& record,bool checkExport,const QString& previewPath,int timeoutMs,bool checkLarge)
{
    const auto module=record["module"].toString(),action=record["action"].toString();
    const auto summary=record["result"].toObject();
    const auto hasPublishedResult=[&](const char* key){const auto ref=summary[key].toString();return !ref.isEmpty() && !ref.endsWith(":0");};
    const bool requiresReport=(module=="Wall" && (action=="Start" || action=="Result")) || (module=="Gap" && (action=="Start" || action=="Evaluate" || action=="Defects"))
        || (module=="Surface" && (action=="GlobalAutomatic" || action=="Determine")) || (module=="Artifact" && action=="Commit")
        || (module=="Alignment" && QStringList{"InspectGeometry","InspectSection","InspectPair","EvaluateSurface","EvaluateVoxelVolume"}.contains(action))
        || (module=="Alignment" && ((action=="DeviationResult" && hasPublishedResult("deviation"))
            || (action=="InspectionResult" && hasPublishedResult("inspection"))
            || (action=="VoxelVolumeResult" && hasPublishedResult("voxelVolume"))));
    const bool successful=QStringList{"Succeeded","SucceededWithDisplayFailure","FullyDetermined","Published","Observed"}.contains(record["status"].toString());
    const auto provider=window.GetWorkflow().readReports.find(module);
    if(provider==window.GetWorkflow().readReports.end()){Require(!requiresReport||!successful,"successful business module has a registered native report source");return;}
    if(!successful)return; // 导出、候选等终态不等于新增一份已发布业务报告。
    // 选择与编号分页仅返回局部元数据；正式快照读取才作为表面报告预览入口。
    if(module=="Surface" && !requiresReport && (!hasPublishedResult("generation") || !summary.contains("canonicalParameters")))return;
    // 先冻结完成记录值；后续报告事件可以重入，但不能改变本次统计来源。
    const auto gapHistory=module=="Gap"?window.GetRecords().GetRecords().value("records"):QJsonValue();
    QCoreApplication::processEvents();
    const auto input=window.GetSession()->GetImageDescriptor();const auto graph=window.GetWorkflow().getPublishedGraph();
    auto* view=window.findChild<ResultReport*>("resultReport");Require(view!=nullptr,"shared report window exists");
    int heartbeats=0;QTimer heartbeat;heartbeat.setInterval(10);
    QObject::connect(&heartbeat,&QTimer::timeout,view,[&]{++heartbeats;});heartbeat.start();
    QSet<QString> refs;
    for(const auto* key:{"inspection","deviation","voxelVolume","resultSet","mesh","generation","qualityReport","correctedVolume","result"}) {
        const auto ref=summary[key].toString();if(!ref.isEmpty()&&!ref.endsWith(":0"))refs.insert(ref);
    }
    if(module=="Surface" && hasPublishedResult("generation"))refs={summary["generation"].toString()};
    // 通过实际入口请求按需生成，等待 owner 分块任务完成后复用冻结报告。
    if(requiresReport && !refs.isEmpty()) {
        std::cout<<"REPORT: preparing native "<<module.toStdString()<<std::endl;
        QString selected;for(const auto& ref:refs)if(view->GetReportAvailable(module,ref)){selected=ref;break;}
        Require(!selected.isEmpty(),"successful business result has no frozen or pending native report");
        const auto beforeGeneration=heartbeats;QElapsedTimer generation;generation.start();
        if(!selected.isEmpty()) {
            const auto open=[&]{window.GetWorkflow().onShowBusinessReport(module,selected);};
            if(checkLarge && QStringList{"Surface","Wall","Gap"}.contains(module) && view->GetReports(module,refs).isEmpty())
                CheckNativeReportCancellation(*view,open,timeoutMs);
            open();
        }
        do {
            QCoreApplication::processEvents(QEventLoop::AllEvents,20);
            if(!view->property("reportGenerationError").toString().isEmpty()&&!view->property("reportGenerationBusy").toBool()&&view->GetReports(module,refs).isEmpty())
                throw std::runtime_error(view->property("reportGenerationError").toString().toStdString());
            Require(view->GetGenerationPending() || !view->GetReports(module,refs).isEmpty(),"native report request ended without publishing its exact result");
        }
        while((view->property("reportGenerationBusy").toBool() || view->GetReports(module,refs).isEmpty()) && (timeoutMs==0 || generation.elapsed()<timeoutMs));
        Require(!view->property("reportGenerationBusy").toBool(),"native report generation exceeded the scenario deadline");
        // 同期渲染事件与定时器分派顺序会影响总耗时，不能据此推断报告回调阻塞。
        // 分块交还、处理中取消和不发布部分结果由 CheckGenerationAndTasks 独立验证。
        std::cout<<"REPORT: native generation "<<module.toStdString()<<" elapsedMs="<<generation.elapsed()
            <<" ownerHeartbeats="<<heartbeats-beforeGeneration<<std::endl;
    }
    const auto reports=view->GetReports(module,refs);
    if(requiresReport&&successful)Require(!reports.isEmpty(),"successful business computation must expose its native report");
    if(reports.isEmpty())return;
    if(!previewPath.isEmpty())Require(!QFileInfo::exists(previewPath+"-reports.json"),"report preview output prefix must be fresh");
    if(requiresReport) {
        // 相同正式结果首次回读时，查询选择器不得替换已经冻结的计算条件。
        // 直接比较两次原生回读，避免相关结果筛选带入其他已缓存文档。
        const auto original=provider->second(record,{});
        if(!original.isEmpty()) {
            auto query=record;query["operationId"]="query-conditions-check";query["parameters"]=QJsonObject{{"result","current"}};
            const auto queried=provider->second(query,{});
            Require(queried.size()==original.size(),"query selectors changed the frozen business report population");
            for(const auto& doc:original) {
                const auto same=std::find_if(queried.cbegin(),queried.cend(),[&](const auto& candidate){return candidate.id==doc.id;});
                Require(same!=queried.cend() && same->conditions==doc.conditions,"query selectors changed frozen business report conditions");
            }
            std::cout<<"PASS: frozen native report conditions module="<<module.toStdString()<<" operation="<<record["operationId"].toString().toStdString()<<std::endl;
        }
    }
    auto* panel=window.GetModule(module);Require(panel!=nullptr,"business report module exists");
    const auto actions=panel->GetActions();window.GetWorkflow().onNavigate(module,actions.contains(action)?action:actions.value(0),{});
    QCoreApplication::processEvents();
    auto* open=panel->findChild<QPushButton*>("viewBusinessReport");auto* dialog=window.findChild<QDockWidget*>("reportDock");
    Require(open&&dialog&&open->isVisible()&&open->isEnabled(),"native report has a visible enabled user entry");
    if(module=="Surface")Require(open->property("reportResult").toString()==summary["generation"].toString(),"surface business report button targets the exact generation instead of its shared mesh");
    for(auto* parent=open->parentWidget();parent;parent=parent->parentWidget())if(auto* scroll=qobject_cast<QScrollArea*>(parent)) {
        scroll->ensureWidgetVisible(open);QCoreApplication::processEvents();
        Require(scroll->viewport()->rect().intersects(QRect(open->mapTo(scroll->viewport(),QPoint()),open->size())),"business report entry lies inside its visible scroll viewport");
    }
    dialog->close();open->click();QCoreApplication::processEvents();
    Require(dialog->isVisible()&&view->isVisible(),"actual business report button opens the report window");
    Require(std::any_of(reports.begin(),reports.end(),[&](const auto& doc){return doc.id==view->GetSelectedReport().id;}),"report entry selects the exact completed result");
    auto* tree=view->findChild<QTreeWidget*>("reportList");auto* table=view->findChild<QTableView*>("reportTable");
    auto* conditions=view->findChild<QPlainTextEdit*>("reportConditions");auto* tabs=view->findChild<QTabWidget*>();
    Require(tree&&table&&conditions&&tabs,"report preview exposes details and calculation conditions");
    QJsonArray previews;int reportIndex=0;
    const auto capture=[&](const QString& suffix) {
        if(previewPath.isEmpty())return QString();
        const auto path=previewPath+QString("-%1-%2.png").arg(reportIndex).arg(suffix);
        Require(!QFileInfo::exists(path),"report preview never overwrites an existing screenshot");
        Require(QDir().mkpath(QFileInfo(path).absolutePath()),"report preview output directory exists");
        QElapsedTimer captureClock;captureClock.start();const auto pixels=dialog->grab();window.SetSynchronousSpan("Automation.ReportCapture",captureClock.nsecsElapsed());Require(!pixels.isNull()&&pixels.save(path),"actual visible report preview screenshot saved");return path;
    };
    QTemporaryDir directory;
    ReportRowId inspectionRows=0;
    for(const auto& doc:reports) {
        QTreeWidgetItem* item=nullptr;for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==doc.id){item=*it;break;}
        Require(item&&!item->isHidden(),"exact native report is visible in the report tree");tree->scrollToItem(item);tree->setCurrentItem(item);QCoreApplication::processEvents();
        Require(view->GetSelectedReport().id==doc.id,"report tree selection opens the requested frozen document");
        WaitRows(*view,timeoutMs);
        auto* reportTable=view->findChild<QTableView*>("reportTable");
        if(reportTable->selectionModel()->selectedRows().isEmpty() && reportTable->model()->rowCount()>0)reportTable->selectRow(0);
        Require(view->findChild<QPushButton*>("exportSelectedReport")->isEnabled()==(reportTable->model()->rowCount()>0) && view->findChild<QPushButton*>("exportReportCsv")->isEnabled()
            && view->findChild<QAction*>("exportFilteredReport")->isEnabled(),"report preview actions do not reflect the complete page and full report");
        Require(!view->findChild<QPushButton*>("copyReportCsv")&&!reportTable->findChild<QAction*>("copyReportCsvAction"),"clipboard copy action remains available");
        Require(view->GetSelectedReport().table->GetCount()==doc.table->GetCount(),"native business full row count");
        if(module=="Alignment" && action=="EvaluateVoxelVolume" && record["parameters"].toObject()["requireEnclosedSelection"].toBool()) {
            const auto row=doc.table->Read(0);double observed=0;
            Require(row[4]=="2" && row[2].isEmpty() && GetReportNumber(row[3],observed) && observed>0,"real full-volume boundary case did not preserve incomplete full volume and usable observed volume");
        }
        if(module=="Wall") {
            const auto measurement=doc.conditions["计算参数"].toObject(),evaluation=doc.conditions["评价参数"].toObject();
            Require(measurement.contains("maxDistance") && measurement.contains("sampleSpacing") && measurement.contains("materialThreshold")
                && measurement["source"]==doc.source && evaluation.contains("histogramBins"),"wall report conditions are not the frozen measurement/evaluation archive");
            std::cout<<"PASS: real wall frozen archive conditions action="<<action.toStdString()<<" operation="<<record["operationId"].toString().toStdString()<<std::endl;
        }
        if(doc.module=="Wall" && summary.contains("sampleCount"))Require(static_cast<std::uint64_t>(doc.table->GetCount())==GetId(summary["sampleCount"]),"wall report count equals independent native statistics");
        if(doc.module=="Gap") {
            const auto expected=GetGapReportCounts(record,gapHistory,doc);
            const auto tableRef=GetRef(doc.id.mid(4));
            auto page=window.GetWorkflow().readRecordPage(tableRef,0,256,std::nullopt);
            const auto total=GetId(page["total"]);
            Require(static_cast<std::uint64_t>(doc.table->GetCount())==total && page["source"]==doc.source,
                "gap report covers the exact native table and source independently of UI page size");
            Require(doc.columns.size()==9 && doc.columns[0].name=="void_id" && doc.columns[3].name=="gap_statistics_volume_mm3"
                && doc.columns[7].name=="visible_volume_mm3" && doc.columns[8].name=="boundary_status","gap report separates formal and visible native quantities");
            std::uint64_t offset=0,formal=0,boundary=0,formalVoxels=0,boundaryVoxels=0;
            while(offset<total) {
                const auto rows=page["rows"].toArray();const auto next=GetId(page["next"]);
                Require(next>offset && next<=total && static_cast<std::uint64_t>(rows.size())==next-offset,"gap native catalog pages advance without missing objects");
                for(const auto value:rows) {
                    const auto native=value.toObject();
                    Require(native.contains("boundary-status") && native.contains("voxel-count")
                        && GetId(native["rowIndex"])==offset,"gap native catalog exposes exact boundary state, voxel count and row identity");
                    const auto state=GetId(native["boundary-status"]),voxels=GetId(native["voxel-count"]);
                    Require(state<=1 && voxels>0,"gap native object boundary state and voxel count are valid");
                    const auto row=doc.table->Read(offset++);double visible=0;
                    Require(row[0]==native["void-id"].toString() && GetReportNumber(row[7],visible) && visible==native["volume-mm3"].toDouble()
                        && row[8]==(state?"BoundaryUncertain":"Interior"),"gap report preserves every native object ID, visible volume and boundary status");
                    const auto nativeIntersection=native.value("intersection-volume-mm3");
                    Require(doc.columns[2].name=="intersection_volume_mm3" && !nativeIntersection.isUndefined(),
                        "gap native intersection volume column is present rather than a missing value converted to zero");
                    if(nativeIntersection.isNull())Require(row[2].isEmpty(),"nonfinite native intersection volume remains an empty report cell");
                    else {
                        double intersection=0;
                        Require(nativeIntersection.isDouble() && std::isfinite(nativeIntersection.toDouble())
                            && GetReportNumber(row[2],intersection) && intersection==nativeIntersection.toDouble(),
                            "gap report intersection volume equals its finite native double value");
                    }
                    Require(native.contains("roi-partial") && native.contains("whole-object-geometry")
                        && row[5]==(GetId(native["roi-partial"])?"1":"0") && row[6]==(GetId(native["whole-object-geometry"])?"1":"0"),
                        "gap report preserves native ROI intersection and complete-geometry qualification separately");
                    // 按既有一页上限记录小目录原值；大目录仍逐页核对，只输出汇总，避免日志洪泛。
                    if(total<=256)std::cout<<"GAP_NATIVE_CATALOG_ROW "<<QJsonDocument(QJsonObject{
                        {"table",GetRefText(tableRef)},{"source",doc.source},{"native",native}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
                    if(state) {
                        ++boundary;boundaryVoxels+=voxels;
                        Require(row[1].isEmpty() && row[3].isEmpty() && row[4].isEmpty(),"native boundary objects keep complete and formal report quantities empty");
                    } else {
                        ++formal;formalVoxels+=voxels;double measured=0;
                        Require(GetReportNumber(row[3],measured) && measured==visible,"native interior objects retain formal report volumes");
                    }
                }
                if(offset<total)page=window.GetWorkflow().readRecordPage(tableRef,offset,256,std::nullopt);
            }
            Require(formal==expected[0] && boundary==expected[2] && formalVoxels==expected[1] && boundaryVoxels==expected[3],
                "gap formal and boundary statistics match the complete independent native catalog");
            std::cout<<"PASS: Gap native catalog rows="<<total<<" formal="<<formal<<" boundary="<<boundary
                <<" formalVoxels="<<formalVoxels<<" boundaryVoxels="<<boundaryVoxels<<std::endl;
        }
        if(doc.module=="Surface") {
            // 原生表面条件采用版本化文本；不能把解析失败的空 JSON 对象当成冻结条件。
            const auto native=summary["canonicalParameters"].toString();
            Require(native.startsWith("surface-parameters 3 ") && doc.conditions["计算参数"].isString()
                && doc.conditions["计算参数"].toString()==native,"surface report preserves the complete native canonical parameter text");
            if(doc.id.startsWith("surface:"))Require(doc.conditions["正式计算条件"].isString()
                && doc.conditions["正式计算条件"].toString()==native,"surface object report preserves the exact formal native conditions");
            const auto inputs=summary["inputs"].toArray();
            Require(!inputs.isEmpty() && doc.conditions["输入修订"].isArray() && doc.conditions["输入修订"].toArray()==inputs,
                "surface report preserves every frozen native input role and revision");
            Require(std::any_of(inputs.cbegin(),inputs.cend(),[&](const auto& value){const auto input=value.toObject();return input["role"]=="source-volume" && input["revision"]==doc.source;}),
                "surface report source matches its frozen native source-volume input");
            std::cout<<"PASS: real surface frozen native parameter text operation="<<record["operationId"].toString().toStdString()<<std::endl;
        }
        if(doc.id.startsWith("surface:") && summary.contains("objectCount")) {
            Require(doc.table->GetCount()==GetReportCount(summary["objectCount"]),"surface report covers all native objects");
            // 独立摘要直接读取原生快照，限定面积有值时也须保留，不能按资格二次抹空。
            const auto objects=summary["objects"].toArray();const auto offset=GetId(summary["objectOffset"]);
            Require(offset<=static_cast<std::uint64_t>(doc.table->GetCount())
                && static_cast<std::uint64_t>(objects.size())<=doc.table->GetCount()-offset,"native surface object page is within the full report");
            quint64 index=0;for(const auto value:objects) {
                const auto object=value.toObject();
                const auto number=[&](const char* key){const auto v=object[key];return v.isDouble()?QString::number(v.toDouble(),'g',17):QString();};
                const QStringList expected{QString::number(GetReportCount(object["objectIndex"])),number("area"),number("volume"),
                    QString::number(object["areaValidity"].toInt()),QString::number(object["volumeValidity"].toInt()),
                    object["closed"].toBool()?"true":"false",object["manifold"].toBool()?"true":"false"};
                const auto row=static_cast<ReportRowId>(offset)+index++;
                Require(SameRow(doc.table->Read(row),expected),"surface report preserves native quantities and their separate qualification");
                if(row<table->model()->rowCount())for(int column=0;column<expected.size();++column)
                    Require(table->model()->data(table->model()->index(static_cast<int>(row),column)).toString()==expected[column],"surface report page preserves every native object field");
            }
        }
        if(doc.id.startsWith("inspection"))inspectionRows+=doc.table->GetCount();
        const auto field=doc.id.startsWith("deviation-reverse:")?QString("reverse"):doc.id.startsWith("deviation:")?QString("forward"):QString();
        if(!field.isEmpty() && summary[field].isObject()) {
            std::uint64_t count=0;for(const auto value:summary[field].toObject()["counts"].toArray())count+=GetId(value);
            Require(static_cast<std::uint64_t>(doc.table->GetCount())==count,"deviation report includes every native validity state");
        }
        if(doc.module=="Artifact") {
            const auto native=window.GetWorkflow().readRecordPage(GetRef(doc.result),0,256,std::nullopt);
            Require(doc.source==native["source"].toString() && doc.conditions["计算参数"].toObject()==QJsonDocument::fromJson(native["canonicalParameters"].toString().toUtf8()).object(),"artifact report source and conditions are frozen from the native published revision");
            Require(static_cast<std::uint64_t>(doc.table->GetCount())==GetId(native["total"]),"artifact report covers the native quality table");
            int rowIndex=0;for(const auto value:native["rows"].toArray()) {
                const auto row=value.toObject();const auto actual=doc.table->Read(rowIndex++);
                Require(actual[0]==row["metric"].toString()&&actual[1]==QString::number(row["value"].toDouble(),'g',17)&&actual[2]==row["unit"].toString(),"artifact preview preserves native metric, value and unit");
                if(row["metric"]=="fidelity-verified")Require(actual[1]=="0","artifact report does not promote gray change into verified fidelity");
            }
        }
        tabs->setCurrentWidget(table);QCoreApplication::processEvents();
        const auto checkPage=[&](int offset) {
            Require(table->isVisible()&&table->model()->rowCount()==std::min<ReportRowId>(200,doc.table->GetCount()-offset),"visible report preview uses a bounded native page");
            if(table->model()->rowCount()) {
                const auto expected=doc.table->Read(offset);
                for(int column=0;column<doc.columns.size();++column)Require(table->model()->data(table->model()->index(0,column)).toString()==GetReportCell(expected,column),"visible report cells preserve native text, missing values and precision");
                const auto visible=table->visualRect(table->model()->index(0,0)).intersected(table->viewport()->rect());
                Require(!visible.isEmpty(),"report first row lies within the visible viewport");
                const auto image=table->viewport()->grab().toImage().scaled(table->viewport()->size());const auto textArea=visible.adjusted(4,4,-4,-4);
                bool painted=false;if(!textArea.isEmpty()) {
                    const auto background=image.pixel(textArea.topLeft());
                    for(int y=textArea.top();y<=textArea.bottom()&&!painted;++y)for(int x=textArea.left();x<=textArea.right();++x)if(image.pixel(x,y)!=background){painted=true;break;}
                }
                Require(painted,"visible native report first cell contains rendered text pixels");
            }
        };
        checkPage(0);Require(!table->horizontalHeader()->isSortIndicatorShown(),"fresh report preview declares its original native row order");
        QJsonArray pages{0};const auto details=capture("details");QString rightColumns;
        if(table->model()->rowCount()&&table->horizontalScrollBar()->maximum()>0) {
            table->horizontalScrollBar()->setValue(table->horizontalScrollBar()->maximum());QCoreApplication::processEvents();
            Require(table->viewport()->rect().intersects(table->visualRect(table->model()->index(0,doc.columns.size()-1))),"rightmost report columns can be previewed through the actual scrollbar");
            rightColumns=capture("right-columns");table->horizontalScrollBar()->setValue(0);QCoreApplication::processEvents();
        }
        if(doc.table->GetCount()>200) {
            view->findChild<QPushButton*>("reportNext")->click();QCoreApplication::processEvents();checkPage(200);pages.append(200);capture("next-page");
            view->findChild<QPushButton*>("reportPrevious")->click();QCoreApplication::processEvents();checkPage(0);
        }
        tabs->setCurrentWidget(conditions);conditions->verticalScrollBar()->setValue(0);QCoreApplication::processEvents();
        Require(conditions->isVisible()&&conditions->toPlainText()==QString::fromUtf8(QJsonDocument(doc.conditions).toJson(QJsonDocument::Indented)),"visible report conditions belong to the exact document");
        const auto conditionImage=capture("conditions");tabs->setCurrentWidget(table);
        previews.append(QJsonObject{{"id",doc.id},{"source",doc.source},{"result",doc.result},{"rows",GetReportCountValue(doc.table->GetCount())},
            {"pageOffsets",pages},{"details",details},{"rightColumns",rightColumns},{"conditions",conditionImage},{"opened",true}});++reportIndex;
        if(checkExport) {
        const auto path=previewPath.isEmpty()?directory.filePath("native.csv")
            :previewPath+QString("-%1-full.csv").arg(reportIndex-1);
        if(!previewPath.isEmpty())Require(!QFileInfo::exists(path),"full report CSV has its own unused preview path");
        std::cout<<"REPORT: async full export "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
        view->ExportReportFile(path);Require(view->property("reportTaskBusy").toBool(),"real report export returns after asynchronous admission");WaitTasks(*view,timeoutMs);
        if(doc.table->GetCount()>200) {
            const auto ids=view->GetReportIds();bool cancelled=false;
            view->ImportReport(path);QTimer::singleShot(0,view,[&]{cancelled=view->property("reportTaskBusy").toBool();view->CancelTasks();});
            WaitTasks(*view,timeoutMs,false);
            Require(cancelled && view->GetReportIds()==ids && QFile::exists(path),"real report cancellation published partial rows or removed the original CSV");
        }
        view->ImportReport(path);Require(view->property("reportTaskBusy").toBool(),"real report import returns after asynchronous admission");WaitTasks(*view,timeoutMs);
        auto copy=view->GetSelectedReport();Require(copy.category=="导入报告","actual async import did not select its document");
        Require(copy.table->GetCount()==doc.table->GetCount(),"native full CSV count matches source");
        if(doc.valueColumn>=0)Require(copy.valueColumn==doc.valueColumn,"native/imported UI preferred value columns must match without manual repair");
        Require(copy.columns.size()==doc.columns.size(),"native CSV changed its full column set");
        for(int c=0;c<doc.columns.size();++c) {
            Require(copy.columns[c].name==doc.columns[c].name,"native CSV changed a column name");
            if(!doc.columns[c].quantity.isEmpty())Require(copy.columns[c].quantity==doc.columns[c].quantity && copy.columns[c].unit==doc.columns[c].unit,"native CSV retains automatic business quantity and unit mapping");
        }
        const auto originalHash=doc.table->GetContentHash(),restoredHash=copy.table->GetContentHash();
        Require(originalHash==restoredHash,"native/imported normalized full-table SHA-256 differs");
        std::cout<<"PASS: full content SHA256 "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()
            <<" digest="<<originalHash.toHex().constData()<<std::endl;
        int value=doc.valueColumn;
        bool comparisonPerformed=false;
        if(value<0)for(int c=0;c<doc.columns.size();++c)if(doc.columns[c].numeric){value=c;break;}
        if(value>=0 && !doc.columns[value].quantity.isEmpty() && !doc.columns[value].unit.startsWith("model")) {
            view->SelectReport(doc.id);view->findChild<QAction*>("setReportBaseline")->trigger();view->SelectReport(copy.id);
            const auto compare=[&]{DrivePopupInteraction([&]{view->findChild<QAction*>("compareReports")->trigger();},[view]{
                auto* mapping=qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if(!mapping || mapping->parentWidget()!=view || !mapping->findChild<QComboBox*>("reportMatchMode"))return false;
                mapping->accept();return true;
            },"real report comparison mapping is not visible");};
            if(doc.table->GetCount()>200) {
                const auto ids=view->GetReportIds();compare();QTimer::singleShot(0,view,[&]{view->CancelTasks();});WaitTasks(*view,timeoutMs,false);
                Require(view->GetReportIds()==ids,"cancelled real report comparison published a partial result");
            }
            std::cout<<"REPORT: real UI comparison "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
            compare();Require(view->property("reportTaskBusy").toBool(),"real report comparison returns after asynchronous admission");WaitTasks(*view,timeoutMs);
            const auto compared=view->GetSelectedReport();Require(compared.category=="对比结果" && compared.table->GetCount()==2
                && compared.table->Read(0)[2]==compared.table->Read(1)[2] && compared.table->Read(0)[3]==compared.table->Read(1)[3],"real UI automatic mapping changed full-data valid counts or mean");
            comparisonPerformed=true;
            view->RemoveReport(compared.id);
        }
        if(checkLarge && doc.table->GetCount()>4096)CheckLargeReportOperations(*view,doc,copy,path,timeoutMs);
        if(module=="Alignment" && doc.columns.size()>4 && doc.columns[4].name=="selection_completeness")CheckVolumeColumns(*view,doc,copy,timeoutMs);
        std::weak_ptr<ReportTable> importedStorage=copy.table;const auto importedId=copy.id;copy.table.reset();
        view->RemoveReport(importedId);Require(importedStorage.expired() && QFile::exists(path),"real imported report storage was retained or its original CSV deleted");
        view->SelectReport(doc.id);
        if(!previewPath.isEmpty()) {
            auto preview=previews[reportIndex-1].toObject();
            preview["csv"]=path;preview["fullTableSha256"]=QString::fromLatin1(originalHash.toHex());
            preview["csvReadbackSha256"]=QString::fromLatin1(restoredHash.toHex());preview["csvReadbackMatches"]=true;
            preview["comparisonPerformed"]=comparisonPerformed;
            if(comparisonPerformed)preview["comparisonMatches"]=true;
            previews[reportIndex-1]=preview;
        }
        std::cout<<"PASS: full real report async export/import/compare and content hash "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
        }
        std::cout<<"PASS: visible native report "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
    }
    dialog->close();QCoreApplication::processEvents();Require(!dialog->isVisible(),"report preview closes without stopping the business session");
    if(summary.contains("items") && summary.contains("inspection"))Require(inspectionRows==summary["items"].toArray().size(),"inspection metric tables preserve all independently read items");
    if(requiresReport&&QStringList{"Succeeded","SucceededWithDisplayFailure","FullyDetermined"}.contains(record["status"].toString())
        &&(module=="Surface"||module=="Gap")) {
        auto* panel=window.GetModule(module);
        const auto read=[&](const QString& action,const QJsonObject& parameters){
            const auto id=panel->SendAction(action,parameters);
            // 这些读取动作同步完成记录；completion 回调刻意排队，不能同步检查回调。
            const auto observed=window.GetRecords().GetRecord(id);
            Require(observed["isTerminal"].toBool()&&observed["status"]=="Observed","object page action completes through the real UI binding");
            return observed["result"].toObject();
        };
        const auto control=[&](const QString& action,const QJsonObject& parameters=QJsonObject{}){
            const auto id=panel->SendAction(action,parameters);const auto observed=window.GetRecords().GetRecord(id);
            Require(observed["isTerminal"].toBool()&&observed["status"]=="Succeeded","object selection action is accepted through the real UI binding");
            return observed["result"].toObject();
        };
        if(module=="Surface") {
            const auto objects=read("ReadObjects",{{"pageOffset","0"},{"pageSize","2"},{"generation",summary["generation"]}});
            const auto entries=objects["objects"].toArray();
            if(!entries.isEmpty()) {
                const auto id=entries[0].toObject()["objectIndex"];
                const auto points=read("ReadObjectPoints",{{"generation","listed"},{"objectIndex",id},{"pageOffset","0"},{"pageSize","3"}});
                const auto triangles=read("ReadObjectTriangles",{{"generation","listed"},{"objectIndex",id},{"pageOffset","0"},{"pageSize","3"}});
                Require(points["generation"]==objects["generation"]&&triangles["generation"]==objects["generation"]
                    &&points["rows"].toArray().size()<=3&&triangles["rows"].toArray().size()<=3,"surface detail actions keep listed generation and bounded object ranges");
                const QJsonObject selection{{"generation","listed"},{"objectIndex",id}};
                Require(control("SelectObject",selection)["objectIndex"]==id,"surface UI selects original object ID");
                Require(control("IsolateObject",selection)["isIsolated"].toBool(),"surface UI isolation is explicit");
                control("ClearObjectSelection");control("ObjectPicking",{{"isEnabled",true}});control("ObjectPicking",{{"isEnabled",false}});
            }
        } else {
            const auto objects=read("Defects",{{"pageOffset","0"}});const auto entries=objects["page"].toObject()["rows"].toArray();
            if(!entries.isEmpty()) {
                const auto geometry=read("ReadDefectGeometry",{{"result","listed"},{"regionId",entries[0].toObject()["void-id"]},{"pointOffset","0"},{"triangleOffset","0"},{"pageSize","3"}});
                Require(geometry["resultSet"]==objects["resultSet"]&&geometry["points"].toArray().size()<=3&&geometry["triangles"].toArray().size()<=3,
                    "gap detail action keeps listed result and original object ID with bounded geometry");
                const QJsonObject selection{{"result","listed"},{"regionId",entries[0].toObject()["void-id"]}};
                Require(control("SelectObject",selection)["regionId"]==selection["regionId"],"gap UI selects original region ID");
                Require(control("IsolateObject",selection)["isIsolated"].toBool(),"gap UI isolation is explicit");
                control("ClearObjectSelection");control("ObjectPicking",{{"isEnabled",true}});control("ObjectPicking",{{"isEnabled",false}});
            }
        }
    }
    const auto after=window.GetSession()->GetImageDescriptor();Require((!input && !after) || (input && after && input->dataRevision==after->dataRevision&&input->bindingRevision==after->bindingRevision),"report operations preserve exact input and binding");
    Require(window.GetWorkflow().getPublishedGraph()==graph,"report operations preserve result graph and active bindings");
    if(!previewPath.isEmpty())ExportJson(previewPath+"-reports.json",{{"module",module},{"operationId",record["operationId"]},{"reports",previews},
        {"selectedDataRevision",input?GetRefText(input->dataRevision):QString()},
        {"selectedBindingRevision",QString::number(input?input->bindingRevision:0)}});
}
}


namespace Manual {
void CheckGapBoundaryReports(const QString& outputDirectory,const QString& realInput)
{
#if defined(MANUAL_GAP) && defined(MANUAL_CROP)
    Require(!outputDirectory.isEmpty() && QDir().mkpath(outputDirectory),"Gap report output directory is required");
    int failures=0;
    const auto expect=[&](bool valid,const char* message){if(!valid){++failures;std::cerr<<"GAP_REPORT_FAIL: "<<message<<std::endl;}};
    const auto wait=[](const std::function<bool()>& ready,const char* message) {
        QElapsedTimer timer;timer.start();
        while(!ready() && timer.elapsed()<60000)QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        Require(ready(),message);
    };
    for(int scenario=0;scenario<(realInput.isEmpty()?2:3);++scenario) {
        const bool real=scenario==2,mixed=scenario==1;
        const QString label=real?"real32":mixed?"mixed":"boundary";
        const auto folder=QDir(outputDirectory).filePath(label);Require(QDir().mkpath(folder),"scenario directory creation failed");
        QString raw=realInput;
        if(!real) {
            raw=QDir(folder).filePath("input-11x11x11.raw");std::vector<float> voxels(1331,100.f);
            const auto hole=[&](int x0,int x1,int y0,int y1,int z0,int z1){
                for(int z=z0;z<=z1;++z)for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x)voxels[(10-x)+11*((10-y)+11*z)]=0.f;
            };
            // RAW加载按契约反转X/Y；预先反排存储，配合LPS原点(-10,-10,0)得到原点0的单位RAS网格。
            // 3^3=27体素孔与半径1.25的无效球域在x=4/5处面接触，球不覆盖孔体素。
            hole(2,4,4,6,4,6);
            // 两个2^3=8体素确定孔在扫描顺序中夹住边界孔，过滤后正式ID不连续。
            if(mixed){hole(1,2,1,2,1,2);hole(7,8,7,8,7,8);}
            QFile file(raw);Require(file.open(QIODevice::WriteOnly),"synthetic raw open failed");
            const auto bytes=static_cast<qint64>(voxels.size()*sizeof(float));
            Require(file.write(reinterpret_cast<const char*>(voxels.data()),bytes)==bytes,"synthetic raw write failed");file.close();
        }
        TestWindow window;window.setAttribute(Qt::WA_ShowWithoutActivating,true);window.show();
        wait([&]{return window.GetIsReady() || !window.GetFailure().isEmpty();},"test window session did not initialize");
        Require(window.GetIsReady(),"test window session initialization failed");
        const auto send=[&](const QString& module,const QString& action,const QJsonObject& patch,const QString& expected) {
            auto* panel=window.GetModule(module);Require(panel!=nullptr,"real module is missing");
            window.GetWorkflow().onNavigate(module,action,{});panel->SetParameterPatch(action,patch);
            // 复用业务自动化的真实按钮门控；操作完成回调不代表后续视图刷新已收口。
            wait([&]{panel->Observe();const auto* button=panel->findChild<QPushButton*>("action_"+action);
                return button && button->isEnabled();},"real business action did not become available after its guards");
            struct Completion {bool done=false;QString status;};auto completed=std::make_shared<Completion>();
            const auto id=panel->SendAction(action,panel->GetActionParameters(action),[completed](const QString& status,const QJsonObject&){completed->status=status;completed->done=true;});
            wait([&]{return completed->done;},"real module operation timed out");
            const auto record=window.GetRecords().GetRecord(id);
            if(completed->status!=expected && !(module=="Gap" && expected=="Succeeded" && completed->status=="SucceededWithDisplayFailure"))
                throw std::runtime_error((module+"."+action+" status="+completed->status+" "+QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact))).toStdString());
            std::cout<<"GAP_REPORT_ACTION module="<<module.toStdString()<<" action="<<action.toStdString()<<" status="<<completed->status.toStdString()<<std::endl;
            return record;
        };
        const int side=real?32:11;const double spacing=real?.1537:1.,origin=real?74.3908:0.;
        send("Data","Load",{{"filePath",raw},{"datasetId","gap-report-"+label},{"dimensions",QJsonArray{side,side,side}},
            {"spacingLPS",QJsonArray{spacing,spacing,spacing}},{"originLPS",QJsonArray{-origin-spacing*(side-1),-origin-spacing*(side-1),origin}},
            {"directionLPS",QJsonArray{1,0,0,0,1,0,0,0,1}},{"evidenceKind",real?"real-roi":"synthetic-geometry"}},"Succeeded");
        const auto input=window.GetSession()->GetImageDescriptor();Require(input.has_value(),"loaded input descriptor is unavailable");
        std::cout<<"GAP_REPORT_INPUT scenario="<<label.toStdString()<<" geometry="<<QJsonDocument(GetDescriptor(input)).toJson(QJsonDocument::Compact).constData()<<std::endl;
        Require(input->dims==std::array<int,3>{side,side,side} && input->origin==std::array<double,3>{origin,origin,origin} && input->direction==std::array<double,9>{1,0,0,0,1,0,0,0,1},"independent input grid must be identity RAS");
        send("View","Set",{{"viewId","primary-3d"},{"mode","CompositeVolume"}},"Succeeded");
        auto* crop=window.GetModule("Crop");Require(crop!=nullptr,"real Crop module is missing");
        const auto framesReady=[&]{crop->Observe();return crop->GetObservedState()["framesReady"].toBool();};
        if(!real) {
            window.GetWorkflow().ensureCropViews();wait(framesReady,"input frames not ready for exact crop");
            send("Crop","Start",{{"shape","Sphere"},{"removalMode","RemoveInside"}},"Succeeded");
            send("Crop","AppendExact",{{"parentNodeId","Root"},{"shape","Sphere"},{"removalMode","RemoveInside"},
                {"center",QJsonArray{6,5,5}},{"radius",1.25}},"Succeeded");
            wait(framesReady,"exact crop frames did not settle");
            send("Crop","FinishEditing",{},"PreviewConfirmed");
            wait(framesReady,"crop widget removal did not finish rendering before publication");
            send("Crop","BuildResult",{},"Published");send("Crop","SelectOutput",{},"Succeeded");
        }
        auto record=send("Gap","Start",{{"isoMode","AbsoluteValue"},{"absoluteIsoValue",real?.172:50.},
            {"backgroundMean",real?-1.617:0.},{"materialMean",real?.453:100.},{"filter",false}},"Succeeded");
        const auto core=record["result"].toObject();const int formal=mixed?2:0,visible=real?0:mixed?3:1;
        const double formalVolume=mixed?16.:0.;
        // 合成场景：边界孔27不进正式统计；混合场景另有两个互不接触的2^3孔，正式2孔/16 mm3。
        expect(core["voidVolumeMM3"].toDouble(-1)==formalVolume,"core formal volume must be 0/16/0 mm3");
        expect(core["voidCount"].toString()==QString::number(formal),"core formal count excludes the uncertain fragment");
        expect(core["boundaryUncertainCount"].toString()==QString::number(real?0:1) && core["boundaryUncertainVolumeMM3"].toDouble(-1)==(real?0.:27.),"core boundary population keeps the real visible fragment volume");
        expect(GetId(core["voidVoxels"])==static_cast<std::uint64_t>(mixed?16:0),"core formal voxel count follows independent geometry");
        const auto native=window.GetWorkflow().readRecordPage(GetRef(core["voidTable"]),0,256,std::nullopt);
        expect(GetId(native["total"])==static_cast<std::uint64_t>(visible),"native visible object population is independent of formal count");
        QJsonArray formalIds,formalRows;
        for(int i=0;i<native["rows"].toArray().size();++i) {
            const auto row=native["rows"].toArray()[i].toObject();
            if(row["volume-mm3"].toDouble()==8.){formalIds.append(row["void-id"]);formalRows.append(QString::number(i));}
        }
        if(mixed)expect(formalIds.size()==2 && GetId(formalIds[1])>GetId(formalIds[0])+1,"mixed source must expose non-contiguous formal object IDs");
        CheckBusinessReport(window,record,true,{},60000);
        {
            auto* view=window.findChild<ResultReport*>("resultReport");Require(view!=nullptr,"actual result report widget is unavailable");
            const auto documents=view->GetReports("Gap",{core["resultSet"].toString()});Require(documents.size()==1,"actual Gap provider must expose exactly one document");
            const auto document=documents[0];expect(document.table->GetCount()==visible,"report retains visible uncertain objects");
            const auto summary=document.conditions["汇总"].toObject();
            expect(summary["孔隙体积 mm3"].toDouble(-1)==formalVolume && summary["正式孔数"].toString()==QString::number(formal),"actual report summary uses core formal volume and count");
            expect(summary["边界不确定对象数"].toString()==QString::number(real?0:1) && summary["边界不确定可见体积 mm3"].toDouble(-1)==(real?0.:27.),"actual report summary separates boundary objects from formal statistics");
            const auto column=[&](const QString& name){for(int i=0;i<document.columns.size();++i)if(document.columns[i].name==name)return i;return -1;};
            const int statistics=column("gap_statistics_volume_mm3"),shown=column("visible_volume_mm3"),status=column("boundary_status");
            expect(statistics>=0 && shown>=0 && status>=0,"report has separate formal volume, visible fragment volume and boundary status");
            Require(view->SelectReport(document.id),"native report selection failed");WaitRows(*view,60000);
            const auto csv=QDir(folder).filePath("gap-report.csv");view->ExportReportFile(csv);WaitTasks(*view,60000);
            view->ImportReport(csv);WaitTasks(*view,60000);const auto copy=view->GetSelectedReport();
            expect(copy.table->GetCount()==visible && copy.table->GetContentHash()==document.table->GetContentHash(),"actual CSV import preserves every native cell");
            int uncertain=0,included=0;double sum=0;
            for(ReportRowId i=0;i<copy.table->GetCount();++i) {
                const auto row=copy.table->Read(i);double volume=0;
                const auto shownCell=GetReportCell(row,shown>=0?shown:statistics);
                const bool numeric=GetReportNumber(shownCell,volume);
                if(numeric && volume==27.) {
                    ++uncertain;expect(GetReportCell(row,status)=="BoundaryUncertain","CSV explicitly marks boundary uncertainty");
                    expect(GetReportCell(row,statistics).isEmpty(),"boundary object formal statistics cell must be empty, not 27 or measured zero");
                    expect(GetReportCell(row,column("parent_volume_mm3")).isEmpty() && GetReportCell(row,column("equivalent_diameter_mm")).isEmpty(),"unknown complete pore quantities remain empty");
                }else {
                    ++included;double measured=0;
                    expect(numeric && volume==8. && GetReportCell(row,status)=="Interior","mixed formal object preserves 8 mm3 and interior status");
                    expect(GetReportNumber(GetReportCell(row,statistics),measured) && measured==8.,"formal statistics retains each certain pore");sum+=measured;
                }
                std::cout<<"GAP_REPORT_CSV scenario="<<label.toStdString()<<" row="<<i<<" cells="<<QJsonDocument(QJsonArray::fromStringList(row)).toJson(QJsonDocument::Compact).constData()<<std::endl;
            }
            expect(uncertain==(real?0:1) && included==formal && sum==formalVolume,"CSV population and formal sum follow independently derived geometry");
        }
        auto* panel=window.GetModule("Gap");panel->Observe();window.GetWorkflow().onShowStatistics("Gap");
        Require(panel->onStatistics && panel->onStatistics(),"actual Gap histogram source did not start");
        auto* statistics=panel->GetStatisticsWidget();
        wait([&]{const auto status=statistics->GetHistogram()["status"].toString();return status=="Ready"||status=="Empty"||status=="Failed";},"Gap histogram did not reach a terminal state");
        const auto histogram=statistics->GetHistogram();double samples=0;for(const auto count:histogram["counts"].toArray())samples+=count.toDouble();
        std::cout<<"GAP_REPORT_HISTOGRAM scenario="<<label.toStdString()<<" data="<<QJsonDocument(histogram).toJson(QJsonDocument::Compact).constData()<<std::endl;
        expect(histogram["status"]==(formal?"Ready":"Empty") && samples==formal,"actual histogram excludes uncertain objects and does not invent zero samples");
        if(mixed)expect(histogram["edges"].toArray().last().toDouble()<27.,"formal histogram range excludes the 27 mm3 boundary object");
        const auto listedRecord=send("Gap","Defects",{},"Observed");
        CheckBusinessReport(window,listedRecord,true,{},60000);
        const auto listed=listedRecord["result"].toObject()["page"].toObject()["rows"].toArray();
        const auto displayedIds=statistics->property("defectIds").toJsonArray(),displayedRows=statistics->property("defectRows").toJsonArray();
        expect(listed.size()==formal && displayedIds==formalIds && displayedRows==formalRows,"formal list preserves real object IDs and original record row identities after filtering");
        auto* tree=statistics->findChild<QTreeWidget*>();expect(tree && tree->topLevelItemCount()==formal,"actual list widget contains only formal objects");
        if(mixed && listed.size()==formal && displayedIds==formalIds && tree && tree->topLevelItemCount()>=2) {
            statistics->ShowTable();tree->scrollToItem(tree->topLevelItem(1));QCoreApplication::processEvents();
            const auto point=tree->visualItemRect(tree->topLevelItem(1)).center();
            QMouseEvent press(QEvent::MouseButtonPress,QPointF(point),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease,QPointF(point),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(),&press);QApplication::sendEvent(tree->viewport(),&release);
            QCoreApplication::processEvents();panel->Observe();
            expect(panel->GetObservedState()["selectedObject"]==formalIds[1],"actual second-row selection highlights the true non-contiguous object ID");
            send("Gap","LocateDefect",{{"rowIndex",formalRows[1]}},"Succeeded");
            const auto state=window.GetSession()->GetRenderViewState({"primary-3d"});
            expect(state && std::all_of(state->cursorWorld.begin(),state->cursorWorld.end(),[](double value){return value>=7. && value<=8.;}),"record location uses the original row identity, not the filtered row index");
            std::cout<<"GAP_REPORT_IDENTITY expected_id="<<formalIds[1].toString().toStdString()
                <<" selected_id="<<panel->GetObservedState()["selectedObject"].toString().toStdString()
                <<" source_row="<<formalRows[1].toString().toStdString()<<std::endl;
        }
        std::cout<<"GAP_REPORT_AUDIT scenario="<<label.toStdString()<<" formal_volume="<<core["voidVolumeMM3"].toDouble()
            <<" visible="<<visible<<" listed="<<listed.size()<<" histogram_samples="<<samples<<" csv="<<QDir(folder).filePath("gap-report.csv").toStdString()<<std::endl;
        if(mixed) {
            // A 的两份8 mm3确定孔被9 mm3筛选排除；27 mm3边界对象仍可见，不能据新B改写旧A摘要。
            Require(GetId(core["voidCount"])==2 && GetId(core["voidVoxels"])==16
                && GetId(core["boundaryUncertainCount"])==1 && GetId(core["boundaryUncertainVoxels"])==27,
                "unfiltered A has independently expected formal 2/16 and boundary 1/27");
            auto parameters=record["parameters"].toObject();parameters["filter"]=true;parameters["minVolumeMM3"]=9.;
            const auto newer=send("Gap","Start",parameters,"Succeeded"),newCore=newer["result"].toObject();
            Require(GetId(newCore["voidCount"])==0 && GetId(newCore["voidVoxels"])==0
                && GetId(newCore["boundaryUncertainCount"])==1 && GetId(newCore["boundaryUncertainVoxels"])==27,
                "filtered B has independently expected formal 0/0 and boundary 1/27");
            Require(newCore["source"]==core["source"] && newCore["voidTable"]!=core["voidTable"] && newCore["resultSet"]!=core["resultSet"],
                "new Gap computation publishes distinct table/resultSet on the same source");
            // 后续页沿用A的既有列表读者；在此之前不能用B首页覆盖该固定修订。
            const auto historical=send("Gap","Defects",{{"pageOffset","1"}},"Observed");
            const auto oldPage=historical["result"].toObject();
            Require(oldPage["table"]==core["voidTable"] && oldPage["resultSet"]==core["resultSet"],"historical Defects page keeps A table/resultSet");
            panel->Observe();const auto current=panel->GetObservedState();
            Require(current["resultSet"]==newCore["resultSet"] && current["voidTable"]==newCore["voidTable"]
                && GetId(current["voidCount"])==0 && GetId(current["voidVoxels"])==0,"historical query does not replace current B");
            CheckBusinessReport(window,historical,true,{},60000);
            const auto refreshed=send("Gap","Defects",{{"pageOffset","0"}},"Observed")["result"].toObject();
            Require(refreshed["table"]==newCore["voidTable"] && refreshed["resultSet"]==newCore["resultSet"],"explicit first page returns to B before its direct report check");
            CheckBusinessReport(window,newer,true,{},60000);
        }
        send("Gap","Exit",{},"Exited");wait([&]{return window.StopSession();},"Gap report fixture did not stop cleanly");window.close();
    }
    if(failures)throw std::runtime_error("Gap boundary report assertions failed: "+std::to_string(failures));
#else
    (void)outputDirectory;(void)realInput;throw std::runtime_error("Gap report fixture requires GapAnalysis and OrthogonalCrop");
#endif
}
}
