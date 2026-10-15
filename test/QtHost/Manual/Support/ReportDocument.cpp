#include "ReportDocument.h"
#include "MappedArrayStorage.h"
#include <QDataStream>
#include <QTemporaryFile>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextCodec>
#include <QUuid>
#include <QRegularExpression>
#include <QHash>
#include <QThread>
#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <queue>
#include <atomic>
namespace Manual {
namespace {
void WriteBytes(QIODevice& output, const QByteArray& bytes)
{
    if (output.write(bytes) != bytes.size()) throw std::runtime_error("报告文件写入失败");
}
QString CsvCell(QString value)
{
    if (value.contains(',') || value.contains('"') || value.contains('\n') || value.contains('\r')) {
        value.replace('"', "\"\""); return '"' + value + '"';
    }
    return value;
}
bool GetResultIdentityValid(const QString& value) {
    if(!QRegularExpression("^[0-9a-fA-F]{32}:[0-9]+$").match(value).hasMatch()
        ||value.left(32)==QString(32,'0'))return false;
    bool valid=false;const auto revision=value.mid(33).toULongLong(&valid);return valid&&revision>0;
}
}
class ReportTable::Impl {
public:
    Impl() : offsets(std::make_shared<DataArray<qint64>>(NewOffsets())) {}
    static DataArray<qint64> NewOffsets() {
        DataArrayAllocator<qint64> allocator;allocator.storage=std::make_shared<MappedArrayStorage>();
        return DataArray<qint64>(allocator);
    }
    mutable QTemporaryFile file;
    QString filePath;
    std::shared_ptr<DataArray<qint64>> offsets;
    mutable quint64 cachedRow = UINT64_MAX;
    mutable QStringList cached;
    quint64 sourceCount=0;
    std::function<QStringList(quint64)> readRow;
    std::function<QStringList(quint64)> readSource;
    mutable std::atomic<bool> isFrozen{false},isPrepared{false};
};
ReportTable::ReportTable() : m_impl(std::make_unique<Impl>())
{
    if (!m_impl->file.open()) throw std::runtime_error("无法创建报告临时表");
    m_impl->filePath=m_impl->file.fileName();
}
ReportTable::ReportTable(quint64 count,std::function<QStringList(quint64)> readRow):m_impl(std::make_unique<Impl>()) {
    if(!readRow)throw std::invalid_argument("报告只读目录不可用");
    m_impl->sourceCount=count;m_impl->readRow=std::move(readRow);
}
ReportTable::~ReportTable() = default;
void ReportTable::Append(const QStringList& row)
{
    if(m_impl->readRow||m_impl->isFrozen)throw std::logic_error("不可向冻结报告追加行");
    auto& file = m_impl->file;
    if (!file.seek(file.size())) throw std::runtime_error("报告临时表定位失败");
    const auto offset = file.pos();
    WriteBytes(file, QJsonDocument(QJsonArray::fromStringList(row)).toJson(QJsonDocument::Compact) + '\n');
    m_impl->offsets->push_back(offset);
    m_impl->isPrepared=false;
}
QStringList ReportTable::Read(quint64 row) const
{
    if (row >= GetCount()) throw std::out_of_range("报告行不存在");
    if(row>=m_impl->offsets->size()&&m_impl->readSource)return m_impl->readSource(row);
    if (m_impl->cachedRow == row) return m_impl->cached;
    if(m_impl->readRow){auto cells=m_impl->readRow(row);m_impl->cachedRow=row;m_impl->cached=cells;return cells;}
    if (!m_impl->file.seek((*m_impl->offsets)[row])) throw std::runtime_error("报告临时表读取失败");
    QJsonParseError error;
    const auto values = QJsonDocument::fromJson(m_impl->file.readLine(), &error);
    if (error.error != QJsonParseError::NoError || !values.isArray()) throw std::runtime_error("报告临时表损坏");
    QStringList cells;
    for (const auto cell : values.array()) cells.append(cell.toString());
    m_impl->cachedRow = row; m_impl->cached = cells;
    return cells;
}
quint64 ReportTable::GetCount() const { return m_impl->readRow||m_impl->readSource?m_impl->sourceCount:m_impl->offsets->size(); }
class ReportRows::Impl {
public:
    Impl():rows(NewRows()) {}
    static DataArray<quint64> NewRows() {
        DataArrayAllocator<quint64> allocator;allocator.storage=std::make_shared<MappedArrayStorage>();
        return DataArray<quint64>(allocator);
    }
    DataArray<quint64> rows;
};
ReportRows ReportRows::Natural(quint64 count){ReportRows rows;rows.m_naturalCount=count;return rows;}
quint64 ReportRows::size()const{return m_impl?m_impl->rows.size():m_naturalCount;}
quint64 ReportRows::operator[](quint64 offset)const {
    if(offset>=size())throw std::out_of_range("报告索引不存在");
    return m_impl?m_impl->rows[static_cast<std::size_t>(offset)]:offset;
}
QVector<quint64> ReportRows::mid(quint64 offset,int count)const {
    QVector<quint64> page;
    if(offset>size()||count<=0)return page;
    const auto amount=std::min<quint64>(count,size()-offset);page.reserve(static_cast<int>(amount));
    for(quint64 i=0;i<amount;++i)page.append((*this)[offset+i]);return page;
}
void ReportRows::clear(){m_impl.reset();m_naturalCount=0;}
void ReportRows::append(quint64 row) {
    if(!m_impl){if(m_naturalCount)throw std::logic_error("不能向自然顺序索引追加筛选行");m_impl=std::make_shared<Impl>();}
    if(m_impl.use_count()!=1){auto copy=std::make_shared<Impl>();copy->rows=m_impl->rows;m_impl=std::move(copy);}
    m_impl->rows.push_back(row);
}
void ReportRows::Sort(const ReportDocument& document,int column,bool ascending,bool numeric,const ReportCheckpoint& checkpoint,bool ignoreCase) {
    if(!document.table||size()<2)return;
    const auto check=[&]{if(checkpoint)checkpoint();};check();
    const auto readRow=document.table->CreateReader();
    // 固定大小的内存排序段，最多 16 路顺序归并；不缓存全表字符串或分配全量 stable_sort 工作区。
    struct Entry {QString key;quint64 row=0,order=0;};
    struct Run {qint64 offset=0;quint64 count=0;};
    const auto less=[&](const Entry& a,const Entry& b){
        check();double av=0,bv=0;const bool an=numeric&&GetReportNumber(a.key,av),bn=numeric&&GetReportNumber(b.key,bv);
        int order;
        if(an!=bn)order=an?-1:1;
        else if(an&&av!=bv)order=av<bv?-1:1;
        else order=QString::compare(a.key,b.key,!an&&(numeric||ignoreCase)?Qt::CaseInsensitive:Qt::CaseSensitive);
        return order?(ascending?order<0:order>0):a.order<b.order;
    };
    const auto open=[] {auto file=std::make_unique<QTemporaryFile>();if(!file->open())throw std::runtime_error("无法创建报告排序临时文件");return file;};
    const auto write=[](QTemporaryFile& file,const Entry& e){QDataStream stream(&file);stream<<e.row<<e.order<<e.key;if(stream.status()!=QDataStream::Ok)throw std::runtime_error("报告排序写入失败");};
    const auto read=[](QTemporaryFile& file,qint64& offset){if(!file.seek(offset))throw std::runtime_error("报告排序定位失败");Entry e;QDataStream stream(&file);stream>>e.row>>e.order>>e.key;if(stream.status()!=QDataStream::Ok)throw std::runtime_error("报告排序读取失败");offset=file.pos();return e;};
    ResultArrayScope storage;
    auto file=open();DataArray<Run> runs;
    constexpr quint64 segmentSize=8192;
    for(quint64 offset=0;offset<size();) {
        const auto count=(std::min)(segmentSize,size()-offset);std::vector<Entry> segment;segment.reserve(static_cast<std::size_t>(count));
        std::size_t keyBytes=0;
        for(quint64 i=0;i<count;++i){check();const auto row=(*this)[offset+i];auto key=GetReportCell(readRow(row),column);
            const auto bytes=static_cast<std::size_t>(key.size())*sizeof(QChar);
            if(!segment.empty()&&bytes>8*1024*1024-(std::min)(keyBytes,std::size_t(8*1024*1024)))break;
            keyBytes+=bytes;segment.push_back({std::move(key),row,offset+i});}
        std::sort(segment.begin(),segment.end(),less);runs.push_back({file->pos(),segment.size()});
        for(const auto& e:segment)write(*file,e);offset+=segment.size();
    }
    while(runs.size()>1) {
        auto merged=open();DataArray<Run> next;
        for(std::size_t first=0;first<runs.size();first+=16) {
            struct Cursor {qint64 offset;quint64 remaining;Entry value;};std::vector<Cursor> cursors;
            const auto last=(std::min)(first+16,runs.size());quint64 count=0;
            for(auto i=first;i<last;++i){auto offset=runs[i].offset;auto value=read(*file,offset);cursors.push_back({offset,runs[i].count-1,std::move(value)});count+=runs[i].count;}
            const auto compare=[&](std::size_t a,std::size_t b){return less(cursors[b].value,cursors[a].value);};
            std::priority_queue<std::size_t,std::vector<std::size_t>,decltype(compare)> queue(compare);
            for(std::size_t i=0;i<cursors.size();++i)queue.push(i);
            next.push_back({merged->pos(),count});
            while(!queue.empty()){check();const auto i=queue.top();queue.pop();auto& cursor=cursors[i];write(*merged,cursor.value);
                if(cursor.remaining){cursor.value=read(*file,cursor.offset);--cursor.remaining;queue.push(i);}}
        }
        file=std::move(merged);runs=std::move(next);
    }
    auto sorted=std::make_shared<Impl>();sorted->rows.reserve(static_cast<std::size_t>(size()));
    auto offset=runs.front().offset;
    for(quint64 i=0;i<size();++i){check();sorted->rows.push_back(read(*file,offset).row);}
    storage.Finish();std::static_pointer_cast<MappedArrayStorage>(sorted->rows.get_allocator().storage)->Finish();
    m_impl=std::move(sorted);m_naturalCount=0;
}
void ReportTable::SetRows(quint64 count, std::function<QStringList(quint64)> read)
{
    if (!m_impl->offsets->empty() || m_impl->readSource || m_impl->readRow || m_impl->isFrozen || (count && !read))
        throw std::invalid_argument("报告源行数或构建状态无效");
    m_impl->sourceCount = count;
    if (count) m_impl->readSource = std::move(read);
}
bool ReportTable::ContinueBuild()
{
    if (!m_impl->readSource) return true;
    Append(m_impl->readSource(m_impl->offsets->size()));
    if (m_impl->offsets->size() == m_impl->sourceCount) m_impl->readSource = {};
    return !m_impl->readSource;
}
bool ReportTable::GetIsComplete() const { return !m_impl->readSource; }
void ReportTable::PrepareRead()
{
    if(!GetIsComplete())throw std::logic_error("报告尚未完成物化");
    if(m_impl->readRow)return;
    if (!m_impl->file.flush()) throw std::runtime_error("报告临时表刷新失败");
    m_impl->isPrepared=true;
}
void ReportTable::SetOwnerThread(QThread* owner) { m_impl->file.moveToThread(owner); }
std::function<QStringList(quint64)> ReportTable::CreateReader() const
{
    if (!GetIsComplete()) throw std::logic_error("原生报告尚未完成物化");
    if(m_impl->readRow)return m_impl->readRow;
    if(!m_impl->isPrepared) {
        if(m_impl->file.thread()!=QThread::currentThread())throw std::logic_error("后台读取前须在 owner 线程冻结报告");
        if(!m_impl->file.flush())throw std::runtime_error("报告临时表刷新失败");
        m_impl->isPrepared=true;
    }
    m_impl->isFrozen=true;
    struct Cursor {
        QFile file;
        std::shared_ptr<const DataArray<qint64>> offsets;
        Cursor(const QString& path, std::shared_ptr<const DataArray<qint64>> positions) : file(path), offsets(std::move(positions))
        { if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("报告读取器打开失败"); }
    };
    // 写入已结束；每个任务有独立文件游标，共享不可变后备偏移，不复制全表索引。
    auto cursor = std::make_shared<Cursor>(m_impl->filePath, m_impl->offsets);
    return [cursor](quint64 row) {
        if (row >= cursor->offsets->size() || !cursor->file.seek((*cursor->offsets)[row]))
            throw std::out_of_range("报告读取器行不存在");
        QJsonParseError error;
        const auto values = QJsonDocument::fromJson(cursor->file.readLine(), &error);
        if (error.error != QJsonParseError::NoError || !values.isArray()) throw std::runtime_error("报告临时表损坏");
        QStringList cells; for (const auto cell : values.array()) cells.append(cell.toString()); return cells;
    };
}
QString GetReportCell(const QStringList& row, int column) { return column >= 0 && column < row.size() ? row[column] : QString(); }
bool GetReportNumber(const QString& text, double& value)
{
    bool ok = false; value = text.trimmed().toDouble(&ok); return ok && std::isfinite(value);
}
ReportDocument ReadReportCsv(QIODevice& input, const QString& title, const ReportCheckpoint& checkpoint)
{
    ReportDocument doc; doc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    doc.title = title; doc.category = "导入报告"; doc.table = std::make_shared<ReportTable>();
    QString cell; QStringList row;
    bool quoted = false, closed = false, started = false, skipLf = false, first = true, header = true;
    int scopedColumn=-1;
    const auto emitRow = [&] {
        row.append(cell); cell.clear();
        if (header) {
            if (std::all_of(row.cbegin(),row.cend(),[](const QString& name){return name.trimmed().isEmpty();})) throw std::runtime_error("CSV 缺少列名");
            // 身份属于列头注记，文件始终只有一行表头和同宽数据行，通用 CSV 工具可直接读取。
            for (auto name : row) {
                const auto annotated=QRegularExpression("^(.+) \\[mvvcvtk-result=([^\\]]+)\\]$").match(name);
                if(annotated.hasMatch()) {
                    const auto ref=annotated.captured(2);
                    if(!GetResultIdentityValid(ref)||!doc.identityResult.isEmpty())throw std::runtime_error("CSV 结果编号上下文无效或重复");
                    const auto identity=ref.left(32).toLower()+":"+QString::number(ref.mid(33).toULongLong());
                    doc.identityResult=identity;doc.result=identity;doc.relatedResults.append(identity);name=annotated.captured(1);
                    scopedColumn=doc.columns.size();
                }
                doc.columns.append({name, {}, {}, false});
            }
            header = false;
        } else {
            if (row.size() != doc.columns.size()) throw std::runtime_error("CSV 行的列数与表头不一致");
            doc.table->Append(row);
        }
        row.clear(); started = false; closed = false;
        if (checkpoint) checkpoint();
    };
    QTextCodec::ConverterState state;
    auto* codec = QTextCodec::codecForName("UTF-8");
    const auto consume = [&](const QString& text) {
        for (const QChar ch : text) {
            if (first) { first = false; if (ch == QChar(0xfeff)) continue; }
            if (skipLf) { skipLf = false; if (ch == '\n') continue; }
            if (quoted) {
                if (ch == '"') { quoted = false; closed = true; }
                else cell += ch;
                continue;
            }
            if (closed && ch == '"') { cell += '"'; quoted = true; closed = false; continue; }
            if (ch == ',') { row.append(cell); cell.clear(); started = true; closed = false; continue; }
            if (ch == '\r' || ch == '\n') { emitRow(); skipLf = ch == '\r'; continue; }
            if (closed) throw std::runtime_error("CSV 结束引号后存在非法字符");
            if (ch == '"') {
                if (!cell.isEmpty()) throw std::runtime_error("CSV 未转义的引号");
                quoted = true; started = true;
            } else { cell += ch; started = true; }
        }
    };
    while (!input.atEnd()) {
        if (checkpoint) checkpoint();
        const auto bytes = input.read(65536);
        if (bytes.isEmpty() && !input.atEnd()) throw std::runtime_error("CSV 读取失败");
        consume(codec->toUnicode(bytes.constData(), bytes.size(), &state));
        if (state.invalidChars) throw std::runtime_error("CSV 必须采用有效 UTF-8 编码");
    }
    if (state.remainingChars || quoted) throw std::runtime_error("CSV 编码或引号未结束");
    if (started || closed || !row.isEmpty()) emitRow();
    if (header) throw std::runtime_error("CSV 文件为空");
    // 只识别明确列名；未知列原样保留，比较前再由用户确认语义。
    QHash<QString,int> nameCounts;
    for (int i = 0; i < doc.columns.size(); ++i) {
        if(checkpoint)checkpoint();
        auto& column = doc.columns[i]; const auto name = column.name;
        const auto definition = QRegularExpression("^(.*) \\[(.*); ([^;\\[\\]]+)\\]$").match(name);
        if (definition.hasMatch()) {
            column.name = definition.captured(1); column.quantity = definition.captured(2);
            column.unit = definition.captured(3); column.numeric = true;
        }
        ++nameCounts[column.name];
    }
    for (int i = 0; i < doc.columns.size(); ++i) {
        if(checkpoint)checkpoint();
        auto& column = doc.columns[i]; const auto name = column.name;
        const auto unique=[&](const QString& key){return nameCounts.value(key)==1;};
        if(!unique(name))doc.mappingRequired=true;
        const auto match=QRegularExpression("^(.*)_(mm|cm|um|m)([23]?)$").match(name);
        const auto quantity=match.hasMatch()?match.captured(1):QString();
        if(match.hasMatch()){
            column.numeric=true;const auto unit=match.captured(2)+match.captured(3);
            if(column.unit.isEmpty())column.unit=unit;else if(column.unit!=unit)doc.mappingRequired=true;
        }
        if ((name == "sample_id" || name == "void_id" || name == "item_id" || name == "object_id") && unique(name)) {
            if(doc.identityColumn>=0)doc.identityColumn=-2;else if(doc.identityColumn!=-2)doc.identityColumn=i;
        }
        if (name == "validity" && unique(name)) doc.validityColumn = i;
        if (quantity == "x" && unique(name)) doc.xColumn = i;
        if (quantity == "y" && unique(name)) doc.yColumn = i;
        if (quantity == "z" && unique(name)) doc.zColumn = i;
        if (quantity == "thickness") {if(column.quantity.isEmpty())column.quantity = "聚合节点厚度场值";doc.valueColumn=i;}
        if (quantity == "measured" || quantity == "selected_cell_volume") doc.valueColumn=i;
        if(column.quantity.isEmpty()) {
            if (quantity == "parent_volume") column.quantity = "完整父缺陷体积";
            if (quantity == "intersection_volume") column.quantity = "ROI 内相交体积";
            if (quantity == "gap_statistics_volume") column.quantity = "本次统计体积";
            if (quantity == "surface_object_volume") column.quantity = "表面对象体积";
            if (quantity == "selected_cell_volume") column.quantity = "选区单元体积和";
            if (quantity == "volume") column.quantity = "体积";
        }
    }
    if(doc.identityColumn==-2)doc.identityColumn=-1;
    // 点级表中的 object_id 可以重复；全局 point_id 才是该结果内的行身份。
    int pointColumn=-1;
    for(int i=0;i<doc.columns.size();++i)if(doc.columns[i].name=="point_id") {
        if(pointColumn>=0){pointColumn=-2;break;}pointColumn=i;
    }
    if(pointColumn>=0)doc.identityColumn=pointColumn;
    if(scopedColumn>=0&&scopedColumn!=doc.identityColumn)
        throw std::runtime_error("CSV 结果身份注记必须属于实际编号列");
    doc.description = "独立只读表格；导入不会恢复 SDK 结果或修改当前输入。";
    if(doc.columns.size()==4 && doc.columns[0].name=="分组" && doc.columns[1].name=="字段" && doc.columns[2].name=="类型" && doc.columns[3].name=="值")
        doc.description="旧版运行记录；保留原文件字段，不表示已恢复完整测点或孔隙业务结果。";
    doc.table->PrepareRead(); return doc;
}
void WriteReportCsv(QIODevice& output, const ReportDocument& doc, const ReportRows* rows, const ReportCheckpoint& checkpoint)
{
    const auto write = [&](const QStringList& cells) {
        QStringList escaped; for (const auto& cell : cells) escaped.append(CsvCell(cell));
        WriteBytes(output, (escaped.join(',') + "\r\n").toUtf8());
    };
    WriteBytes(output, QByteArray("\xef\xbb\xbf"));
    // 业务含义与单位属于列定义，留在同一 CSV 表头；不混入说明行或附属元数据。
    QStringList headings; for (const auto& column : doc.columns)
        headings.append(column.numeric && !column.quantity.isEmpty() && !column.unit.isEmpty()
            ? column.name+" ["+column.quantity+"; "+column.unit+"]" : column.name);
    if(!doc.identityResult.isEmpty()) {
        if(!GetResultIdentityValid(doc.identityResult)||doc.identityColumn<0||doc.identityColumn>=headings.size())
            throw std::runtime_error("报告结果编号上下文无效或缺少对应编号列");
        headings[doc.identityColumn]+=" [mvvcvtk-result="+doc.identityResult+"]";
    }
    write(headings);
    if (!doc.table) return;
    if(!checkpoint)doc.table->PrepareRead();
    const auto read=doc.table->CreateReader();
    const auto count=rows?rows->size():doc.table->GetCount();
    for(quint64 i=0;i<count;++i){if(checkpoint)checkpoint();write(read(rows?(*rows)[i]:i));}
}
void ExportReport(const QString& path, const ReportDocument& doc, const ReportRows* rows, const ReportCheckpoint& checkpoint)
{
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("无法打开报告输出文件");
    WriteReportCsv(output, doc, rows, checkpoint);
    if(checkpoint)checkpoint();
    if (!output.commit()) throw std::runtime_error("报告保存失败");
}
}
