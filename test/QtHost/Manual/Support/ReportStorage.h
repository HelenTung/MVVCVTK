#pragma once
#include "ReportDocument.h"
#include <QTemporaryFile>
#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
namespace Manual {
// 工作文件承载完整索引，页缓存只影响调度和性能，不限制可接受的总行数。
class ReportIndex final {
public:
    ReportIndex();
    ~ReportIndex();
    void Append(ReportRowId value);
    ReportRowId Read(ReportRowId row) const;
    ReportRowId GetCount() const;
    void PrepareRead() const;
    std::function<ReportRowId(ReportRowId)> CreateReader() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
// 匹配状态和哈希节点有磁盘支持，由操作系统按需换页，不分配 N 项堆数组。
// 单个文件的自然地址/IO 错误如实报告，不以预估内存或数据规模拒绝任务。
template<class T> class ReportDiskArray final {
    static_assert(std::is_trivially_copyable_v<T>);
public:
    explicit ReportDiskArray(ReportRowId count,const ReportCheckpoint& checkpoint = {}) : m_count(count) {
        if(count<0 || count>std::numeric_limits<qint64>::max()/static_cast<qint64>(sizeof(T)))throw std::overflow_error("报告索引文件地址溢出");
        if(!m_file.open())throw std::runtime_error("无法创建报告匹配索引");
        std::array<T,1024> zero{};
        for(ReportRowId next=0;next<count;) {
            if(checkpoint)checkpoint();
            const ReportRowId records=count-next<1024?count-next:1024;const auto bytes=records*static_cast<qint64>(sizeof(T));
            if(m_file.write(reinterpret_cast<const char*>(zero.data()),bytes)!=bytes)throw std::runtime_error("报告匹配索引空间分配失败");
            next+=records;
        }
        if(!m_file.flush())throw std::runtime_error("报告匹配索引刷新失败");
        if(count){m_data=reinterpret_cast<T*>(m_file.map(0,count*static_cast<qint64>(sizeof(T))));if(!m_data)throw std::runtime_error("报告匹配索引映射失败");}
    }
    ~ReportDiskArray(){if(m_data)m_file.unmap(reinterpret_cast<uchar*>(m_data));}
    T& operator[](ReportRowId row){if(row<0 || row>=m_count)throw std::out_of_range("报告匹配索引行不存在");return m_data[row];}
    const T& operator[](ReportRowId row) const {if(row<0 || row>=m_count)throw std::out_of_range("报告匹配索引行不存在");return m_data[row];}
private:
    QTemporaryFile m_file;
    ReportRowId m_count;
    T* m_data=nullptr;
};
struct ReportSortKey final {ReportRowId row=0;QString text;double number=0;bool numeric=false;};
// 固定大小排序段 + 多轮归并；每次 Continue 只推进一个记录，不设总量预算。
class ReportSorter final {
public:
    explicit ReportSorter(Qt::SortOrder direction);
    ~ReportSorter();
    void Add(ReportSortKey key);
    void Finish();
    bool Continue();
    bool GetCanAdd() const;
    std::shared_ptr<ReportIndex> GetRows() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
