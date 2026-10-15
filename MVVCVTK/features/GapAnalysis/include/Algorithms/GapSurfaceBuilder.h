#pragma once

#include "GapAnalysisTypes.h"
#include "Host/RoiReadTypes.h"
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

// 正式几何与显示共用的私有提面器；块只限制临时掩膜，不限制完整结果规模。
class GapSurfaceBuilder final {
public:
    static vtkSmartPointer<vtkPolyData> Build(vtkImageData* image,
        const std::vector<VoidRegion>& regions,const std::atomic<bool>& stopping,
        const RoiReadSnapshot& roi={},const std::vector<VoidRegion>* parents=nullptr) {
        if(!image||image->GetScalarType()!=VTK_INT||image->GetNumberOfScalarComponents()!=1)return {};
        auto output=vtkSmartPointer<vtkPolyData>::New();
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> cells;vtkNew<vtkIntArray> labels;
        labels->SetName("gap.region.id");output->SetPoints(points);output->SetPolys(cells);output->GetCellData()->SetScalars(labels);
        int source[6];image->GetExtent(source);
        vtkIdType expected=1;
        for(int axis=0;axis<3;++axis) {
            const auto size=std::int64_t(source[axis*2+1])-source[axis*2]+1;
            if(size<=0||size>std::numeric_limits<vtkIdType>::max()/expected)return {};
            expected*=size;
        }
        auto* scalars=image->GetPointData()->GetScalars();
        if(!scalars||scalars->GetNumberOfTuples()!=expected||!scalars->GetVoidPointer(0))return {};
        std::map<std::int32_t,const VoidRegion*> parentIndex;
        if(parents)for(const auto& region:*parents)if(!parentIndex.emplace(region.id,&region).second)return {};
        vtkNew<vtkImageData> mask;vtkNew<vtkFlyingEdges3D> filter;
        filter->SetInputData(mask);filter->SetValue(0,.5);filter->ComputeNormalsOff();filter->ComputeScalarsOff();
        // 64 个 cell 及共享的一层采样点，临时字节数至多 65^3；相邻块不重复 cell。
        constexpr std::int64_t blockCells=64;
        std::map<std::int32_t,bool> seen;
        for(const auto& region:regions) {
            if(stopping.load())return {};
            if(region.id<=0||!seen.emplace(region.id,true).second)return {};
            if(region.voxelCount==0)continue;
            const VoidRegion* parent=&region;
            if(parents){const auto found=parentIndex.find(region.id);if(found==parentIndex.end())return {};parent=found->second;}
            for(int axis=0;axis<3;++axis)if(parent->bbox[axis*2]<source[axis*2]||parent->bbox[axis*2+1]>source[axis*2+1]
                ||parent->bbox[axis*2]>parent->bbox[axis*2+1]||parent->bbox[axis*2]==INT_MIN||parent->bbox[axis*2+1]==INT_MAX)return {};
            // 不同孔隙绝不焊接。未裁剪顶点以半网格整数索引复用，避免大物理原点及跨块浮点误差。
            std::map<std::array<std::int64_t,3>,vtkIdType> indexPoints;
            std::map<std::array<double,3>,vtkIdType> clippedPoints;
            const auto insertPoint=[&](const std::array<double,3>& p) {
                if(points->GetNumberOfPoints()==std::numeric_limits<vtkIdType>::max())throw std::length_error("Gap point indices overflow.");
                return points->InsertNextPoint(p.data());
            };
            const auto insertCell=[&](const vtkIdType* ids) {
                if(cells->GetNumberOfCells()>=std::numeric_limits<vtkIdType>::max()/4)throw std::length_error("Gap cell indices overflow.");
                cells->InsertNextCell(3,ids);labels->InsertNextValue(region.id);
            };
            for(std::int64_t z=std::int64_t(parent->bbox[4])-1;z<std::int64_t(parent->bbox[5])+1;z+=blockCells)
            for(std::int64_t y=std::int64_t(parent->bbox[2])-1;y<std::int64_t(parent->bbox[3])+1;y+=blockCells)
            for(std::int64_t x=std::int64_t(parent->bbox[0])-1;x<std::int64_t(parent->bbox[1])+1;x+=blockCells) {
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
                                    const auto found=clippedPoints.find(piece[corner]);
                                    ids[corner]=found==clippedPoints.end()?clippedPoints.emplace(piece[corner],insertPoint(piece[corner])).first->second:found->second;
                                }
                                insertCell(ids);
                            }
                            return !stopping.load();
                        },0,[&]{return stopping.load();});
                        if(error.error!=RoiError::None)return {};
                    } else {
                        vtkIdType ids[3];
                        for(int corner=0;corner<3;++corner) {
                            const auto found=indexPoints.find(keys[corner]);
                            ids[corner]=found==indexPoints.end()?indexPoints.emplace(keys[corner],insertPoint(triangle[corner])).first->second:found->second;
                        }
                        insertCell(ids);
                    }
                }
            }
        }
        points->Squeeze();cells->Squeeze();labels->Squeeze();return stopping.load()?nullptr:output;
    }
};
