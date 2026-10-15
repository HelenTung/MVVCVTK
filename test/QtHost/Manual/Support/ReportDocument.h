#pragma once
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <memory>
#include <functional>
#include <cstddef>
#include <iterator>
class QIODevice;
class QThread;
namespace Manual {
using ReportCheckpoint = std::function<void()>;
// 仅持有所需的只读目录或临时表；不拥有 Session、体数据或完整几何。
class ReportTable final {
public:
    ReportTable();
    ReportTable(quint64 count,std::function<QStringList(quint64)> readRow);
    ~ReportTable();
    void Append(const QStringList& row);
    QStringList Read(quint64 row) const;
    quint64 GetCount() const;
    void SetRows(quint64 count, std::function<QStringList(quint64)> read);
    bool ContinueBuild();
    bool GetIsComplete() const;
    void PrepareRead();
    void SetOwnerThread(QThread* owner);
    // 独立文件游标；调用方持有 table，且仅在物化完成后创建读取器。
    std::function<QStringList(quint64)> CreateReader() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
struct ReportDocument;
// 自然顺序只保存总数；筛选/排序目录使用同一后备存储，不与 Qt 页内 int 行号混用。
class ReportRows final {
public:
    class const_iterator final {
    public:
        using value_type=quint64;using difference_type=std::ptrdiff_t;
        using pointer=void;using reference=quint64;using iterator_category=std::forward_iterator_tag;
        const_iterator(const ReportRows* rows,quint64 offset):m_rows(rows),m_offset(offset){}
        quint64 operator*()const{return (*m_rows)[m_offset];}
        const_iterator& operator++(){++m_offset;return *this;}
        const_iterator operator++(int){auto before=*this;++*this;return before;}
        bool operator==(const const_iterator& other)const{return m_rows==other.m_rows&&m_offset==other.m_offset;}
        bool operator!=(const const_iterator& other)const{return !(*this==other);}
    private:const ReportRows* m_rows;quint64 m_offset;
    };
    ReportRows()=default;
    static ReportRows Natural(quint64 count);
    quint64 size() const;
    bool isEmpty() const {return size()==0;}
    const_iterator begin()const{return {this,0};}const_iterator end()const{return {this,size()};}
    const_iterator cbegin()const{return begin();}const_iterator cend()const{return end();}
    quint64 operator[](quint64 offset) const;
    QVector<quint64> mid(quint64 offset,int count) const;
    void clear();
    void append(quint64 row);
    void Sort(const ReportDocument& document,int column,bool ascending,bool numeric=true,
        const ReportCheckpoint& checkpoint={},bool ignoreCase=false);
private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
    quint64 m_naturalCount=0;
};
struct ReportColumn final {
    QString name, quantity, unit;
    bool numeric = false;
};
struct ReportDocument final {
    QString id, title, category, module, source, result, description;
    // 仅在该正式结果内稳定的对象/点编号；与跨结果稳定的外部业务编号分开。
    QString identityResult;
    QString coordinateFrame;
    QStringList relatedResults, relatedSources, relatedModules;
    QVector<ReportColumn> columns;
    std::shared_ptr<ReportTable> table;
    QJsonObject conditions;
    int identityColumn = -1, validityColumn = -1;
    int valueColumn = -1;
    int xColumn = -1, yColumn = -1, zColumn = -1;
    bool identityStable = false;
    bool mappingRequired = false;
    QStringList validValues{"0","Valid"};
};
ReportDocument ReadReportCsv(QIODevice& input, const QString& title, const ReportCheckpoint& checkpoint = {});
void WriteReportCsv(QIODevice& output, const ReportDocument& document, const ReportRows* rows = nullptr, const ReportCheckpoint& checkpoint = {});
void ExportReport(const QString& path, const ReportDocument& document, const ReportRows* rows = nullptr, const ReportCheckpoint& checkpoint = {});
QString GetReportCell(const QStringList& row, int column);
bool GetReportNumber(const QString& text, double& value);
}
