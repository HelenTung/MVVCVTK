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
#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>
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
}
class ReportTable::Impl {
public:
    mutable QTemporaryFile file;
    QString filePath;
    QVector<qint64> offsets;
    mutable int cachedRow = -1;
    mutable QStringList cached;
    int sourceCount = 0;
    std::function<QStringList(int)> readSource;
};
ReportTable::ReportTable() : m_impl(std::make_unique<Impl>())
{
    if (!m_impl->file.open()) throw std::runtime_error("无法创建报告临时表");
    m_impl->filePath=m_impl->file.fileName();
}
ReportTable::~ReportTable() = default;
void ReportTable::Append(const QStringList& row)
{
    if (m_impl->offsets.size() == std::numeric_limits<int>::max()) throw std::runtime_error("报告行数超出 Qt 表格范围");
    auto& file = m_impl->file;
    if (!file.seek(file.size())) throw std::runtime_error("报告临时表定位失败");
    const auto offset = file.pos();
    WriteBytes(file, QJsonDocument(QJsonArray::fromStringList(row)).toJson(QJsonDocument::Compact) + '\n');
    m_impl->offsets.append(offset);
}
QStringList ReportTable::Read(int row) const
{
    if (row < 0 || row >= GetCount()) throw std::out_of_range("报告行不存在");
    if (row >= m_impl->offsets.size() && m_impl->readSource) return m_impl->readSource(row);
    if (m_impl->cachedRow == row) return m_impl->cached;
    if (!m_impl->file.seek(m_impl->offsets[row])) throw std::runtime_error("报告临时表读取失败");
    QJsonParseError error;
    const auto values = QJsonDocument::fromJson(m_impl->file.readLine(), &error);
    if (error.error != QJsonParseError::NoError || !values.isArray()) throw std::runtime_error("报告临时表损坏");
    QStringList cells;
    for (const auto cell : values.array()) cells.append(cell.toString());
    m_impl->cachedRow = row; m_impl->cached = cells;
    return cells;
}
int ReportTable::GetCount() const { return m_impl->readSource ? m_impl->sourceCount : m_impl->offsets.size(); }
void ReportTable::SetRows(std::size_t count, std::function<QStringList(int)> read)
{
    if (!m_impl->offsets.isEmpty() || m_impl->readSource || (count && !read) || count > std::size_t(std::numeric_limits<int>::max()))
        throw std::invalid_argument("报告源行数或构建状态无效");
    m_impl->sourceCount = static_cast<int>(count);
    if (count) m_impl->readSource = std::move(read);
}
bool ReportTable::ContinueBuild()
{
    if (!m_impl->readSource) return true;
    Append(m_impl->readSource(m_impl->offsets.size()));
    if (m_impl->offsets.size() == m_impl->sourceCount) m_impl->readSource = {};
    return !m_impl->readSource;
}
bool ReportTable::GetIsComplete() const { return !m_impl->readSource; }
void ReportTable::PrepareRead()
{
    if (!m_impl->file.flush()) throw std::runtime_error("报告临时表刷新失败");
}
void ReportTable::SetOwnerThread(QThread* owner) { m_impl->file.moveToThread(owner); }
std::function<QStringList(int)> ReportTable::CreateReader() const
{
    if (!GetIsComplete()) throw std::logic_error("原生报告尚未完成物化");
    struct Cursor {
        QFile file;
        QVector<qint64> offsets;
        Cursor(const QString& path, QVector<qint64> positions) : file(path), offsets(std::move(positions))
        { if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("报告读取器打开失败"); }
    };
    // 写入已结束；每个任务有自己的 QFile、偏移快照，不共享游标或单行缓存。
    auto cursor = std::make_shared<Cursor>(m_impl->filePath, m_impl->offsets);
    return [cursor](int row) {
        if (row < 0 || row >= cursor->offsets.size() || !cursor->file.seek(cursor->offsets[row]))
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
    const auto emitRow = [&] {
        row.append(cell); cell.clear();
        if (header) {
            if (std::all_of(row.cbegin(),row.cend(),[](const QString& name){return name.trimmed().isEmpty();})) throw std::runtime_error("CSV 缺少列名");
            for (const auto& name : row) doc.columns.append({name, {}, {}, false});
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
    doc.description = "独立只读表格；导入不会恢复 SDK 结果或修改当前输入。";
    if(doc.columns.size()==4 && doc.columns[0].name=="分组" && doc.columns[1].name=="字段" && doc.columns[2].name=="类型" && doc.columns[3].name=="值")
        doc.description="旧版运行记录；保留原文件字段，不表示已恢复完整测点或孔隙业务结果。";
    doc.table->PrepareRead(); return doc;
}
void WriteReportCsv(QIODevice& output, const ReportDocument& doc, const QVector<int>* rows, const ReportCheckpoint& checkpoint)
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
    write(headings);
    if (!doc.table) return;
    if(!checkpoint)doc.table->PrepareRead(); // 后台入口在 owner thread 准备，同步调用在此刷新。
    const auto read=doc.table->GetIsComplete()?doc.table->CreateReader():[&doc](int row){return doc.table->Read(row);};
    if (rows) { for (const auto row : *rows) {if(checkpoint)checkpoint();write(read(row));} }
    else for (int row = 0; row < doc.table->GetCount(); ++row) {if(checkpoint)checkpoint();write(read(row));}
}
void ExportReport(const QString& path, const ReportDocument& doc, const QVector<int>* rows, const ReportCheckpoint& checkpoint)
{
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("无法打开报告输出文件");
    WriteReportCsv(output, doc, rows, checkpoint);
    if(checkpoint)checkpoint();
    if (!output.commit()) throw std::runtime_error("报告保存失败");
}
}
