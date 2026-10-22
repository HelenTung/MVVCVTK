// 测试用途：报告序列化和表格显示只消费运行记录，不调用业务或渲染核心。
#include "ResultReport.h"
#include "UiText.h"
#include "ReportComparison.h"
#include "ReportStorage.h"
#include <QAbstractTableModel>
#include <QTableView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QTreeWidget>
#include <QLineEdit>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QFileInfo>
#include <QFile>
#include <QDialog>
#include <numeric>
#include <QApplication>
#include <QAction>
#include <QDesktopServices>
#include <QUrl>
#include <QProgressBar>
#include <QLocale>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QToolTip>
#include <QHelpEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOption>
#include <QPushButton>
#include <QSaveFile>
#include <QShowEvent>
#include <QCloseEvent>
#include <condition_variable>
#include <mutex>
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

// QLabel 保留完整原文供自动化和辅助技术读取，只在绘制时省略，避免状态撑高停靠面板。
class ReportLineLabel final : public QLabel {
public:
    explicit ReportLineLabel(const QString& text,QWidget* parent) : QLabel(text,parent) {
        setTextFormat(Qt::PlainText);setWordWrap(false);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    }
    QSize sizeHint() const override {
        auto size=QLabel::sizeHint();const auto margins=contentsMargins();
        size.setHeight(fontMetrics().height()+2*margin()+margins.top()+margins.bottom());return size;
    }
    QSize minimumSizeHint() const override {auto size=sizeHint();size.setWidth(0);return size;}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);QStyleOption option;option.initFrom(this);style()->drawPrimitive(QStyle::PE_Widget,&option,&painter,this);
        const auto line=QString(text()).replace('\n',QStringLiteral(" · "));
        style()->drawItemText(&painter,contentsRect(),alignment()|Qt::TextSingleLine,palette(),isEnabled(),
            fontMetrics().elidedText(line,Qt::ElideRight,contentsRect().width()),foregroundRole());
    }
    bool event(QEvent* event) override {
        if(event->type()==QEvent::ToolTip&&!text().isEmpty()) {
            QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(),text(),this);return true;
        }
        return QLabel::event(event);
    }
};

// 停靠布局压缩时仍保留表头、完整首行及横向滚动条，尺寸随当前字体和样式计算。
class ReportTableView final : public QTableView {
public:
    using QTableView::QTableView;
    QSize minimumSizeHint() const override {
        auto size=QTableView::minimumSizeHint();
        size.setHeight(horizontalHeader()->sizeHint().height()+verticalHeader()->defaultSectionSize()
            +horizontalScrollBar()->sizeHint().height()+2*frameWidth());return size;
    }
};

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
    std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
    ReportDocument document;
    std::shared_ptr<std::atomic<ReportRowId>> next=std::make_shared<std::atomic<ReportRowId>>(0);
    std::shared_ptr<std::atomic<bool>> ordering=std::make_shared<std::atomic<bool>>(false);
    ReportRowId count=0;
    std::uint64_t request=0;
    std::function<ReportRows()> work;
    std::future<ReportRows> future;
    ~RowScan()=default;
};
namespace {
struct SortKey final { ReportRowId row; ReportTextRef text; double number=0; bool numeric=false; };
ReportRows ReadReportRows(const ReportDocument& doc,const QString& search,int column,int sortColumn,
    double low,double high,bool hasLow,bool hasHigh,Qt::SortOrder direction,const ReportCheckpoint& check,
    const std::shared_ptr<std::atomic<ReportRowId>>& progress,const std::shared_ptr<std::atomic<bool>>& ordering)
{
    const auto read=doc.table->CreateReader();
    PagedArray<ReportRowId> rows;PagedArray<SortKey> keys;ReportTextStore texts;
    for(ReportRowId index=0;index<doc.table->GetCount();++index) {
        check();const auto row=read(index);progress->store(index+1,std::memory_order_relaxed);
        if(!search.isEmpty() && !row.join('\t').contains(search,Qt::CaseInsensitive))continue;
        if(column>=0 && (hasLow || hasHigh)){double value;if(!GetReportNumber(GetReportCell(row,column),value) || (hasLow && value<low) || (hasHigh && value>high))continue;}
        if(sortColumn<0)rows.push_back(index);
        else {
            const auto text=GetReportCell(row,sortColumn);SortKey key{index,texts.Append(text)};
            const auto& definition=doc.columns[sortColumn];
            const bool identifier=sortColumn==doc.identityColumn || definition.name=="id" || definition.name.endsWith("_id");
            key.numeric=!identifier && GetReportNumber(text,key.number);keys.push_back(key);
        }
    }
    if(sortColumn>=0) {
        ordering->store(true);texts.Finish();
        SortPagedArray(keys,[&](const SortKey& a,const SortKey& b) {
            int order;
            if(a.numeric!=b.numeric)order=a.numeric?-1:1;
            else if(a.numeric && a.number!=b.number)order=a.number<b.number?-1:1;
            else order=QString::compare(texts.Read(a.text),texts.Read(b.text),a.numeric?Qt::CaseSensitive:Qt::CaseInsensitive);
            // 两种方向均保留等键的原行顺序。
            if(!order)return a.row<b.row;
            return direction==Qt::AscendingOrder?order<0:order>0;
        },check);
        rows.reserve(keys.size());for(const auto& key:keys){check();rows.push_back(key.row);}
    }
    check();return rows.Finish();
}
}
class ResultReport::ExportState final {
public:
    enum class Phase { Writing, Cancelled, Committing, Committed };
    QString path;
    ReportDocument document;
    ReportRowId rowCount=0;
    std::atomic<ReportRowId> rowsWritten{0};
    std::atomic<bool> committed{false},readFailed{false};
    // CAS 决定取消或提交谁先接纳；owner 不等待工作线程上的磁盘 commit。
    std::atomic<Phase> phase{Phase::Writing};
    const std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::hours(24);
};
class ResultReport::Task final {
public:
    struct Gate final {std::mutex mutex;std::condition_variable changed;bool ready=false;std::exception_ptr failure;};
    std::shared_ptr<Gate> gate=std::make_shared<Gate>();
    std::uint64_t request=0;
    void Cancel(){
        {std::lock_guard<std::mutex> lock(gate->mutex);
            if(exportState){auto expected=ExportState::Phase::Writing;
                if(exportState->phase.compare_exchange_strong(expected,ExportState::Phase::Cancelled)
                    || expected==ExportState::Phase::Cancelled)cancelled->store(true);}
            else cancelled->store(true);}
        gate->changed.notify_all();
    }
    ~Task(){Cancel();if(future.valid())future.wait();}

    std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<qulonglong>> progress=std::make_shared<std::atomic<qulonglong>>(0);
    QVector<ReportDocument> inputs;
    std::shared_ptr<ExportState> exportState;
    std::uint64_t viewRevision=0;
    QString nativeOperation;
    int preparing=0;
    bool inputsReady=false;
    std::future<QVector<ReportDocument>> future;
};
ResultReport::~ResultReport()
{
    m_acceptingRequests=false;CancelTasks();
    m_buildTimer->stop();m_taskTimer->stop();m_scanTimer->stop();
    // 关闭收尾才等待；工作闭包只持有输入和控制对象，不访问窗口。
    if(m_task && m_task->future.valid())m_task->future.wait();
    if(m_rowScan && m_rowScan->future.valid())m_rowScan->future.wait();
    m_task.reset();m_rowScan.reset();m_pendingRowScan.reset();
}
ResultReport::ResultReport(QWidget* parent) : QWidget(parent)
{
    setObjectName("resultReport");
    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout;
    auto* import = new QPushButton("导入 CSV…",this); import->setObjectName("importReportCsv"); toolbar->addWidget(import);
    m_export = new QPushButton("导出全部…",this); m_export->setObjectName("exportReportCsv"); m_export->setProperty("primaryCommand",true); toolbar->addWidget(m_export);
    m_exportSelection = new QPushButton("导出选中行…",this); m_exportSelection->setObjectName("exportSelectedReport"); toolbar->addWidget(m_exportSelection);
    m_exportSelection->setToolTip("将当前页选中的行保存为 CSV，保留表头、单位和原始值");
    m_export->setToolTip("将当前报告的全部行保存为 CSV，不受分页和筛选影响");
    auto* tools=new QToolButton(this);tools->setObjectName("reportActions");tools->setText("报告操作");tools->setFocusPolicy(Qt::StrongFocus);
    tools->setToolTip("比较报告、导出筛选结果、关联数据与保存汇总");tools->setPopupMode(QToolButton::InstantPopup);
    auto* actions=new QMenu(tools);tools->setMenu(actions);toolbar->addWidget(tools);
    auto* baseline=actions->addAction("设为基准 A");baseline->setObjectName("setReportBaseline");
    auto* compare=actions->addAction("与基准比较…");compare->setObjectName("compareReports");actions->addSeparator();
    m_exportFiltered=actions->addAction("导出筛选结果…");m_exportFiltered->setObjectName("exportFilteredReport");
    m_locate=actions->addAction("定位到视图");m_locate->setObjectName("locateReportRow");m_locate->setVisible(false);actions->addSeparator();
    auto* associate=actions->addAction("关联到数据…");associate->setObjectName("associateReportSource");
    auto* saveConditions=actions->addAction("保存汇总与条件…");saveConditions->setObjectName("saveReportConditions");
    auto* remove=actions->addAction("移除报告");remove->setObjectName("removeReport");
    m_cancelTask=new QPushButton("取消报告任务",this);m_cancelTask->setObjectName("cancelReportTask");toolbar->addWidget(m_cancelTask);
    layout->addLayout(toolbar);
    auto* taskFeedback=new QHBoxLayout;
    m_taskStatus=new ReportLineLabel(QString(),this);m_taskStatus->setObjectName("reportTaskStatus");taskFeedback->addWidget(m_taskStatus,1);
    m_exportProgress=new QProgressBar(this);m_exportProgress->setObjectName("reportExportProgress");m_exportProgress->setAccessibleName("报告导出进度");m_exportProgress->setLocale(QLocale::c());m_exportProgress->setRange(0,1000);m_exportProgress->setMaximumWidth(160);m_exportProgress->hide();taskFeedback->addWidget(m_exportProgress);
    m_openExportFolder=new QPushButton("打开所在文件夹",this);m_openExportFolder->setObjectName("openReportExportFolder");m_openExportFolder->hide();taskFeedback->addWidget(m_openExportFolder);
    toolbar->insertLayout(toolbar->count()-1,taskFeedback,1);
    connect(m_openExportFolder,&QPushButton::clicked,this,[this]{
        const auto path=property("reportExportPath").toString();
        if(!path.isEmpty()&&!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())))m_taskStatus->setText("无法打开所在文件夹，请从保存位置查看报告。");
    });
    auto* failures=new QHBoxLayout;
    m_failedReport=new QComboBox(this);m_failedReport->setObjectName("failedReportOperations");failures->addWidget(m_failedReport,1);
    m_retryReport=new QPushButton("重试所选失败报告",this);m_retryReport->setObjectName("retryFailedReport");failures->addWidget(m_retryReport);layout->addLayout(failures);
    connect(m_retryReport,&QPushButton::clicked,this,[this]{
        const auto id=m_failedReport->currentData().toString();if(!m_failedReports.remove(id))return;
        RefreshFailures();ShowReports();
    });
    RefreshFailures();
    m_buildTimer=new QTimer(this);m_buildTimer->setSingleShot(true);m_buildTimer->setTimerType(Qt::CoarseTimer);
    connect(m_buildTimer,&QTimer::timeout,this,[this]{ContinueGeneration();});
    // Qt 5.14.2 Win32 的短定时器依赖投递事件唤醒；完成/取消收口使用原生 WM_TIMER。
    // 证据：out/final-confluence-post-stall/diagnosis/merged/native-wakeup/；输入分块额度与取消门槛不变。
    m_taskTimer=new QTimer(this);m_taskTimer->setTimerType(Qt::CoarseTimer);m_taskTimer->setInterval(20);
    connect(m_taskTimer,&QTimer::timeout,this,[this]{ContinueTask();});
    connect(m_cancelTask,&QPushButton::clicked,this,[this]{CancelTasks();m_taskStatus->setText("已请求取消，正在回收后台任务…");});
    m_cancelTask->setEnabled(false);
    connect(remove,&QAction::triggered,this,[this]{RemoveReport(m_selected.id);});
    connect(associate,&QAction::triggered,this,[this]{
        if(m_selected.category!="导入报告" || !getSources)return;
        const auto sources=getSources();QStringList labels{"独立报告（解除关联）"};QStringList refs{QString()};
        for(auto it=sources.cbegin();it!=sources.cend();++it){labels.append(it.value()+" · "+it.key());refs.append(it.key());}
        bool accepted=false;const auto label=QInputDialog::getItem(this,"仅设置报告归属，不改变工作输入","数据修订",labels,0,false,&accepted);
        if(accepted)SetReportSource(m_selected.id,refs.value(labels.indexOf(label)));
    });
    connect(saveConditions,&QAction::triggered,this,[this]{
        if(!m_selected.table)return;const auto path=QFileDialog::getSaveFileName(this,"保存业务汇总与计算条件",{},"文本 (*.txt)");if(path.isEmpty())return;
        QSaveFile file(path);const auto bytes=(m_selected.title+"\n"+m_selected.description+"\n"+QString::fromUtf8(QJsonDocument(m_selected.conditions).toJson(QJsonDocument::Indented))).toUtf8();
        if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())QMessageBox::warning(this,"保存失败","无法保存汇总与计算条件");
    });
    auto* split = new QSplitter(this);split->setChildrenCollapsible(false);layout->addWidget(split,1);
    auto* list=new QWidget(split);auto* choices=new QVBoxLayout(list);choices->setContentsMargins(0,0,0,0);
    auto* all=new QPushButton("全部报告",list);all->setObjectName("showAllReports");choices->addWidget(all);
    m_reports=new QTreeWidget(list);m_reports->setObjectName("reportList");m_reports->setHeaderLabels({"报告"});m_reports->setMinimumWidth(160);choices->addWidget(m_reports,1);
    m_follow=new QCheckBox("跟随最新结果",list);m_follow->setObjectName("followLatestReport");choices->addWidget(m_follow);
    m_baselineLabel=new ReportLineLabel("尚未选择基准",list);choices->addWidget(m_baselineLabel);
    auto* content = new QWidget(split); auto* body = new QVBoxLayout(content); body->setContentsMargins(0,0,0,0);
    m_summary = new ReportLineLabel("可直接导入并查看报告，无需加载体数据。",content);body->addWidget(m_summary);
    auto* filters = new QHBoxLayout;
    m_search = new QLineEdit(content); m_search->setObjectName("reportSearch"); m_search->setPlaceholderText("搜索整个报告"); m_search->setClearButtonEnabled(true);m_search->setMinimumWidth(60);filters->addWidget(m_search,1);
    m_filterColumn = new QComboBox(content); m_filterColumn->setObjectName("reportFilterColumn");m_filterColumn->setMinimumContentsLength(8);m_filterColumn->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);filters->addWidget(m_filterColumn);
    m_minimum = new QLineEdit(content); m_minimum->setPlaceholderText("数值下限"); m_minimum->setMaximumWidth(95); filters->addWidget(m_minimum);
    m_maximum = new QLineEdit(content); m_maximum->setPlaceholderText("数值上限"); m_maximum->setMaximumWidth(95); filters->addWidget(m_maximum); body->addLayout(filters);
    m_cancelScan=new QPushButton("停止筛选 / 排序",content);m_cancelScan->setObjectName("cancelReportRows");filters->addWidget(m_cancelScan);
    m_scanTimer=new QTimer(this);m_scanTimer->setSingleShot(true);m_scanTimer->setTimerType(Qt::CoarseTimer);
    connect(m_scanTimer,&QTimer::timeout,this,[this]{ContinueRows();});
    connect(m_cancelScan,&QPushButton::clicked,this,[this]{CancelRows();m_range->setText("筛选 / 排序已停止");});
    auto* tabs = new QTabWidget(content); body->addWidget(tabs,1);
    m_table = new ReportTableView(tabs); m_table->setObjectName("reportTable"); m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true); tabs->addTab(m_table,"明细 / 比较");
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_conditions = new QPlainTextEdit(tabs); m_conditions->setReadOnly(true); m_conditions->setObjectName("reportConditions"); tabs->addTab(m_conditions,"汇总与计算条件");
    auto* pages = new QHBoxLayout;
    m_first=new QPushButton("首页",content);m_first->setObjectName("reportFirst");pages->addWidget(m_first);
    m_previous = new QPushButton("上一页",content); m_previous->setObjectName("reportPrevious"); pages->addWidget(m_previous);
    m_range = new ReportLineLabel(QString(),content); m_range->setObjectName("reportRange"); pages->addWidget(m_range,1);
    m_next = new QPushButton("下一页",content); m_next->setObjectName("reportNext"); pages->addWidget(m_next); body->addLayout(pages);
    m_last=new QPushButton("末页",content);m_last->setObjectName("reportLast");pages->addWidget(m_last);
    connect(m_reports,&QTreeWidget::currentItemChanged,this,[this]{SelectCurrent();});
    connect(all,&QPushButton::clicked,this,[this]{ShowReports();});
    connect(import,&QPushButton::clicked,this,[this]{
        const auto path = QFileDialog::getOpenFileName(this,"导入 UTF-8 CSV 报告",{},"CSV (*.csv);;所有文件 (*)"); if(path.isEmpty())return;
        try { ImportReport(path); } catch(const std::exception& e){QMessageBox::warning(this,"导入失败",QString::fromUtf8(e.what()));}
    });
    connect(baseline,&QAction::triggered,this,[this]{if(!m_selected.table)return;m_baseline=m_selected.id;m_baselineLabel->setText("基准 A · "+m_selected.title);});
    connect(compare,&QAction::triggered,this,[this]{try{CompareSelected();}catch(const std::exception& e){QMessageBox::warning(this,"无法比较",QString::fromUtf8(e.what()));}});
    connect(m_search,&QLineEdit::textChanged,this,[this]{RefreshRows();});
    connect(m_filterColumn,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{RefreshRows();});
    connect(m_minimum,&QLineEdit::editingFinished,this,[this]{RefreshRows();});
    connect(m_maximum,&QLineEdit::editingFinished,this,[this]{RefreshRows();});
    connect(m_previous,&QPushButton::clicked,this,[this]{if(m_page>0)--m_page;RefreshPage();});
    connect(m_first,&QPushButton::clicked,this,[this]{m_page=0;RefreshPage();});
    connect(m_next,&QPushButton::clicked,this,[this]{if(GetRowCount()-m_page*200>200)++m_page;RefreshPage();});
    connect(m_last,&QPushButton::clicked,this,[this]{m_page=GetRowCount()>0?(GetRowCount()-1)/200:0;RefreshPage();});
    connect(m_table->horizontalHeader(),&QHeaderView::sectionClicked,this,[this](int column){
        m_sortOrder=m_sortColumn==column && m_sortOrder==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder;m_sortColumn=column;RefreshRows();
    });
    connect(m_export,&QPushButton::clicked,this,[this]{SelectExportPath(false);});
    connect(m_exportSelection,&QPushButton::clicked,this,[this]{SelectExportPath(true);});
    connect(m_exportFiltered,&QAction::triggered,this,[this]{SelectExportPath(false,true);});
    // 表格上下文只提供与选中行有关的操作，整份报告从工具栏导出。
    m_exportSelectionAction=new QAction("导出选中行…",m_table);m_exportSelectionAction->setObjectName("exportSelectedReportAction");
    m_table->addAction(m_exportSelectionAction);m_table->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_table->installEventFilter(this);m_table->viewport()->installEventFilter(this);
    connect(m_exportSelectionAction,&QAction::triggered,this,[this]{SelectExportPath(true);});
    connect(m_locate,&QAction::triggered,this,[this]{
        if(!m_pageReady || !onLocate || !m_table->selectionModel())return;const auto selected=m_table->selectionModel()->selectedRows();
        if(selected.size()!=1){QMessageBox::information(this,"定位","请选择一条可定位记录。");return;}
        ReportRowId original;
        try{const auto row=m_page*200+selected.first().row();original=m_allRows?row:m_rows[static_cast<std::size_t>(row)];}
        catch(const std::exception& e){SetReadFailure(QString::fromUtf8(e.what()));return;}
        try{onLocate(m_selected,original);}catch(const std::exception& e){QMessageBox::warning(this,"无法定位",QString::fromUtf8(e.what()));}
    });
    split->setStretchFactor(0,0);split->setStretchFactor(1,1);split->setSizes({220,780});RefreshPage();
    connect(m_table->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{RefreshExportActions();});
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
bool ResultReport::eventFilter(QObject* object,QEvent* event)
{
    // Qt 5.14 的 item view 自带 Copy 快捷键，会直接写剪贴板；报告表格统一从文件导出。
    if((object==m_table || object==m_table->viewport()) && (event->type()==QEvent::KeyPress || event->type()==QEvent::ShortcutOverride)
        &&static_cast<QKeyEvent*>(event)->matches(QKeySequence::Copy)) {event->accept();return true;}
    return QWidget::eventFilter(object,event);
}
void ResultReport::showEvent(QShowEvent* event) { m_acceptingRequests=true;QWidget::showEvent(event); if(!m_rowsReady && m_selected.table)RefreshRows();else Refresh(); if(!m_pendingReports.isEmpty())m_buildTimer->start(0); }
void ResultReport::hideEvent(QHideEvent* event) { CancelTasks(); QWidget::hideEvent(event); }
void ResultReport::closeEvent(QCloseEvent* event) {m_acceptingRequests=false;CancelTasks();QWidget::closeEvent(event);}
void ResultReport::Refresh()
{
    RefreshPage();
}
namespace {
class ReportPageModel final : public QAbstractTableModel {
public:
    ReportDocument document;
    QVector<ReportRowId> rows;
    std::vector<QStringList> cells;
    explicit ReportPageModel(QObject* parent) : QAbstractTableModel(parent) {}
    void SetPage(const ReportDocument& value,QVector<ReportRowId> page,std::vector<QStringList> values = {}) {beginResetModel();document=value;rows=std::move(page);cells=std::move(values);endResetModel();}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid()?0:rows.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid()?0:document.columns.size(); }
    QVariant data(const QModelIndex& index,int role) const override {
        if(!index.isValid() || index.model()!=this || index.row()<0 || index.row()>=rows.size()
            || index.column()<0 || index.column()>=document.columns.size() || role!=Qt::DisplayRole || !document.table)return {};
        return GetReportCell(cells[index.row()],index.column());
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
    const bool added=!m_documents.contains(doc.id);
    if(!added && doc.category!="运行记录")return; // 精确结果的表格冻结后不被后续运行替换。
    m_documents[doc.id]=doc;
    QTreeWidgetItem* group=nullptr;
    for(int i=0;i<m_reports->topLevelItemCount();++i)if(m_reports->topLevelItem(i)->text(0)==doc.category)group=m_reports->topLevelItem(i);
    if(!group){group=new QTreeWidgetItem(m_reports,{doc.category});group->setExpanded(true);group->setHidden(true);}
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
        m_buildTimer->stop();setProperty("reportGenerationBusy",false);
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
    if(!m_selected.table || (filtered && (!m_rowsReady || !m_pageReady)))throw std::runtime_error("报告或完整筛选结果尚未就绪");
    const auto doc=m_selected;doc.table->PrepareRead();const bool selected=filtered&&!m_allRows;
    const auto rows=selected?m_rows:ReportRows{};
    StartExport(path,rows,selected);
}
void ResultReport::ExportSelectedRows(const QString& path)
{
    if(!m_selected.table || !m_pageReady || !m_table->selectionModel())throw std::runtime_error("报告当前页尚未就绪");
    auto selected=m_table->selectionModel()->selectedRows();
    if(selected.isEmpty())throw std::runtime_error("请先选择需要导出的行");
    std::sort(selected.begin(),selected.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()<b.row();});
    std::vector<ReportRowId> rows;rows.reserve(static_cast<std::size_t>(selected.size()));
    try {
        for(const auto& index:selected){const auto row=m_page*200+index.row();rows.push_back(m_allRows?row:m_rows[static_cast<std::size_t>(row)]);}
    }catch(const std::exception& error){SetReadFailure(QString::fromUtf8(error.what()));throw;}
    StartExport(path,ReportRows(std::move(rows)),true);
}
void ResultReport::SelectExportPath(bool selected,bool filtered)
{
    if(m_task || !m_selected.table || (selected && !m_exportSelection->isEnabled()) || (filtered && !m_exportFiltered->isEnabled()))return;
    // 系统对话框有自己的事件循环；暂保留任务入口，防止原生报告在其中启动。
    m_comparisonMapping=true;
    const auto resume=qScopeGuard([this]{m_comparisonMapping=false;if(m_acceptingRequests&&isVisible()&&!m_task&&!m_pendingReports.isEmpty())m_buildTimer->start(0);});
    const auto title=selected?QString("导出选中行"):filtered?QString("导出筛选结果"):QString("导出全部");
    const auto path=m_chooseExportPath?m_chooseExportPath(title):QFileDialog::getSaveFileName(this,title,"报告.csv","CSV (*.csv)");
    if(path.isEmpty())return;
    try{if(selected)ExportSelectedRows(path);else ExportReportFile(path,filtered);}
    catch(const std::exception& error){const auto message=QString::fromUtf8(error.what());setProperty("reportTaskError",message);m_taskStatus->setText("导出失败："+message);}
}
void ResultReport::StartExport(const QString& path,ReportRows rows,bool restricted)
{
    if(path.isEmpty())throw std::invalid_argument("报告保存路径为空");
    auto state=std::make_shared<ExportState>();state->path=path;state->document=m_selected;
    state->rowCount=restricted?static_cast<ReportRowId>(rows.size()):m_selected.table->GetCount();
    auto doc=m_selected;
    bool indexed=restricted;
    if(restricted && !doc.table->GetIsComplete()){
        const auto source=doc.table;
        doc.table=std::make_shared<ReportTable>();
        // 未物化的来源回调仍在 owner 的有界准备链读取；候选只包含请求的行，不准备未选中的正文。
        doc.table->SetRows(rows.size(),[source,rows](ReportRowId index){return source->Read(rows[static_cast<std::size_t>(index)]);});
        indexed=false;
    }
    StartReportsTask([state,doc,rows=std::move(rows),indexed](const ReportCheckpoint& check){
        QSaveFile output(state->path);output.setDirectWriteFallback(false);
        if(!output.open(QIODevice::WriteOnly))throw std::runtime_error(("无法打开报告输出文件："+output.errorString()).toStdString());
        try{WriteReportCsv(output,doc,indexed?&rows:nullptr,check,[state](ReportRowId count){state->rowsWritten.store(count,std::memory_order_relaxed);});}
        catch(const ReportReadError&){state->readFailed.store(true);throw;}
        check();auto expected=ExportState::Phase::Writing;
        if(!state->phase.compare_exchange_strong(expected,ExportState::Phase::Committing))throw std::runtime_error("报告任务已取消");
        if(!output.commit())throw std::runtime_error(("报告保存失败："+output.errorString()).toStdString());
        state->committed.store(true);state->phase.store(ExportState::Phase::Committed);
        return QVector<ReportDocument>{};
    },{doc},{},state);
    setProperty("reportExportPath",QString());setProperty("reportExportRows",qlonglong(0));setProperty("reportExportTotal",qlonglong(state->rowCount));
    setProperty("reportExportStatus","writing");m_openExportFolder->hide();m_exportProgress->setValue(0);m_exportProgress->show();
    // Qt 5 的系统中文数字可能不是连续码位；行数沿用表格的十进制计数，避免产生非数字字形。
    m_taskStatus->setText(QString("正在导出 · 0 / %1 行").arg(state->rowCount));
}
void ResultReport::RefreshExportActions()
{
    const bool available=m_selected.table && !m_task;
    const bool selected=available && m_pageReady && m_table->selectionModel() && m_table->selectionModel()->hasSelection();
    m_export->setEnabled(available);m_exportSelection->setEnabled(selected);m_exportSelectionAction->setEnabled(selected);
    m_exportFiltered->setEnabled(available && m_rowsReady && m_pageReady);
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
ReportRows ResultReport::GetFilteredRows()const {
    if(GetRowCount() && (!m_rowsReady || !m_pageReady))throw std::logic_error("报告页面或筛选行号不可用，请重新读取");
    return m_allRows?GetReportRowRange(GetRowCount()):m_rows;
}
ReportRowId ResultReport::GetFilteredCount()const{return GetRowCount();}
std::function<ReportRowId(ReportRowId)> ResultReport::CreateFilteredReader()const {
    if(!m_rowsReady || !m_pageReady)throw std::logic_error("完整筛选索引尚未准备完成");
    const auto rows=GetFilteredRows();return [rows](ReportRowId row){if(row<0||static_cast<std::size_t>(row)>=rows.size())throw std::out_of_range("报告筛选行不存在");return rows[static_cast<std::size_t>(row)];};
}
ReportRowId ResultReport::GetRowCount()const {return m_allRows && m_selected.table?m_selected.table->GetCount():static_cast<ReportRowId>(m_rows.size());}
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
bool ResultReport::GetGenerationPending() const {return m_buildTimer->isActive() || !m_buildingOperation.isEmpty() || bool(m_task);}
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
    if(!m_acceptingRequests||!isVisible()||m_comparisonMapping)return;
    // 重开窗口时旧取消任务可能仍在收尾，保留这次恢复队列的请求。
    if(m_task){m_buildTimer->start(20);return;}
    try {
        if(m_buildingOperation.isEmpty()) {
            for(auto it=m_pendingReports.cbegin();it!=m_pendingReports.cend();++it) {
                if(m_failedReports.contains(it.key()))continue;
                const auto record=it.value();
                if(!m_moduleFilter.isEmpty()&&record["module"].toString()!=m_moduleFilter)continue;
                const bool resultHint=m_resultFilter.isEmpty()||GetPendingResults(record).contains(m_resultFilter);
                const bool sourceHint=m_sourceFilter.isEmpty()||record["source"].toObject()["dataRevision"].toString()==m_sourceFilter
                    ||record["result"].toObject()["source"].toString()==m_sourceFilter;
                if((!resultHint||!sourceHint)&&record["module"].toString()!="Alignment")continue;
                m_buildingOperation=it.key();m_buildingViewRevision=m_viewRevision;m_buildingIndex=0;m_buildingPreviewRow=0;m_generationProgress=0;
                setProperty("reportGenerationProgress",qulonglong(0));setProperty("reportGenerationError",QString());
                const auto known=GetReportIds()+m_removedReports;
                if(auto prepare=onPrepareReports?onPrepareReports(record,known):ReportPreparation{}) {
                    const auto operation=m_buildingOperation;StartReportsTask(std::move(prepare),{},operation);
                    m_buildingOperation.clear();setProperty("reportGenerationBusy",true);return;
                }
                auto documents=onReadReports?onReadReports(record,known):QVector<ReportDocument>{};
                if((!resultHint||!sourceHint)&&!std::any_of(documents.cbegin(),documents.cend(),[&](const auto& doc){
                    return MatchesSource(doc,m_sourceFilter)&&MatchesResult(doc,m_resultFilter)&&MatchesModule(doc,m_moduleFilter);
                })){m_buildingOperation.clear();continue;}
                m_buildingReports=std::move(documents);break;
            }
        }
        if(m_buildingOperation.isEmpty()){setProperty("reportGenerationBusy",false);return;}
        setProperty("reportGenerationBusy",true);m_cancelTask->setEnabled(true);
        QElapsedTimer elapsed;elapsed.start();int work=0;
        while(m_buildingIndex<m_buildingReports.size()&&work++<256&&elapsed.elapsed()<4) {
            ++m_generationProgress;auto& candidate=m_buildingReports[m_buildingIndex];
            if(candidate.prepareReports) {
                setProperty("reportGenerationPhase","prepare");
                QVector<ReportDocument> prepared;if(!candidate.prepareReports(prepared))continue;
                m_buildingReports.removeAt(m_buildingIndex);
                for(int index=0;index<prepared.size();++index)m_buildingReports.insert(m_buildingIndex+index,std::move(prepared[index]));
                continue;
            }
            setProperty("reportGenerationPhase","rows");
            const auto table=candidate.table;
            if(table&&!candidate.materializeBeforePublish&&!table->GetIsComplete()
                &&m_buildingPreviewRow<std::min<ReportRowId>(200,table->GetCount())) {
                // 只检查首个可见页，提前报告读取失败；不为准入扫描完整原生表。
                (void)table->Read(m_buildingPreviewRow++);continue;
            }
            if(!table||!candidate.materializeBeforePublish||table->ContinueBuild()){if(table)table->PrepareRead();++m_buildingIndex;m_buildingPreviewRow=0;}
        }
        setProperty("reportGenerationProgress",m_generationProgress);
        if(m_buildingIndex<m_buildingReports.size()){m_buildTimer->start(0);return;}
        auto documents=std::move(m_buildingReports);m_buildingReports.clear();
        const bool select=m_buildingViewRevision==m_viewRevision&&(m_selected.id.isEmpty()||m_follow->isChecked());
        m_completedOperations.insert(m_buildingOperation);m_pendingReports.remove(m_buildingOperation);m_buildingOperation.clear();
        QString firstNew;
        for(auto& doc:documents){
            if(firstNew.isEmpty()&&!m_documents.contains(doc.id)&&MatchesSource(doc,m_sourceFilter)&&MatchesResult(doc,m_resultFilter)&&MatchesModule(doc,m_moduleFilter))firstNew=doc.id;
            AddReport(std::move(doc),false);
        }
        if(select&&!firstNew.isEmpty())SelectReport(firstNew);
        setProperty("reportGenerationBusy",false);m_cancelTask->setEnabled(false);m_buildTimer->start(0);
    }catch(const std::exception& error) {
        const auto operation=m_buildingOperation;m_buildingReports.clear();m_buildingOperation.clear();
        setProperty("reportGenerationBusy",false);setProperty("reportGenerationError",QString::fromUtf8(error.what()));
        if(!operation.isEmpty())m_failedReports[operation]=QString::fromUtf8(error.what());RefreshFailures();
        m_taskStatus->setText("报告读取失败；其他报告继续，可显式重试失败项："+QString::fromUtf8(error.what()));
        if(isVisible())m_buildTimer->start(0);
    }
}
void ResultReport::StartTask(std::function<ReportDocument(const ReportCheckpoint&)> work,QVector<ReportDocument> inputs)
{
    StartReportsTask([work=std::move(work)](const ReportCheckpoint& check){
        auto document=work(check);QVector<ReportDocument> documents;if(document.table)documents.append(std::move(document));return documents;
    },std::move(inputs));
}
void ResultReport::StartReportsTask(ReportPreparation work,QVector<ReportDocument> inputs,const QString& operation,std::shared_ptr<ExportState> exportState)
{
    if(!m_acceptingRequests)throw std::runtime_error("报告窗口正在关闭");
    if(m_task)throw std::runtime_error("请等待当前报告任务完成或取消后重试");
    // 先完成所有可能失败的分配和线程启动，成功后才发布任务占位、暂停生成队列。
    auto candidate=std::make_unique<Task>();candidate->inputs=std::move(inputs);candidate->viewRevision=m_viewRevision;
    candidate->exportState=exportState;
    candidate->nativeOperation=operation;candidate->request=m_taskRequest+1;
    const auto gate=candidate->gate;const auto flag=candidate->cancelled;const auto counter=candidate->progress;auto* owner=thread();
    try {
        if(m_beforeWorkerStart)m_beforeWorkerStart();
        candidate->future=std::async(std::launch::async,[work=std::move(work),gate,flag,counter,owner,exportState] {
            {std::unique_lock<std::mutex> lock(gate->mutex);
                const auto ready=[&]{return gate->ready||flag->load();};
                if(exportState){if(!gate->changed.wait_until(lock,exportState->deadline,ready))throw std::runtime_error("报告导出超过 24 小时绝对期限");}
                else gate->changed.wait(lock,ready);
                if(gate->failure)std::rethrow_exception(gate->failure);}
            const ReportCheckpoint check=[flag,counter,exportState]{
                if(flag->load())throw std::runtime_error("报告任务已取消");
                if(exportState && std::chrono::steady_clock::now()>=exportState->deadline)throw std::runtime_error("报告导出超过 24 小时绝对期限");
                counter->fetch_add(1,std::memory_order_relaxed);
            };
            check();auto documents=work(check);if(!exportState || !exportState->committed.load())check();
            for(auto& document:documents)if(document.table)document.table->SetOwnerThread(owner);return documents;
        });
    }catch(const std::exception& error){
        setProperty("reportTaskBusy",false);setProperty("reportTaskError",QString::fromUtf8(error.what()));m_taskStatus->setText(QString::fromUtf8(error.what()));
        if(exportState){setProperty("reportExportStatus","failed");setProperty("reportExportPath",QString());m_openExportFolder->hide();m_exportProgress->hide();}
        if(isVisible()&&!m_pendingReports.isEmpty())m_buildTimer->start(0);
        throw;
    }
    m_taskRequest=candidate->request;m_task=std::move(candidate);m_buildTimer->stop();
    if(!exportState || m_rowScan || m_pendingRowScan)CancelRows();
    setProperty("reportTaskBusy",true);setProperty("reportTaskError",QString());setProperty("reportTaskProgress",qulonglong(0));
    m_taskStatus->setText("正在处理报告…");m_cancelTask->setEnabled(true);m_taskTimer->start();
    RefreshExportActions();
}
void ResultReport::ContinueTask()
{
    if(!m_task)return;
    const auto progress=m_task->progress->load();setProperty("reportTaskProgress",progress);
    const auto exportState=m_task->exportState;
    if(exportState){
        const auto rows=exportState->rowsWritten.load(std::memory_order_relaxed);setProperty("reportExportRows",qlonglong(rows));
        m_exportProgress->setValue(exportState->rowCount?static_cast<int>(1000.0L*rows/exportState->rowCount):0);
        const bool committing=exportState->phase.load()==ExportState::Phase::Committing;
        m_taskStatus->setText(committing?QString("正在提交 CSV 文件…"):exportState->phase.load()==ExportState::Phase::Cancelled?QString("正在取消导出…"):QString("正在导出 · %1 / %2 行").arg(rows).arg(exportState->rowCount));
        if(committing)m_cancelTask->setEnabled(false);
    }
    if(!m_task->nativeOperation.isEmpty())setProperty("reportGenerationProgress",progress);
    if(exportState && !m_task->inputsReady && std::chrono::steady_clock::now()>=exportState->deadline){
        {std::lock_guard<std::mutex> lock(m_task->gate->mutex);m_task->gate->failure=std::make_exception_ptr(std::runtime_error("报告导出超过 24 小时绝对期限"));m_task->gate->ready=true;}
        m_task->inputsReady=true;m_task->gate->changed.notify_all();
    }
    // 旧扫描仍由自己的轮询回收；唯一任务在它退出前不执行新的后台工作。
    if(!m_task->cancelled->load() && m_rowScan && !m_task->inputsReady)return;
    if(!m_task->inputsReady && !m_task->cancelled->load()) {
        std::exception_ptr failure;
        try {
            QElapsedTimer elapsed;elapsed.start();int work=0;
            while(m_task->preparing<m_task->inputs.size()&&!m_task->cancelled->load()&&work++<256&&elapsed.elapsed()<4) {
                auto& doc=m_task->inputs[m_task->preparing];
                if(!doc.table||doc.table->ContinueBuild()){if(doc.table)doc.table->PrepareRead();++m_task->preparing;}
            }
            if(m_task->preparing<m_task->inputs.size()&&!m_task->cancelled->load())return;
        }catch(...){failure=std::current_exception();if(exportState)exportState->readFailed.store(true);}
        {std::lock_guard<std::mutex> lock(m_task->gate->mutex);m_task->gate->failure=failure;m_task->gate->ready=true;}
        m_task->inputsReady=true;m_task->gate->changed.notify_all();
    }
    if(m_task->future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
    const bool cancelled=!(exportState && exportState->committed.load()) && (m_task->cancelled->load() || m_task->request!=m_taskRequest || !m_acceptingRequests);
    const bool select=m_task->viewRevision==m_viewRevision &&(m_task->nativeOperation.isEmpty()||m_selected.id.isEmpty()||m_follow->isChecked());
    const auto operation=m_task->nativeOperation;
    try {
        auto documents=m_task->future.get();m_task.reset();
        if(!cancelled) {
            if(!operation.isEmpty()){m_completedOperations.insert(operation);m_pendingReports.remove(operation);}
            else if(select&&!documents.isEmpty())ShowReports();
            for(auto& document:documents){const auto id=document.id;AddReport(std::move(document),select);if(select&&operation.isEmpty())SelectReport(id);}
            if(select&&!operation.isEmpty()&&!documents.isEmpty())ShowReports(m_sourceFilter,m_resultFilter,m_moduleFilter);
        }
        if(exportState && exportState->committed.load()){
            setProperty("reportExportStatus","completed");setProperty("reportExportPath",exportState->path);setProperty("reportExportRows",qlonglong(exportState->rowCount));
            m_exportProgress->setValue(1000);m_openExportFolder->show();
            m_taskStatus->setText(QString("已导出 %1 行 · %2").arg(exportState->rowCount).arg(QFileInfo(exportState->path).fileName()));
        }else m_taskStatus->setText(cancelled?"报告任务已取消":"报告任务完成");
    }catch(const std::exception& error){m_task.reset();
        const auto message=QString::fromUtf8(error.what());
        // 取消仅吞掉自身的协作式取消异常；同时发生的真实读写/计算失败仍保留原因。
        if(!cancelled || message!="报告任务已取消"){setProperty("reportTaskError",message);m_taskStatus->setText(exportState?"导出失败："+message:message);
            if(exportState){setProperty("reportExportStatus","failed");if(exportState->readFailed.load() && m_selected.id==exportState->document.id && m_selected.table==exportState->document.table)SetReadFailure(message);}
            if(!cancelled&&!operation.isEmpty()){m_failedReports[operation]=message;setProperty("reportGenerationError",message);RefreshFailures();}}
        else {m_taskStatus->setText(exportState?"导出已取消，未保存文件；已有文件保持原样。":"报告任务已取消");if(exportState)setProperty("reportExportStatus","cancelled");}
    }
    setProperty("reportGenerationBusy",false);m_taskTimer->stop();setProperty("reportTaskBusy",false);m_cancelTask->setEnabled(false);RefreshFailures();
    if(exportState)m_exportProgress->hide();RefreshExportActions();
    if(m_acceptingRequests&&isVisible()) {
        if(m_pendingRowScan)m_scanTimer->start(0);else if(!m_rowsReady&&m_selected.table)RefreshRows();
        if(!cancelled&&!m_pendingReports.isEmpty())m_buildTimer->start(0);
    }
}
void ResultReport::CancelRows()
{
    ++m_rowsRequest;if(m_rowScan)m_rowScan->cancelled->store(true);m_pendingRowScan.reset();
    if(m_rowScan)m_scanTimer->start(20);else m_scanTimer->stop();
    setProperty("reportRowsBusy",false);m_rows={};m_allRows=false;m_rowsReady=false;RefreshPage();
}
void ResultReport::CancelTasks(bool waitForExit)
{
    if(m_rowScan || m_pendingRowScan){
        const bool hasCurrentScan=m_pendingRowScan || !m_rowScan->cancelled->load();
        if(hasCurrentScan)CancelRows();
    }
    m_buildTimer->stop();m_buildingReports.clear();m_buildingOperation.clear();m_buildingIndex=0;
    setProperty("reportGenerationBusy",false);++m_taskRequest;
    if(m_task)m_task->Cancel();
    if(waitForExit){
        // 只有最终关闭收尾等待；普通取消和查询切换保留在途读者，由轮询回收。
        m_acceptingRequests=false;m_scanTimer->stop();m_taskTimer->stop();
        if(m_rowScan && m_rowScan->future.valid())m_rowScan->future.wait();
        m_rowScan.reset();m_pendingRowScan.reset();
        if(m_task){if(m_task->future.valid()){m_task->future.wait();ContinueTask();}else m_task.reset();}
    }
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
    CancelRows();
    setProperty("reportRowsProgress",qlonglong(0));setProperty("reportRowsPhase",QString());setProperty("reportRowsError",QString());m_page=0;
    m_table->horizontalHeader()->setSortIndicatorShown(false);
    if(!m_acceptingRequests || !m_selected.table){RefreshPage();return;}
    const auto search=m_search->text();const int column=m_filterColumn->currentData().toInt();
    double low=0,high=0;const bool hasLow=!m_minimum->text().trimmed().isEmpty(),hasHigh=!m_maximum->text().trimmed().isEmpty();
    if((hasLow&&!GetReportNumber(m_minimum->text(),low))||(hasHigh&&!GetReportNumber(m_maximum->text(),high))||(hasLow&&hasHigh&&low>high)){RefreshPage();m_range->setText("请输入有效数值区间");return;}
    if(search.isEmpty()&&!(column>=0&&(hasLow||hasHigh))&&m_sortColumn<0){m_allRows=true;m_rowsReady=true;RefreshPage();return;}
    try {
        auto candidate=std::make_unique<RowScan>();const auto doc=m_selected;
        candidate->request=m_rowsRequest;candidate->document=doc;candidate->count=doc.table->GetCount();
        const auto cancelled=candidate->cancelled;const auto next=candidate->next;const auto ordering=candidate->ordering;
        const auto sortColumn=m_sortColumn;const auto direction=m_sortOrder;
        candidate->work=[doc,cancelled,next,ordering,search,column,sortColumn,low,high,hasLow,hasHigh,direction] {
            const ReportCheckpoint check=[cancelled]{if(cancelled->load())throw std::runtime_error("报告筛选 / 排序已取消");};
            return ReadReportRows(doc,search,column,sortColumn,low,high,hasLow,hasHigh,direction,check,next,ordering);
        };
        m_pendingRowScan=std::move(candidate); // 唯一待执行描述；连续变化只覆盖这一份。
        setProperty("reportRowsBusy",true);RefreshPage();m_range->setText("正在筛选 / 排序完整报告…");m_scanTimer->start(0);
    }catch(const std::exception& error){setProperty("reportRowsBusy",false);SetReadFailure(QString::fromUtf8(error.what()));}
}
void ResultReport::ContinueRows()
{
    if(m_rowScan) {
        const bool current=m_rowScan->request==m_rowsRequest&&!m_rowScan->cancelled->load()&&m_acceptingRequests;
        if(current){setProperty("reportRowsProgress",m_rowScan->next->load());setProperty("reportRowsPhase",m_rowScan->ordering->load()?"order":"read");}
        if(m_rowScan->future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){m_scanTimer->start(20);return;}
        try {
            auto rows=m_rowScan->future.get();m_rowScan.reset();
            if(current){m_rows=std::move(rows);m_rowsReady=true;setProperty("reportRowsBusy",false);
                if(m_sortColumn>=0){m_table->horizontalHeader()->setSortIndicator(m_sortColumn,m_sortOrder);m_table->horizontalHeader()->setSortIndicatorShown(true);}RefreshPage();}
        }catch(const std::exception& error){m_rowScan.reset();if(current){setProperty("reportRowsBusy",false);m_rows={};RefreshPage();setProperty("reportRowsError",QString::fromUtf8(error.what()));m_range->setText("报告筛选 / 排序失败："+QString::fromUtf8(error.what()));}}
    }
    if(!m_pendingRowScan || m_task)return;
    auto candidate=std::move(m_pendingRowScan);
    try {
        QElapsedTimer elapsed;elapsed.start();int work=0;const auto table=candidate->document.table;
        while(!table->GetIsComplete()&&work++<256&&elapsed.elapsed()<4)table->ContinueBuild();
        if(!table->GetIsComplete()){m_pendingRowScan=std::move(candidate);m_scanTimer->start(0);return;}
        table->PrepareRead();if(m_beforeWorkerStart)m_beforeWorkerStart();
        candidate->future=std::async(std::launch::async,std::move(candidate->work));
        m_rowScan=std::move(candidate);m_scanTimer->start(20);
    }catch(const std::exception& error){setProperty("reportRowsBusy",false);m_rows={};m_rowsReady=false;RefreshPage();setProperty("reportRowsError",QString::fromUtf8(error.what()));m_range->setText("报告筛选 / 排序启动失败："+QString::fromUtf8(error.what()));if(isVisible()&&!m_pendingReports.isEmpty())m_buildTimer->start(0);}
}
void ResultReport::SetReadFailure(const QString& error)
{
    m_pageReady=false;
    if(auto* model=dynamic_cast<ReportPageModel*>(m_table->model()))model->SetPage({},{});
    setProperty("reportRowsError",error);m_range->setText("报告读取失败，请重新翻页或选择报告重试："+error);
    RefreshExportActions();m_locate->setEnabled(false);
}
void ResultReport::RefreshPage()
{
    const auto rowCount=GetRowCount();m_page=rowCount?std::min<ReportRowId>(m_page,(rowCount-1)/200):0;
    // 保留模型身份；Qt5 清空全部列时不替换仍被表头/选择模型观察的对象。
    auto* model=dynamic_cast<ReportPageModel*>(m_table->model());
    if(!model){model=new ReportPageModel(m_table);m_table->setModel(model);}
    const auto offset=m_page*200,count=GetRowCount();
    m_previous->setEnabled(m_rowsReady && m_page>0);m_first->setEnabled(m_previous->isEnabled());
    m_next->setEnabled(m_rowsReady && count>0 && m_page<(count-1)/200);m_last->setEnabled(m_next->isEnabled());
    m_cancelScan->setEnabled(property("reportRowsBusy").toBool());
    m_locate->setVisible((m_selected.module=="Gap" || (m_selected.module=="Surface" && m_selected.id.startsWith("surface:"))) && m_selected.category=="本次会话结果" && bool(onLocate));
    try {
        QVector<ReportRowId> page;std::vector<QStringList> cells;
        const auto length=std::min<ReportRowId>(200,count-offset);
        // 当前页的行号与正文全部读取成功后才替换模型，Qt 绘制不再执行磁盘 IO。
        for(ReportRowId i=0;i<length;++i){const auto row=m_allRows?offset+i:m_rows[static_cast<std::size_t>(offset+i)];page.append(row);cells.push_back(m_selected.table->Read(row));}
        model->SetPage(m_selected,std::move(page),std::move(cells));m_pageReady=m_rowsReady;
        setProperty("reportRowsError",QString());
        m_range->setText(QString("总数 %1 · 筛选后 %2 · 显示 %3–%4").arg(m_selected.table?m_selected.table->GetCount():0).arg(count).arg(count?offset+1:0).arg(offset+length));
        RefreshExportActions();m_locate->setEnabled(m_pageReady);
    } catch(const std::exception& e){SetReadFailure(QString::fromUtf8(e.what()));}
}
void ResultReport::CompareSelected()
{
    if(!m_documents.contains(m_baseline) || !m_selected.table)throw std::invalid_argument("请先选择基准 A，再选择待比较报告 B");
    auto a=m_documents.value(m_baseline),b=m_selected;
    if(a.id==b.id)throw std::invalid_argument("请选择另一份报告作为 B");
    if(m_task||m_comparisonMapping)throw std::runtime_error("请等待当前报告任务完成或取消后重试");
    // 模态窗口仍处理计时器；映射确认前为这次比较保留任务入口。
    m_comparisonMapping=true;
    const auto resumeGeneration=qScopeGuard([this]{
        m_comparisonMapping=false;
        if(m_acceptingRequests&&isVisible()&&!m_task&&!m_pendingReports.isEmpty())m_buildTimer->start(0);
    });
    m_buildTimer->stop();
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
    auto numberLocale=QLocale();if(!numberLocale.zeroDigit().isDigit())numberLocale=QLocale::c();
    numberLocale.setNumberOptions(QLocale::OmitGroupSeparator);
    auto* distance=new QDoubleSpinBox(&dialog);distance->setObjectName("reportDistanceTolerance");distance->setLocale(numberLocale);distance->setDecimals(9);distance->setRange(.000000001,1e12);distance->setValue(.01);form->addRow("空间距离容差 mm",distance);
    auto* tolerance=new QDoubleSpinBox(&dialog);tolerance->setObjectName("reportValueTolerance");tolerance->setLocale(numberLocale);tolerance->setDecimals(9);tolerance->setRange(0,1e12);form->addRow("数值容差（基准单位）",tolerance);
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
