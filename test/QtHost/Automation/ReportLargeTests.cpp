#include "ReportLargeTests.h"
#include "TestCaseRunner.h"
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
#include <QFile>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
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
    const auto error=view.property("reportTaskError").toString();
    if(success&&!error.isEmpty())throw std::runtime_error(("large real report task failed: "+error).toStdString());
}
void CheckMidTaskCancel(ResultReport& view,const std::function<void()>& start,int timeoutMs,const char* phase) {
    const auto ids=view.GetReportIds();const auto selected=view.GetSelectedReport().id;
    start();
    Wait([&]{return view.property("reportTaskProgress").toULongLong()>=4096 || !view.property("reportTaskBusy").toBool();},timeoutMs);
    if(!view.property("reportTaskBusy").toBool()){
        Require(view.property("reportTaskError").toString().isEmpty(),"real task failed before cancellation could be requested");
        // 完整完成与已取消是两个合法终态；不把调度先后当成产品失败或取消证据。
        const auto added=view.GetReportIds()-ids;for(const auto& id:added)view.RemoveReport(id);view.SelectReport(selected);
        std::cout<<"INFO: real task completed before cancellation was observed: "<<phase<<"; full output is checked separately"<<std::endl;
        return;
    }
    const auto progress=view.property("reportTaskProgress").toULongLong();QElapsedTimer cancellation;cancellation.start();
    view.findChild<QPushButton*>("cancelReportTask")->click();WaitTask(view,timeoutMs,false);
    const auto error=view.property("reportTaskError").toString();Require(error.isEmpty()||error=="报告任务已取消","IO or processing failure was misreported as a successful cancellation");
    Require(view.GetReportIds()==ids && view.GetSelectedReport().id==selected,"cancelled real task published partial data or stole selection");
    std::cout<<"PASS: real mid-task cancel "<<phase<<" checkpoints="<<progress<<" cancelMs="<<cancellation.elapsed()<<std::endl;
}
void StartComparison(ResultReport& view,const ReportDocument& native,const ReportDocument& imported,int mode) {
    view.SelectReport(native.id);view.findChild<QPushButton*>("setReportBaseline")->click();view.SelectReport(imported.id);
    DrivePopupInteraction([&]{view.findChild<QPushButton*>("compareReports")->click();},[mode,&view]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog || dialog->parentWidget()!=&view || !dialog->findChild<QComboBox*>("reportMatchMode"))return false;
        dialog->findChild<QComboBox*>("reportMatchMode")->setCurrentIndex(mode);
        // 同一原生报告的原样 CSV 回读：编号与坐标来自同一份冻结结果，不推断跨计算稳定性。
        dialog->findChild<QCheckBox*>("reportMatchConfirmed")->setChecked(true);
        if(mode==2)dialog->findChild<QDoubleSpinBox*>("reportDistanceTolerance")->setValue(1e-9);
        dialog->accept();return true;
    },"large real comparison mapping dialog missing");
}
bool Valid(const ReportDocument& doc,const QStringList& row) {
    if(doc.validityColumn<0)return true;
    for(const auto& value:doc.validValues)if(value.compare(GetReportCell(row,doc.validityColumn).trimmed(),Qt::CaseInsensitive)==0)return true;
    return false;
}
void CheckPairedSummary(const ReportDocument& native,const ReportDocument& result,int mode) {
    const auto count=native.table->GetCount();
    const auto pairs=GetReportCount(result.conditions["有效配对数"]);
    const auto invalid=GetReportCount(result.conditions["无效/缺测配对数"]);
    const auto ambiguous=GetReportCount(result.conditions["歧义基准项数"]);
    Require(pairs+invalid+ambiguous==static_cast<quint64>(count),"same-result comparison lost a baseline population");
    Require(GetReportCount(result.conditions["基准侧无候选数"])==0 && GetReportCount(result.conditions["当前侧无候选数"])==0,
        "same-result comparison lost an identity or coordinate candidate");
    Require(result.table->GetCount()==count+static_cast<ReportRowId>(ambiguous),"same-result comparison lost unmatched ambiguous current rows");
    if(mode==1)Require(ambiguous==0,"unique native row identifiers became ambiguous");
    // 整表同值由导出/导入哈希验证；配对算法的逐项语义由小数据用例覆盖。
    // 大表仅抽查首尾及均匀位置，避免再次全表解析或建立第二套全量空间定位器。
    const auto read=native.table->CreateReader(),paired=result.table->CreateReader();
    for(ReportRowId sample=0;sample<std::min<ReportRowId>(count,32);++sample) {
        const auto i=count<=32?sample:(count-1)/31*sample+(count-1)%31*sample/31;
        const auto source=read(i),row=paired(i);
        Require(row[0]==source[native.identityColumn],"paired sample lost baseline identity or row order");
        if(row[7].isEmpty()){Require(mode==2 && row[6].contains("歧义") && row[4].isEmpty(),"unpaired sample lost ambiguity evidence");continue;}
        Require(row[7]==row[0],"same-result sample paired to another identifier");
        if(mode==2)Require(row[8]=="0","same-result sample has nonzero matched distance");
        double value=0;const bool valid=Valid(native,source)&&GetReportNumber(GetReportCell(source,native.valueColumn),value);
        if(valid)Require(row[2]==source[native.valueColumn] && row[3]==row[2] && row[4]=="0" && row[5]=="0","paired sample changed a valid value or delta");
        else Require(row[4].isEmpty() && row[5].isEmpty() && row[6].contains("无效"),"paired sample fabricated a valid measurement");
    }
    std::cout<<"PASS: real paired populations mode="<<mode<<" input="<<count<<" valid="<<pairs<<" invalid="<<invalid
        <<" ambiguous="<<ambiguous<<" SHA256="<<result.table->GetContentHash().toHex().constData()<<std::endl;
}
void CheckFullPairedComparisons(ResultReport& view,const ReportDocument& native,const ReportDocument& imported,int timeoutMs,int firstMode=1) {
    for(int mode=firstMode;mode<=2;++mode) {
        if(mode==2 && (native.xColumn<0||native.yColumn<0||native.zColumn<0))continue;
        // 空间比较本轮只接受 mm 坐标的整卷原生壁厚表，配对边界语义由独立小用例覆盖。
        if(mode==2)for(const auto column:{native.xColumn,native.yColumn,native.zColumn})Require(native.columns[column].unit=="mm","real spatial reference expects explicit mm coordinates");
        view.SelectReport(imported.id);
        CheckMidTaskCancel(view,[&]{StartComparison(view,native,imported,mode);},timeoutMs,mode==1?"identity comparison":"spatial comparison");
        std::cout<<"REPORT: full real paired comparison mode="<<mode<<" input="<<native.table->GetCount()<<std::endl;
        StartComparison(view,native,imported,mode);WaitTask(view,timeoutMs);
        auto result=view.GetSelectedReport();Require(result.category=="对比结果","real paired UI comparison did not select its output");
        {
            auto check=std::async(std::launch::async,[native,result,mode]{CheckPairedSummary(native,result,mode);});
            Wait([&]{return check.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;},timeoutMs);check.get();
        }
        const auto id=result.id;std::weak_ptr<ReportTable> storage=result.table;result.table.reset();view.RemoveReport(id);
        Require(storage.expired(),"removed real paired comparison retained its file or offsets");
        std::cout<<"PASS: removed real paired storage mode="<<mode<<std::endl;
    }

}

}
void CheckNativeReportCancellation(ResultReport& view,const std::function<void()>& open,int timeoutMs)
{
    struct Restore final {ResultReport& view;decltype(view.onReadReports) provider;~Restore(){view.onReadReports=provider;}} restore{view,view.onReadReports};
    std::vector<std::weak_ptr<ReportTable>> storage;bool observed=false,needsBuild=false;
    view.onReadReports=[&](const QJsonObject& record,const QSet<QString>& known){
        auto docs=restore.provider(record,known);observed=true;
        // 只有显式要求物化的候选需要经历长生成；普通冻结原生表可直接分页发布。
        for(const auto& doc:docs){needsBuild=needsBuild||bool(doc.prepareReports)
            ||(doc.materializeBeforePublish&&doc.table&&!doc.table->GetIsComplete());if(doc.table)storage.push_back(doc.table);}
        return docs;
    };
    const auto ids=view.GetReportIds();open();
    Wait([&]{return (observed&&(!view.property("reportGenerationBusy").toBool()||view.property("reportGenerationProgress").toULongLong()>=4096))
        || !view.property("reportGenerationError").toString().isEmpty() || !view.GetGenerationPending();},timeoutMs);
    if(!view.property("reportGenerationError").toString().isEmpty())throw std::runtime_error(view.property("reportGenerationError").toString().toStdString());
    if(!needsBuild){
        Require(observed&&!storage.empty()&&!view.property("reportGenerationBusy").toBool(),"readonly native catalog did not publish its complete report");
        std::cout<<"PASS: native readonly catalog completes without full-row materialization; worker cancellation is checked during full CSV/search/sort/comparison"<<std::endl;
        return;
    }
    Require(observed && view.property("reportGenerationBusy").toBool() && !storage.empty(),"real native generation did not expose partial bounded work");
    const auto progress=view.property("reportGenerationProgress").toULongLong();QElapsedTimer cancellation;cancellation.start();
    view.findChild<QPushButton*>("cancelReportTask")->click();
    Require(view.property("reportGenerationError").toString().isEmpty(),"native generation failure was misreported as cancellation");
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
        if(!view.property("reportRowsBusy").toBool()){
            Require(view.property("reportRowsError").toString().isEmpty(),"real scan failed before cancellation could be requested");
            Require(view.GetFilteredCount()==(QString::fromLatin1(phase)=="search"?0:count),"completed real scan published an incomplete row index");
            std::cout<<"INFO: real scan completed before cancellation was observed: "<<phase<<"; full traversal and bounded output checks follow"<<std::endl;
            return;
        }
        const auto progress=view.property("reportRowsProgress").toLongLong();QElapsedTimer elapsed;elapsed.start();
        view.findChild<QPushButton*>("cancelReportRows")->click();QCoreApplication::processEvents();
        Require(view.property("reportRowsError").toString().isEmpty(),"row processing failure was misreported as cancellation");
        Require(view.GetFilteredCount()==0 && !view.property("reportRowsBusy").toBool() && !view.findChild<QPushButton*>("exportFilteredReport")->isEnabled(),"cancelled real scan exposed partial or exportable rows");
        std::cout<<"PASS: real mid-scan cancel "<<phase<<" rows="<<progress<<" cancelMs="<<elapsed.elapsed()<<std::endl;
    };
    view.SelectReport(native.id);search->setText("__cancel_full_real_scan__");cancelScan("search");
    view.SelectReport(native.id);sort();cancelScan("sort");
    std::cout<<"REPORT: full real search rows="<<count<<std::endl;
    view.SelectReport(native.id);const auto needle=native.table->Read(count-1).join('\t');search->setText(needle);
    Require(view.property("reportRowsBusy").toBool(),"real full-table search did not return after asynchronous admission");
    bool searchYielded=false;QTimer::singleShot(0,&view,[&]{searchYielded=true;});
    Wait([&]{return !view.property("reportRowsBusy").toBool();},timeoutMs);
    const auto read=native.table->CreateReader();const auto found=view.CreateFilteredReader();const auto matched=view.GetFilteredCount();
    Require(view.property("reportRowsProgress").toLongLong()==count && matched>0 && found(matched-1)==count-1,"real search did not finish the full input or lost its last-row match");
    // 小用例独立验证完整匹配集合；大表保留全量生产遍历及首尾/均匀抽查，不再二次逐行解析。
    ReportRowId previousMatch=-1;const auto checks=std::min<ReportRowId>(32,matched),divisor=std::max<ReportRowId>(1,checks-1);
    for(ReportRowId sample=0;sample<checks;++sample){const auto position=(matched-1)/divisor*sample+(matched-1)%divisor*sample/divisor;const auto index=found(position);
        Require(index>previousMatch&&index<count&&read(index).join('\t').contains(needle,Qt::CaseInsensitive),"real search sample lost order or predicate");previousMatch=index;}
    Require(searchYielded,"real full-table search blocked the queued owner callback");
    std::cout<<"PASS: full real search rows="<<count<<" matched="<<matched<<std::endl;
    std::cout<<"REPORT: full real numeric sort rows="<<count<<std::endl;
    view.SelectReport(native.id);sort();Require(view.property("reportRowsBusy").toBool(),"real full-table sorting did not return after asynchronous admission");
    bool sortYielded=false;QTimer::singleShot(0,&view,[&]{sortYielded=true;});
    Wait([&]{return !view.property("reportRowsBusy").toBool();},timeoutMs);
    {
        const auto rows=view.CreateFilteredReader();Require(view.GetFilteredCount()==count,"real sorting truncated its full permutation");
        QString lastText;double lastValue=0;bool lastNumeric=false;ReportRowId lastIndex=-1;
        Require(view.property("reportRowsProgress").toLongLong()==count,"real sorting did not read its full input");
        // 排列和全序语义由小用例严格核对；大表读取首尾及均匀位置，避免额外整表解析。
        for(ReportRowId sample=0;sample<32;++sample){const auto position=(count-1)/31*sample+(count-1)%31*sample/31;const auto index=rows(position);Require(index>=0&&index<count,"real sorting returned an invalid original row");
            const auto text=GetReportCell(read(index),native.valueColumn);double value=0;const bool numeric=GetReportNumber(text,value);
            if(lastIndex>=0){Require(!(!lastNumeric&&numeric),"numeric rows appear after text rows");
                if(lastNumeric&&numeric)Require(lastValue<=value,"real numeric sort is not monotonic");
                const bool equalNumber=lastNumeric&&numeric&&lastValue==value;
                const auto order=QString::compare(lastText,text,numeric?Qt::CaseSensitive:Qt::CaseInsensitive);
                if(equalNumber||(!lastNumeric&&!numeric)){Require(order<=0,"real tied keys have inconsistent text order");if(order==0)Require(lastIndex<index,"real equal keys lost original row order");}}
            lastText=text;lastValue=value;lastNumeric=numeric;lastIndex=index;
        }
    }
    Require(sortYielded,"real full-table sort blocked the queued owner callback");view.SelectReport(native.id);
    std::cout<<"PASS: full real numeric sort (32 checks) rows="<<count<<std::endl;
    const auto bytes=QFileInfo(csv).size();CheckMidTaskCancel(view,[&]{view.ImportReport(csv);},timeoutMs,"import");
    Require(QFileInfo(csv).size()==bytes,"cancelled import altered the original CSV");
    CheckMidTaskCancel(view,[&]{view.ExportReportFile(csv);},timeoutMs,"export");Require(QFileInfo(csv).size()==bytes,"cancelled atomic export replaced the existing CSV");
    CheckFullPairedComparisons(view,native,imported,timeoutMs);
    std::cout<<"PASS: real report operations rows="<<count<<" ownerHeartbeats="<<heartbeats<<" maximumHeartbeatGapMs="<<maximumGap<<std::endl;
    view.SelectReport(native.id);
}
void CheckSavedReportComparisons(const QString& csv,bool spatialOnly) {
    QFile input(csv);if(!input.open(QIODevice::ReadOnly))throw std::runtime_error("无法打开已保存的完整报告");
    auto native=ReadReportCsv(input,"已核验原生报告的完整 CSV");
    std::cout<<"REPORT: saved full CSV rows="<<native.table->GetCount()<<" SHA256="<<native.table->GetContentHash().toHex().constData()<<std::endl;
    auto imported=native;imported.id+="-comparison-copy";
    ResultReport view;view.AddReport(native);view.AddReport(imported);view.show();
    CheckFullPairedComparisons(view,native,imported,0,spatialOnly?2:1);
}

}
