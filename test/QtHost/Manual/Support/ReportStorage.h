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
    void SetOwnerThread(QThread* owner);
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
#include "Data/Support/PagedArray.h"
#include <list>
#include <unordered_map>

namespace Manual {
// 变长键只在文件中保留 UTF-16；排序记录保存区间，不拥有每行字符串。
struct ReportTextRef final { std::size_t offset=0,count=0; };
class ReportTextStore final {
public:
    ReportTextRef Append(const QString& text) {
        if(m_finished)throw std::logic_error("报告键已冻结");
        ReportTextRef ref{m_writing.size(),static_cast<std::size_t>(text.size())};
        for(const auto ch:text)m_writing.push_back(ch.unicode());return ref;
    }
    void Finish(){m_writing.GetReadSnapshot();m_finished=true;}
    QString Read(ReportTextRef ref)const {
        if(!m_finished || ref.offset>m_writing.size() || ref.count>m_writing.size()-ref.offset)throw std::out_of_range("报告键区间无效");
        if(!ref.count)return {};
        const auto found=m_cache.find(ref.offset);
        if(found!=m_cache.end() && found->second.value.size()==static_cast<int>(ref.count)) {
            m_order.splice(m_order.begin(),m_order,found->second.position);return found->second.value;
        }
        QString text; text.resize(static_cast<int>(ref.count));
        std::array<ushort,256> buffer{};
        for(std::size_t offset=0;offset<ref.count;) {
            const auto count=std::min(buffer.size(),ref.count-offset);
            m_writing.CopyValues(ref.offset+offset,count,buffer.data());
            // 按原始 code unit 恢复；保留单元格开头的 BOM 和跨块代理项，不做编码探测。
            for(std::size_t i=0;i<count;++i)text[static_cast<int>(offset+i)]=QChar(buffer[i]);offset+=count;
        }
        // 缓存只降低重复键读取成本；达到固定工作区大小时淘汰，仍处理全部键。
        if(ref.count<=32768) {
            while(!m_order.empty() && (m_cache.size()>=4096 || m_cacheBytes+ref.count*2>2*1024*1024)) {
                const auto old=m_order.back();m_order.pop_back();m_cacheBytes-=static_cast<std::size_t>(m_cache.at(old).value.size())*2;m_cache.erase(old);
            }
            m_order.push_front(ref.offset);m_cache.emplace(ref.offset,Entry{text,m_order.begin()});m_cacheBytes+=ref.count*2;
        }
        return text;
    }
private:
    PagedArray<ushort> m_writing;
    bool m_finished=false;
    struct Entry { QString value; std::list<std::size_t>::iterator position; };
    mutable std::list<std::size_t> m_order;
    mutable std::unordered_map<std::size_t,Entry> m_cache;
    mutable std::size_t m_cacheBytes=0;
};
}
