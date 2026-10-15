#include "ReportWorkflowTests.h"
#include "Support/ResultReport.h"
#include "Support/ReportComparison.h"
#if defined(MANUAL_ALIGNMENT)
#include "Support/InspectionReport.h"
#include "Support/JsonInput.h"
#endif
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <iostream>
#include <stdexcept>

namespace Manual {
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Wait(F pending) {
    QElapsedTimer elapsed;elapsed.start();
    do {QCoreApplication::processEvents(QEventLoop::AllEvents,10);}while(pending() && elapsed.elapsed()<30000);
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
    for(const auto& quantity:QStringList{"表面对象体积","选区单元体积和","本次统计体积","半径相对名义偏差"}) {
        auto native=Document(quantity,quantity);
        QByteArray bytes;QBuffer output(&bytes);output.open(QIODevice::WriteOnly);WriteReportCsv(output,native);output.close();
        auto copy=Read(bytes);
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
    Require(!view.findChild<QPushButton*>("locateReportRow")->isEnabled(),"cleared selection can still locate an old row");
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
    QTimer::singleShot(0,[]{auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());Require(dialog,"source association chooser missing");dialog->findChild<QComboBox*>()->setCurrentIndex(1);dialog->accept();});
    view.findChild<QPushButton*>("associateReportSource")->click();
    view.ShowReports("data-b");Require(Visible(view,imported.id),"explicit imported report association was not used by filtering");
    view.SetReportSource(imported.id,{});view.ShowReports("data-b");Require(!Visible(view,imported.id),"removing association left stale source membership");
    auto removable=Document("remove");std::weak_ptr<ReportTable> storage=removable.table;view.AddReport(removable);view.SelectReport(removable.id);
    view.findChild<QPushButton*>("setReportBaseline")->click();removable.table.reset();view.RemoveReport(removable.id);
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
    auto far=Read("point_label,x_mm,y_mm,z_mm,thickness_mm\nfar,1e20,0,0,3\nnear,0,0,0,4\n");
    o.distanceTolerance=1e-9;const auto fallback=CompareReports(far,far,o);
    Require(fallback.conditions["有效配对数"]==2 && fallback.table->GetCount()==2 && fallback.table->Read(0)[8]=="0", "spatial acceleration range rejected valid finite coordinates instead of completing a full scan");
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
    Wait([&]{return !view.property("reportGenerationBusy").toBool();});
    Require(readRows>0 && readRows<50000 && !lease.expired(),"native generation did not yield a bounded owner chunk");
    view.findChild<QPushButton*>("cancelReportTask")->click();QCoreApplication::processEvents();
    Require(lease.expired() && !view.GetReportIds().contains("lazy"),"cancelled native generation retained its snapshot or published partial rows");
    view.ShowReports({},"lazy-result","Wall");
    Wait([&]{return !view.GetReportIds().contains("lazy");});
    Require(lease.expired() && view.GetSelectedReport().table->GetCount()==50000,"retry failed to materialize all rows and release its native source");
    QTemporaryDir directory;const auto path=directory.filePath("original.csv");ExportReport(path,view.GetSelectedReport());
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
    view.SelectReport("lazy");view.findChild<QPushButton*>("setReportBaseline")->click();
    auto current=Document("current","聚合节点厚度场值",50000);view.AddReport(current);view.SelectReport(current.id);
    QTimer::singleShot(0,[]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());Require(dialog,"comparison mapping dialog missing");dialog->accept();});
    view.findChild<QPushButton*>("compareReports")->click();Require(view.property("reportTaskBusy").toBool(),"comparison blocked its UI admission");
    const auto comparisonIds=view.GetReportIds();QTimer::singleShot(0,&view,[&]{view.findChild<QPushButton*>("cancelReportTask")->click();});
    Wait([&]{return view.property("reportTaskBusy").toBool();});
    Require(view.GetReportIds()==comparisonIds && view.GetSelectedReport().id==current.id,"cancelled comparison published a partial result or changed selection");
    bool fail=true;ResultReport retry;
    retry.onReadReports=[&](const QJsonObject&,const QSet<QString>&){if(fail)throw std::runtime_error("暂时不可读取");return QVector<ReportDocument>{Document("retry")};};
    retry.QueueReport(record);retry.show();QCoreApplication::processEvents();
    Require(retry.GetReportAvailable("Wall","lazy-result") && retry.GetReportIds().isEmpty(),"read failure permanently discarded the retryable operation");
    fail=false;retry.findChild<QPushButton*>("retryFailedReport")->click();Wait([&]{return !retry.GetReportIds().contains("retry");});retry.hide();
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
    ReportDocument doc;doc.id="wide-row-contract";doc.title="64 位分页契约";doc.category="本次会话结果";doc.module="Gap";
    doc.columns={{"void_id",{},{},false},{"value","体积","mm3",true}};doc.table=std::make_shared<ReportTable>();
    doc.table->SetRows(static_cast<std::size_t>(count),[](ReportRowId row){return QStringList{QString::number(row),QString::number(row+1)};});
    Require(doc.table->GetCount()==count && doc.table->Read(count-1)[0]==QString::number(count-1),"report source truncates global row IDs to int");
    ResultReport view;ReportRowId located=-1;view.onLocate=[&](const ReportDocument&,ReportRowId row){located=row;};view.AddReport(doc);view.show();
    view.findChild<QPushButton*>("reportLast")->click();QCoreApplication::processEvents();auto* table=view.findChild<QTableView*>("reportTable");
    const int local=table->model()->rowCount()-1;Require(local>=0 && table->model()->data(table->model()->index(local,0)).toString()==QString::number(count-1),"last report page truncates a 64-bit row");
    table->selectRow(local);view.findChild<QPushButton*>("locateReportRow")->click();Require(located==count-1,"locate callback truncates a 64-bit global row");
    view.findChild<QPushButton*>("reportFirst")->click();Require(table->model()->data(table->model()->index(0,0)).toString()=="0","first report page lost original order");view.hide();
}
#if defined(MANUAL_ALIGNMENT)
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
    view.ShowReports({},text,"Alignment");Wait([&]{return view.property("reportGenerationProgress").toULongLong()<150512;});
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
void CheckReportRepairs() {
    CheckSemanticRoundtrip();CheckAssociationsAndClearing();CheckSpatialEvidence();CheckGenerationAndTasks();CheckFailureIsolation();CheckVolumeCompleteness();CheckConcurrentReportReads();CheckWideRowAddressing();
#if defined(MANUAL_ALIGNMENT)
    CheckInspectionPreparation();
#endif
    std::cout<<"PASS: report semantics, associations, spatial evidence, lazy generation, cancellation and resource release"<<std::endl;
}
}
