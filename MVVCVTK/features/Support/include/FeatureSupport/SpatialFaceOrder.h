#pragma once
#include "Data/DataPayloads.h"
#include "MappedArrayStorage.h"
#include "MeshPayloadBuilder.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <variant>

namespace FeatureInternal {
// 只排列显示索引，正式面数组及其编号不变。小索引保持 32 位。
class SpatialFaceOrder final {
public:
    using Cancel=std::function<bool()>;
    static constexpr std::size_t pageFaces=16384;
    static constexpr bool GetUses32BitForCount(std::uint64_t count) noexcept {return count==0||count-1<=UINT32_MAX;}
    static SpatialFaceOrder Build(const SurfaceMeshPayload& mesh,const Cancel& cancel,std::size_t page=pageFaces)
    {
        if(!page||page>pageFaces)throw std::invalid_argument("Invalid spatial display page size.");
        SpatialFaceOrder result;
        if(GetUses32BitForCount(mesh.GetTriangleIndices().size()/3))result.Set<std::uint32_t>(mesh,cancel,page);
        else result.Set<std::uint64_t>(mesh,cancel,page);
        return result;
    }
    std::uint64_t GetFace(std::size_t spatial) const {return std::visit([&](const auto& order)->std::uint64_t{return order.at(spatial);},m_order);}
    std::uint64_t GetSpatial(std::size_t face) const {return std::visit([&](const auto& inverse)->std::uint64_t{return inverse.at(face);},m_inverse);}
    bool GetIs32Bit() const noexcept {return m_order.index()==0;}
    std::pair<std::size_t,std::size_t> GetRange(std::size_t begin,std::size_t end,
        std::uint64_t first,std::uint64_t last,std::uint64_t* records=nullptr) const
    {
        return std::visit([&](const auto& order){
            if(begin>end||end>order.size())throw std::out_of_range("Display page range is unavailable.");
            const auto compare=[&](auto id,std::uint64_t target){if(records)++*records;return static_cast<std::uint64_t>(id)<target;};
            const auto low=std::lower_bound(order.begin()+begin,order.begin()+end,first,compare);
            const auto high=std::lower_bound(low,order.begin()+end,last,compare);
            return std::make_pair(static_cast<std::size_t>(low-order.begin()),static_cast<std::size_t>(high-order.begin()));
        },m_order);
    }
private:
    template<class Index> struct Entry final {std::uint32_t key=0;Index face=0;};
    static void Check(const Cancel& cancel){if(cancel&&cancel())throw std::runtime_error("Display spatial ordering cancelled.");}
    static std::uint32_t Spread(std::uint32_t x) noexcept
    {
        x=(x|(x<<16))&0x030000FFU;x=(x|(x<<8))&0x0300F00FU;
        x=(x|(x<<4))&0x030C30C3U;return (x|(x<<2))&0x09249249U;
    }
    template<class Index> void Set(const SurfaceMeshPayload& mesh,const Cancel& cancel,std::size_t page)
    {
        const auto count=mesh.GetTriangleIndices().size()/3;
        // 正式显示目录使用调用方的 owner；radix 工作文件在本函数结束时独立释放。
        DataArray<Index> order(count),inverse(count);
        if(!count){m_order=std::move(order);m_inverse=std::move(inverse);return;}
        const auto& points=mesh.GetPointValues();const auto& faces=mesh.GetTriangleIndices();
        const auto inf=std::numeric_limits<double>::infinity();
        std::array<double,3> low{inf,inf,inf},high{-inf,-inf,-inf};
        for(std::size_t offset=0;offset<points.size();) {
            Check(cancel);const auto chunk=points.GetChunk(offset,3*4096,cancel);
            if(chunk.error!=MeshReadError::None || chunk.values.empty())throw std::runtime_error("Display point block read failed.");
            for(std::size_t i=0;i<chunk.values.size();i+=3)for(unsigned a=0;a<3;++a){low[a]=std::min(low[a],chunk.values[i+a]);high[a]=std::max(high[a],chunk.values[i+a]);}
            offset=chunk.nextOffset;
        }
        {
            ResultArrayScope workspace(0);
            DataArray<Entry<Index>> keys(count),scratch(count);
            for(std::size_t first=0;first<count;) {
              Check(cancel);const auto chunk=faces.GetChunk(first*3,3*std::min(std::size_t(4096),count-first),cancel);
              if(chunk.error!=MeshReadError::None || chunk.values.empty())throw std::runtime_error("Display topology block read failed.");
              for(std::size_t local=0;local<chunk.values.size()/3;++local) {
                const auto face=first+local;
                // 随机坐标只读取当前三角形的 9 个值，工作区不随完整点数增长。
                std::array<std::array<double,3>,3> vertices{};
                for(unsigned corner=0;corner<3;++corner)MeshPayloadBuilder::CopyValues(points,
                    static_cast<std::size_t>(chunk.values[3*local+corner])*3,3,vertices[corner].data());
                std::array<std::uint32_t,3> bin{};
                for(unsigned axis=0;axis<3;++axis) {
                    if(high[axis]==low[axis]){bin[axis]=512;continue;}
                    const std::array<double,3> p{vertices[0][axis],vertices[1][axis],vertices[2][axis]};
                    const auto scale=std::max({std::abs(p[0]),std::abs(p[1]),std::abs(p[2])});
                    const double center=scale==0?0:((p[0]/scale+p[1]/scale+p[2]/scale)/3.)*scale;
                    const auto width=high[axis]-low[axis];
                    const auto fraction=high[axis]==low[axis]?.5:std::isfinite(width)?(center-low[axis])/width
                        :(center*.5-low[axis]*.5)/(high[axis]*.5-low[axis]*.5);
                    bin[axis]=static_cast<std::uint32_t>(std::clamp(fraction,0.,1.)*1023.+.5);
                }
                keys[face]={Spread(bin[0])|(Spread(bin[1])<<1)|(Spread(bin[2])<<2),static_cast<Index>(face)};
              }
              first+=chunk.values.size()/3;
            }
            for(unsigned shift=0;shift<30;shift+=10) {
                std::array<std::size_t,1024> offsets{};
                for(std::size_t i=0;i<count;++i){if(!(i%4096))Check(cancel);++offsets[(keys[i].key>>shift)&1023U];}
                std::size_t total=0;for(auto& offset:offsets){const auto n=offset;offset=total;total+=n;}
                for(std::size_t i=0;i<count;++i){if(!(i%4096))Check(cancel);scratch[offsets[(keys[i].key>>shift)&1023U]++]=keys[i];}
                keys.swap(scratch);
            }
            for(std::size_t i=0;i<count;++i){if(!(i%4096))Check(cancel);order[i]=keys[i].face;}
            // 空间页内部按原始面号排序，范围查询只需二分，不再线性扫描叶子中的面。
            for(std::size_t begin=0;begin<count;begin+=page) {
                Check(cancel);const auto end=std::min(count,begin+page);std::size_t compared=0;
                std::sort(order.begin()+begin,order.begin()+end,[&](Index a,Index b){if(!(compared++%4096))Check(cancel);return a<b;});
                for(auto i=begin;i<end;++i){if(!(i%4096))Check(cancel);inverse[order[i]]=static_cast<Index>(i);}
            }
            Check(cancel);workspace.Finish();
        }
        m_order=std::move(order);m_inverse=std::move(inverse);
    }
    std::variant<DataArray<std::uint32_t>,DataArray<std::uint64_t>> m_order,m_inverse;
};
} // namespace FeatureInternal
