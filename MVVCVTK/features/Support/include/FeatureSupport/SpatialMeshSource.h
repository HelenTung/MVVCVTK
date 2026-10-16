#pragma once

#include "Data/DataPayloads.h"
#include "MappedArrayStorage.h"
#include "FeatureSupport/SpatialFaceOrder.h"
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDoubleArray.h>
#include <vtkIdTypeArray.h>
#include <vtkIntArray.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <vector>

namespace FeatureInternal {

// 仅组织显示访问：正式网格、全局点面编号和顺序均不改动，不持有视图或线程。
// BVH 叶子按空间面排列形成，既不按对象分配实例，也不按帧扫描对象目录。
class SpatialMeshSource final {
public:
    using Bounds=std::array<double,6>;
    using Planes=std::array<std::array<double,4>,6>;
    using Cancel=std::function<bool()>;
    static constexpr std::size_t facesPerBatch=SpatialFaceOrder::pageFaces;
    static_assert(facesPerBatch<=static_cast<std::size_t>(std::numeric_limits<int>::max())/3,
        "Display batch topology must fit 32-bit offsets and connectivity.");
    inline static constexpr const char* faceIds="mvvcvtk.formal.face";
    inline static constexpr const char* pointIds="mvvcvtk.formal.point";
    struct Query final {
        std::vector<std::uint64_t> batches;
        std::uint64_t visitedNodes=0,indexRecordsRead=0;
        bool isComplete=true;
    };
    struct Batch final {
        vtkSmartPointer<vtkPolyData> mesh;
        std::uint64_t firstFace=0;
        std::uint64_t sourceFacesRead=0;
        std::uint64_t sourcePointsRead=0;
        std::uint64_t sourceIndexRecordsRead=0;
    };

    static std::shared_ptr<const SpatialMeshSource> Build(
        std::shared_ptr<const SurfaceMeshPayload> mesh,const Cancel& cancel={},std::size_t page=facesPerBatch)
    {
        if(!mesh||!mesh->GetValid())throw std::invalid_argument("Invalid display source mesh.");
        if(!page||page>facesPerBatch)throw std::invalid_argument("Invalid display batch size.");
        ResultArrayScope storage;
        auto result=std::shared_ptr<SpatialMeshSource>(new SpatialMeshSource(std::move(mesh),page));
        const auto count=result->GetFaceCount();
        result->m_order=SpatialFaceOrder::Build(*result->m_mesh,cancel,page);
        const auto batches=count/page+(count%page!=0);
        if(batches>SIZE_MAX/2)throw std::length_error("Display batch directory overflows.");
        result->m_bounds.reserve(static_cast<std::size_t>(batches));
        result->m_ranges.reserve(static_cast<std::size_t>(batches));
        const auto& faces=result->m_mesh->GetTriangleIndices();
        const auto& points=result->m_mesh->GetPointValues();
        for(std::uint64_t batch=0;batch<batches;++batch) {
            Check(cancel);Bounds bounds=EmptyBounds();std::array<std::uint64_t,2> range{UINT64_MAX,0};
            const auto end=std::min(count,(batch+1)*page);
            for(auto spatial=batch*page;spatial<end;++spatial) {
                if(!(spatial%4096))Check(cancel);
                const auto face=result->m_order.GetFace(static_cast<std::size_t>(spatial));
                range[0]=std::min(range[0],face);range[1]=std::max(range[1],face);
                for(unsigned corner=0;corner<3;++corner) {
                    const auto point=faces[static_cast<std::size_t>(face*3+corner)];
                    for(unsigned axis=0;axis<3;++axis) {
                        const double value=points[static_cast<std::size_t>(point*3+axis)];
                        bounds[2*axis]=std::min(bounds[2*axis],value);
                        bounds[2*axis+1]=std::max(bounds[2*axis+1],value);
                    }
                }
            }
            result->m_bounds.push_back(bounds);
            result->m_ranges.push_back(range);
        }
        DataArray<std::uint64_t> order(static_cast<std::size_t>(batches));
        std::iota(order.begin(),order.end(),0);
        result->m_nodes.reserve(static_cast<std::size_t>(batches)*2);
        if(batches)result->BuildNode(order,0,order.size(),cancel);
        Check(cancel);storage.Finish();return result;
    }
    std::uint64_t GetFaceCount() const noexcept {return m_mesh->GetTriangleIndices().size()/3;}
    std::uint64_t GetBatchCount() const noexcept {return m_bounds.size();}
    bool GetIs32BitIndex() const noexcept {return m_order.GetIs32Bit();}
    const SurfaceMeshPayload& GetMesh() const noexcept {return *m_mesh;}
    Bounds GetBounds() const noexcept {return m_nodes.empty()?Bounds{0,0,0,0,0,0}:m_nodes.front().bounds;}
    const Bounds& GetBatchBounds(std::uint64_t id) const {return m_bounds.at(static_cast<std::size_t>(id));}

    // planes 的内侧满足 ax+by+cz+d >= 0；调用方在 owner 线程把视锥变换到模型坐标。
    // 容量不足显式返回非完整查询，不把截断后的集合冒充精确完整显示。
    Query Find(const Planes& planes,std::size_t capacity,
        std::uint64_t firstFace=0,std::uint64_t endFace=UINT64_MAX,const Cancel& cancel={},
        const std::optional<std::array<double,4>>& section={}) const
    {
        Query result;
        Check(cancel);
        if(firstFace>=std::min(endFace,GetFaceCount()))return result;
        endFace=std::min(endFace,GetFaceCount());
        result.batches.reserve(std::min(capacity,m_bounds.size()));
        // 小对象直接通过反向排列定位显示页，不因面重排而遍历全目录。
        if(endFace-firstFace<=m_page) {
            std::vector<std::uint64_t> candidates;candidates.reserve(static_cast<std::size_t>(endFace-firstFace));
            for(auto face=firstFace;face<endFace;++face){if(!(face%4096))Check(cancel);candidates.push_back(m_order.GetSpatial(static_cast<std::size_t>(face))/m_page);++result.indexRecordsRead;}
            std::sort(candidates.begin(),candidates.end());candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
            for(const auto batch:candidates){++result.visitedNodes;if(!Intersects(m_bounds[static_cast<std::size_t>(batch)],planes,section))continue;
                if(result.batches.size()==capacity){result.isComplete=false;break;}result.batches.push_back(batch);}
            return result;
        }
        if(!m_nodes.empty())FindNode(0,planes,capacity,firstFace,endFace,result,cancel,section);
        std::sort(result.batches.begin(),result.batches.end());return result;
    }

    // 每次读取仅建立当前批次的局部 32 位拓扑，保留精确全局 uint64 身份。
    // worker 构造完成后才把 VTK 对象交给 owner；此入口不接触 renderer。
    Batch Read(std::uint64_t id,const Cancel& cancel={},
        std::uint64_t firstFace=0,std::uint64_t endFace=UINT64_MAX) const
    {
        if(id>=GetBatchCount())throw std::out_of_range("Display batch is unavailable.");
        Check(cancel);
        Batch result;result.firstFace=firstFace;
        const auto end=std::min(GetFaceCount(),(id+1)*m_page);
        result.mesh=vtkSmartPointer<vtkPolyData>::New();
        if(firstFace>=endFace)return result;
        const auto selectedRange=m_order.GetRange(static_cast<std::size_t>(id*m_page),static_cast<std::size_t>(end),firstFace,endFace,&result.sourceIndexRecordsRead);
        std::vector<std::uint64_t> selected;selected.reserve(selectedRange.second-selectedRange.first);
        for(auto spatial=selectedRange.first;spatial<selectedRange.second;++spatial){if(!(spatial%4096))Check(cancel);selected.push_back(m_order.GetFace(spatial));++result.sourceIndexRecordsRead;}
        if(selected.empty())return result;
        result.firstFace=*std::min_element(selected.begin(),selected.end());
        const auto count=selected.size();
        const auto& faces=m_mesh->GetTriangleIndices();const auto& points=m_mesh->GetPointValues();
        std::vector<std::uint64_t> globals;globals.reserve(count*3);
        std::size_t visited=0;
        for(const auto face:selected) {
            if(!(visited++%4096))Check(cancel);
            for(unsigned corner=0;corner<3;++corner)globals.push_back(faces[static_cast<std::size_t>(face*3+corner)]);
        }
        std::sort(globals.begin(),globals.end());globals.erase(std::unique(globals.begin(),globals.end()),globals.end());
        vtkNew<vtkDoubleArray> coordinates;coordinates->SetNumberOfComponents(3);coordinates->SetNumberOfTuples(static_cast<vtkIdType>(globals.size()));
        vtkNew<vtkIdTypeArray> globalPoints;globalPoints->SetName(pointIds);globalPoints->SetNumberOfValues(static_cast<vtkIdType>(globals.size()));
        for(std::size_t i=0;i<globals.size();++i) {
            if(!(i%4096))Check(cancel);
            if(globals[i]>static_cast<std::uint64_t>(std::numeric_limits<vtkIdType>::max()))throw std::length_error("Display point identity overflows VTK id.");
            const auto base=static_cast<std::size_t>(3*globals[i]);const double position[3]{points[base],points[base+1],points[base+2]};
            coordinates->SetTypedTuple(static_cast<vtkIdType>(i),position);
            globalPoints->SetValue(static_cast<vtkIdType>(i),static_cast<vtkIdType>(globals[i]));
        }
        vtkNew<vtkIntArray> offsets,connectivity;
        offsets->SetNumberOfValues(static_cast<vtkIdType>(count+1));connectivity->SetNumberOfValues(static_cast<vtkIdType>(count*3));
        vtkNew<vtkIdTypeArray> globalFaces;globalFaces->SetName(faceIds);globalFaces->SetNumberOfValues(static_cast<vtkIdType>(count));
        for(std::size_t face=0;face<count;++face) {
            if(!(face%4096))Check(cancel);
            const auto global=selected[face];
            if(global>static_cast<std::uint64_t>(std::numeric_limits<vtkIdType>::max()))throw std::length_error("Display face identity overflows VTK id.");
            offsets->SetValue(static_cast<vtkIdType>(face),static_cast<int>(face*3));globalFaces->SetValue(static_cast<vtkIdType>(face),static_cast<vtkIdType>(global));
            for(unsigned corner=0;corner<3;++corner) {
                const auto point=faces[static_cast<std::size_t>(global*3+corner)];
                const auto local=std::lower_bound(globals.begin(),globals.end(),point)-globals.begin();
                connectivity->SetValue(static_cast<vtkIdType>(face*3+corner),static_cast<int>(local));
            }
        }
        offsets->SetValue(static_cast<vtkIdType>(count),static_cast<int>(count*3));
        vtkNew<vtkPoints> vtkPoints;vtkPoints->SetData(coordinates);
        vtkNew<vtkCellArray> cells;cells->SetData(offsets,connectivity);
        result.mesh->SetPoints(vtkPoints);result.mesh->SetPolys(cells);
        result.mesh->GetPointData()->AddArray(globalPoints);result.mesh->GetCellData()->AddArray(globalFaces);
        result.sourceFacesRead=count;result.sourcePointsRead=globals.size();Check(cancel);return result;
    }
private:
    struct Node final {Bounds bounds;std::uint64_t firstFace=0,lastFace=0,left=0,right=0,batch=UINT64_MAX;};
    explicit SpatialMeshSource(std::shared_ptr<const SurfaceMeshPayload> mesh,std::size_t page):m_mesh(std::move(mesh)),m_page(page){}
    static void Check(const Cancel& cancel){if(cancel&&cancel())throw std::runtime_error("Display preparation cancelled.");}
    static Bounds EmptyBounds(){const auto inf=std::numeric_limits<double>::infinity();return {inf,-inf,inf,-inf,inf,-inf};}
    static void Include(Bounds& target,const Bounds& value){for(unsigned a=0;a<3;++a){target[a*2]=std::min(target[a*2],value[a*2]);target[a*2+1]=std::max(target[a*2+1],value[a*2+1]);}}
    std::uint64_t BuildNode(DataArray<std::uint64_t>& order,std::size_t begin,std::size_t end,const Cancel& cancel)
    {
        Check(cancel);const auto index=m_nodes.size();m_nodes.push_back({EmptyBounds(),UINT64_MAX,0});
        for(auto i=begin;i<end;++i){if(!((i-begin)%256))Check(cancel);const auto batch=static_cast<std::size_t>(order[i]);Include(m_nodes[index].bounds,m_bounds[batch]);m_nodes[index].firstFace=std::min(m_nodes[index].firstFace,m_ranges[batch][0]);m_nodes[index].lastFace=std::max(m_nodes[index].lastFace,m_ranges[batch][1]);}
        if(end-begin==1){m_nodes[index].batch=order[begin];return index;}
        unsigned axis=0;for(unsigned a=1;a<3;++a)if(m_nodes[index].bounds[a*2+1]-m_nodes[index].bounds[a*2]>m_nodes[index].bounds[axis*2+1]-m_nodes[index].bounds[axis*2])axis=a;
        const auto middle=begin+(end-begin)/2;
        std::size_t compared=0;
        std::nth_element(order.begin()+begin,order.begin()+middle,order.begin()+end,[&](auto a,auto b){
            if(!(compared++%1024))Check(cancel);
            const auto& x=m_bounds[static_cast<std::size_t>(a)];const auto& y=m_bounds[static_cast<std::size_t>(b)];
            const auto cx=x[2*axis]*.5+x[2*axis+1]*.5,cy=y[2*axis]*.5+y[2*axis+1]*.5;return cx==cy?a<b:cx<cy;});
        const auto left=BuildNode(order,begin,middle,cancel),right=BuildNode(order,middle,end,cancel);
        m_nodes[index].left=left;m_nodes[index].right=right;return index;
    }
    void FindNode(std::uint64_t index,const Planes& planes,std::size_t capacity,
        std::uint64_t firstFace,std::uint64_t endFace,Query& result,const Cancel& cancel,
        const std::optional<std::array<double,4>>& section) const
    {
        if(!result.isComplete)return;
        Check(cancel);
        const auto& node=m_nodes[static_cast<std::size_t>(index)];++result.visitedNodes;
        if(node.firstFace>=endFace||node.lastFace<firstFace||!Intersects(node.bounds,planes,section))return;
        if(node.batch!=UINT64_MAX){
            bool matches=firstFace==0&&endFace==GetFaceCount();
            if(!matches){const auto end=std::min(GetFaceCount(),(node.batch+1)*m_page);
                const auto range=m_order.GetRange(static_cast<std::size_t>(node.batch*m_page),static_cast<std::size_t>(end),firstFace,endFace,&result.indexRecordsRead);matches=range.first<range.second;}
            if(!matches)return;
            if(result.batches.size()==capacity){result.isComplete=false;return;}result.batches.push_back(node.batch);return;
        }
        FindNode(node.left,planes,capacity,firstFace,endFace,result,cancel,section);FindNode(node.right,planes,capacity,firstFace,endFace,result,cancel,section);
    }
    static bool Intersects(const Bounds& bounds,const Planes& planes,const std::optional<std::array<double,4>>& section)
    {
        if(section) {
            double low=(*section)[3],high=low,scale=std::abs(low);
            for(unsigned a=0;a<3;++a){const auto n=(*section)[a];low+=n*bounds[2*a+(n<0)];high+=n*bounds[2*a+(n>=0)];scale+=std::abs(n)*std::max(std::abs(bounds[2*a]),std::abs(bounds[2*a+1]));}
            const auto roundoff=scale*32*std::numeric_limits<double>::epsilon();
            if(low>roundoff||high< -roundoff)return false;
        }
        for(const auto& plane:planes) {
            double maximum=plane[3];
            for(unsigned a=0;a<3;++a)maximum+=plane[a]*bounds[2*a+(plane[a]>=0)];
            if(maximum<0)return false;
        }
        return true;
    }
    std::shared_ptr<const SurfaceMeshPayload> m_mesh;
    std::size_t m_page=facesPerBatch;
    SpatialFaceOrder m_order;
    DataArray<Bounds> m_bounds;
    DataArray<std::array<std::uint64_t,2>> m_ranges;
    DataArray<Node> m_nodes;
};
} // namespace FeatureInternal
