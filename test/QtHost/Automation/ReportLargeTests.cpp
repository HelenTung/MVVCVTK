#include "ReportLargeTests.h"
#include "Support/ResultReport.h"
#include "Support/ReportStorage.h"
#include "Support/JsonInput.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkStaticPointLocator.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace Manual {
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Predicate> void Wait(Predicate complete,int timeoutMs) {
    QElapsedTimer elapsed;elapsed.start();
    while(!complete() && (timeoutMs==0 || elapsed.elapsed()<timeoutMs))QCoreApplication::processEvents(QEventLoop::AllEvents,10);
    Require(complete(),"large real report operation exceeded its scenario deadline");
}
void WaitTask(ResultReport& view,int timeoutMs,bool success=true) {
    Wait([&]{return !view.property("reportTaskBusy").toBool();},timeoutMs);
    if(success)Require(view.property("reportTaskError").toString().isEmpty(),"large real report task failed");
}
void CheckMidTaskCancel(ResultReport& view,const std::function<void()>& start,int timeoutMs,const char* phase) {
    const auto ids=view.GetReportIds();const auto selected=view.GetSelectedReport().id;
    start();
    Wait([&]{return view.property("reportTaskProgress").toULongLong()>=4096 || !view.property("reportTaskBusy").toBool();},timeoutMs);
    Require(view.property("reportTaskBusy").toBool(),"real task finished before the planned in-progress cancellation");
    const auto progress=view.property("reportTaskProgress").toULongLong();QElapsedTimer cancellation;cancellation.start();
    view.findChild<QPushButton*>("cancelReportTask")->click();WaitTask(view,timeoutMs,false);
    Require(view.GetReportIds()==ids && view.GetSelectedReport().id==selected,"cancelled real task published partial data or stole selection");
    std::cout<<"PASS: real mid-task cancel "<<phase<<" checkpoints="<<progress<<" cancelMs="<<cancellation.elapsed()<<std::endl;
}
void StartComparison(ResultReport& view,const ReportDocument& native,const ReportDocument& imported,int mode) {
    view.SelectReport(native.id);view.findChild<QPushButton*>("setReportBaseline")->click();view.SelectReport(imported.id);
    QTimer::singleShot(0,&view,[mode]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());Require(dialog,"large real comparison mapping dialog missing");
        dialog->findChild<QComboBox*>("reportMatchMode")->setCurrentIndex(mode);
        // 同一原生报告的原样 CSV 回读：编号与坐标来自同一份冻结结果，不推断跨计算稳定性。
        dialog->findChild<QCheckBox*>("reportMatchConfirmed")->setChecked(true);
        if(mode==2)dialog->findChild<QDoubleSpinBox*>("reportDistanceTolerance")->setValue(1e-9);
        dialog->accept();
    });
    view.findChild<QPushButton*>("compareReports")->click();
}
bool Valid(const ReportDocument& doc,const QStringList& row) {
    if(doc.validityColumn<0)return true;
    for(const auto& value:doc.validValues)if(value.compare(GetReportCell(row,doc.validityColumn).trimmed(),Qt::CaseInsensitive)==0)return true;
    return false;
}
void CheckPairedRows(const ReportDocument& native,const ReportDocument& result,int mode,int timeoutMs) {
    const auto read=native.table->CreateReader(),paired=result.table->CreateReader();const auto count=native.table->GetCount();
    vtkNew<vtkPoints> points;vtkNew<vtkPolyData> cloud;vtkNew<vtkStaticPointLocator> locator;
    vtkNew<vtkIdList> neighbors;
    ReportDiskArray<unsigned char> neighborCounts(mode==2?count:0);
    if(mode==2) {
        // 参考定位器分块覆盖全部 B 点；每块遍历全部 A，候选数落盘累加，不截断输入。
        constexpr ReportRowId block=4000000;
        const auto position=[&](const QStringList& row,double* p){const int columns[]{native.xColumn,native.yColumn,native.zColumn};
            for(int axis=0;axis<3;++axis)Require(GetReportNumber(GetReportCell(row,columns[axis]),p[axis]),"real spatial fixture contains a missing coordinate");};
        for(ReportRowId first=0;first<count;first+=std::min(block,count-first)) {
            const auto length=std::min(block,count-first);points->SetDataTypeToDouble();points->SetNumberOfPoints(length);
            for(ReportRowId i=0;i<length;++i){double p[3];position(read(first+i),p);points->SetPoint(i,p);}
            points->Modified();cloud->SetPoints(points);cloud->Modified();locator->SetDataSet(cloud);locator->BuildLocator();
            for(ReportRowId i=0;i<count;++i){double p[3];position(read(i),p);locator->FindPointsWithinRadius(1e-9,p,neighbors);
                neighborCounts[i]=static_cast<unsigned char>(std::min<vtkIdType>(2,neighborCounts[i]+neighbors->GetNumberOfIds()));}
        }
    }
    ReportRowId validPairs=0,invalidPairs=0,ambiguous=0;
    QElapsedTimer elapsed;elapsed.start();
    for(ReportRowId i=0;i<count;++i) {
        const auto source=read(i),row=paired(i);
        Require(row[0]==source[native.identityColumn],"paired report lost the actual baseline identifier or native row order");
        double value=0;const bool valid=Valid(native,source)&&GetReportNumber(GetReportCell(source,native.valueColumn),value);
        bool unique=true;
        if(mode==2)unique=neighborCounts[i]==1;
        if(unique) {
            Require(row[7]==row[0],"same-result roundtrip paired to another identifier");
            if(mode==2)Require(row[8]=="0","same-result coordinates have nonzero matched distance");
            if(valid){Require(row[2]==source[native.valueColumn] && row[3]==row[2] && row[4]=="0" && row[5]=="0","real matched values or delta changed");++validPairs;}
            else {Require(row[4].isEmpty() && row[5].isEmpty() && row[6].contains("无效"),"invalid native measurement was turned into a valid zero difference");++invalidPairs;}
        } else {
            Require(row[7].isEmpty() && row[8].isEmpty() && row[4].isEmpty() && row[6].contains("歧义"),"coincident real points were arbitrarily paired");++ambiguous;
        }
        if(i%256==0)Require(timeoutMs==0 || elapsed.elapsed()<timeoutMs,"full paired-row validation exceeded its scenario deadline");
    }
    Require(GetId(result.conditions["有效配对数"])==static_cast<std::uint64_t>(validPairs) && GetId(result.conditions["无效/缺测配对数"])==static_cast<std::uint64_t>(invalidPairs)
        && GetId(result.conditions["歧义基准项数"])==static_cast<std::uint64_t>(ambiguous),"paired summary changed the independently checked populations");
    Require(result.table->GetCount()==count+ambiguous,"paired report dropped unpaired B rows or added duplicate rows");
    for(ReportRowId i=count;i<result.table->GetCount();++i){const auto row=paired(i);Require(row[0].isEmpty()&&!row[7].isEmpty()&&row[4].isEmpty()&&row[8].isEmpty()&&row[6].contains("歧义"),"unpaired B evidence was fabricated");}
    std::cout<<"PASS: full real paired rows mode="<<mode<<" input="<<count<<" valid="<<validPairs<<" invalid="<<invalidPairs<<" ambiguous="<<ambiguous<<std::endl;
}
}
void CheckNativeReportCancellation(ResultReport& view,const std::function<void()>& open,int timeoutMs)
{
    struct Restore final {ResultReport& view;decltype(view.onReadReports) provider;~Restore(){view.onReadReports=provider;}} restore{view,view.onReadReports};
    std::vector<std::weak_ptr<ReportTable>> storage;
    view.onReadReports=[&](const QJsonObject& record,const QSet<QString>& known){auto docs=restore.provider(record,known);for(const auto& doc:docs)if(doc.table)storage.push_back(doc.table);return docs;};
    const auto ids=view.GetReportIds();open();
    Wait([&]{return (view.property("reportGenerationBusy").toBool() && view.property("reportGenerationProgress").toULongLong()>=4096) || !view.property("reportGenerationError").toString().isEmpty();},timeoutMs);
    Require(view.property("reportGenerationBusy").toBool() && !storage.empty(),"real native generation did not expose partial bounded work");
    const auto progress=view.property("reportGenerationProgress").toULongLong();QElapsedTimer cancellation;cancellation.start();
    view.findChild<QPushButton*>("cancelReportTask")->click();
    Require(!view.property("reportGenerationBusy").toBool() && view.GetReportIds()==ids,"cancelled native generation published a partial report");
    for(const auto& weak:storage)Require(weak.expired(),"cancelled native report retained its temporary storage or source callback");
    std::cout<<"PASS: real native generation mid-cancel progress="<<progress<<" cancelMs="<<cancellation.elapsed()<<std::endl;
}
void CheckLargeReportOperations(ResultReport& view,const ReportDocument& input,const ReportDocument& imported,const QString& csv,int timeoutMs)
{
    auto native=input;
    if(native.valueColumn<0)for(int column=0;column<native.columns.size();++column)if(native.columns[column].numeric){native.valueColumn=column;break;}
    const auto count=native.table->GetCount();Require(count>4096 && native.valueColumn>=0,"large report acceptance needs the full native numeric report");
    int heartbeats=0; qint64 maximumGap=0;QElapsedTimer clock;clock.start();qint64 previous=0;
    QTimer heartbeat;heartbeat.setInterval(10);QObject::connect(&heartbeat,&QTimer::timeout,&view,[&]{const auto now=clock.elapsed();maximumGap=std::max(maximumGap,now-previous);previous=now;++heartbeats;});heartbeat.start();
    auto* search=view.findChild<QLineEdit*>("reportSearch");auto* table=view.findChild<QTableView*>("reportTable");
    const auto sort=[&]{QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Q_ARG(int,native.valueColumn));};
    const auto cancelScan=[&](const char* phase){
        Wait([&]{return view.property("reportRowsProgress").toLongLong()>=4096 || !view.property("reportRowsBusy").toBool();},timeoutMs);
        Require(view.property("reportRowsBusy").toBool(),"real scan completed before its planned in-progress cancellation");
        const auto progress=view.property("reportRowsProgress").toLongLong();QElapsedTimer elapsed;elapsed.start();
        view.findChild<QPushButton*>("cancelReportRows")->click();QCoreApplication::processEvents();
        Require(view.GetFilteredRows().empty() && !view.property("reportRowsBusy").toBool() && !view.findChild<QPushButton*>("exportFilteredReport")->isEnabled(),"cancelled real scan exposed partial or exportable rows");
        std::cout<<"PASS: real mid-scan cancel "<<phase<<" rows="<<progress<<" cancelMs="<<elapsed.elapsed()<<std::endl;
    };
    view.SelectReport(native.id);search->setText("__cancel_full_real_scan__");cancelScan("search");
    view.SelectReport(native.id);sort();cancelScan("sort");
    std::cout<<"REPORT: full real search rows="<<count<<std::endl;
    view.SelectReport(native.id);const auto needle=native.table->Read(count-1).join('\t');search->setText(needle);
    const auto beforeSearch=heartbeats;Wait([&]{return !view.property("reportRowsBusy").toBool();},timeoutMs);
    const auto read=native.table->CreateReader();const auto found=view.CreateFilteredReader();ReportRowId matched=0;bool lastFound=false;
    for(ReportRowId i=0;i<count;++i){if(read(i).join('\t').contains(needle,Qt::CaseInsensitive)){
            Require(matched<view.GetFilteredCount() && found(matched)==i,"real full-table search lost or reordered rows");++matched;if(i==count-1)lastFound=true;}
        if(i%256==0)QCoreApplication::processEvents(QEventLoop::AllEvents,1);}
    Require(matched==view.GetFilteredCount() && lastFound,"real full-table search lost rows outside the displayed page");
    Require(heartbeats>beforeSearch,"real full-table search blocked owner events");
    std::cout<<"PASS: full real search rows="<<count<<" matched="<<matched<<std::endl;
    std::cout<<"REPORT: full real numeric sort rows="<<count<<std::endl;
    view.SelectReport(native.id);sort();const auto beforeSort=heartbeats;
    Wait([&]{return !view.property("reportRowsBusy").toBool();},timeoutMs);
    {
        const auto rows=view.CreateFilteredReader();Require(view.GetFilteredCount()==count,"real sorting truncated its full permutation");
        QString lastText;double lastValue=0;bool lastNumeric=false;ReportRowId lastIndex=-1;
        // N 个范围内行号，在含原行号的严格全序中递增，便同时证明无重复与完整排列。
        for(ReportRowId position=0;position<count;++position){const auto index=rows(position);Require(index>=0&&index<count,"real sorting returned an invalid original row");
            const auto text=GetReportCell(read(index),native.valueColumn);double value=0;const bool numeric=GetReportNumber(text,value);
            if(lastIndex>=0){Require(!(!lastNumeric&&numeric),"numeric rows appear after text rows");
                if(lastNumeric&&numeric)Require(lastValue<=value,"real numeric sort is not monotonic");
                const bool equalNumber=lastNumeric&&numeric&&lastValue==value;
                const auto order=QString::compare(lastText,text,numeric?Qt::CaseSensitive:Qt::CaseInsensitive);
                if(equalNumber||(!lastNumeric&&!numeric)){Require(order<=0,"real tied keys have inconsistent text order");if(order==0)Require(lastIndex<index,"real equal keys lost original row order");}}
            lastText=text;lastValue=value;lastNumeric=numeric;lastIndex=index;
            if(index%256==0)QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        }
    }
    Require(heartbeats>beforeSort,"real full-table sort blocked owner events");view.SelectReport(native.id);
    std::cout<<"PASS: full real numeric sort rows="<<count<<std::endl;
    const auto bytes=QFileInfo(csv).size();CheckMidTaskCancel(view,[&]{view.ImportReport(csv);},timeoutMs,"import");
    Require(QFileInfo(csv).size()==bytes,"cancelled import altered the original CSV");
    CheckMidTaskCancel(view,[&]{view.ExportReportFile(csv);},timeoutMs,"export");Require(QFileInfo(csv).size()==bytes,"cancelled atomic export replaced the existing CSV");
    for(int mode=1;mode<=2;++mode) {
        if(mode==2 && (native.xColumn<0||native.yColumn<0||native.zColumn<0))continue;
        // 空间比较本轮只接受 mm 坐标的整卷原生壁厚表，参考定位器独立校验所有候选数。
        if(mode==2)for(const auto column:{native.xColumn,native.yColumn,native.zColumn})Require(native.columns[column].unit=="mm","real spatial reference expects explicit mm coordinates");
        view.SelectReport(imported.id);
        CheckMidTaskCancel(view,[&]{StartComparison(view,native,imported,mode);},timeoutMs,mode==1?"identity comparison":"spatial comparison");
        std::cout<<"REPORT: full real paired comparison mode="<<mode<<" input="<<count<<std::endl;
        StartComparison(view,native,imported,mode);WaitTask(view,timeoutMs);
        auto result=view.GetSelectedReport();Require(result.category=="对比结果","real paired UI comparison did not select its output");
        {
            auto check=std::async(std::launch::async,[native,result,mode,timeoutMs]{CheckPairedRows(native,result,mode,timeoutMs);});
            Wait([&]{return check.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},timeoutMs);check.get();
        }
        const auto id=result.id;std::weak_ptr<ReportTable> storage=result.table;result.table.reset();view.RemoveReport(id);
        Require(storage.expired(),"removed real paired comparison retained its file or offsets");
        std::cout<<"PASS: removed real paired storage mode="<<mode<<std::endl;
    }
    std::cout<<"PASS: real report operations rows="<<count<<" ownerHeartbeats="<<heartbeats<<" maximumHeartbeatGapMs="<<maximumGap<<std::endl;
    view.SelectReport(native.id);
}
}
