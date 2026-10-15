#include "ReportDocument.h"
#include <QTemporaryFile>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextCodec>
#include <QUuid>
#include <QRegularExpression>
#include <QHash>
#include <array>
#include <QThread>
#include <atomic>
#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <cstring>
namespace Manual {
QJsonValue GetReportCountValue(quint64 count)
{
    return count<=9007199254740991ULL?QJsonValue(static_cast<double>(count)):QJsonValue(QString::number(count));
}
quint64 GetReportCount(const QJsonValue& value)
{
    if(value.isString()) {
        const auto text=value.toString();bool ok=false;const auto count=text.toULongLong(&ok);
        if(ok && QRegularExpression("^[0-9]+$").match(text).hasMatch())return count;
    } else if(value.isDouble()) {
        const auto number=value.toDouble();
        if(std::isfinite(number) && number>=0 && number<=9007199254740991. && number==std::trunc(number))return static_cast<quint64>(number);
    }
    throw std::invalid_argument("报告计数必须为精确非负整数；大整数使用十进制字符串");
}
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
    mutable QTemporaryFile file;
    mutable QTemporaryFile index;
    QString filePath,indexPath;
    ReportRowId rowCount = 0;
    mutable ReportRowId cachedRow = -1;
    mutable QStringList cached;
    ReportRowId sourceCount = 0;
    std::function<QStringList(ReportRowId)> readSource;
    mutable ReportRowId indexStart = -1;
    mutable int indexCount = 0;
    mutable QByteArray indexPage;
    mutable bool indexDirty=false;
    std::function<QStringList(ReportRowId)> readRow;
    mutable std::atomic<bool> isFrozen{false},isPrepared{false};
};
namespace {
qint64 ReadOffset(QFile& index,ReportRowId row,ReportRowId count,ReportRowId& start,int& length,QByteArray& page)
{
    if(row<start || row>=start+length) {
        const auto first=row/1024*1024;
        const ReportRowId records=count-first<1024?count-first:1024;
        const auto bytes=records*static_cast<qint64>(sizeof(qint64));
        if(!index.seek(first*static_cast<qint64>(sizeof(qint64))))throw std::runtime_error("报告偏移索引定位失败");
        auto loaded=index.read(bytes);if(loaded.size()!=bytes)throw std::runtime_error("报告偏移索引读取失败");
        page=std::move(loaded);
        start=first;length=static_cast<int>(records);
    }
    qint64 offset;std::memcpy(&offset,page.constData()+(row-start)*sizeof(qint64),sizeof(offset));return offset;
}
}
ReportTable::ReportTable() : m_impl(std::make_unique<Impl>())
{
    if (!m_impl->file.open() || !m_impl->index.open()) throw std::runtime_error("无法创建报告临时表或索引");
    m_impl->filePath=m_impl->file.fileName();
    m_impl->indexPath=m_impl->index.fileName();
}
ReportTable::ReportTable(quint64 count,std::function<QStringList(ReportRowId)> readRow):m_impl(std::make_unique<Impl>()) {
    if(!readRow)throw std::invalid_argument("报告只读目录不可用");
    if(count>static_cast<quint64>(std::numeric_limits<ReportRowId>::max()))throw std::overflow_error("报告目录地址溢出");
    m_impl->sourceCount=count;m_impl->readRow=std::move(readRow);
}
ReportTable::~ReportTable() = default;
void ReportTable::Append(const QStringList& row)
{
    if(m_impl->readRow||m_impl->isFrozen)throw std::logic_error("不可向冻结报告追加行");
    m_impl->isPrepared=false;
    if(m_impl->rowCount>=std::numeric_limits<qint64>::max()/static_cast<qint64>(sizeof(qint64)))throw std::overflow_error("报告偏移索引地址溢出");
    auto& file = m_impl->file;
    if (!file.seek(file.size())) throw std::runtime_error("报告临时表定位失败");
    const auto offset = file.pos();
    WriteBytes(file, QJsonDocument(QJsonArray::fromStringList(row)).toJson(QJsonDocument::Compact) + '\n');
    auto& index=m_impl->index;
    if(!index.seek(index.size()) || index.write(reinterpret_cast<const char*>(&offset),sizeof(offset))!=sizeof(offset))
        throw std::runtime_error("报告偏移索引写入失败");
    m_impl->indexStart=-1;m_impl->indexCount=0;
    m_impl->indexDirty=true;
    ++m_impl->rowCount;
}
QStringList ReportTable::Read(ReportRowId row) const
{
    if (row < 0 || row >= GetCount()) throw std::out_of_range("报告行不存在");
    if (row >= m_impl->rowCount && m_impl->readSource) return m_impl->readSource(row);
    if (m_impl->cachedRow == row) return m_impl->cached;
    if(m_impl->readRow){auto cells=m_impl->readRow(row);m_impl->cachedRow=row;m_impl->cached=cells;return cells;}
    if(m_impl->indexDirty){if(!m_impl->index.flush())throw std::runtime_error("报告索引刷新失败");m_impl->indexDirty=false;}
    const auto offset=ReadOffset(m_impl->index,row,m_impl->rowCount,m_impl->indexStart,m_impl->indexCount,m_impl->indexPage);
    if (!m_impl->file.seek(offset)) throw std::runtime_error("报告临时表读取失败");
    QJsonParseError error;
    const auto bytes=m_impl->file.readLine();const auto values = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !values.isArray()) {
        throw std::runtime_error(QString("报告临时表损坏：行 %1，偏移 %2，字节数 %3，%4").arg(row).arg(offset).arg(bytes.size()).arg(error.errorString()).toStdString());
    }
    QStringList cells;
    for (const auto cell : values.array()) cells.append(cell.toString());
    m_impl->cachedRow = row; m_impl->cached = cells;
    return cells;
}
ReportRowId ReportTable::GetCount() const { return m_impl->readRow || m_impl->readSource ? m_impl->sourceCount : m_impl->rowCount; }
void ReportTable::SetRows(std::size_t count, std::function<QStringList(ReportRowId)> read)
{
    if (m_impl->rowCount || m_impl->readSource || m_impl->readRow || m_impl->isFrozen || (count && !read) || count > std::size_t(std::numeric_limits<ReportRowId>::max()))
        throw std::invalid_argument("报告源行数或构建状态无效");
    m_impl->sourceCount = static_cast<ReportRowId>(count);
    if (count) m_impl->readSource = std::move(read);
}
bool ReportTable::ContinueBuild()
{
    if (!m_impl->readSource) return true;
    Append(m_impl->readSource(m_impl->rowCount));
    if (m_impl->rowCount == m_impl->sourceCount) m_impl->readSource = {};
    return !m_impl->readSource;
}
bool ReportTable::GetIsComplete() const { return !m_impl->readSource; }
void ReportTable::PrepareRead()
{
    if(m_impl->readRow)return;
    if (!m_impl->file.flush() || !m_impl->index.flush()) throw std::runtime_error("报告临时表或索引刷新失败");
    m_impl->indexDirty=false;m_impl->isPrepared=true;
}
void ReportTable::SetOwnerThread(QThread* owner) { m_impl->file.moveToThread(owner);m_impl->index.moveToThread(owner); }
std::function<QStringList(ReportRowId)> ReportTable::CreateReader() const
{
    if (!GetIsComplete()) throw std::logic_error("原生报告尚未完成物化");
    if(m_impl->readRow)return m_impl->readRow;
    if(!m_impl->isPrepared) {
        if(m_impl->file.thread()!=QThread::currentThread())throw std::logic_error("后台读取前须在 owner 线程冻结报告");
        if(!m_impl->file.flush() || !m_impl->index.flush())throw std::runtime_error("报告临时表或索引刷新失败");
        m_impl->indexDirty=false;m_impl->isPrepared=true;
    }
    m_impl->isFrozen=true;
    struct Cursor {
        QFile file;
        QFile index;
        const ReportRowId count;
        ReportRowId start=-1;
        int length=0;
        QByteArray page;
        Cursor(const QString& path,const QString& indexPath,ReportRowId rows) : file(path),index(indexPath),count(rows)
        { if (!file.open(QIODevice::ReadOnly) || !index.open(QIODevice::ReadOnly)) throw std::runtime_error("报告读取器打开失败"); }
    };
    // 正文和偏移均在文件中；每个读取器只缓存一个索引页，不复制全表索引。
    auto cursor = std::make_shared<Cursor>(m_impl->filePath,m_impl->indexPath,m_impl->rowCount);
    return [cursor](ReportRowId row) {
        if(row<0 || row>=cursor->count)throw std::out_of_range("报告读取器行不存在");
        const auto offset=ReadOffset(cursor->index,row,cursor->count,cursor->start,cursor->length,cursor->page);
        if (!cursor->file.seek(offset))
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
    const auto count=doc.table->GetCount();
    if(!checkpoint)doc.table->PrepareRead(); // 后台入口在 owner thread 准备，同步调用在此刷新。
    const auto read=doc.table->GetIsComplete()?doc.table->CreateReader():[&doc](ReportRowId row){return doc.table->Read(row);};
    if (rows) { for (const auto row : *rows) {if(checkpoint)checkpoint();write(read(row));} }
    else for (ReportRowId row = 0; row < count; ++row) {if(checkpoint)checkpoint();write(read(row));}
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
