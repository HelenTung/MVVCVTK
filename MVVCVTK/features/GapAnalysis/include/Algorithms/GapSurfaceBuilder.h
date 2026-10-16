#pragma once

#include "GapAnalysisTypes.h"
#include "PagedArray.h"
#include "MeshVtkStorage.h"
#include "Host/RoiReadTypes.h"
#include "BackedVtkArray.h"
#include "MappedArrayStorage.h"
#include "GapOccupiedBlocks.h"
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkFlyingEdges3D.h>
#include <vtkIntArray.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

struct GapSurfaceRange final {
    std::int32_t id = 0;
    std::uint64_t firstPoint = 0, pointCount = 0, firstTriangle = 0, triangleCount = 0;
};
// 正式提面直接写入几何文件；临时掩膜和点复用表均不累积完整 RAM 网格。
class GapSurfaceBuilder final {
    template<class Key> using PointMap = std::map<Key,vtkIdType,std::less<Key>,
        DataArrayAllocator<std::pair<const Key,vtkIdType>>>;
public:
    static std::shared_ptr<const SurfaceMeshPayload> BuildPayload(vtkImageData* image,
        const DataArray<VoidRegion>& regions,const std::atomic<bool>& stopping,
        const RoiReadSnapshot& roi={},const DataArray<VoidRegion>* parents=nullptr,const std::string& frame="RAS",DataArray<GapSurfaceRange>* ranges=nullptr,bool useOccupancyIndex=true) {
        if(!image||image->GetScalarType()!=VTK_INT||image->GetNumberOfScalarComponents()!=1)return {};
        PagedArray<double> points,labels;PagedArray<std::uint64_t> cells;
        if(ranges)ranges->clear();
        int source[6];image->GetExtent(source);
        vtkIdType expected=1;
        for(int axis=0;axis<3;++axis) {
            const auto size=std::int64_t(source[axis*2+1])-source[axis*2]+1;
            if(size<=0||size>std::numeric_limits<vtkIdType>::max()/expected)return {};
            expected*=size;
        }
        auto* scalars=image->GetPointData()->GetScalars();
        if(!scalars||scalars->GetNumberOfTuples()!=expected||!scalars->GetVoidPointer(0))return {};
        GapRegionIndex parentIndex;
        if(parents)for(const auto& region:*parents)if(!parentIndex.emplace(region.id,&region).second)return {};
        const auto directory=GetGapOccupiedBlocks(image,regions,parentIndex,parents!=nullptr,stopping,useOccupancyIndex);
        if(!directory)return {};
        vtkNew<vtkImageData> mask;vtkNew<vtkFlyingEdges3D> filter;
        filter->SetInputData(mask);filter->SetValue(0,.5);filter->ComputeNormalsOff();filter->ComputeScalarsOff();
        // 64 个 cell 及共享的一层采样点，临时字节数至多 65^3；相邻块不重复 cell。
        constexpr std::int64_t blockCells=64;
        PagedMap<std::int32_t,bool> seen;
        for(const auto& region:regions) {
            if(stopping.load())return {};
            if(region.id<=0||seen.Find(region.id))return {};seen.Insert(region.id,true);
            if(region.voxelCount==0){if(ranges)ranges->push_back({region.id,points.size()/3,0,cells.size()/3,0});continue;}
            const auto firstPoint=points.size()/3,firstTriangle=cells.size()/3;
            const VoidRegion* parent=&region;
            if(parents){const auto found=parentIndex.find(region.id);if(found==parentIndex.end())return {};parent=found->second;}
            for(int axis=0;axis<3;++axis)if(parent->bbox[axis*2]<source[axis*2]||parent->bbox[axis*2+1]>source[axis*2+1]
                ||parent->bbox[axis*2]>parent->bbox[axis*2+1]||parent->bbox[axis*2]==INT_MIN||parent->bbox[axis*2+1]==INT_MAX)return {};
            // 不同孔隙绝不焊接。未裁剪顶点以半网格整数索引复用，避免大物理原点及跨块浮点误差。
            PagedMap<std::array<std::int64_t,3>,vtkIdType,PagedArrayHash<std::int64_t,3>> indexPoints;
            PagedMap<std::array<double,3>,vtkIdType,PagedArrayHash<double,3>> clippedPoints;
            const auto insertPoint=[&](const std::array<double,3>& p) {
                if(points.size()/3==std::size_t(std::numeric_limits<vtkIdType>::max()))throw std::length_error("Gap point indices overflow.");
                const auto id=static_cast<vtkIdType>(points.size()/3);points.insert(points.end(),p.begin(),p.end());return id;
            };
            const auto insertCell=[&](const vtkIdType* ids) {
                if(cells.size()/3>=std::size_t(std::numeric_limits<vtkIdType>::max()/4))throw std::length_error("Gap cell indices overflow.");
                for(int i=0;i<3;++i)cells.push_back(static_cast<std::uint64_t>(ids[i]));labels.push_back(region.id);
            };
            const auto firstBlock=std::lower_bound(directory->blocks.begin(),directory->blocks.end(),region.id,
                [](const auto& block,auto id){return block.id<id;});
            const auto lastBlock=std::upper_bound(firstBlock,directory->blocks.end(),region.id,
                [](auto id,const auto& block){return id<block.id;});
            const std::uint64_t nx=(std::int64_t(parent->bbox[1])-parent->bbox[0]+1)/blockCells+1;
            const std::uint64_t ny=(std::int64_t(parent->bbox[3])-parent->bbox[2]+1)/blockCells+1;
            const std::uint64_t nz=(std::int64_t(parent->bbox[5])-parent->bbox[4]+1)/blockCells+1;
            if(nx>UINT64_MAX/ny||nx*ny>UINT64_MAX/nz)return {};
            const auto blockCount=directory->isIndexed?static_cast<std::uint64_t>(lastBlock-firstBlock):nx*ny*nz;
            for(std::uint64_t block=0;block<blockCount;++block) {
                const auto* indexed=directory->isIndexed?&firstBlock[static_cast<std::ptrdiff_t>(block)]:nullptr;
                const auto x=std::int64_t(parent->bbox[0])-1+blockCells*(indexed?indexed->x:static_cast<std::int64_t>(block%nx));
                const auto y=std::int64_t(parent->bbox[2])-1+blockCells*(indexed?indexed->y:static_cast<std::int64_t>((block/nx)%ny));
                const auto z=std::int64_t(parent->bbox[4])-1+blockCells*(indexed?indexed->z:static_cast<std::int64_t>(block/(nx*ny)));
                if(stopping.load())return {};
                const std::array<std::int64_t,3> start{x,y,z};
                const std::array<int,3> dimensions{
                    static_cast<int>(std::min(blockCells,std::int64_t(parent->bbox[1])+1-x)+1),
                    static_cast<int>(std::min(blockCells,std::int64_t(parent->bbox[3])+1-y)+1),
                    static_cast<int>(std::min(blockCells,std::int64_t(parent->bbox[5])+1-z)+1)};
                mask->SetDimensions(dimensions[0],dimensions[1],dimensions[2]);mask->AllocateScalars(VTK_UNSIGNED_CHAR,1);
                auto* data=static_cast<unsigned char*>(mask->GetScalarPointer());
                std::fill_n(data,std::size_t(dimensions[0])*dimensions[1]*dimensions[2],0);
                bool hasForeground=false;
                for(std::int64_t k=std::max(z,std::int64_t(parent->bbox[4]));k<=std::min(z+dimensions[2]-1,std::int64_t(parent->bbox[5]));++k)
                for(std::int64_t j=std::max(y,std::int64_t(parent->bbox[2]));j<=std::min(y+dimensions[1]-1,std::int64_t(parent->bbox[3]));++j) {
                    if(stopping.load())return {};
                    const auto first=std::max(x,std::int64_t(parent->bbox[0])),last=std::min(x+dimensions[0]-1,std::int64_t(parent->bbox[1]));
                    auto* input=static_cast<const int*>(image->GetScalarPointer(static_cast<int>(first),static_cast<int>(j),static_cast<int>(k)));
                    auto* target=data+((k-z)*dimensions[1]+(j-y))*dimensions[0]+first-x;
                    for(auto i=first;i<=last;++i){const bool inside=*input++==region.id;*target++=static_cast<unsigned char>(inside);hasForeground|=inside;}
                }
                if(!hasForeground)continue;
                mask->Modified();filter->Update();if(stopping.load())return {};
                auto* poly=filter->GetOutput();auto* triangles=poly->GetPolys();triangles->InitTraversal();
                vtkIdType count=0;const vtkIdType* inputIds=nullptr;
                while(triangles->GetNextCell(count,inputIds)) {
                    if(stopping.load())return {};
                    if(count!=3)return {};
                    std::array<std::array<std::int64_t,3>,3> keys{};
                    RoiTriangle triangle{};
                    for(int corner=0;corner<3;++corner) {
                        double local[3],index[3];poly->GetPoint(inputIds[corner],local);
                        for(int axis=0;axis<3;++axis){keys[corner][axis]=2*start[axis]+std::llround(2*local[axis]);index[axis]=keys[corner][axis]*.5;}
                        image->TransformContinuousIndexToPhysicalPoint(index,triangle[corner].data());
                    }
                    if(roi) {
                        const auto error=roi->GetTriangleChunks(triangle,64,[&](const auto& pieces) {
                            for(const auto& piece:pieces) {
                                vtkIdType ids[3];
                                for(int corner=0;corner<3;++corner) {
                                    const auto found=clippedPoints.Find(piece[corner]);
                                    ids[corner]=found?*found:clippedPoints.Insert(piece[corner],insertPoint(piece[corner]));
                                }
                                insertCell(ids);
                            }
                            return !stopping.load();
                        },0,[&]{return stopping.load();});
                        if(error.error!=RoiError::None)return {};
                    } else {
                        vtkIdType ids[3];
                        for(int corner=0;corner<3;++corner) {
                            const auto found=indexPoints.Find(keys[corner]);
                            ids[corner]=found?*found:indexPoints.Insert(keys[corner],insertPoint(triangle[corner]));
                        }
                        insertCell(ids);
                    }
                }
            }
            if(ranges)ranges->push_back({region.id,firstPoint,points.size()/3-firstPoint,
                firstTriangle,cells.size()/3-firstTriangle});
        }
        return stopping.load()?nullptr:MeshPayloadBuilder::Build(points.Finish(),cells.Finish(),{},frame,{{"gap.region.id",1,labels.Finish(),0}},[&]{return stopping.load();});
    }
    static vtkSmartPointer<vtkPolyData> Build(vtkImageData* image,const DataArray<VoidRegion>& regions,
        const std::atomic<bool>& stopping,const RoiReadSnapshot& roi={},const DataArray<VoidRegion>* parents=nullptr,
        DataArray<GapSurfaceRange>* ranges=nullptr,bool useOccupancyIndex=true){
        const auto payload=BuildPayload(image,regions,stopping,roi,parents,"RAS",ranges,useOccupancyIndex);
        return payload?MeshVtkStorage::Build(*payload,[&]{return stopping.load();}):nullptr;
    }
};
