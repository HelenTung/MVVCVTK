// 测试用途：报告序列化和表格显示只消费运行记录，不调用业务或渲染核心。
#include "ResultReport.h"
#include "UiText.h"
#include "ReportComparison.h"
#include "ReportStorage.h"
#include <QAbstractTableModel>
#include <QTableView>
#include <QTreeWidget>
#include <QLineEdit>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QFileInfo>
#include <QFile>
#include <QDialog>
#include <numeric>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QShowEvent>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QHideEvent>
#include <future>
#include <atomic>
#include <chrono>
#include <queue>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <algorithm>
namespace Manual {
namespace {
QString CsvCell(QString value)
{
    value.replace('"', "\"\"");
    return '"' + value + '"';
}
bool IsMatrix(const QString& key)
{
    return key.contains("matrix", Qt::CaseInsensitive) || QStringList{"directionLPS", "directionRAS", "boxToSource", "localToSource", "sourceToTarget", "modelToWorld"}.contains(key);
}
bool MatchesResult(const ReportDocument& doc,const QString& result)
{
    return result.isEmpty() || doc.result==result || doc.relatedResults.contains(result);
}
bool MatchesSource(const ReportDocument& doc,const QString& source)
{
    return source.isEmpty() || doc.source==source || doc.relatedSources.contains(source);
}
bool MatchesModule(const ReportDocument& doc,const QString& module)
{
    return module.isEmpty() || doc.module==module || doc.relatedModules.contains(module);
}
QStringList GetPendingResults(const QJsonObject& record)
{
    const auto module=record["module"].toString();const auto result=record["result"].toObject();QStringList keys,refs;
    // 这里只列报告生产者的输出；mesh/source 等字段在壁厚、计量记录中是输入。
    if(module=="Wall")keys=QStringList{"result"};
    else if(module=="Surface")keys=QStringList{"mesh","generation"};
    else if(module=="Gap")keys=QStringList{"resultSet","voidTable"};
    else if(module=="Artifact")keys=QStringList{"qualityReport","correctedVolume"};
    else if(module=="Alignment")keys=QStringList{"inspection","deviation","voxelVolume"};
    for(const auto& key:keys){const auto ref=result[key].toString();if(!ref.isEmpty() && !ref.endsWith(":0"))refs.append(ref);}
    return refs;
}
}
QVector<ReportRow> GetReportRows(const QJsonObject& record)
{
    QVector<ReportRow> rows;
    const std::function<void(const QString&, const QString&, const QJsonValue&, const QString&)> append =
        [&](const QString& section, const QString& path, const QJsonValue& value, const QString& key) {
            if (value.isObject()) {
                const auto object = value.toObject();
                if (object.isEmpty()) rows.append({section,path,"object","{}"});
                for (auto it = object.begin(); it != object.end(); ++it)
                    append(section,path.isEmpty() ? it.key() : path + '.' + it.key(),it.value(),it.key());
            } else if (value.isArray()) {
                const auto array = value.toArray();
                if (array.isEmpty()) rows.append({section,path,"array","[]"});
                const int side = IsMatrix(key) && (array.size() == 9 || array.size() == 16) ? (array.size() == 9 ? 3 : 4) : 0;
                for (int index = 0; index < array.size(); ++index) {
                    const auto suffix = side ? QString("[%1,%2]").arg(index / side + 1).arg(index % side + 1) : QString("[%1]").arg(index);
                    append(section,path + suffix,array[index],key);
                }
            } else {
                QString type, text;
                if (value.isNull() || value.isUndefined()) { type = "null"; text = "未提供"; }
                else if (value.isBool()) { type = "boolean"; text = value.toBool() ? "true" : "false"; }
                else if (value.isDouble()) { type = "number"; text = QString::number(value.toDouble(),'g',17); }
                else { type = "string"; text = value.toString(); }
                rows.append({section,path,type,text});
            }
        };
    for (const auto* key : {"operationId","module","action","status","startedUtc","elapsedMs","completeCount"})
        if (record.contains(key)) append("执行信息", key, record[key], key);
    append("参数", QString(), record["parameters"], QString());
    append("结果", QString(), record["result"], QString());
    append("输入", "source", record["source"], "source");
    if (record.contains("current")) append("输入", "current", record["current"], "current");
    return rows;
}
QString GetReportCsv(const QJsonObject& record)
{
    QString text = "分组,字段,类型,值\r\n";
    for (const auto& row : GetReportRows(record))
        text += CsvCell(row.section) + ',' + CsvCell(row.field) + ',' + CsvCell(row.type) + ',' + CsvCell(row.value) + "\r\n";
    return text;
}
void ExportReportCsv(const QString& path, const QJsonObject& record)
{
    const auto bytes = QByteArray("\xef\xbb\xbf") + GetReportCsv(record).toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("CSV 报告保存失败");
}
class ResultReport::RowScan final {
public:
    // 文档留在 owner，后台读者退出后才释放；候选索引只在完整完成后发布。
    ReportDocument document;
    std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<ReportRowId>> next=std::make_shared<std::atomic<ReportRowId>>(0);
    std::shared_ptr<std::atomic<bool>> ordering=std::make_shared<std::atomic<bool>>(false);
    ReportRowId count=0;
    std::future<std::shared_ptr<ReportIndex>> future;
    ~RowScan(){cancelled->store(true);if(future.valid())future.wait();}
};
class ResultReport::Task final {
public:
    std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<qulonglong>> progress=std::make_shared<std::atomic<qulonglong>>(0);
    QVector<ReportDocument> inputs;
    std::shared_ptr<ReportIndex> rows;
    std::uint64_t viewRevision=0;
    std::future<ReportDocument> future;
};
ResultReport::~ResultReport()
{
    CancelTasks();
    // 取消点覆盖解析行和空间候选；析构等待唯一任务退出，禁止脱离线程访问窗口。
    if(m_task && m_task->future.valid())m_task->future.wait();
}
ResultReport::ResultReport(QWidget* parent) : QWidget(parent)
{
    setObjectName("resultReport");
    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout;
    auto* import = new QPushButton("导入 CSV…",this); import->setObjectName("importReportCsv"); toolbar->addWidget(import);
    auto* baseline = new QPushButton("设为基准 A",this); baseline->setObjectName("setReportBaseline"); toolbar->addWidget(baseline);
    auto* compare = new QPushButton("与基准比较…",this); compare->setObjectName("compareReports"); toolbar->addWidget(compare);
    m_copy = new QPushButton("复制选中行",this); m_copy->setObjectName("copyReportCsv"); toolbar->addWidget(m_copy);
    m_export = new QPushButton("导出完整明细…",this); m_export->setObjectName("exportReportCsv"); toolbar->addWidget(m_export);
    m_exportFiltered = new QPushButton("导出筛选结果…",this);m_exportFiltered->setObjectName("exportFilteredReport");toolbar->addWidget(m_exportFiltered);
    m_locate=new QPushButton("定位到视图",this);m_locate->setObjectName("locateReportRow");m_locate->hide();toolbar->addWidget(m_locate);
    layout->addLayout(toolbar);
    auto* reportActions=new QHBoxLayout;
    auto* associate=new QPushButton("关联到数据…",this);associate->setObjectName("associateReportSource");reportActions->addWidget(associate);
    auto* remove=new QPushButton("移除报告",this);remove->setObjectName("removeReport");reportActions->addWidget(remove);
    auto* saveConditions=new QPushButton("保存汇总与条件…",this);saveConditions->setObjectName("saveReportConditions");reportActions->addWidget(saveConditions);
    m_cancelTask=new QPushButton("取消报告任务",this);m_cancelTask->setObjectName("cancelReportTask");reportActions->addWidget(m_cancelTask);
    m_taskStatus=new QLabel(this);m_taskStatus->setObjectName("reportTaskStatus");reportActions->addWidget(m_taskStatus,1);layout->addLayout(reportActions);
    auto* failures=new QHBoxLayout;
    m_failedReport=new QComboBox(this);m_failedReport->setObjectName("failedReportOperations");failures->addWidget(m_failedReport,1);
    m_retryReport=new QPushButton("重试所选失败报告",this);m_retryReport->setObjectName("retryFailedReport");failures->addWidget(m_retryReport);layout->addLayout(failures);
    connect(m_retryReport,&QPushButton::clicked,this,[this]{
        const auto id=m_failedReport->currentData().toString();if(!m_failedReports.remove(id))return;
        RefreshFailures();ShowReports();
    });
    RefreshFailures();
    m_buildTimer=new QTimer(this);m_buildTimer->setSingleShot(true);
    connect(m_buildTimer,&QTimer::timeout,this,[this]{ContinueGeneration();});
    m_taskTimer=new QTimer(this);m_taskTimer->setInterval(10);
    connect(m_taskTimer,&QTimer::timeout,this,[this]{ContinueTask();});
    connect(m_cancelTask,&QPushButton::clicked,this,[this]{CancelTasks();m_taskStatus->setText("报告任务已取消；未加入部分结果，可重新打开报告重试。");});
    m_cancelTask->setEnabled(false);
    connect(remove,&QPushButton::clicked,this,[this]{RemoveReport(m_selected.id);});
    connect(associate,&QPushButton::clicked,this,[this]{
        if(m_selected.category!="导入报告" || !getSources)return;
        const auto sources=getSources();QStringList labels{"独立报告（解除关联）"};QStringList refs{QString()};
        for(auto it=sources.cbegin();it!=sources.cend();++it){labels.append(it.value()+" · "+it.key());refs.append(it.key());}
        bool accepted=false;const auto label=QInputDialog::getItem(this,"仅设置报告归属，不改变工作输入","数据修订",labels,0,false,&accepted);
        if(accepted)SetReportSource(m_selected.id,refs.value(labels.indexOf(label)));
    });
    connect(saveConditions,&QPushButton::clicked,this,[this]{
        if(!m_selected.table)return;const auto path=QFileDialog::getSaveFileName(this,"保存业务汇总与计算条件",{},"文本 (*.txt)");if(path.isEmpty())return;
        QSaveFile file(path);const auto bytes=(m_selected.title+"\n"+m_selected.description+"\n"+QString::fromUtf8(QJsonDocument(m_selected.conditions).toJson(QJsonDocument::Indented))).toUtf8();
        if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())QMessageBox::warning(this,"保存失败","无法保存汇总与计算条件");
    });
    auto* choices = new QHBoxLayout;
    auto* all = new QPushButton("全部报告",this); all->setObjectName("showAllReports"); choices->addWidget(all);
    m_follow = new QCheckBox("跟随最新结果",this); m_follow->setObjectName("followLatestReport"); choices->addWidget(m_follow);
    m_baselineLabel = new QLabel("尚未选择基准",this); choices->addWidget(m_baselineLabel,1); layout->addLayout(choices);
    auto* split = new QSplitter(this); layout->addWidget(split,1);
    m_reports = new QTreeWidget(split); m_reports->setObjectName("reportList"); m_reports->setHeaderLabels({"报告"}); m_reports->setMinimumWidth(200);
    auto* content = new QWidget(split); auto* body = new QVBoxLayout(content); body->setContentsMargins(0,0,0,0);
    m_summary = new QLabel("可直接导入并查看报告，无需加载体数据。",content); m_summary->setWordWrap(true); body->addWidget(m_summary);
    auto* filters = new QHBoxLayout;
    m_search = new QLineEdit(content); m_search->setObjectName("reportSearch"); m_search->setPlaceholderText("搜索整个报告"); m_search->setClearButtonEnabled(true); filters->addWidget(m_search,1);
    m_filterColumn = new QComboBox(content); m_filterColumn->setObjectName("reportFilterColumn"); filters->addWidget(m_filterColumn);
    m_minimum = new QLineEdit(content); m_minimum->setPlaceholderText("数值下限"); m_minimum->setMaximumWidth(95); filters->addWidget(m_minimum);
    m_maximum = new QLineEdit(content); m_maximum->setPlaceholderText("数值上限"); m_maximum->setMaximumWidth(95); filters->addWidget(m_maximum); body->addLayout(filters);
    m_cancelScan=new QPushButton("停止筛选 / 排序",content);m_cancelScan->setObjectName("cancelReportRows");filters->addWidget(m_cancelScan);
    m_scanTimer=new QTimer(this);m_scanTimer->setSingleShot(true);
    connect(m_scanTimer,&QTimer::timeout,this,[this]{ContinueRows();});
    connect(m_cancelScan,&QPushButton::clicked,this,[this]{
        m_scanTimer->stop();m_rowScan.reset();setProperty("reportRowsBusy",false);
        m_rows.reset();m_allRows=false;m_rowsReady=false;RefreshPage();m_range->setText("筛选 / 排序已停止");
    });
    auto* tabs = new QTabWidget(content); body->addWidget(tabs,1);
    m_table = new QTableView(tabs); m_table->setObjectName("reportTable"); m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true); tabs->addTab(m_table,"明细 / 比较");
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_conditions = new QPlainTextEdit(tabs); m_conditions->setReadOnly(true); m_conditions->setObjectName("reportConditions"); tabs->addTab(m_conditions,"汇总与计算条件");
    auto* pages = new QHBoxLayout;
    m_first=new QPushButton("首页",content);m_first->setObjectName("reportFirst");pages->addWidget(m_first);
    m_previous = new QPushButton("上一页",content); m_previous->setObjectName("reportPrevious"); pages->addWidget(m_previous);
    m_range = new QLabel(content); m_range->setObjectName("reportRange"); pages->addWidget(m_range,1);
    m_next = new QPushButton("下一页",content); m_next->setObjectName("reportNext"); pages->addWidget(m_next); body->addLayout(pages);
    m_last=new QPushButton("末页",content);m_last->setObjectName("reportLast");pages->addWidget(m_last);
    connect(m_reports,&QTreeWidget::currentItemChanged,this,[this]{SelectCurrent();});
    connect(all,&QPushButton::clicked,this,[this]{ShowReports();});
    connect(import,&QPushButton::clicked,this,[this]{
        const auto path = QFileDialog::getOpenFileName(this,"导入 UTF-8 CSV 报告",{},"CSV (*.csv);;所有文件 (*)"); if(path.isEmpty())return;
        try { ImportReport(path); } catch(const std::exception& e){QMessageBox::warning(this,"导入失败",QString::fromUtf8(e.what()));}
    });
    connect(baseline,&QPushButton::clicked,this,[this]{if(!m_selected.table)return;m_baseline=m_selected.id;m_baselineLabel->setText("基准 A · "+m_selected.title);});
    connect(compare,&QPushButton::clicked,this,[this]{try{CompareSelected();}catch(const std::exception& e){QMessageBox::warning(this,"无法比较",QString::fromUtf8(e.what()));}});
    connect(m_search,&QLineEdit::textChanged,this,[this]{RefreshRows();});
    connect(m_filterColumn,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{RefreshRows();});
    connect(m_minimum,&QLineEdit::editingFinished,this,[this]{RefreshRows();});
    connect(m_maximum,&QLineEdit::editingFinished,this,[this]{RefreshRows();});
    connect(m_previous,&QPushButton::clicked,this,[this]{if(m_page>0)--m_page;RefreshPage();});
    connect(m_first,&QPushButton::clicked,this,[this]{m_page=0;RefreshPage();});
    connect(m_next,&QPushButton::clicked,this,[this]{if(GetRowCount()>0 && m_page<(GetRowCount()-1)/200)++m_page;RefreshPage();});
    connect(m_last,&QPushButton::clicked,this,[this]{m_page=GetRowCount()>0?(GetRowCount()-1)/200:0;RefreshPage();});
    connect(m_table->horizontalHeader(),&QHeaderView::sectionClicked,this,[this](int column){
        m_sortOrder=m_sortColumn==column && m_sortOrder==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder;m_sortColumn=column;RefreshRows();
    });
    connect(m_copy,&QPushButton::clicked,this,[this]{
        if(!m_table->selectionModel() || !m_selected.table)return;
        auto selected=m_table->selectionModel()->selectedRows();std::sort(selected.begin(),selected.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()<b.row();});
        QStringList lines;for(const auto& index:selected){const auto row=m_page*200+index.row();QStringList cells;for(auto cell:m_selected.table->Read(m_allRows?row:m_rows->Read(row)))cells.append(CsvCell(cell));lines.append(cells.join(','));}
        QApplication::clipboard()->setText(lines.join("\r\n"));
    });
    const auto save=[this](bool filtered){
        if(!m_selected.table)return;const auto path=QFileDialog::getSaveFileName(this,filtered?"导出全部筛选行":"导出完整明细",{},"CSV (*.csv)");if(path.isEmpty())return;
        try{ExportReportFile(path,filtered);}
        catch(const std::exception& e){QMessageBox::warning(this,"导出失败",QString::fromUtf8(e.what()));}
    };
    connect(m_export,&QPushButton::clicked,this,[save]{save(false);});
    connect(m_exportFiltered,&QPushButton::clicked,this,[save]{save(true);});
    connect(m_locate,&QPushButton::clicked,this,[this]{
        if(!onLocate || !m_table->selectionModel())return;const auto selected=m_table->selectionModel()->selectedRows();
        if(selected.size()!=1){QMessageBox::information(this,"定位","请选择一条可定位记录。");return;}
        try{const auto row=m_page*200+selected.first().row();onLocate(m_selected,m_allRows?row:m_rows->Read(row));}catch(const std::exception& e){QMessageBox::warning(this,"无法定位",QString::fromUtf8(e.what()));}
    });
    split->setStretchFactor(1,1); RefreshPage();
}
void ResultReport::SetRecord(const QJsonObject& record)
{
    if (!record["isTerminal"].toBool()) return;
    const auto id = record["operationId"].toString();
    if(m_records.value(id)==record)return;
    m_records[id] = record;
    ReportDocument doc;doc.id="operation:"+id;doc.title=GetModuleText(record["module"].toString())+" · "+GetActionText(record["module"].toString(),record["action"].toString())+" · "+record["status"].toString();
    doc.category="运行记录";doc.source=record["source"].toObject()["dataRevision"].toString();doc.table=std::make_shared<ReportTable>();
    doc.columns={{"分组",{},{},false},{"字段",{},{},false},{"类型",{},{},false},{"值",{},{},false}};
    for(const auto& row:GetReportRows(record))doc.table->Append({row.section,row.field,row.type,row.value});
    AddReport(std::move(doc));
}
QJsonObject ResultReport::GetSelectedRecord() const { return m_records.value(m_selected.id.startsWith("operation:")?m_selected.id.mid(10):QString()); }
void ResultReport::showEvent(QShowEvent* event) { QWidget::showEvent(event); if(!m_rowsReady && m_selected.table)RefreshRows();else Refresh(); if(!m_pendingReports.isEmpty())m_buildTimer->start(0); }
void ResultReport::hideEvent(QHideEvent* event) { CancelTasks(); QWidget::hideEvent(event); }
void ResultReport::Refresh()
{
    RefreshPage();
}
namespace {
class ReportPageModel final : public QAbstractTableModel {
public:
    ReportDocument document;
    ReportRows rows;
    explicit ReportPageModel(QObject* parent) : QAbstractTableModel(parent) {}
    void SetPage(const ReportDocument& value,ReportRows page) {beginResetModel();document=value;rows=std::move(page);endResetModel();}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid()?0:static_cast<int>(rows.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid()?0:document.columns.size(); }
    QVariant data(const QModelIndex& index,int role) const override {
        if(!index.isValid() || index.model()!=this || index.row()<0 || index.row()>=rows.size()
            || index.column()<0 || index.column()>=document.columns.size() || role!=Qt::DisplayRole || !document.table)return {};
        try{return GetReportCell(document.table->Read(rows[index.row()]),index.column());}
        catch(const std::exception& e){return QString::fromUtf8(e.what());}
    }
    QVariant headerData(int section,Qt::Orientation orientation,int role) const override {
        if(role!=Qt::DisplayRole)return {};
        if(section<0 || (orientation==Qt::Horizontal?section>=document.columns.size():section>=rows.size()))return {};
        return orientation==Qt::Horizontal?QVariant(document.columns[section].name):QVariant::fromValue<qulonglong>(rows[section]+1);
    }
};
}
void ResultReport::AddReport(ReportDocument doc,bool select)
{
    if(doc.id.isEmpty() || !doc.table)return;
    doc.table->PrepareRead();
    (void)doc.table->CreateReader();
    const bool added=!m_documents.contains(doc.id);
    if(!added && doc.category!="运行记录")return; // 精确结果的表格冻结后不被后续运行替换。
    m_documents[doc.id]=doc;
    QTreeWidgetItem* group=nullptr;
    for(int i=0;i<m_reports->topLevelItemCount();++i)if(m_reports->topLevelItem(i)->text(0)==doc.category)group=m_reports->topLevelItem(i);
    if(!group){group=new QTreeWidgetItem(m_reports,{doc.category});group->setExpanded(true);}
    QTreeWidgetItem* item=nullptr;
    for(int i=0;i<group->childCount();++i)if(group->child(i)->data(0,Qt::UserRole)==doc.id)item=group->child(i);
    if(!item)item=new QTreeWidgetItem(group);
    item->setText(0,doc.title);item->setData(0,Qt::UserRole,doc.id);
    item->setToolTip(0,doc.description+"\n结果修订："+doc.result+"\n数据："+doc.source);
    item->setHidden(!MatchesSource(doc,m_sourceFilter)
        || !MatchesModule(doc,m_moduleFilter) || !MatchesResult(doc,m_resultFilter));
    if(!item->isHidden())group->setHidden(false);
    if(select && !item->isHidden() && (m_selected.id.isEmpty() || (added && m_follow->isChecked()))) {
        m_reports->setCurrentItem(item);
    } else if(added && !m_selected.id.isEmpty())m_summary->setText(m_selected.title+" · 有新报告，已保留当前选择");
    else if(m_selected.id==doc.id)SelectCurrent();
}
bool ResultReport::SelectReport(const QString& id)
{
    if(!m_documents.contains(id))return false;
    if(!MatchesSource(m_documents[id],m_sourceFilter)
        || !MatchesModule(m_documents[id],m_moduleFilter) || !MatchesResult(m_documents[id],m_resultFilter))ShowReports();
    for(int i=0;i<m_reports->topLevelItemCount();++i)for(int j=0;j<m_reports->topLevelItem(i)->childCount();++j) {
        auto* item=m_reports->topLevelItem(i)->child(j);
        if(item->data(0,Qt::UserRole)==id){m_reports->setCurrentItem(item);SelectCurrent();return true;}
    }
    return false;
}
void ResultReport::ShowReports(const QString& source,const QString& result,const QString& module)
{
    ++m_viewRevision;
    if(source!=m_sourceFilter || result!=m_resultFilter || module!=m_moduleFilter) {
        m_buildTimer->stop();m_buildingReports.clear();m_buildingOperation.clear();setProperty("reportGenerationBusy",false);
    }
    m_sourceFilter=source;m_resultFilter=result;m_moduleFilter=module;QString select;
    for(int i=0;i<m_reports->topLevelItemCount();++i) {
        auto* group=m_reports->topLevelItem(i);bool visible=false;
        for(int j=0;j<group->childCount();++j) {
            auto* item=group->child(j);const auto doc=m_documents.value(item->data(0,Qt::UserRole).toString());
            const bool match=MatchesSource(doc,source) && MatchesModule(doc,module)
                && MatchesResult(doc,result);item->setHidden(!match);visible=visible||match;
            if(select.isEmpty() && match && !result.isEmpty() && doc.category!="运行记录")select=doc.id;
        }
        group->setHidden(!visible);
    }
    if(!select.isEmpty())SelectReport(select);
    else if(!m_selected.id.isEmpty() && (!MatchesSource(m_selected,source) || !MatchesModule(m_selected,module))) {
        ClearSelection();m_summary->setText("所选数据的已有报告；尚无匹配项时，可先完成相应业务计算。");
    }
    if(!result.isEmpty() && select.isEmpty()) {
        ClearSelection();
        m_summary->setText("尚无该精确结果的业务报告，或结果明细已不可读取。");
    }
    if(isVisible() && !m_pendingReports.isEmpty())m_buildTimer->start(0);
}
void ResultReport::ImportReport(const QString& path)
{
    StartTask([path](const ReportCheckpoint& checkpoint){
        QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("无法打开 CSV 文件");
        return ReadReportCsv(file,QFileInfo(path).fileName(),checkpoint);
    });
}
void ResultReport::ExportReportFile(const QString& path,bool filtered)
{
    if(!m_selected.table || (filtered && !m_rowsReady))throw std::runtime_error("报告或完整筛选结果尚未就绪");
    const auto doc=m_selected;doc.table->PrepareRead();const bool selected=filtered && !m_allRows;
    const auto rows=selected?m_rows:std::shared_ptr<ReportIndex>{};
    if(rows)rows->PrepareRead();
    StartTask([doc,path,selected,rows](const ReportCheckpoint& checkpoint){
        if(selected){
            const auto index=rows->CreateReader();const auto read=doc.table->CreateReader();auto projection=doc;
            projection.table=std::make_shared<ReportTable>();
            projection.table->SetRows(static_cast<std::size_t>(rows->GetCount()),[index,read](ReportRowId row){return read(index(row));});
            ExportReport(path,projection,nullptr,checkpoint);
        }else ExportReport(path,doc,nullptr,checkpoint);
        return ReportDocument{};
    },{doc},rows);
}
void ResultReport::ClearSelection()
{
    const QSignalBlocker treeBlock(m_reports),filterBlock(m_filterColumn),searchBlock(m_search);
    m_reports->setCurrentItem(nullptr);m_selected={};m_conditions->clear();
    m_summary->setText("请选择报告");
    m_search->clear();m_minimum->clear();m_maximum->clear();m_filterColumn->clear();m_sortColumn=-1;
    RefreshRows();
}
void ResultReport::SelectCurrent()
{
    ++m_viewRevision;
    const auto* item=m_reports->currentItem();if(!item){ClearSelection();return;}
    const auto id=item->data(0,Qt::UserRole).toString();if(!m_documents.contains(id)){ClearSelection();return;}
    m_selected=m_documents.value(id);m_sortColumn=-1;m_page=0;
    const QSignalBlocker block(m_filterColumn),searchBlock(m_search);
    m_search->clear();m_minimum->clear();m_maximum->clear();m_filterColumn->clear();m_filterColumn->addItem("数值筛选列",-1);
    for(int i=0;i<m_selected.columns.size();++i)m_filterColumn->addItem(m_selected.columns[i].name,i);
    m_summary->setText(m_selected.title+"\n"+m_selected.description);
    m_conditions->setPlainText(QString::fromUtf8(QJsonDocument(m_selected.conditions).toJson(QJsonDocument::Indented)));
    RefreshRows();
    m_table->resizeColumnsToContents();
    for(int column=0;column<m_selected.columns.size();++column)m_table->setColumnWidth(column,std::clamp(m_table->columnWidth(column),90,220));
}
ReportRows ResultReport::GetFilteredRows() const {
    // 小规模调用方的便利接口；大数据验证和生产分页/导出使用流式读取器。
    ReportRows rows;const auto count=GetRowCount();const auto read=CreateFilteredReader();
    for(ReportRowId i=0;i<count;++i)rows.push_back(read(i));return rows;
}
ReportRowId ResultReport::GetFilteredCount() const{return GetRowCount();}
std::function<ReportRowId(ReportRowId)> ResultReport::CreateFilteredReader() const {
    if(m_allRows)return [](ReportRowId row){return row;};
    if(!m_rows)return [](ReportRowId)->ReportRowId{throw std::out_of_range("报告筛选行不存在");};
    m_rows->PrepareRead();return m_rows->CreateReader();
}
ReportRowId ResultReport::GetRowCount() const {return m_allRows && m_selected.table?m_selected.table->GetCount():(m_rows?m_rows->GetCount():0);}
QSet<QString> ResultReport::GetReportIds() const { QSet<QString> ids;for(auto it=m_documents.cbegin();it!=m_documents.cend();++it)ids.insert(it.key());return ids; }
QVector<ReportDocument> ResultReport::GetReports(const QString& module,const QSet<QString>& results) const
{
    QVector<ReportDocument> reports;
    for(const auto& doc:m_documents) {
        if(!MatchesModule(doc,module) || doc.category=="运行记录")continue;
        if(results.contains(doc.result) || std::any_of(doc.relatedResults.begin(),doc.relatedResults.end(),
            [&](const auto& ref){return results.contains(ref);}))reports.append(doc);
    }
    return reports;
}
void ResultReport::QueueReport(const QJsonObject& record)
{
    const auto id=record["operationId"].toString();
    if(id.isEmpty() || m_completedOperations.contains(id) || m_pendingReports.value(id)==record)return;
    m_pendingReports[id]=record;
    m_failedReports.remove(id);RefreshFailures();
    if(isVisible())m_buildTimer->start(0);
}
bool ResultReport::GetReportAvailable(const QString& module,const QString& result) const
{
    if(!GetReports(module,{result}).isEmpty())return true;
    if(result.isEmpty())return false;
    for(const auto& record:m_pendingReports)if(record["module"].toString()==module && GetPendingResults(record).contains(result))return true;
    return false;
}
void ResultReport::RefreshFailures()
{
    const auto selected=m_failedReport->currentData().toString();m_failedReport->clear();
    for(auto it=m_failedReports.cbegin();it!=m_failedReports.cend();++it) {
        const auto module=m_pendingReports.value(it.key())["module"].toString();
        m_failedReport->addItem(QString("%1 · 操作 %2 · %3").arg(GetModuleText(module),it.key(),it.value()),it.key());
    }
    const int index=m_failedReport->findData(selected);if(index>=0)m_failedReport->setCurrentIndex(index);
    const bool failed=!m_failedReports.isEmpty();m_failedReport->setVisible(failed);m_retryReport->setVisible(failed);
    m_retryReport->setEnabled(failed && !m_task);
    setProperty("reportFailureCount",m_failedReports.size());
}
void ResultReport::ContinueGeneration()
{
    if(!isVisible() || m_task)return;
    try {
        if(m_buildingOperation.isEmpty()) {
            for(auto it=m_pendingReports.cbegin();it!=m_pendingReports.cend();++it) {
                if(m_failedReports.contains(it.key()))continue;
                const auto& record=it.value();
                if(!m_moduleFilter.isEmpty() && record["module"].toString()!=m_moduleFilter)continue;
                const bool resultHint=m_resultFilter.isEmpty() || GetPendingResults(record).contains(m_resultFilter);
                const bool sourceHint=m_sourceFilter.isEmpty() || record["source"].toObject()["dataRevision"].toString()==m_sourceFilter
                    || record["result"].toObject()["source"].toString()==m_sourceFilter;
                // 计量报告的父对齐结果和真实来源在冻结快照中，不一定出现在运行摘要里。
                if((!resultHint || !sourceHint) && record["module"].toString()!="Alignment")continue;
                m_buildingOperation=it.key();m_buildingIndex=0;m_generationProgress=0;
                setProperty("reportGenerationProgress",qulonglong(0));setProperty("reportGenerationError",QString());
                auto documents=onReadReports?onReadReports(record,GetReportIds()+m_removedReports):QVector<ReportDocument>{};
                if((!resultHint || !sourceHint) && !std::any_of(documents.cbegin(),documents.cend(),[&](const auto& doc){
                    return MatchesSource(doc,m_sourceFilter) && MatchesResult(doc,m_resultFilter) && MatchesModule(doc,m_moduleFilter);
                })){m_buildingOperation.clear();continue;}
                m_buildingViewRevision=m_viewRevision;
                m_buildingReports=std::move(documents);
                break;
            }
            if(m_buildingOperation.isEmpty()) {setProperty("reportGenerationBusy",false);m_cancelTask->setEnabled(false);return;}
        }
        setProperty("reportGenerationBusy",true);m_cancelTask->setEnabled(true);m_taskStatus->setText("正在生成业务报告…");
        QElapsedTimer elapsed;elapsed.start();int work=0;
        while(m_buildingIndex<m_buildingReports.size() && work++<256 && elapsed.elapsed()<4) {
            ++m_generationProgress;
            auto& candidate=m_buildingReports[m_buildingIndex];
            if(candidate.prepareReports) {
                QVector<ReportDocument> prepared;
                if(!candidate.prepareReports(prepared))continue;
                m_buildingReports.removeAt(m_buildingIndex);
                for(int index=0;index<prepared.size();++index)m_buildingReports.insert(m_buildingIndex+index,std::move(prepared[index]));
                continue;
            }
            const auto table=m_buildingReports[m_buildingIndex].table;
            if(!table || table->ContinueBuild()){if(table)table->PrepareRead();++m_buildingIndex;}
        }
        setProperty("reportGenerationProgress",m_generationProgress);
        if(m_buildingIndex<m_buildingReports.size()){m_buildTimer->start(0);return;}
        auto documents=std::move(m_buildingReports);m_buildingReports.clear();
        const bool select=m_buildingViewRevision==m_viewRevision&&(m_selected.id.isEmpty()||m_follow->isChecked());
        m_completedOperations.insert(m_buildingOperation);m_pendingReports.remove(m_buildingOperation);m_buildingOperation.clear();
        QString firstNew;
        for(auto& doc:documents){
            if(firstNew.isEmpty()&&!m_documents.contains(doc.id)&&MatchesSource(doc,m_sourceFilter)
                &&MatchesResult(doc,m_resultFilter)&&MatchesModule(doc,m_moduleFilter))firstNew=doc.id;
            AddReport(std::move(doc),false);
        }
        // 空任务不重选已有文档；生成期间的用户选择也不能被旧完成覆盖。
        if(select&&!firstNew.isEmpty())SelectReport(firstNew);
        setProperty("reportGenerationBusy",false);
        m_taskStatus->setText(documents.isEmpty()?"没有新增报告；已归档、已移除或精确结果已不可读取。":"业务报告已生成");
        m_buildTimer->start(0);
    }catch(const std::exception& e) {
        const auto failed=m_buildingOperation;
        m_buildingReports.clear();m_buildingOperation.clear();setProperty("reportGenerationBusy",false);m_cancelTask->setEnabled(false);
        setProperty("reportGenerationError",QString::fromUtf8(e.what()));
        if(!failed.isEmpty())m_failedReports[failed]=QString::fromUtf8(e.what());RefreshFailures();
        m_taskStatus->setText("报告读取失败，其他报告继续生成；可选择失败项重试："+QString::fromUtf8(e.what()));
        if(isVisible())m_buildTimer->start(0);
    }
}
void ResultReport::StartTask(std::function<ReportDocument(const ReportCheckpoint&)> work,QVector<ReportDocument> inputs,std::shared_ptr<ReportIndex> rows)
{
    if(m_task)throw std::runtime_error("请等待当前报告任务完成或取消后重试");
    m_buildTimer->stop();m_buildingReports.clear();m_buildingOperation.clear();setProperty("reportGenerationBusy",false);
    m_task=std::make_unique<Task>();m_task->inputs=std::move(inputs);m_task->rows=std::move(rows);m_task->viewRevision=m_viewRevision;
    RefreshFailures();
    const auto cancelled=m_task->cancelled;const auto progress=m_task->progress;auto* owner=thread();
    // 正常推进的报告不设默认硬时限；取消仍在解析行、输出行和空间候选观察。
    m_task->future=std::async(std::launch::async,[work=std::move(work),cancelled,progress,owner] {
        const ReportCheckpoint checkpoint=[cancelled,progress] {
            if(cancelled->load())throw std::runtime_error("报告任务已取消");
            progress->fetch_add(1,std::memory_order_relaxed);
        };
        checkpoint();auto document=work(checkpoint);checkpoint();
        if(document.table)document.table->SetOwnerThread(owner);
        return document;
    });
    setProperty("reportTaskBusy",true);setProperty("reportTaskError",QString());setProperty("reportTaskProgress",qulonglong(0));
    m_taskStatus->setText("正在处理报告…");m_cancelTask->setEnabled(true);m_taskTimer->start();
}
void ResultReport::ContinueTask()
{
    if(!m_task)return;
    setProperty("reportTaskProgress",m_task->progress->load(std::memory_order_relaxed));
    if(m_task->future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
    const bool cancelled=m_task->cancelled->load();
    const bool select=m_task->viewRevision==m_viewRevision;
    try {
        auto document=m_task->future.get();m_task.reset();
        if(!cancelled && document.table) {const auto id=document.id;if(select)ShowReports();AddReport(std::move(document),select);if(select)SelectReport(id);}
        m_taskStatus->setText(cancelled?"报告任务已取消":"报告任务完成");
    }catch(const std::exception& e){m_task.reset();setProperty("reportTaskError",QString::fromUtf8(e.what()));m_taskStatus->setText(QString::fromUtf8(e.what()));}
    m_taskTimer->stop();setProperty("reportTaskBusy",false);m_cancelTask->setEnabled(false);
    RefreshFailures();
    if(!cancelled && isVisible() && !m_pendingReports.isEmpty())m_buildTimer->start(0);
}
void ResultReport::CancelTasks()
{
    if(m_rowScan){m_scanTimer->stop();m_rowScan.reset();setProperty("reportRowsBusy",false);m_rows.reset();m_allRows=false;m_rowsReady=false;RefreshPage();}
    m_buildTimer->stop();m_buildingReports.clear();m_buildingOperation.clear();m_buildingIndex=0;
    setProperty("reportGenerationBusy",false);
    if(m_task)m_task->cancelled->store(true);
    m_cancelTask->setEnabled(false);
}
void ResultReport::RemoveReport(QString id)
{
    if(!m_documents.contains(id))return;
    CancelTasks();m_removedReports.insert(id);
    if(id==m_baseline){m_baseline.clear();m_baselineLabel->setText("尚未选择基准");}
    if(id==m_selected.id)ClearSelection();
    m_documents.remove(id);if(id.startsWith("operation:"))m_records.remove(id.mid(10));
    for(int i=0;i<m_reports->topLevelItemCount();++i){auto* group=m_reports->topLevelItem(i);
        for(int j=group->childCount()-1;j>=0;--j)if(group->child(j)->data(0,Qt::UserRole).toString()==id)delete group->takeChild(j);
        if(!group->childCount())group->setHidden(true);
    }
    m_summary->setText("报告已移除；用户原始 CSV 和业务结果保留。");
}
void ResultReport::SetReportSource(const QString& id,const QString& source)
{
    auto found=m_documents.find(id);if(found==m_documents.end() || found->category!="导入报告")return;
    found->source=source;found->relatedSources.clear();
    const auto filter=m_sourceFilter,result=m_resultFilter,module=m_moduleFilter;
    if(m_selected.id==id)m_selected=found.value();ShowReports(filter,result,module);
}
void ResultReport::RefreshRows()
{
    m_scanTimer->stop();m_rowScan.reset();setProperty("reportRowsBusy",false);
    setProperty("reportRowsProgress",0);setProperty("reportRowsPhase",QString());
    setProperty("reportRowsError",QString());
    m_rows.reset();m_allRows=false;m_rowsReady=false;m_page=0;
    m_table->horizontalHeader()->setSortIndicatorShown(false);
    if(!m_selected.table){RefreshPage();return;}
    const auto search=m_search->text();const int column=m_filterColumn->currentData().toInt();
    double low=0,high=0;const bool hasLow=!m_minimum->text().trimmed().isEmpty(),hasHigh=!m_maximum->text().trimmed().isEmpty();
    if((hasLow && !GetReportNumber(m_minimum->text(),low)) || (hasHigh && !GetReportNumber(m_maximum->text(),high)) || (hasLow && hasHigh && low>high)) {
        RefreshPage();m_range->setText("请输入有效数值区间");return;
    }
    // 自然顺序无需扫描；筛选/外排由独立读者执行，不占用 owner 事件循环。
    if(search.isEmpty() && !(column>=0 && (hasLow || hasHigh)) && m_sortColumn<0){
        m_allRows=true;m_rowsReady=true;RefreshPage();return;
    }
    m_selected.table->PrepareRead();m_rowScan=std::make_unique<RowScan>();auto& scan=*m_rowScan;
    scan.document=m_selected;scan.count=m_selected.table->GetCount();
    const auto document=scan.document;const auto cancelled=scan.cancelled;const auto next=scan.next;const auto ordering=scan.ordering;
    const auto sortColumn=m_sortColumn;const auto direction=m_sortOrder;auto* owner=thread();
    scan.future=std::async(std::launch::async,[document,cancelled,next,ordering,search,column,low,high,hasLow,hasHigh,sortColumn,direction,owner]{
        const ReportCheckpoint check=[cancelled]{
            if(cancelled->load())throw std::runtime_error("报告筛选 / 排序已取消");
        };
        const auto read=document.table->CreateReader();const auto count=document.table->GetCount();
        auto rows=std::make_shared<ReportIndex>();ReportSorter sorter(direction);
        for(ReportRowId i=0;i<count;++i){
            check();
            if(sortColumn>=0)while(!sorter.GetCanAdd()){check();sorter.Continue();}
            const auto row=read(i);next->store(i+1,std::memory_order_relaxed);
            if(!search.isEmpty()&&!row.join('\t').contains(search,Qt::CaseInsensitive))continue;
            if(column>=0&&(hasLow||hasHigh)){double value;if(!GetReportNumber(GetReportCell(row,column),value)||(hasLow&&value<low)||(hasHigh&&value>high))continue;}
            if(sortColumn<0)rows->Append(i);
            else {
                ReportSortKey key{i,GetReportCell(row,sortColumn)};const auto& definition=document.columns[sortColumn];
                const bool identifier=sortColumn==document.identityColumn||definition.name=="id"||definition.name.endsWith("_id");
                key.numeric=!identifier&&GetReportNumber(key.text,key.number);sorter.Add(std::move(key));
            }
        }
        if(sortColumn>=0){ordering->store(true);sorter.Finish();while(!sorter.Continue())check();rows=sorter.GetRows();}
        check();rows->PrepareRead();rows->SetOwnerThread(owner);return rows;
    });
    setProperty("reportRowsBusy",true);RefreshPage();m_range->setText("正在筛选 / 排序完整报告…");m_scanTimer->start(0);
}
void ResultReport::ContinueRows()
{
    if(!m_rowScan)return;
    const auto next=m_rowScan->next->load(std::memory_order_relaxed);
    setProperty("reportRowsProgress",next);setProperty("reportRowsPhase",m_rowScan->ordering->load()?"order":"read");
    if(m_rowScan->future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){
        m_range->setText(QString("正在筛选 / 外部排序 · 已读取 %1 / %2").arg(next).arg(m_rowScan->count));m_scanTimer->start(1);return;
    }
    try {
        auto rows=m_rowScan->future.get();m_rowScan.reset();m_rows=std::move(rows);m_rowsReady=true;setProperty("reportRowsBusy",false);
        if(m_sortColumn>=0){m_table->horizontalHeader()->setSortIndicator(m_sortColumn,m_sortOrder);m_table->horizontalHeader()->setSortIndicatorShown(true);}
        RefreshPage();
    } catch(const std::exception& error) {
        m_rowScan.reset();setProperty("reportRowsBusy",false);m_rows.reset();RefreshPage();
        setProperty("reportRowsError",QString::fromUtf8(error.what()));m_range->setText("报告筛选 / 排序失败："+QString::fromUtf8(error.what()));
    }
}
void ResultReport::RefreshPage()
{
    const auto total=GetRowCount();m_page=total>0?std::min<ReportRowId>(m_page,(total-1)/200):0;
    // 保留模型身份；Qt5 清空全部列时不替换仍被表头/选择模型观察的对象。
    auto* model=dynamic_cast<ReportPageModel*>(m_table->model());
    if(!model){model=new ReportPageModel(m_table);m_table->setModel(model);}
    const auto offset=m_page*200,count=GetRowCount();ReportRows page;
    if(m_allRows){page.resize(static_cast<std::size_t>(std::min<ReportRowId>(200,count-offset)));std::iota(page.begin(),page.end(),offset);}
    else if(offset<count){const auto length=std::min<ReportRowId>(200,count-offset);for(ReportRowId i=0;i<length;++i)page.push_back(m_rows->Read(offset+i));}
    model->SetPage(m_selected,std::move(page));
    m_range->setText(QString("总数 %1 · 筛选后 %2 · 显示 %3–%4").arg(m_selected.table?m_selected.table->GetCount():0).arg(count).arg(count?offset+1:0).arg(offset+std::min<ReportRowId>(200,count-offset)));
    m_previous->setEnabled(!m_rowScan && m_page>0);m_first->setEnabled(m_previous->isEnabled());
    m_next->setEnabled(!m_rowScan && count>0 && m_page<(count-1)/200);m_last->setEnabled(m_next->isEnabled());
    m_copy->setEnabled(m_selected.table && m_rowsReady);m_export->setEnabled(m_selected.table!=nullptr);
    m_exportFiltered->setEnabled(m_selected.table && m_rowsReady);m_cancelScan->setEnabled(m_rowScan!=nullptr);
    m_locate->setEnabled(m_rowsReady);
    m_locate->setVisible(m_selected.module=="Gap" && m_selected.category=="本次会话结果" && bool(onLocate));
}
void ResultReport::CompareSelected()
{
    if(!m_documents.contains(m_baseline) || !m_selected.table)throw std::invalid_argument("请先选择基准 A，再选择待比较报告 B");
    auto a=m_documents.value(m_baseline),b=m_selected;
    if(a.id==b.id)throw std::invalid_argument("请选择另一份报告作为 B");
    QDialog dialog(this);dialog.setWindowTitle("核对比较指标、单位与匹配依据");auto* layout=new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("A · "+a.title+"\nB · "+b.title+"\n编号只有在确认为同一业务对象时才能配对；空间匹配需要同一坐标系。",&dialog));
    struct Mapping { QComboBox *value,*identity,*validity,*x,*y,*z,*unit,*coordinateUnit;QLineEdit *quantity,*validValues; };
    const auto addMapping=[&](const QString& title,const ReportDocument& doc,const QString& prefix) {
        auto* box=new QWidget(&dialog);auto* form=new QFormLayout(box);form->addRow(new QLabel(title,box));Mapping m;
        const auto combo=[&](int selected){auto* c=new QComboBox(box);c->addItem("未指定",-1);for(int i=0;i<doc.columns.size();++i)c->addItem(doc.columns[i].name,i);c->setCurrentIndex(selected+1);return c;};
        int numeric=doc.valueColumn;if(numeric<0)for(int i=0;i<doc.columns.size();++i)if(doc.columns[i].numeric){numeric=i;break;}
        m.value=combo(numeric);m.identity=combo(doc.identityColumn);m.validity=combo(doc.validityColumn);
        m.value->setObjectName(prefix+"ValueColumn");
        m.x=combo(doc.xColumn);m.y=combo(doc.yColumn);m.z=combo(doc.zColumn);
        m.unit=new QComboBox(box);m.unit->setEditable(true);m.unit->addItems({"","mm","m","mm2","m2","mm3","m3","deg","1"});
        m.coordinateUnit=new QComboBox(box);m.coordinateUnit->setEditable(true);m.coordinateUnit->addItems({"mm","m","cm","um"});
        m.quantity=new QLineEdit(box);
        m.validValues=new QLineEdit(doc.validValues.join(','),box);
        const auto refresh=[doc,m]{const int index=m.value->currentData().toInt();if(index>=0){m.unit->setCurrentText(doc.columns[index].unit);m.quantity->setText(doc.columns[index].quantity);m.unit->setEnabled(doc.category!="本次会话结果");m.quantity->setReadOnly(doc.category=="本次会话结果");}};
        connect(m.value,qOverload<int>(&QComboBox::currentIndexChanged),box,refresh);refresh();
        if(doc.xColumn>=0)m.coordinateUnit->setCurrentText(doc.columns[doc.xColumn].unit);
        form->addRow("数值列",m.value);form->addRow("指标含义",m.quantity);form->addRow("数值单位",m.unit);form->addRow("业务标识列",m.identity);
        form->addRow("有效性列",m.validity);form->addRow("有效值（逗号分隔）",m.validValues);form->addRow("X / Y / Z 坐标",m.x);form->addRow(m.y);form->addRow(m.z);form->addRow("坐标单位",m.coordinateUnit);
        layout->addWidget(box);return m;
    };
    auto* sideBySide=new QHBoxLayout;layout->addLayout(sideBySide);
    // 两个映射面板并排，避免在小屏幕上形成过长向导。
    const auto am=addMapping("基准 A",a,"reportBaseline");sideBySide->addWidget(am.value->parentWidget());
    const auto bm=addMapping("待比较 B",b,"reportCurrent");sideBySide->addWidget(bm.value->parentWidget());
    auto* form=new QFormLayout;layout->addLayout(form);
    auto* mode=new QComboBox(&dialog);mode->setObjectName("reportMatchMode");mode->addItems({"仅分布比较","稳定业务标识","唯一空间匹配"});form->addRow("匹配方式",mode);
    auto* confirmed=new QCheckBox("已确认编号跨报告稳定，或坐标属于同一坐标系（并已核对坐标单位）",&dialog);confirmed->setObjectName("reportMatchConfirmed");layout->addWidget(confirmed);
    auto* mapping=new QCheckBox("已核对重名列的映射与有效性定义",&dialog);mapping->setVisible(a.mappingRequired || b.mappingRequired);layout->addWidget(mapping);
    auto* distance=new QDoubleSpinBox(&dialog);distance->setObjectName("reportDistanceTolerance");distance->setDecimals(9);distance->setRange(.000000001,1e12);distance->setValue(.01);form->addRow("空间距离容差 mm",distance);
    auto* tolerance=new QDoubleSpinBox(&dialog);tolerance->setDecimals(9);tolerance->setRange(0,1e12);form->addRow("数值容差（基准单位）",tolerance);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    const auto map=[&](ReportDocument& doc,const Mapping& m){doc.validityColumn=m.validity->currentData().toInt();doc.validValues=m.validValues->text().split(',',Qt::SkipEmptyParts);for(auto& value:doc.validValues)value=value.trimmed();doc.xColumn=m.x->currentData().toInt();doc.yColumn=m.y->currentData().toInt();doc.zColumn=m.z->currentData().toInt();};map(a,am);map(b,bm);
    ReportComparisonOptions o;o.baselineColumn=am.value->currentData().toInt();o.currentColumn=bm.value->currentData().toInt();
    o.baselineIdentity=am.identity->currentData().toInt();o.currentIdentity=bm.identity->currentData().toInt();
    o.baselineQuantity=am.quantity->text().trimmed();o.currentQuantity=bm.quantity->text().trimmed();o.baselineUnit=am.unit->currentText();o.currentUnit=bm.unit->currentText();
    o.match=mode->currentIndex()==1?ReportMatch::Identity:mode->currentIndex()==2?ReportMatch::Position:ReportMatch::Distribution;
    o.mappingConfirmed=mapping->isChecked();o.identitiesConfirmed=o.coordinatesConfirmed=confirmed->isChecked();o.baselineCoordinateUnit=am.coordinateUnit->currentText();o.currentCoordinateUnit=bm.coordinateUnit->currentText();
    o.distanceTolerance=distance->value();o.valueTolerance=tolerance->value();
    a.table->PrepareRead();b.table->PrepareRead();
    StartTask([a,b,o](const ReportCheckpoint& checkpoint){return CompareReports(a,b,o,checkpoint);},{a,b});
}
}
