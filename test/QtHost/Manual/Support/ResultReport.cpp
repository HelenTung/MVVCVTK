// 测试用途：报告序列化和表格显示只消费运行记录，不调用业务或渲染核心。
#include "ResultReport.h"
#include "UiText.h"
#include "ReportComparison.h"
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
ResultReport::ResultReport(QWidget* parent) : QWidget(parent)
{
    setObjectName("resultReport");
    auto* layout = new QVBoxLayout(this);
    auto* toolbar = new QHBoxLayout;
    auto* import = new QPushButton("导入 CSV…",this); import->setObjectName("importReportCsv"); toolbar->addWidget(import);
    auto* baseline = new QPushButton("设为基准 A",this); baseline->setObjectName("setReportBaseline"); toolbar->addWidget(baseline);
    auto* compare = new QPushButton("与基准比较…",this); compare->setObjectName("compareReports"); toolbar->addWidget(compare);
    m_copy = new QPushButton("复制选中行",this); m_copy->setObjectName("copyReportCsv"); toolbar->addWidget(m_copy);
    m_export = new QPushButton("导出完整报告…",this); m_export->setObjectName("exportReportCsv"); toolbar->addWidget(m_export);
    auto* exportFiltered = new QPushButton("导出筛选结果…",this); toolbar->addWidget(exportFiltered);
    m_locate=new QPushButton("定位到视图",this);m_locate->setObjectName("locateReportRow");m_locate->hide();toolbar->addWidget(m_locate);
    layout->addLayout(toolbar);
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
    auto* tabs = new QTabWidget(content); body->addWidget(tabs,1);
    m_table = new QTableView(tabs); m_table->setObjectName("reportTable"); m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true); tabs->addTab(m_table,"明细 / 比较");
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_conditions = new QPlainTextEdit(tabs); m_conditions->setReadOnly(true); m_conditions->setObjectName("reportConditions"); tabs->addTab(m_conditions,"汇总与计算条件");
    auto* pages = new QHBoxLayout;
    m_previous = new QPushButton("上一页",content); m_previous->setObjectName("reportPrevious"); pages->addWidget(m_previous);
    m_range = new QLabel(content); m_range->setObjectName("reportRange"); pages->addWidget(m_range,1);
    m_next = new QPushButton("下一页",content); m_next->setObjectName("reportNext"); pages->addWidget(m_next); body->addLayout(pages);
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
    connect(m_next,&QPushButton::clicked,this,[this]{if((m_page+1)*200<m_rows.size())++m_page;RefreshPage();});
    connect(m_table->horizontalHeader(),&QHeaderView::sectionClicked,this,[this](int column){
        m_sortOrder=m_sortColumn==column && m_sortOrder==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder;m_sortColumn=column;RefreshRows();
    });
    connect(m_copy,&QPushButton::clicked,this,[this]{
        if(!m_table->selectionModel() || !m_selected.table)return;
        auto selected=m_table->selectionModel()->selectedRows();std::sort(selected.begin(),selected.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()<b.row();});
        QStringList lines;for(const auto& index:selected){QStringList cells;for(auto cell:m_selected.table->Read(m_rows[m_page*200+index.row()]))cells.append(CsvCell(cell));lines.append(cells.join(','));}
        QApplication::clipboard()->setText(lines.join("\r\n"));
    });
    const auto save=[this](bool filtered){
        if(!m_selected.table)return;const auto path=QFileDialog::getSaveFileName(this,filtered?"导出全部筛选行":"导出完整报告",{},"CSV (*.csv)");if(path.isEmpty())return;
        try{ExportReport(path,m_selected,filtered?&m_rows:nullptr);}catch(const std::exception& e){QMessageBox::warning(this,"导出失败",QString::fromUtf8(e.what()));}
    };
    connect(m_export,&QPushButton::clicked,this,[save]{save(false);});
    connect(exportFiltered,&QPushButton::clicked,this,[save]{save(true);});
    connect(m_locate,&QPushButton::clicked,this,[this]{
        if(!onLocate || !m_table->selectionModel())return;const auto selected=m_table->selectionModel()->selectedRows();
        if(selected.size()!=1){QMessageBox::information(this,"定位","请选择一条可定位记录。");return;}
        try{onLocate(m_selected,m_rows[m_page*200+selected.first().row()]);}catch(const std::exception& e){QMessageBox::warning(this,"无法定位",QString::fromUtf8(e.what()));}
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
void ResultReport::showEvent(QShowEvent* event) { QWidget::showEvent(event); Refresh(); }
void ResultReport::Refresh()
{
    RefreshPage();
}
namespace {
class ReportPageModel final : public QAbstractTableModel {
public:
    ReportDocument document;
    QVector<int> rows;
    explicit ReportPageModel(QObject* parent) : QAbstractTableModel(parent) {}
    void SetPage(const ReportDocument& value,QVector<int> page) {beginResetModel();document=value;rows=std::move(page);endResetModel();}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid()?0:rows.size(); }
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
        return orientation==Qt::Horizontal?QVariant(document.columns[section].name):QVariant(rows[section]+1);
    }
};
}
void ResultReport::AddReport(ReportDocument doc)
{
    if(doc.id.isEmpty() || !doc.table)return;
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
    item->setHidden((!m_sourceFilter.isEmpty() && doc.source!=m_sourceFilter) || (!m_resultFilter.isEmpty() && doc.result!=m_resultFilter));
    if(!item->isHidden())group->setHidden(false);
    if(m_selected.id.isEmpty() || (added && m_follow->isChecked())) {
        if(item->isHidden()){m_sourceFilter.clear();m_resultFilter.clear();ShowReports();}m_reports->setCurrentItem(item);
    } else if(added)m_summary->setText(m_selected.title+" · 有新报告，已保留当前选择");
    else if(m_selected.id==doc.id)SelectCurrent();
}
bool ResultReport::SelectReport(const QString& id)
{
    if(!m_documents.contains(id))return false;
    if((!m_sourceFilter.isEmpty() && m_documents[id].source!=m_sourceFilter) || (!m_resultFilter.isEmpty() && m_documents[id].result!=m_resultFilter))ShowReports();
    for(int i=0;i<m_reports->topLevelItemCount();++i)for(int j=0;j<m_reports->topLevelItem(i)->childCount();++j) {
        auto* item=m_reports->topLevelItem(i)->child(j);
        if(item->data(0,Qt::UserRole)==id){m_reports->setCurrentItem(item);SelectCurrent();return true;}
    }
    return false;
}
void ResultReport::ShowReports(const QString& source,const QString& result)
{
    m_sourceFilter=source;m_resultFilter=result;QString select;
    for(int i=0;i<m_reports->topLevelItemCount();++i) {
        auto* group=m_reports->topLevelItem(i);bool visible=false;
        for(int j=0;j<group->childCount();++j) {
            auto* item=group->child(j);const auto doc=m_documents.value(item->data(0,Qt::UserRole).toString());
            const bool match=(source.isEmpty() || doc.source==source) && (result.isEmpty() || doc.result==result);item->setHidden(!match);visible=visible||match;
            if(select.isEmpty() && match && !result.isEmpty() && doc.result==result && doc.category!="运行记录")select=doc.id;
        }
        group->setHidden(!visible);
    }
    if(!select.isEmpty())SelectReport(select);
    else if(result.isEmpty() && !source.isEmpty() && m_selected.source!=source) {
        m_reports->setCurrentItem(nullptr);m_selected={};RefreshRows();m_summary->setText("所选数据的已有报告；尚无匹配项时，可先完成相应业务计算。");
    }
    if(!result.isEmpty() && select.isEmpty()) {
        m_reports->setCurrentItem(nullptr);m_selected={};m_conditions->clear();RefreshRows();
        m_summary->setText("尚无该精确结果的业务报告，或结果明细已不可读取。");
    }
}
void ResultReport::ImportReport(const QString& path)
{
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("无法打开 CSV 文件");
    auto doc=ReadReportCsv(file,QFileInfo(path).fileName());const auto id=doc.id;ShowReports();AddReport(std::move(doc));SelectReport(id);
}
void ResultReport::SelectCurrent()
{
    const auto* item=m_reports->currentItem();if(!item)return;
    const auto id=item->data(0,Qt::UserRole).toString();if(!m_documents.contains(id))return;
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
QVector<int> ResultReport::GetFilteredRows() const { return m_rows; }
QSet<QString> ResultReport::GetReportIds() const { QSet<QString> ids;for(auto it=m_documents.cbegin();it!=m_documents.cend();++it)ids.insert(it.key());return ids; }
void ResultReport::RefreshRows()
{
    m_rows.clear();m_page=0;
    if(!m_selected.table){RefreshPage();return;}
    const auto search=m_search->text();const int column=m_filterColumn->currentData().toInt();
    double low=0,high=0;const bool hasLow=!m_minimum->text().trimmed().isEmpty(),hasHigh=!m_maximum->text().trimmed().isEmpty();
    if((hasLow && !GetReportNumber(m_minimum->text(),low)) || (hasHigh && !GetReportNumber(m_maximum->text(),high)) || (hasLow && hasHigh && low>high)) {
        RefreshPage();m_range->setText("请输入有效数值区间");return;
    }
    for(int i=0;i<m_selected.table->GetCount();++i) {
        const auto row=m_selected.table->Read(i);
        if(!search.isEmpty() && !row.join('\t').contains(search,Qt::CaseInsensitive))continue;
        if(column>=0 && (hasLow || hasHigh)){double value;if(!GetReportNumber(GetReportCell(row,column),value) || (hasLow && value<low) || (hasHigh && value>high))continue;}
        m_rows.append(i);
    }
    if(m_sortColumn>=0) {
        QHash<int,QString> keys;for(const auto row:m_rows)keys[row]=GetReportCell(m_selected.table->Read(row),m_sortColumn);
        std::stable_sort(m_rows.begin(),m_rows.end(),[&](int a,int b){
            double av,bv;int order;
            if(GetReportNumber(keys[a],av) && GetReportNumber(keys[b],bv))order=av<bv?-1:av>bv?1:QString::compare(keys[a],keys[b]);
            else order=QString::compare(keys[a],keys[b],Qt::CaseInsensitive);
            return m_sortOrder==Qt::AscendingOrder?order<0:order>0;
        });
        m_table->horizontalHeader()->setSortIndicator(m_sortColumn,m_sortOrder);
    }
    RefreshPage();
}
void ResultReport::RefreshPage()
{
    // 保留模型身份；Qt5 清空全部列时不替换仍被表头/选择模型观察的对象。
    auto* model=dynamic_cast<ReportPageModel*>(m_table->model());
    if(!model){model=new ReportPageModel(m_table);m_table->setModel(model);}
    const auto offset=m_page*200;model->SetPage(m_selected,m_rows.mid(offset,200));
    m_range->setText(QString("总数 %1 · 筛选后 %2 · 显示 %3–%4").arg(m_selected.table?m_selected.table->GetCount():0).arg(m_rows.size()).arg(m_rows.isEmpty()?0:offset+1).arg(std::min(offset+200,m_rows.size())));
    m_previous->setEnabled(m_page>0);m_next->setEnabled(offset+200<m_rows.size());
    m_copy->setEnabled(m_selected.table!=nullptr);m_export->setEnabled(m_selected.table!=nullptr);
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
    const auto addMapping=[&](const QString& title,const ReportDocument& doc) {
        auto* box=new QWidget(&dialog);auto* form=new QFormLayout(box);form->addRow(new QLabel(title,box));Mapping m;
        const auto combo=[&](int selected){auto* c=new QComboBox(box);c->addItem("未指定",-1);for(int i=0;i<doc.columns.size();++i)c->addItem(doc.columns[i].name,i);c->setCurrentIndex(selected+1);return c;};
        int numeric=doc.valueColumn;if(numeric<0)for(int i=0;i<doc.columns.size();++i)if(doc.columns[i].numeric){numeric=i;break;}
        m.value=combo(numeric);m.identity=combo(doc.identityColumn);m.validity=combo(doc.validityColumn);
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
    const auto am=addMapping("基准 A",a);sideBySide->addWidget(am.value->parentWidget());
    const auto bm=addMapping("待比较 B",b);sideBySide->addWidget(bm.value->parentWidget());
    auto* form=new QFormLayout;layout->addLayout(form);
    auto* mode=new QComboBox(&dialog);mode->setObjectName("reportMatchMode");mode->addItems({"仅分布比较","稳定业务标识","唯一空间匹配"});form->addRow("匹配方式",mode);
    auto* confirmed=new QCheckBox("已确认编号跨报告稳定，或坐标属于同一坐标系（并已核对坐标单位）",&dialog);confirmed->setObjectName("reportMatchConfirmed");layout->addWidget(confirmed);
    auto* mapping=new QCheckBox("已核对重名列的映射与有效性定义",&dialog);mapping->setVisible(a.mappingRequired || b.mappingRequired);layout->addWidget(mapping);
    auto* distance=new QDoubleSpinBox(&dialog);distance->setDecimals(9);distance->setRange(.000000001,1e12);distance->setValue(.01);form->addRow("空间距离容差 mm",distance);
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
    o.distanceTolerance=distance->value();o.valueTolerance=tolerance->value();auto result=CompareReports(a,b,o);const auto id=result.id;ShowReports();AddReport(std::move(result));SelectReport(id);
}
}
