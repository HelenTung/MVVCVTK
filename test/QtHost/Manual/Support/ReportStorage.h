#pragma once
#include "ReportDocument.h"
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
