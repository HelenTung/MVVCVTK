#include "ReportWorkflowTests.h"
#include "App/TestWindow.h"
#include "Modules/ModulePanel.h"
#include "Support/ResultReport.h"
#include "Support/ReportComparison.h"
#include "Support/JsonInput.h"
#include <QBuffer>
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
#include <QComboBox>
#include <QApplication>
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
namespace Manual {
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool SameRow(const QStringList& a,const QStringList& b){return a.size()==b.size() && std::equal(a.cbegin(),a.cend(),b.cbegin());}
bool SameIndices(const ReportRows& rows,std::initializer_list<int> expected){return std::size_t(rows.size())==expected.size() && std::equal(rows.begin(),rows.end(),expected.begin());}
ReportDocument Read(QByteArray bytes){QBuffer input(&bytes);input.open(QIODevice::ReadOnly);return ReadReportCsv(input,"测试报告");}
template<class F> void Reject(F action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}Require(rejected,message);}
ReportComparisonOptions Options(){ReportComparisonOptions o;o.baselineColumn=o.currentColumn=1;o.baselineIdentity=o.currentIdentity=0;o.baselineQuantity=o.currentQuantity="厚度";o.baselineUnit=o.currentUnit="mm";o.match=ReportMatch::Identity;o.identitiesConfirmed=true;return o;}
void WaitRows(ResultReport& view,int timeoutMs=30000) {
    QElapsedTimer elapsed;elapsed.start();
    while(view.property("reportRowsBusy").toBool() && (timeoutMs<=0 || elapsed.elapsed()<timeoutMs))QCoreApplication::processEvents(QEventLoop::AllEvents,20);
    Require(!view.property("reportRowsBusy").toBool(),"report row processing did not finish within test deadline");
}
void WaitTasks(ResultReport& view,int timeoutMs=30000,bool success=true) {
    QElapsedTimer elapsed;elapsed.start();
    while(view.property("reportTaskBusy").toBool() && elapsed.elapsed()<timeoutMs)QCoreApplication::processEvents(QEventLoop::AllEvents,20);
    Require(!view.property("reportTaskBusy").toBool() && (!success || view.property("reportTaskError").toString().isEmpty()),"report task did not complete successfully");
}
void SortRows(ResultReport& view,int column,int timeoutMs=30000) {
    auto* table=view.findChild<QTableView*>("reportTable");
    QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Q_ARG(int,column));WaitRows(view,timeoutMs);
}
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
    int located=-1;view.onLocate=[&](const ReportDocument& document,ReportIndex row){Require(document.id==large.id,"locate changed selected report identity");located=row;};
    view.AddReport(large);view.SelectReport(large.id);auto* table=view.findChild<QTableView*>("reportTable");
    QElapsedTimer elapsed;elapsed.start();QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Q_ARG(int,1));
    Require(view.property("reportRowsBusy").toBool() && view.GetFilteredRows().empty(),"sorting blocks admission or exposes partial rows");
    bool yielded=false;QTimer::singleShot(0,&view,[&]{yielded=view.property("reportRowsBusy").toBool();});WaitRows(view);
    Require(yielded && view.GetFilteredRows().size()==50000 && table->model()->data(table->model()->index(0,0)).toString()=="row-49999","complete sorting did not yield to owner events or lost rows");
    table->selectRow(0);view.findChild<QPushButton*>("locateReportRow")->click();Require(located==49999,"sorted page location did not map to original row");
    std::cout<<"Reports: 50000-row sort elapsedMs="<<elapsed.elapsed()<<std::endl;
    auto* search=view.findChild<QLineEdit*>("reportSearch");search->setText("row-");search->setText("row-49999");WaitRows(view);
    Require(SameIndices(view.GetFilteredRows(),{49999}),"superseded search committed stale rows");
    QTemporaryDir directory;const auto path=directory.filePath("filtered.csv");const auto filteredRows=view.GetFilteredRows();ExportReport(path,large,&filteredRows);
    QFile filteredInput(path);Require(filteredInput.open(QIODevice::ReadOnly),"filtered export unreadable");const auto filtered=ReadReportCsv(filteredInput,"筛选结果");
    Require(filtered.table->GetCount()==1 && SameRow(filtered.table->Read(0),large.table->Read(49999)),"filtered export lost original row identity or values");
    search->setText("row-");bool cancelledWhileBusy=false;
    QTimer::singleShot(0,&view,[&]{cancelledWhileBusy=view.property("reportRowsBusy").toBool();view.findChild<QPushButton*>("cancelReportRows")->click();});WaitRows(view);
    Require(cancelledWhileBusy && view.GetFilteredRows().empty(),"cancelled scan published partial or late results");
    Require(!view.findChild<QPushButton*>("exportFilteredReport")->isEnabled() && !view.findChild<QPushButton*>("copyReportCsv")->isEnabled(),"cancelled scan is presented as an exportable complete result");
    search->setText("row-4");view.SelectReport(ties.id);QCoreApplication::processEvents();
    Require(view.GetSelectedReport().id==ties.id && view.GetFilteredRows().size()==8,"old report scan overwrote a new selection");
    ReportDocument virtualRows;virtualRows.id="64-bit-native";virtualRows.title="64 位原生页";virtualRows.category="本次会话结果";
    virtualRows.columns={{"id",{},{},false}};virtualRows.table=std::make_shared<ReportTable>();
    const ReportIndex beyondInt=ReportIndex(std::numeric_limits<int>::max())+19;
    int reads=0;virtualRows.table->SetRows(static_cast<std::size_t>(beyondInt),[&](ReportIndex row){++reads;return QStringList{QString::number(row)};});
    view.AddReport(virtualRows);view.SelectReport(virtualRows.id);
    const auto implicit=view.GetFilteredRows();Require(implicit.size()==static_cast<std::size_t>(beyondInt) && implicit.back()==beyondInt-1 && reads<1000,"natural 64-bit range was capped or fully expanded");
    Require(virtualRows.table->Read(beyondInt-1).front()==QString::number(beyondInt-1),"native row callback narrowed the requested tail index");
    view.SelectReport(ties.id);
    {ResultReport pending;pending.AddReport(large);auto* pendingTable=pending.findChild<QTableView*>("reportTable");QMetaObject::invokeMethod(pendingTable->horizontalHeader(),"sectionClicked",Q_ARG(int,1));}
    QCoreApplication::processEvents();
}
void CheckExternalReportRuns() {
    bool supplied=false;const auto configured=qEnvironmentVariable("MVVCVTK_REPORT_STRESS_ROWS").toLongLong(&supplied);
    const ReportIndex count=supplied && configured>0?configured:120003;
    ReportDocument a;a.id="external-a";a.title=a.id;a.category="导入报告";a.columns={{"id",{},{},false},{"value",{},{},true}};a.table=std::make_shared<ReportTable>();
    auto b=a;b.id="external-b";b.title=b.id;b.table=std::make_shared<ReportTable>();
    QElapsedTimer elapsed;elapsed.start();
    for(ReportIndex row=0;row<count;++row){a.table->Append({QString::number(row),QString::number(count-row)});b.table->Append({QString::number(count-row-1),QString::number(row+1+.25,'g',17)});}
    ResultReport view;view.AddReport(a);view.SelectReport(a.id);SortRows(view,1,0);
    const auto rows=view.GetFilteredRows();Require(rows.size()==static_cast<std::size_t>(count) && rows.front()==count-1 && rows.back()==0,"external merge lost rows or changed numeric order");
    std::cout<<"Reports: external sort complete rows="<<count<<" elapsedMs="<<elapsed.elapsed()<<std::endl;
    const auto comparison=CompareReports(a,b,Options());
    Require(comparison.table->GetCount()==count && comparison.conditions["有效配对数"].toDouble()==double(count),"disk identity index lost uniquely matched rows");
    for(const auto row:std::array<ReportIndex,3>{0,count/2,count-1}) {
        const auto actual=comparison.table->Read(row);Require(actual[0]==QString::number(row) && actual[4]=="0.25","external ID merge changed exact value or original baseline order");
    }
    std::cout<<"Reports: external matching complete elapsedMs="<<elapsed.elapsed()<<std::endl;
    b.table->Append({QString::number(count/2),"999"});const auto duplicate=CompareReports(a,b,Options());
    Require(duplicate.table->GetCount()==count+2 && duplicate.conditions["有效配对数"].toDouble()==double(count-1) && duplicate.table->Read(count/2)[4].isEmpty(),"disk duplicate group silently chose one current row");
    std::cout<<"Reports: external rows="<<count<<" elapsedMs="<<elapsed.elapsed()<<" memory="<<QJsonDocument(GetProcessMeasurements()).toJson(QJsonDocument::Compact).constData()<<std::endl;
}

}
void CheckReportWorkflow()
{
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
    Require(table->model()->data(table->model()->index(0,0)).toString()=="1202","numeric sorting covers the complete table, not the current page");
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
    compareView.AddReport(nativeA);compareView.findChild<QPushButton*>("setReportBaseline")->click();compareView.AddReport(nativeB);compareView.SelectReport(nativeB.id);
    QTimer::singleShot(0,[]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());Require(dialog!=nullptr,"comparison dialog opens");
        dialog->findChild<QComboBox*>("reportMatchMode")->setCurrentIndex(1);dialog->findChild<QCheckBox*>("reportMatchConfirmed")->setChecked(true);dialog->accept();
    });
    compareView.findChild<QPushButton*>("compareReports")->click();
    WaitTasks(compareView);
    Require(compareView.GetSelectedReport().category=="对比结果" && compareView.GetSelectedReport().table->Read(0)[4]=="3","actual report UI creates and selects comparison without input data");
    compareView.resize(1180,720);compareView.show();QCoreApplication::processEvents();compareView.grab().save(QDir::current().filePath("report-workflow.png"));compareView.hide();
    view.hide();CheckMixedSortAndCancellation();CheckExternalReportRuns();CheckReportRepairs();std::cout<<"PASS: Report CSV, units, matching, selection, full-table paging and responsive sorting"<<std::endl;
}
void CheckBusinessReport(TestWindow& window,const QJsonObject& record,bool checkExport,const QString& previewPath,int timeoutMs)
{
    const auto module=record["module"].toString(),action=record["action"].toString();
    const auto summary=record["result"].toObject();
    const auto observedDeviation=summary["deviation"].toString();
    const bool requiresReport=(module=="Wall" && action=="Start") || (module=="Gap" && (action=="Start" || action=="Evaluate"))
        || (module=="Surface" && (action=="GlobalAutomatic" || action=="Determine")) || (module=="Artifact" && action=="Commit")
        || (module=="Alignment" && QStringList{"InspectGeometry","InspectSection","EvaluateSurface","EvaluateVoxelVolume"}.contains(action))
        || (module=="Alignment" && action=="DeviationResult" && !observedDeviation.isEmpty()&&!observedDeviation.endsWith(":0"));
    const bool successful=QStringList{"Succeeded","SucceededWithDisplayFailure","FullyDetermined","Published","Observed"}.contains(record["status"].toString());
    const auto provider=window.GetWorkflow().readReports.find(module);
    if(provider==window.GetWorkflow().readReports.end()){Require(!requiresReport||!successful,"successful business module has a registered native report source");return;}
    if(!successful)return; // 导出、候选等终态不等于新增一份已发布业务报告。
    QCoreApplication::processEvents();
    const auto input=window.GetSession()->GetImageDescriptor();const auto graph=window.GetWorkflow().getPublishedGraph();
    auto* view=window.findChild<ResultReport*>("resultReport");Require(view!=nullptr,"shared report window exists");
    int heartbeats=0;QTimer heartbeat;heartbeat.setInterval(10);
    QObject::connect(&heartbeat,&QTimer::timeout,view,[&]{++heartbeats;});heartbeat.start();
    QSet<QString> refs;
    for(const auto* key:{"inspection","deviation","voxelVolume","resultSet","mesh","generation","qualityReport","correctedVolume","result"}) {
        const auto ref=summary[key].toString();if(!ref.isEmpty()&&!ref.endsWith(":0"))refs.insert(ref);
    }
    // 通过实际入口请求按需生成，等待 owner 分块任务完成后复用冻结报告。
    if(requiresReport && !refs.isEmpty()) {
        std::cout<<"REPORT: preparing native "<<module.toStdString()<<std::endl;
        QString selected;for(const auto& ref:refs)if(view->GetReportAvailable(module,ref)){selected=ref;break;}
        if(!selected.isEmpty())window.GetWorkflow().onShowBusinessReport(module,selected);
        QElapsedTimer generation;generation.start();
        do {QCoreApplication::processEvents(QEventLoop::AllEvents,20);}
        while((view->property("reportGenerationBusy").toBool() || view->GetReports(module,refs).isEmpty()) && generation.elapsed()<timeoutMs);
        Require(!view->property("reportGenerationBusy").toBool(),"native report generation exceeded the scenario deadline");
        if(generation.elapsed()>heartbeat.interval()*2)Require(heartbeats>0,"native report generation blocked owner events");
    }
    const auto reports=view->GetReports(module,refs);
    if(requiresReport&&successful)Require(!reports.isEmpty(),"successful business computation must expose its native report");
    if(reports.isEmpty())return;
    auto* panel=window.GetModule(module);Require(panel!=nullptr,"business report module exists");
    const auto actions=panel->GetActions();window.GetWorkflow().onNavigate(module,actions.contains(action)?action:actions.value(0),{});
    QCoreApplication::processEvents();
    auto* open=panel->findChild<QPushButton*>("viewBusinessReport");auto* dialog=window.findChild<QDialog*>("operationReports");
    Require(open&&dialog&&open->isVisible()&&open->isEnabled(),"native report has a visible enabled user entry");
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
        Require(QDir().mkpath(QFileInfo(path).absolutePath()),"report preview output directory exists");
        const auto pixels=dialog->grab();Require(!pixels.isNull()&&pixels.save(path),"actual visible report preview screenshot saved");return path;
    };
    QTemporaryDir directory;
    int inspectionRows=0;
    for(const auto& doc:reports) {
        QTreeWidgetItem* item=nullptr;for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==doc.id){item=*it;break;}
        Require(item&&!item->isHidden(),"exact native report is visible in the report tree");tree->scrollToItem(item);tree->setCurrentItem(item);QCoreApplication::processEvents();
        Require(view->GetSelectedReport().id==doc.id,"report tree selection opens the requested frozen document");
        WaitRows(*view,timeoutMs);
        Require(view->findChild<QPushButton*>("copyReportCsv")->isEnabled() && view->findChild<QPushButton*>("exportFilteredReport")->isEnabled(),"report preview uses complete rows rather than a cancelled or failed partial scan");
        Require(view->GetSelectedReport().table->GetCount()==doc.table->GetCount(),"native business full row count");
        if(doc.module=="Wall" && summary.contains("sampleCount"))Require(static_cast<std::uint64_t>(doc.table->GetCount())==GetId(summary["sampleCount"]),"wall report count equals independent native statistics");
        if(doc.module=="Gap") {
            const auto first=window.GetWorkflow().readRecordPage(GetRef(doc.id.mid(4)),0,1,std::nullopt);
            Require(static_cast<std::uint64_t>(doc.table->GetCount())==GetId(first["total"]),"gap report covers full native table independently of UI page size");
        }
        if(doc.id.startsWith("surface:") && summary.contains("objectCount"))Require(doc.table->GetCount()==summary["objectCount"].toInt(),"surface report covers all native objects");
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
            Require(table->isVisible()&&table->model()->rowCount()==std::min<ReportIndex>(200,doc.table->GetCount()-offset),"visible report preview uses a bounded native page");
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
        previews.append(QJsonObject{{"id",doc.id},{"source",doc.source},{"result",doc.result},{"rows",doc.table->GetCount()},
            {"pageOffsets",pages},{"details",details},{"rightColumns",rightColumns},{"conditions",conditionImage},{"opened",true}});++reportIndex;
        if(checkExport) {
        const auto path=directory.filePath("native.csv");
        std::cout<<"REPORT: async full export "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
        view->ExportReportFile(path);WaitTasks(*view,timeoutMs);
        if(doc.table->GetCount()>200) {
            const auto ids=view->GetReportIds();bool cancelled=false;
            view->ImportReport(path);QTimer::singleShot(0,view,[&]{cancelled=view->property("reportTaskBusy").toBool();view->CancelTasks();});
            WaitTasks(*view,timeoutMs,false);
            Require(cancelled && view->GetReportIds()==ids && QFile::exists(path),"real report cancellation published partial rows or removed the original CSV");
        }
        const auto beforeImport=heartbeats;view->ImportReport(path);WaitTasks(*view,timeoutMs);
        if(doc.table->GetCount()>200)Require(heartbeats>beforeImport,"real report import blocked owner events");
        auto copy=view->GetSelectedReport();Require(copy.category=="导入报告","actual async import did not select its document");
        Require(copy.table->GetCount()==doc.table->GetCount(),"native full CSV count matches source");
        if(doc.valueColumn>=0)Require(copy.valueColumn==doc.valueColumn,"native/imported UI preferred value columns must match without manual repair");
        Require(copy.columns.size()==doc.columns.size(),"native CSV changed its full column set");
        for(int c=0;c<doc.columns.size();++c) {
            Require(copy.columns[c].name==doc.columns[c].name,"native CSV changed a column name");
            if(!doc.columns[c].quantity.isEmpty())Require(copy.columns[c].quantity==doc.columns[c].quantity && copy.columns[c].unit==doc.columns[c].unit,"native CSV retains automatic business quantity and unit mapping");
        }
        std::cout<<"REPORT: full row fidelity "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
        {
            const auto original=doc.table->CreateReader(),restored=copy.table->CreateReader();QElapsedTimer fidelity;fidelity.start();
            for(int row=0;row<doc.table->GetCount();++row) {
                Require(SameRow(original(row),restored(row)),"native/imported report differs in an actual full-data row");
                if(row%256==0){Require(fidelity.elapsed()<timeoutMs,"full report row validation exceeded the scenario deadline");QCoreApplication::processEvents(QEventLoop::AllEvents,1);}
            }
        }
        int value=doc.valueColumn;
        if(value<0)for(int c=0;c<doc.columns.size();++c)if(doc.columns[c].numeric){value=c;break;}
        if(value>=0 && !doc.columns[value].quantity.isEmpty() && !doc.columns[value].unit.startsWith("model")) {
            view->SelectReport(doc.id);view->findChild<QPushButton*>("setReportBaseline")->click();view->SelectReport(copy.id);
            const auto compare=[&]{QTimer::singleShot(0,view,[]{auto* mapping=qobject_cast<QDialog*>(QApplication::activeModalWidget());Require(mapping,"real report comparison mapping is not visible");mapping->accept();});view->findChild<QPushButton*>("compareReports")->click();};
            if(doc.table->GetCount()>200) {
                const auto ids=view->GetReportIds();compare();QTimer::singleShot(0,view,[&]{view->CancelTasks();});WaitTasks(*view,timeoutMs,false);
                Require(view->GetReportIds()==ids,"cancelled real report comparison published a partial result");
            }
            std::cout<<"REPORT: real UI comparison "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
            const auto beforeCompare=heartbeats;compare();WaitTasks(*view,timeoutMs);
            if(doc.table->GetCount()>200)Require(heartbeats>beforeCompare,"real report comparison blocked owner events");
            const auto compared=view->GetSelectedReport();Require(compared.category=="对比结果" && compared.table->GetCount()==2
                && compared.table->Read(0)[2]==compared.table->Read(1)[2] && compared.table->Read(0)[3]==compared.table->Read(1)[3],"real UI automatic mapping changed full-data valid counts or mean");
            view->RemoveReport(compared.id);
        }
        std::weak_ptr<ReportTable> importedStorage=copy.table;const auto importedId=copy.id;copy.table.reset();
        view->RemoveReport(importedId);Require(importedStorage.expired() && QFile::exists(path),"real imported report storage was retained or its original CSV deleted");
        view->SelectReport(doc.id);
        std::cout<<"PASS: full real report async export/import/compare and row fidelity "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
        }
        std::cout<<"PASS: visible native report "<<doc.module.toStdString()<<" rows="<<doc.table->GetCount()<<std::endl;
    }
    dialog->close();QCoreApplication::processEvents();Require(!dialog->isVisible(),"report preview closes without stopping the business session");
    if(!previewPath.isEmpty())ExportJson(previewPath+"-reports.json",{{"module",module},{"operationId",record["operationId"]},{"reports",previews}});
    if(summary.contains("items") && summary.contains("inspection"))Require(inspectionRows==summary["items"].toArray().size(),"inspection metric tables preserve all independently read items");
    const auto after=window.GetSession()->GetImageDescriptor();Require((!input && !after) || (input && after && input->dataRevision==after->dataRevision&&input->bindingRevision==after->bindingRevision),"report operations preserve exact input and binding");
    Require(window.GetWorkflow().getPublishedGraph()==graph,"report operations preserve result graph and active bindings");
}
}
