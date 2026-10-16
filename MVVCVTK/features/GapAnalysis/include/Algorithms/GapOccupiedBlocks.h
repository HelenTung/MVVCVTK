#pragma once
#include "GapAnalysisTypes.h"
#include "Data/DataPayloads.h"
#include <vtkImageData.h>
#include <atomic>
#include <climits>
#include <map>
#include <tuple>

using GapRegionIndex=std::map<std::int32_t,const VoidRegion*,std::less<std::int32_t>,
    DataArrayAllocator<std::pair<const std::int32_t,const VoidRegion*>>>;
struct GapOccupiedBlock final {
    std::int32_t id=0;
    std::int64_t z=0,y=0,x=0;
    bool operator<(const GapOccupiedBlock& other)const {return std::tie(id,z,y,x)<std::tie(other.id,other.z,other.y,other.x);}
    bool operator==(const GapOccupiedBlock& other)const {return std::tie(id,z,y,x)==std::tie(other.id,other.z,other.y,other.x);}
};
struct GapBlockDirectory final {
    bool isIndexed=false;
    DataArray<GapOccupiedBlock> blocks;
};
// 仅改变标签访问计划：同一个二值提面核心仍按原 z/y/x 顺序处理所有非空采样块。
inline std::optional<GapBlockDirectory> GetGapOccupiedBlocks(vtkImageData* image,
    const DataArray<VoidRegion>& regions,const GapRegionIndex& parents,bool useParents,
    const std::atomic<bool>& cancelled,bool allowIndex=true)
{
    GapBlockDirectory directory;GapRegionIndex requested;
    int extent[6];image->GetExtent(extent);
    long double work=0;
    for(const auto& region:regions) {
        if(cancelled.load())return {};
        if(region.id<=0||region.voxelCount<0||!requested.emplace(region.id,&region).second)return {};
        if(!region.voxelCount)continue;
        const auto found=parents.find(region.id);
        if(useParents&&found==parents.end())return {};
        const auto* parent=useParents?found->second:&region;requested[region.id]=parent;
        if(parent->voxelCount<=0)return {};
        long double size=1;
        for(int a=0;a<3;++a) {
            if(parent->bbox[2*a]<extent[2*a]||parent->bbox[2*a+1]>extent[2*a+1]
                ||parent->bbox[2*a]>parent->bbox[2*a+1]||parent->bbox[2*a]==INT_MIN||parent->bbox[2*a+1]==INT_MAX)return {};
            size*=std::int64_t(parent->bbox[2*a+1])-parent->bbox[2*a]+3;
        }
        work+=size;
    }
    // 独立小包围盒直接读取更省；大量重叠包围盒才值得先顺序扫描一次标签源。
    if(!allowIndex||work<=2.0L*image->GetNumberOfPoints())return directory;
    directory.isIndexed=true;
    constexpr std::int64_t tile=64;
    using Bounds=std::array<std::int64_t,6>;
    using TileIndex=std::map<std::int32_t,Bounds,std::less<std::int32_t>,
        DataArrayAllocator<std::pair<const std::int32_t,Bounds>>>;
    for(std::int64_t z=extent[4];z<=extent[5];z+=tile)
    for(std::int64_t y=extent[2];y<=extent[3];y+=tile)
    for(std::int64_t x=extent[0];x<=extent[1];x+=tile) {
        TileIndex occupied;
        const auto endX=(std::min)(x+tile-1,std::int64_t(extent[1]));
        const auto endY=(std::min)(y+tile-1,std::int64_t(extent[3]));
        const auto endZ=(std::min)(z+tile-1,std::int64_t(extent[5]));
        for(auto k=z;k<=endZ;++k)for(auto j=y;j<=endY;++j) {
            if(cancelled.load())return {};
            const auto* row=static_cast<const std::int32_t*>(image->GetScalarPointer(static_cast<int>(x),static_cast<int>(j),static_cast<int>(k)));
            for(auto i=x;i<=endX;++i) {
                const auto id=*row++;if(id<=0)continue;
                const auto wanted=requested.find(id);if(wanted==requested.end()||!wanted->second->voxelCount)continue;
                auto inserted=occupied.emplace(id,Bounds{i,i,j,j,k,k});auto& box=inserted.first->second;
                box[0]=(std::min)(box[0],i);box[1]=(std::max)(box[1],i);
                box[2]=(std::min)(box[2],j);box[3]=(std::max)(box[3],j);
                box[4]=(std::min)(box[4],k);box[5]=(std::max)(box[5],k);
            }
        }
        for(const auto& item:occupied) {
            const auto* parent=requested.find(item.first)->second;Bounds blocks{};bool empty=false;
            for(int a=0;a<3;++a) {
                const auto origin=std::int64_t(parent->bbox[2*a])-1;
                const auto low=(std::max)(item.second[2*a],std::int64_t(parent->bbox[2*a]));
                const auto high=(std::min)(item.second[2*a+1],std::int64_t(parent->bbox[2*a+1]));
                if(low>high){empty=true;break;}
                // 采样点恰落在块边界时，两侧 cell 块都必须纳入。
                blocks[2*a]=(low-origin-1)/tile;
                blocks[2*a+1]=(high-origin)/tile;
            }
            if(empty)continue;
            for(auto k=blocks[4];k<=blocks[5];++k)for(auto j=blocks[2];j<=blocks[3];++j)for(auto i=blocks[0];i<=blocks[1];++i)
                directory.blocks.push_back({item.first,k,j,i});
        }
    }
    struct Cancelled final {};
    try {
        std::sort(directory.blocks.begin(),directory.blocks.end(),[&](const auto& a,const auto& b){
            if(cancelled.load())throw Cancelled{};return a<b;});
        const auto end=std::unique(directory.blocks.begin(),directory.blocks.end(),[&](const auto& a,const auto& b){
            if(cancelled.load())throw Cancelled{};return a==b;});
        directory.blocks.erase(end,directory.blocks.end());
    }catch(const Cancelled&){return {};}
    return directory;
}
