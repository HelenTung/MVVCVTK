#include "Geometry/RoiEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <mutex>
#include <tuple>

class RoiVoxelSet::Impl final {
public:
    std::vector<RoiVoxelRun> runs;
    std::vector<std::size_t> offsets{0};
    std::map<std::uint32_t,std::array<double,6>> bounds;
    std::once_flag expanded;
    std::vector<RoiVoxel> voxels;
};
RoiVoxelSet::RoiVoxelSet(std::vector<RoiVoxel> voxels)
    :RoiVoxelSet([&] {
        std::sort(voxels.begin(),voxels.end(),[](const auto& a,const auto& b){return std::tie(a.index[1],a.index[2],a.index[0])<std::tie(b.index[1],b.index[2],b.index[0]);});
        std::vector<RoiVoxelRun> runs;
        for(const auto& voxel:voxels) {
            if(!runs.empty()&&runs.back().first[1]==voxel.index[1]&&runs.back().first[2]==voxel.index[2]) {
                auto& previous=runs.back();if(previous.last>=voxel.index[0])throw std::invalid_argument("Duplicate ROI voxel.");
                if(previous.label==voxel.label&&std::int64_t(previous.last)+1==voxel.index[0]){previous.last=voxel.index[0];continue;}
            }
            runs.push_back({voxel.index,voxel.index[0],voxel.label});
        }
        return runs;
    }()) {}
RoiVoxelSet::RoiVoxelSet(std::vector<RoiVoxelRun> runs):m_impl(std::make_shared<Impl>()) {
    std::sort(runs.begin(),runs.end(),[](const auto& a,const auto& b){return std::tie(a.first[1],a.first[2],a.first[0])<std::tie(b.first[1],b.first[2],b.first[0]);});
    for(const auto& run:runs) {
        if(!run.label||run.first[0]>run.last)throw std::invalid_argument("Invalid ROI voxel run.");
        if(!m_impl->runs.empty()) {
            auto& previous=m_impl->runs.back();
            if(previous.first[1]==run.first[1]&&previous.first[2]==run.first[2]) {
                if(previous.last>=run.first[0])throw std::invalid_argument("Overlapping ROI voxel runs.");
                if(previous.label==run.label&&std::int64_t(previous.last)+1==run.first[0]){previous.last=run.last;continue;}
            }
        }
        m_impl->runs.push_back(run);
    }
    for(const auto& run:m_impl->runs) {
        const auto count=static_cast<std::size_t>(std::int64_t(run.last)-run.first[0]+1);
        if(count>std::numeric_limits<std::size_t>::max()-m_impl->offsets.back())throw std::length_error("ROI voxel count overflows.");
        m_impl->offsets.push_back(m_impl->offsets.back()+count);
        auto& bounds=m_impl->bounds.emplace(run.label,std::array<double,6>{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY}).first->second;
        for(int a=0;a<3;++a){bounds[a*2]=std::min(bounds[a*2],run.first[a]-.5);bounds[a*2+1]=std::max(bounds[a*2+1],(a==0?run.last:run.first[a])+.5);}
    }
}
const std::vector<RoiVoxelRun>& RoiVoxelSet::GetRuns()const noexcept{return m_impl->runs;}
std::size_t RoiVoxelSet::GetVoxelCount()const noexcept{return m_impl->offsets.back();}
std::vector<RoiVoxel> RoiVoxelSet::GetVoxelChunk(std::size_t offset,std::size_t count)const {
    if(offset>GetVoxelCount())throw std::out_of_range("ROI voxel chunk offset.");
    count=std::min(count,GetVoxelCount()-offset);std::vector<RoiVoxel> result;result.reserve(count);
    auto run=static_cast<std::size_t>(std::upper_bound(m_impl->offsets.begin(),m_impl->offsets.end(),offset)-m_impl->offsets.begin()-1);
    while(result.size()<count) {
        const auto& span=m_impl->runs[run];const auto first=std::int64_t(span.first[0])+static_cast<std::int64_t>(offset-m_impl->offsets[run]);
        const auto take=std::min(count-result.size(),m_impl->offsets[run+1]-offset);
        for(std::size_t i=0;i<take;++i)result.push_back({{static_cast<int>(first+static_cast<std::int64_t>(i)),span.first[1],span.first[2]},span.label});
        offset+=take;++run;
    }
    return result;
}
const std::vector<RoiVoxel>& RoiVoxelSet::GetVoxels()const {
    std::call_once(m_impl->expanded,[&] {
        auto values=GetVoxelChunk(0,GetVoxelCount());
        std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.index<b.index;});
        m_impl->voxels=std::move(values);
    });
    return m_impl->voxels;
}
std::uint32_t RoiVoxelSet::GetLabel(const std::array<int,3>& index) const noexcept
{
    const auto found=std::upper_bound(m_impl->runs.begin(),m_impl->runs.end(),index,
        [](const auto& key,const auto& run){return std::tie(key[1],key[2],key[0])<std::tie(run.first[1],run.first[2],run.first[0]);});
    if(found==m_impl->runs.begin())return 0;
    const auto& run=*std::prev(found);
    return run.first[1]==index[1]&&run.first[2]==index[2]&&index[0]<=run.last?run.label:0;
}

std::array<double,6> RoiVoxelSet::GetIndexBounds(const std::vector<std::uint64_t>& labels) const noexcept
{
    std::array<double,6> result{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
    const auto add=[&](const auto& bounds){for(int a=0;a<3;++a){result[a*2]=std::min(result[a*2],bounds[a*2]);result[a*2+1]=std::max(result[a*2+1],bounds[a*2+1]);}};
    if(labels.empty())for(const auto& entry:m_impl->bounds)add(entry.second);
    else for(auto label:labels){if(label>UINT32_MAX)continue;const auto found=m_impl->bounds.find(static_cast<std::uint32_t>(label));if(found!=m_impl->bounds.end())add(found->second);}
    return result;
}

namespace {
using Point = std::array<double, 3>;
using Matrix = std::array<double, 16>;

template<std::size_t N> bool GetFinite(const std::array<double, N>& values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

double Dot(const Point& a, const Point& b) noexcept
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

Point GetEdgeIntersection(const Point& a,const Point& b,double distanceA,double distanceB) noexcept {
    // 共享边始终按同一端点顺序插值，使相邻三角形的交点可精确复用，不做近似焊接。
    const bool reverse=b<a;const auto& first=reverse?b:a;const auto& second=reverse?a:b;
    const double from=reverse?distanceB:distanceA,to=reverse?distanceA:distanceB,t=from/(from-to);
    return {first[0]+(second[0]-first[0])*t,first[1]+(second[1]-first[1])*t,first[2]+(second[2]-first[2])*t};
}

bool GetProjectiveValid(const Matrix& matrix) noexcept
{
    if(!GetFinite(matrix))return false;
    double rows[4][4]{},scales[4]{};
    for(int r=0;r<4;++r){for(int c=0;c<4;++c){rows[r][c]=matrix[r*4+c];scales[r]=std::max(scales[r],std::abs(rows[r][c]));}if(scales[r]==0)return false;}
    for(int c=0;c<4;++c){int pivot=c;for(int r=c+1;r<4;++r)if(std::abs(rows[r][c])/scales[r]>std::abs(rows[pivot][c])/scales[pivot])pivot=r;if(std::abs(rows[pivot][c])/scales[pivot]<=1e-12)return false;
        if(pivot!=c){for(int k=0;k<4;++k)std::swap(rows[pivot][k],rows[c][k]);std::swap(scales[pivot],scales[c]);}
        for(int r=c+1;r<4;++r){const double factor=rows[r][c]/rows[c][c];for(int k=c+1;k<4;++k)rows[r][k]-=factor*rows[c][k];}}
    return true;
}
bool GetRingValid(const std::vector<std::array<double,2>>& ring) noexcept
{
    if(ring.size()<3)return false;
    for(const auto& point:ring)if(!GetFinite(point))return false;
    std::size_t second=1;while(second<ring.size() && ring[second]==ring[0])++second;if(second==ring.size())return false;
    const double x=ring[second][0]-ring[0][0],y=ring[second][1]-ring[0][1];
    for(const auto& point:ring)if(x*(point[1]-ring[0][1])-y*(point[0]-ring[0][0])!=0)return true;
    return false;
}

Point Transform(const Matrix& m, const Point& p) noexcept
{
    return { m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3],
        m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],
        m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11] };
}

bool Invert(const Matrix& source, Matrix& inverse) noexcept
{
    if (!GetFinite(source) || source[12] != 0 || source[13] != 0
        || source[14] != 0 || source[15] != 1) return false;
    // 带尺度选主元，拒绝不可可靠求逆的矩阵，避免近平行轴放大数值误差。
    double rows[3][6]{};
    double scales[3]{};
    for (int r=0; r<3; ++r) {
        for (int c=0; c<3; ++c) {
            rows[r][c]=source[r*4+c];
            scales[r]=std::max(scales[r], std::abs(rows[r][c]));
        }
        if (scales[r] == 0) return false;
        rows[r][r+3]=1;
    }
    for (int c=0; c<3; ++c) {
        int pivot=c;
        for (int r=c+1; r<3; ++r)
            if (std::abs(rows[r][c])/scales[r] > std::abs(rows[pivot][c])/scales[pivot]) pivot=r;
        if (std::abs(rows[pivot][c])/scales[pivot] <= 1e-12) return false;
        if (pivot != c) {
            for (int j=0;j<6;++j) std::swap(rows[c][j],rows[pivot][j]);
            std::swap(scales[c],scales[pivot]);
        }
        const double divisor=rows[c][c];
        for (int j=0;j<6;++j) rows[c][j]/=divisor;
        for (int r=0;r<3;++r) if (r!=c) {
            const double factor=rows[r][c];
            for (int j=0;j<6;++j) rows[r][j]-=factor*rows[c][j];
        }
    }
    inverse=roiIdentityMatrix;
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c) inverse[r*4+c]=rows[r][c+3];
        inverse[r*4+3]=-(inverse[r*4]*source[3]+inverse[r*4+1]*source[7]+inverse[r*4+2]*source[11]);
    }
    return GetFinite(inverse);
}

Matrix GetIndexMatrix(const GridGeometry3D& grid) noexcept
{
    Matrix matrix=roiIdentityMatrix;
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c) matrix[r*4+c]=grid.direction[r*3+c]*grid.spacing[c];
        matrix[r*4+3]=grid.origin[r];
    }
    return matrix;
}

bool GetSameGrid(const GridGeometry3D& a, const GridGeometry3D& b) noexcept
{
    return a.extent==b.extent && a.dimensions==b.dimensions && a.spacing==b.spacing
        && a.origin==b.origin && a.direction==b.direction && a.coordinateFrame==b.coordinateFrame;
}

bool GetPrimitiveDefault(const RoiPrimitive& p) noexcept
{
    return p.shape==RoiShape::Box && p.localToSource==roiIdentityMatrix
        && p.origin==Point{0,0,0} && p.normal==Point{0,0,1} && !p.mask
        && p.boundaryPolicy==RoiBoundaryPolicy::Closed && p.radius==0 && p.height==0
        && p.contours.empty() && p.path.empty() && !p.voxels && p.labels.empty()
        && p.depth==std::array<double,2>{-0.5,0.5}
        && p.sourceToView==roiIdentityMatrix && p.viewToClip==roiIdentityMatrix;
}

const GridGeometry3D* GetMaskGrid(const DataSnapshot& data) noexcept
{
    if (!data) return nullptr;
    if (const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get())) return &mask->GetGeometry();
    if (const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get())) return &labels->GetGeometry();
    return nullptr;
}

std::uint64_t GetMaskValue(const DataSnapshot& data, std::size_t index) noexcept
{
    if (const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get()))
        return (*mask->GetValues())[index];
    const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get());
    return labels ? std::visit([index](const auto& values) { return static_cast<std::uint64_t>((*values)[index]); },labels->GetValues()):0;
}

bool GetSelectedLabel(std::uint64_t value,const std::vector<std::uint64_t>& labels) noexcept
{
    return value && (labels.empty() || std::find(labels.begin(),labels.end(),value)!=labels.end());
}

bool GetContourContains(const RoiPrimitive& primitive,const std::array<double,2>& p) noexcept
{
    bool inside=false;
    for (const auto& ring:primitive.contours) {
        for (std::size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
            const auto& a=ring[j]; const auto& b=ring[i];
            const double cross=(p[0]-a[0])*(b[1]-a[1])-(p[1]-a[1])*(b[0]-a[0]);
            if (std::abs(cross)<=1e-12 && p[0]>=std::min(a[0],b[0]) && p[0]<=std::max(a[0],b[0])
                && p[1]>=std::min(a[1],b[1]) && p[1]<=std::max(a[1],b[1])) return true;
            if ((a[1]>p[1])!=(b[1]>p[1]) && p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0]) inside=!inside;
        }
    }
    return inside;
}

std::array<double,6> GetGridBounds(const Matrix& matrix, const std::array<int,6>& extent) noexcept
{
    std::array<double,6> bounds{ INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY };
    for (unsigned corner=0;corner<8;++corner) {
        const auto p=Transform(matrix,{static_cast<double>(extent[(corner&1)?1:0]),
            static_cast<double>(extent[(corner&2)?3:2]),static_cast<double>(extent[(corner&4)?5:4])});
        for (int a=0;a<3;++a) {
            bounds[a*2]=std::min(bounds[a*2],p[a]); bounds[a*2+1]=std::max(bounds[a*2+1],p[a]);
        }
    }
    return bounds;
}

void AddBoxPlanes(std::vector<RoiPlane>& planes, const Matrix& inverse,
    const std::array<double,6>& localBounds)
{
    for (int axis=0;axis<3;++axis) for (int side=0;side<2;++side) {
        const double sign=side ? -1.0 : 1.0;
        Point normal{ sign*inverse[axis*4],sign*inverse[axis*4+1],sign*inverse[axis*4+2] };
        const double length=std::hypot(normal[0],normal[1],normal[2]);
        const double offset=sign*(inverse[axis*4+3]-localBounds[axis*2+side])/length;
        for (auto& n:normal) n/=length;
        planes.push_back({{-offset*normal[0],-offset*normal[1],-offset*normal[2]},normal});
    }
}

class RoiView final : public RoiReadView, public std::enable_shared_from_this<RoiView> {
public:
    RoiView(DataSnapshot roi, DataSnapshot source, const RoiEvaluator::DataLookup& getData)
        : m_revision(roi->self), m_definition(dynamic_cast<const RoiGeometryPayload&>(*roi->payload).GetDefinition())
    {
        m_dependencies.push_back(m_definition.source);
        if (GetDataRevisionRefValid(m_revision)) m_dependencies.push_back(m_revision);
        // 只列成员判定实际读取的修订；历史父 ROI/网格属于追溯，不能使新区域自相矛盾地过期。
        m_image=std::dynamic_pointer_cast<const ImageGrid3DPayload>(source->payload);
        if (m_image) {
            m_indexMatrix=GetIndexMatrix(m_image->GetGeometry());
            if (!Invert(m_indexMatrix,m_sourceToIndex)) throw std::runtime_error("Invalid source matrix.");
            m_bounds=GetGridBounds(m_indexMatrix,m_image->GetGeometry().extent);
            std::array<double,6> ext{};
            std::copy(m_image->GetGeometry().extent.begin(),m_image->GetGeometry().extent.end(),ext.begin());
            AddBoxPlanes(m_planes.planes,m_sourceToIndex,ext);
        } else {
            const auto& vertices=dynamic_cast<const SurfaceMeshPayload&>(*source->payload).GetVertices();
            m_bounds={ INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY };
            for (std::size_t i=0;i<vertices.size();++i) {
                const auto a=i%3;
                m_bounds[a*2]=std::min(m_bounds[a*2],vertices[i]);
                m_bounds[a*2+1]=std::max(m_bounds[a*2+1],vertices[i]);
            }
            AddBoxPlanes(m_planes.planes,roiIdentityMatrix,m_bounds);
        }
        if (!GetFinite(m_bounds)) throw std::runtime_error("Invalid source domain.");
        m_prepared.resize(m_definition.nodes.size());
        m_planes.error=m_image && m_image->GetValidityMask()?RoiError::UnsupportedRoi:RoiError::None;
        for (std::size_t i=0;i<m_definition.nodes.size();++i) {
            const auto& node=m_definition.nodes[i];
            auto& prepared=m_prepared[i];
            if (node.kind==RoiNodeKind::Primitive) {
                const auto& p=node.primitive;
                if (p.shape==RoiShape::Box) {
                    if (!Invert(p.localToSource,prepared.inverse)) throw std::runtime_error("Invalid box matrix.");
                    const double edge=p.boundaryPolicy==RoiBoundaryPolicy::CropV1 ? 1+1e-6:1;
                    AddBoxPlanes(m_planes.planes,prepared.inverse,{-edge,edge,-edge,edge,-edge,edge});
                } else if (p.shape==RoiShape::HalfSpace) {
                    const auto length=std::hypot(p.normal[0],p.normal[1],p.normal[2]);
                    prepared.normal={p.normal[0]/length,p.normal[1]/length,p.normal[2]/length};
                    m_planes.planes.push_back({p.origin,prepared.normal});
                    if (p.boundaryPolicy==RoiBoundaryPolicy::CropV1) m_planes.error=RoiError::UnsupportedRoi;
                } else if (p.shape==RoiShape::Sphere || p.shape==RoiShape::Cylinder) {
                    if (p.shape==RoiShape::Cylinder) {
                        const auto length=std::hypot(p.normal[0],p.normal[1],p.normal[2]);
                        prepared.normal={p.normal[0]/length,p.normal[1]/length,p.normal[2]/length};
                    }
                    m_planes.error=RoiError::UnsupportedRoi;
                } else if (p.shape==RoiShape::MaskReference) {
                    prepared.mask=getData(*p.mask);
                    if (std::find(m_dependencies.begin(),m_dependencies.end(),*p.mask)==m_dependencies.end())
                        m_dependencies.push_back(*p.mask);
                    m_planes.error=RoiError::UnsupportedRoi;
                } else if(p.shape==RoiShape::Contour){
                    if(!Invert(p.localToSource,prepared.inverse))throw std::runtime_error("Invalid contour matrix.");
                    bool convex=p.contours.size()==1;double orientation=0,turn=0;
                    if(convex){const auto& ring=p.contours.front();for(std::size_t j=0;j<ring.size();++j){const auto& a=ring[j];const auto& b=ring[(j+1)%ring.size()];const auto& c=ring[(j+2)%ring.size()];const double cross=(b[0]-a[0])*(c[1]-b[1])-(b[1]-a[1])*(c[0]-b[0]);turn+=std::atan2(cross,(b[0]-a[0])*(c[0]-b[0])+(b[1]-a[1])*(c[1]-b[1]));if(cross){if(orientation && std::signbit(cross)!=std::signbit(orientation))convex=false;orientation=cross;}}if(!orientation || !std::isfinite(turn) || std::abs(std::abs(turn)-6.283185307179586)>1e-7)convex=false;}
                    if(!convex)m_planes.error=RoiError::UnsupportedRoi;
                    else{const auto add=[&](const Point& local,const Point& normal){Point n{};for(int r=0;r<3;++r)n[r]=prepared.inverse[r]*normal[0]+prepared.inverse[4+r]*normal[1]+prepared.inverse[8+r]*normal[2];const auto length=std::hypot(n[0],n[1],n[2]);for(auto& value:n)value/=length;m_planes.planes.push_back({Transform(p.localToSource,local),n});};
                        const auto& ring=p.contours.front();const double sign=orientation>0?1:-1;for(std::size_t j=0;j<ring.size();++j){const auto& a=ring[j];const auto& b=ring[(j+1)%ring.size()];if(a==b)continue;add({a[0],a[1],0},{-sign*(b[1]-a[1]),sign*(b[0]-a[0]),0});}add({0,0,p.depth[0]},{0,0,1});add({0,0,p.depth[1]},{0,0,-1});
                    }
                } else {
                    if ((p.shape==RoiShape::Ellipsoid || p.shape==RoiShape::EllipticCylinder || p.shape==RoiShape::Contour || p.shape==RoiShape::Brush)
                        && !Invert(p.localToSource,prepared.inverse)) throw std::runtime_error("Invalid ROI matrix.");
                    m_planes.error=RoiError::UnsupportedRoi;
                }
            } else if (node.kind!=RoiNodeKind::Intersection && node.kind!=RoiNodeKind::SourceDomain) {
                m_planes.error=RoiError::UnsupportedRoi;
            }
        }
        if (m_planes.error!=RoiError::None) m_planes.planes.clear();
        // 仅作保守读取范围；精确选择始终使用表达式谓词。
        std::vector<std::array<double,6>> bounds(m_definition.nodes.size());
        for (std::size_t i=0;i<m_definition.nodes.size();++i) {
            const auto& node=m_definition.nodes[i];
            auto& box=bounds[i]; box=m_bounds;
            if (node.kind==RoiNodeKind::Empty) box={1,0,1,0,1,0};
            if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Box) {
                auto matrix=node.primitive.localToSource;
                if (node.primitive.boundaryPolicy==RoiBoundaryPolicy::CropV1)
                    for (int row=0;row<3;++row) for (int col=0;col<3;++col) matrix[row*4+col]*=1+1e-6;
                box=GetGridBounds(matrix,{-1,1,-1,1,-1,1});
            }
            if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Ellipsoid)
                box=GetGridBounds(node.primitive.localToSource,{-1,1,-1,1,-1,1});
            if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Sphere)
                for (int a=0;a<3;++a) {box[a*2]=node.primitive.origin[a]-node.primitive.radius;box[a*2+1]=node.primitive.origin[a]+node.primitive.radius;}
            if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Cylinder)
                for (int a=0;a<3;++a) {
                    const auto axis=m_prepared[i].normal[a];
                    const double span=std::abs(axis)*node.primitive.height*0.5+node.primitive.radius*std::sqrt(std::max(0.0,1-axis*axis));
                    box[a*2]=node.primitive.origin[a]-span;box[a*2+1]=node.primitive.origin[a]+span;
                }
            if(node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::VoxelSet){
                const auto ext=node.primitive.voxels->GetIndexBounds(node.primitive.labels);box={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
                if(GetFinite(ext))for(unsigned corner=0;corner<8;++corner){const auto p=Transform(m_indexMatrix,{ext[(corner&1)?1:0],ext[(corner&2)?3:2],ext[(corner&4)?5:4]});for(int a=0;a<3;++a){box[a*2]=std::min(box[a*2],p[a]);box[a*2+1]=std::max(box[a*2+1],p[a]);}}
                else box={1,0,1,0,1,0};
            }
            if(node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::EllipticCylinder){
                const std::array<double,6> ext{-1,1,-1,1,node.primitive.depth[0],node.primitive.depth[1]};box={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
                for(unsigned k=0;k<8;++k){const auto p=Transform(node.primitive.localToSource,{ext[(k&1)?1:0],ext[(k&2)?3:2],ext[(k&4)?5:4]});for(int a=0;a<3;++a){box[a*2]=std::min(box[a*2],p[a]);box[a*2+1]=std::max(box[a*2+1],p[a]);}}
            }
            if(node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Contour){
                std::array<double,6> ext{INFINITY,-INFINITY,INFINITY,-INFINITY,node.primitive.depth[0],node.primitive.depth[1]};
                for(const auto& ring:node.primitive.contours)for(const auto& p:ring)for(int a=0;a<2;++a){ext[a*2]=std::min(ext[a*2],p[a]);ext[a*2+1]=std::max(ext[a*2+1],p[a]);}
                box={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
                for(unsigned corner=0;corner<8;++corner){const auto p=Transform(node.primitive.localToSource,{ext[(corner&1)?1:0],ext[(corner&2)?3:2],ext[(corner&4)?5:4]});for(int a=0;a<3;++a){box[a*2]=std::min(box[a*2],p[a]);box[a*2+1]=std::max(box[a*2+1],p[a]);}}
            }
            if(node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Brush && node.primitive.height==0){
                box={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
                for(const auto& p:node.primitive.path)for(int a=0;a<3;++a){box[a*2]=std::min(box[a*2],p[a]-node.primitive.radius);box[a*2+1]=std::max(box[a*2+1],p[a]+node.primitive.radius);}
            }
            if (node.kind==RoiNodeKind::Difference) box=bounds[node.left];
            if (node.kind==RoiNodeKind::Union || node.kind==RoiNodeKind::Intersection) {
                for (int a=0;a<3;++a) {
                    const bool isUnion=node.kind==RoiNodeKind::Union;
                    box[a*2]=isUnion ? std::min(bounds[node.left][a*2],bounds[node.right][a*2])
                        : std::max(bounds[node.left][a*2],bounds[node.right][a*2]);
                    box[a*2+1]=isUnion ? std::max(bounds[node.left][a*2+1],bounds[node.right][a*2+1])
                        : std::min(bounds[node.left][a*2+1],bounds[node.right][a*2+1]);
                }
            }
            for (int a=0;a<3;++a) { box[a*2]=std::max(box[a*2],m_bounds[a*2]); box[a*2+1]=std::min(box[a*2+1],m_bounds[a*2+1]); }
        }
        m_bounds=bounds[m_definition.nodes.size()-1];
    }
    DataRevisionRef GetRevision() const noexcept override { return m_revision; }
    DataRevisionRef GetSource() const noexcept override { return m_definition.source; }
    const RoiDefinition& GetDefinition() const noexcept override { return m_definition; }
    const std::vector<DataRevisionRef>& GetDependencies() const noexcept override { return m_dependencies; }
    std::array<double,6> GetBounds() const noexcept override { return m_bounds; }
    RoiPlanesResult GetClipPlanes() const override { return m_planes; }
    RoiTriangleSelection GetTriangleSelection(const RoiTriangle& triangle,double tolerance,std::function<bool()> cancelled) const override
    {
        std::vector<RoiTriangle> triangles;
        auto result=GetTriangleChunks(triangle,1024,[&](const auto& batch){triangles.insert(triangles.end(),batch.begin(),batch.end());return true;},tolerance,std::move(cancelled));
        if(result.error==RoiError::None)result.triangles=std::move(triangles);
        return result;
    }
    RoiTriangleSelection GetTriangleChunks(const RoiTriangle& triangle,std::size_t chunkTriangles,
        const RoiTriangleCallback& onTriangles,double tolerance,std::function<bool()> cancelled) const override
    {
        RoiTriangleSelection result;
        if(!chunkTriangles || !onTriangles){result.error=RoiError::InvalidRequest;return result;}
        const auto flush=[&]{if(result.triangles.empty())return true;const bool accepted=onTriangles(result.triangles);result.triangles.clear();return accepted;};
        const auto emit=[&](RoiTriangle piece){result.triangles.push_back(std::move(piece));return result.triangles.size()<chunkTriangles || flush();};
        if (!std::isfinite(tolerance) || tolerance<0) {result.error=RoiError::InvalidRequest;return result;}
        try {if(cancelled && cancelled()){result.error=RoiError::Cancelled;return result;}}
        catch(...){result.error=RoiError::Cancelled;return result;}
        if (m_planes.error==RoiError::None) {
            const auto polygon=GetClippedTriangle(triangle);result.error=polygon.error;
            try {if (polygon.error==RoiError::None) for (std::size_t i=2;i<polygon.points.size();++i)
                if(!emit({polygon.points[0],polygon.points[i-1],polygon.points[i]})){result.error=RoiError::Cancelled;break;}
                if(result.error==RoiError::None && !flush())result.error=RoiError::Cancelled;}
            catch(const std::bad_alloc&){result.triangles.clear();result.error=RoiError::TooLarge;}
            catch(...){result.triangles.clear();result.error=RoiError::InvalidRequest;}
            return result;
        }
        if (!std::isfinite(tolerance) || tolerance<0) {result.error=RoiError::InvalidRequest;return result;}
        for (const auto& point:triangle) if (!GetFinite(point)) {result.error=RoiError::InvalidGeometry;return result;}
        result.isDiscrete=true;
        bool analytic=false;double scale=INFINITY;
        for (const auto& node:m_definition.nodes) if (node.kind==RoiNodeKind::Primitive) {
            const auto& p=node.primitive;
            if (p.shape!=RoiShape::MaskReference && p.shape!=RoiShape::VoxelSet) analytic=true;
            if (p.shape==RoiShape::Sphere || p.shape==RoiShape::Cylinder || p.shape==RoiShape::Brush) scale=std::min(scale,p.radius/4);
            if(p.shape==RoiShape::Box || p.shape==RoiShape::Ellipsoid){Matrix inverse;if(Invert(p.localToSource,inverse)){double norm=0;for(int r=0;r<3;++r)for(int c=0;c<3;++c)norm+=inverse[r*4+c]*inverse[r*4+c];scale=std::min(scale,.25/std::sqrt(norm));}}
            if(p.shape==RoiShape::EllipticCylinder){Matrix inverse;if(Invert(p.localToSource,inverse))for(int r=0;r<3;++r){const double length=std::hypot(inverse[r*4],inverse[r*4+1],inverse[r*4+2]);scale=std::min(scale,(r==2?p.depth[1]-p.depth[0]:1)*.25/length);}}
            if(p.shape==RoiShape::Contour){std::array<double,4> bounds{INFINITY,-INFINITY,INFINITY,-INFINITY};for(const auto& ring:p.contours)for(const auto& point:ring)for(int a=0;a<2;++a){bounds[a*2]=std::min(bounds[a*2],point[a]);bounds[a*2+1]=std::max(bounds[a*2+1],point[a]);}for(int a=0;a<2;++a)if(bounds[a*2+1]>bounds[a*2])scale=std::min(scale,(bounds[a*2+1]-bounds[a*2])/16);if(p.depth[1]>p.depth[0])scale=std::min(scale,(p.depth[1]-p.depth[0])/4);}
            if(p.shape==RoiShape::Brush && p.height>0)scale=std::min(scale,(p.depth[1]-p.depth[0])/4);
        }
        GridGeometry3D virtualGrid;Matrix indexMatrix=m_indexMatrix,sourceToIndex=m_sourceToIndex;
        if (!m_image) {
            for(int a=0;a<3;++a) if(m_bounds[a*2]>m_bounds[a*2+1]){result.error=RoiError::None;return result;}
            if (!std::isfinite(scale)) for(int a=0;a<3;++a) if(m_bounds[a*2+1]>m_bounds[a*2]) scale=std::min(scale,(m_bounds[a*2+1]-m_bounds[a*2])/64);
            const double step=tolerance>0 ? 2*tolerance/std::sqrt(3.0):std::isfinite(scale)&&scale>0 ? scale*.5:1;
            for(int a=0;a<3;++a) {
                const double cells=std::ceil((m_bounds[a*2+1]-m_bounds[a*2])/step);
                if(!std::isfinite(cells)||cells>=std::numeric_limits<int>::max()){result.error=RoiError::TooLarge;return result;}
                virtualGrid.extent[a*2]=0;virtualGrid.extent[a*2+1]=static_cast<int>(cells);virtualGrid.dimensions[a]=static_cast<int>(cells)+1;
                virtualGrid.origin[a]=m_bounds[a*2];virtualGrid.spacing[a]=cells>0 ? (m_bounds[a*2+1]-m_bounds[a*2])/cells:step;
            }
            indexMatrix=GetIndexMatrix(virtualGrid);if(!Invert(indexMatrix,sourceToIndex)){result.error=RoiError::InvalidGeometry;return result;}
        }
        const auto& grid=m_image ? m_image->GetGeometry():virtualGrid;
        double diagonal=0;for (unsigned corner=0;corner<8;++corner) {
            const Point delta=Transform(indexMatrix,{(corner&1)?0.5:-0.5,(corner&2)?0.5:-0.5,(corner&4)?0.5:-0.5});
            diagonal=std::max(diagonal,std::hypot(delta[0]-grid.origin[0],delta[1]-grid.origin[1],delta[2]-grid.origin[2]));
        }
        const double targetTolerance=tolerance>0 ? tolerance:scale;
        const double refine=analytic && std::isfinite(targetTolerance) ? std::max(1.0,std::ceil(diagonal/targetTolerance)):1;
        if (refine>static_cast<double>(std::numeric_limits<int>::max())) {result.error=RoiError::TooLarge;return result;}
        const int divisions=static_cast<int>(refine);result.boundaryTolerance=analytic ? diagonal/divisions:0;
        try {
            const auto prepared=GetQuery();if(prepared.error!=RoiError::None){result.error=prepared.error;return result;}
            std::array<Point,3> indexed{};for (int i=0;i<3;++i) indexed[i]=Transform(sourceToIndex,triangle[i]);
            std::array<double,6> roiIndex{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
            for(int a=0;a<3;++a)if(m_bounds[a*2]>m_bounds[a*2+1]){result.error=RoiError::None;return result;}
            for(unsigned k=0;k<8;++k){const auto p=Transform(sourceToIndex,{m_bounds[(k&1)?1:0],m_bounds[(k&2)?3:2],m_bounds[(k&4)?5:4]});for(int a=0;a<3;++a){roiIndex[a*2]=std::min(roiIndex[a*2],p[a]);roiIndex[a*2+1]=std::max(roiIndex[a*2+1],p[a]);}}
            std::array<int,6> cells{};
            for (int a=0;a<3;++a) {
                const double low=std::max(roiIndex[a*2]-.5,std::min({indexed[0][a],indexed[1][a],indexed[2][a]})),high=std::min(roiIndex[a*2+1]+.5,std::max({indexed[0][a],indexed[1][a],indexed[2][a]}));
                if(low>high){result.error=RoiError::None;return result;}
                if (high<grid.extent[a*2] || low>grid.extent[a*2+1]) {result.error=RoiError::None;return result;}
                cells[a*2]=static_cast<int>(std::max(static_cast<double>(grid.extent[a*2]),std::floor(low+0.5)));
                cells[a*2+1]=static_cast<int>(std::min(static_cast<double>(grid.extent[a*2+1]),std::floor(high+0.5)));
            }
            for (std::int64_t z=cells[4];z<=cells[5];++z) for (std::int64_t y=cells[2];y<=cells[3];++y) for (std::int64_t x=cells[0];x<=cells[1];++x){
                const std::array<std::int64_t,3> cell{x,y,z};std::array<int,6> sub{};bool active=true;
                for(int a=0;a<3;++a){const double base=double(cell[a])-.5;
                    const auto clamp=[&](double value){return static_cast<int>(std::clamp(value,0.,double(divisions-1)));};
                    sub[a*2]=clamp(std::floor((roiIndex[a*2]-base)*divisions));sub[a*2+1]=clamp(std::ceil((roiIndex[a*2+1]-base)*divisions));
                    const double low=std::min({indexed[0][a],indexed[1][a],indexed[2][a]}),high=std::max({indexed[0][a],indexed[1][a],indexed[2][a]});
                    if(high-low<=64*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(low),std::abs(high)}))sub[a*2]=sub[a*2+1]=clamp(std::floor(((low+high)*.5-base)*divisions));
                    if(sub[a*2]>sub[a*2+1])active=false;
                }
                if(!active)continue;
                for (int sz=sub[4];sz<=sub[5];++sz) for (int sy=sub[2];sy<=sub[3];++sy) for (int sx=sub[0];sx<=sub[1];++sx) {
                    bool stopped=false;if(cancelled && sx==sub[0]){try{stopped=cancelled();}catch(...){stopped=true;}}
                    if(stopped){result.triangles.clear();result.error=RoiError::Cancelled;return result;}
                    Point low{static_cast<double>(x)-0.5+static_cast<double>(sx)/divisions,static_cast<double>(y)-0.5+static_cast<double>(sy)/divisions,static_cast<double>(z)-0.5+static_cast<double>(sz)/divisions};
                    Point high{low[0]+1.0/divisions,low[1]+1.0/divisions,low[2]+1.0/divisions},center{};
                    bool overlaps=true;for (int a=0;a<3;++a) {
                        low[a]=std::max(low[a],static_cast<double>(grid.extent[a*2]));high[a]=std::min(high[a],static_cast<double>(grid.extent[a*2+1]));
                        center[a]=(low[a]+high[a])*0.5;if (low[a]>high[a]) overlaps=false;
                    }
                    if (!overlaps || !prepared.query->GetContains(Transform(indexMatrix,center))) continue;
                    std::vector<Point> polygon(indexed.begin(),indexed.end()),next;
                    for (int axis=0;axis<3 && !polygon.empty();++axis) for (int side=0;side<2;++side) {
                        if (polygon.empty()) break;next.clear();Point previous=polygon.back();
                        const double limit=side ? high[axis]:low[axis],sign=side ? -1.0:1.0;
                        double previousDistance=sign*(previous[axis]-limit);
                        for (const auto& point:polygon) {
                            const double distance=sign*(point[axis]-limit);
                            if ((distance>=0)!=(previousDistance>=0)) {
                                next.push_back(GetEdgeIntersection(previous,point,previousDistance,distance));
                            }
                            if (distance>=0) next.push_back(point);previous=point;previousDistance=distance;
                        }polygon.swap(next);
                    }
                    for (std::size_t i=2;i<polygon.size();++i) {
                        std::array<Point,3> piece{Transform(indexMatrix,polygon[0]),Transform(indexMatrix,polygon[i-1]),Transform(indexMatrix,polygon[i])};
                        const Point a{piece[1][0]-piece[0][0],piece[1][1]-piece[0][1],piece[1][2]-piece[0][2]},b{piece[2][0]-piece[0][0],piece[2][1]-piece[0][1],piece[2][2]-piece[0][2]};
                        if (std::hypot(a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])==0) continue;
                        if(!emit(std::move(piece))){result.triangles.clear();result.error=RoiError::Cancelled;return result;}
                    }
                }
            }
            result.error=flush()?RoiError::None:RoiError::Cancelled;
        } catch (const std::bad_alloc&) {result.triangles.clear();result.error=RoiError::TooLarge;}
        catch (...) {result.triangles.clear();result.error=RoiError::InvalidRequest;}
        return result;
    }
    RoiPolygonResult GetClippedTriangle(const std::array<Point,3>& triangle) const override
    {
        RoiPolygonResult result;
        if (m_planes.error!=RoiError::None) return result;
        for (const auto& p:triangle) if (!GetFinite(p)) { result.error=RoiError::InvalidGeometry; return result; }
        result.requiredBytes=(m_planes.planes.size()+3)*sizeof(Point)*2;
        try {
            std::vector<Point> polygon(triangle.begin(),triangle.end()), next;
            polygon.reserve(m_planes.planes.size()+3); next.reserve(m_planes.planes.size()+3);
            for (const auto& plane:m_planes.planes) {
                if (polygon.empty()) break;
                next.clear();
                Point previous=polygon.back();
                const auto distance=[&](const Point& p){return Dot(plane.normal,{p[0]-plane.origin[0],p[1]-plane.origin[1],p[2]-plane.origin[2]});};
                double previousDistance=distance(previous);
                for (const auto& point:polygon) {
                    const double currentDistance=distance(point);
                    if ((currentDistance>=0)!=(previousDistance>=0)) {
                        next.push_back(GetEdgeIntersection(previous,point,previousDistance,currentDistance));
                    }
                    if (currentDistance>=0) next.push_back(point);
                    previous=point; previousDistance=currentDistance;
                }
                polygon.swap(next);
            }
            result.points=std::move(polygon); result.error=RoiError::None;
        } catch (...) { result.error=RoiError::TooLarge; }
        return result;
    }
    double GetBoundaryDistance(const Point& p) const noexcept override
    {
        if(m_planes.error!=RoiError::None)return NAN;
        Point index{};if(!GetSourceContains(p,index))return NAN;
        // 凸定义只有交集，逐叶检查保留原成员容差，不需要表达式工作区。
        for(std::size_t i=0;i<m_definition.nodes.size();++i)
            if(m_definition.nodes[i].kind==RoiNodeKind::Primitive && !GetPrimitiveContains(i,p,index))return NAN;
        double distance=INFINITY;
        for (const auto& plane:m_planes.planes) {
            const Point offset{p[0]-plane.origin[0],p[1]-plane.origin[1],p[2]-plane.origin[2]};
            distance=std::min(distance,std::abs(Dot(plane.normal,offset)));
        }
        return distance;
    }
    bool GetSourceContains(const Point& p,Point& index) const noexcept
    {
        if (!GetFinite(p)) return false;
        if (m_image) {
            index=Transform(m_sourceToIndex,p);
            const auto& extent=m_image->GetGeometry().extent;
            for (int a=0;a<3;++a) if (index[a]<extent[a*2]-1e-9 || index[a]>extent[a*2+1]+1e-9) return false;
            if (m_image->GetValidityMask()) {
                std::size_t offset=0,stride=1;
                for (int a=0;a<3;++a) {offset+=static_cast<std::size_t>(std::floor(index[a]+0.5)-extent[a*2])*stride;stride*=static_cast<std::size_t>(m_image->GetGeometry().dimensions[a]);}
                if (!(*m_image->GetValidityMask())[offset]) return false;
            }
        } else {
            for (int a=0;a<3;++a) if (p[a]<m_bounds[a*2] || p[a]>m_bounds[a*2+1]) return false;
        }
        return true;
    }
    bool GetPrimitiveContains(std::size_t i,const Point& p,const Point& index) const noexcept
    {
        const auto& node=m_definition.nodes[i];bool contains=false;
                const auto& primitive=node.primitive;
                if (primitive.shape==RoiShape::Box) {
                    const auto local=Transform(m_prepared[i].inverse,p);
                    const double edge=primitive.boundaryPolicy==RoiBoundaryPolicy::CropV1 ? 1+1e-6:1+1e-12;
                    contains=GetFinite(local) && std::abs(local[0])<=edge
                        && std::abs(local[1])<=edge && std::abs(local[2])<=edge;
                } else if (primitive.shape==RoiShape::HalfSpace) {
                    const double distance=Dot(m_prepared[i].normal,{p[0]-primitive.origin[0],p[1]-primitive.origin[1],p[2]-primitive.origin[2]});
                    contains=primitive.boundaryPolicy==RoiBoundaryPolicy::CropV1 ? distance>0:distance>=0;
                } else if (primitive.shape==RoiShape::Sphere || primitive.shape==RoiShape::Cylinder) {
                    const Point offset{p[0]-primitive.origin[0],p[1]-primitive.origin[1],p[2]-primitive.origin[2]};
                    if (primitive.shape==RoiShape::Sphere) contains=std::hypot(offset[0],offset[1],offset[2])<=primitive.radius;
                    else {
                        const auto& axis=m_prepared[i].normal; const double axial=Dot(offset,axis);
                        contains=std::abs(axial)<=primitive.height*0.5
                            && std::hypot(offset[0]-axial*axis[0],offset[1]-axial*axis[1],offset[2]-axial*axis[2])<=primitive.radius;
                    }
                } else if (primitive.shape==RoiShape::Ellipsoid) {
                    const auto local=Transform(m_prepared[i].inverse,p);
                    contains=Dot(local,local)<=1;
                } else if(primitive.shape==RoiShape::EllipticCylinder){
                    const auto local=Transform(m_prepared[i].inverse,p);contains=local[0]*local[0]+local[1]*local[1]<=1 && local[2]>=primitive.depth[0] && local[2]<=primitive.depth[1];
                } else if (primitive.shape==RoiShape::Contour) {
                    const auto local=Transform(m_prepared[i].inverse,p);
                    contains=local[2]>=primitive.depth[0] && local[2]<=primitive.depth[1]
                        && GetContourContains(primitive,{local[0],local[1]});
                } else if (primitive.shape==RoiShape::ProjectedContour) {
                    const auto view=Transform(primitive.sourceToView,p);
                    const auto& m=primitive.viewToClip;
                    const double w=m[12]*view[0]+m[13]*view[1]+m[14]*view[2]+m[15];
                    const double x=m[0]*view[0]+m[1]*view[1]+m[2]*view[2]+m[3];
                    const double y=m[4]*view[0]+m[5]*view[1]+m[6]*view[2]+m[7];
                    const double z=m[8]*view[0]+m[9]*view[1]+m[10]*view[2]+m[11];
                    contains=w>0 && std::abs(x)<=w && std::abs(y)<=w && std::abs(z)<=w
                        && -view[2]/std::hypot(primitive.sourceToView[8],primitive.sourceToView[9],primitive.sourceToView[10])>=primitive.depth[0] && -view[2]/std::hypot(primitive.sourceToView[8],primitive.sourceToView[9],primitive.sourceToView[10])<=primitive.depth[1]
                        && GetContourContains(primitive,{x/w,y/w});
                } else if (primitive.shape==RoiShape::Brush) {
                    contains=false;
                    const auto point=primitive.height>0 ? Transform(m_prepared[i].inverse,p):p;
                    for (std::size_t k=0;k<primitive.path.size();++k) {
                        auto a=primitive.path[k ? k-1:0], b=primitive.path[k];
                        if (primitive.height>0) {a=Transform(m_prepared[i].inverse,a);b=Transform(m_prepared[i].inverse,b);}
                        if (primitive.height>0 && (point[2]<primitive.depth[0] || point[2]>primitive.depth[1])) continue;
                        Point d{b[0]-a[0],b[1]-a[1],primitive.height>0 ? 0:b[2]-a[2]};
                        Point q{point[0]-a[0],point[1]-a[1],primitive.height>0 ? 0:point[2]-a[2]};
                        const double t=Dot(d,d)>0 ? std::clamp(Dot(q,d)/Dot(d,d),0.0,1.0):0;
                        contains=std::hypot(q[0]-t*d[0],q[1]-t*d[1],q[2]-t*d[2])<=primitive.radius;
                        if (contains) break;
                    }
                } else {
                    const auto& grid=m_image->GetGeometry();
                    std::size_t offset=0, stride=1;
                    std::array<int,3> voxel{};
                    for (int a=0;a<3;++a) {
                        const double nearest=std::floor(index[a]+0.5);
                        if (nearest<grid.extent[a*2] || nearest>grid.extent[a*2+1]) return false;
                        offset+=static_cast<std::size_t>(nearest-grid.extent[a*2])*stride;
                        voxel[a]=static_cast<int>(nearest);
                        stride*=static_cast<std::size_t>(grid.dimensions[a]);
                    }
                    const auto label=primitive.shape==RoiShape::VoxelSet ? primitive.voxels->GetLabel(voxel):GetMaskValue(m_prepared[i].mask,offset);
                    contains=GetSelectedLabel(label,primitive.labels);
                }
        return contains;
    }
    RoiQueryResult GetQuery() const override
    {
        try{return {RoiError::None,std::make_unique<Query>(shared_from_this())};}
        catch(const std::bad_alloc&){return {RoiError::TooLarge,{}};}
        catch(...){return {RoiError::Unavailable,{}};}
    }
    bool GetContains(const Point& p) const override
    {
        std::array<std::uint8_t,128> local{};
        if(m_definition.nodes.size()<=local.size())return GetContains(p,local.data());
        std::vector<std::uint8_t> scratch(m_definition.nodes.size());
        return GetContains(p,scratch.data());
    }
    bool GetContains(const Point& p,std::uint8_t* values) const noexcept
    {
        Point index{};if(!GetSourceContains(p,index))return false;
        // 定义已验证为拓扑有序，工作区由当前调用者独占。
        for (std::size_t i=0;i<m_definition.nodes.size();++i) {
            const auto& node=m_definition.nodes[i];
            switch (node.kind) {
            case RoiNodeKind::Empty: values[i]=false; break;
            case RoiNodeKind::SourceDomain: values[i]=true; break;
            case RoiNodeKind::Union: values[i]=values[node.left]||values[node.right]; break;
            case RoiNodeKind::Intersection: values[i]=values[node.left]&&values[node.right]; break;
            case RoiNodeKind::Difference: values[i]=values[node.left]&&!values[node.right]; break;
            case RoiNodeKind::Primitive:values[i]=GetPrimitiveContains(i,p,index);break;
            }
        }
        return values[m_definition.nodes.size()-1];
    }
    RoiPointsResult GetPoints(const std::vector<Point>& points) const override
    {
        RoiPointsResult result;
        result.requiredBytes=points.size();
        if (std::any_of(points.begin(),points.end(),[](const Point& p){return !GetFinite(p);})) return result;
        try {
            const auto prepared=GetQuery();if(prepared.error!=RoiError::None){result.error=prepared.error;return result;}
            result.values.reserve(points.size());
            for (const auto& p:points) result.values.push_back(prepared.query->GetContains(p)?1:0);
            result.error=RoiError::None;
        } catch (...) { result.values.clear(); result.error=RoiError::TooLarge; }
        return result;
    }
    RoiMaskResult GetMaskChunk(const RoiMaskRequest& request) const override
    {
        RoiMaskResult result;
        if (!m_image) { result.error=RoiError::UnsupportedRoi; return result; }
        const auto& grid=m_image->GetGeometry();
        std::size_t count=1;
        for (int a=0;a<3;++a) {
            const auto dim=static_cast<std::size_t>(grid.dimensions[a]);
            if (request.region.size[a]==0 || request.region.offset[a]>dim
                || request.region.size[a]>dim-request.region.offset[a]
                || count>std::numeric_limits<std::size_t>::max()/request.region.size[a]) return result;
            count*=request.region.size[a];
        }
        if (request.voxelOffset>count || request.chunkBytes==0) return result;
        const auto bytes=std::min(count-request.voxelOffset,request.chunkBytes);
        result.requiredBytes=bytes;
        try {
            const auto prepared=GetQuery();if(prepared.error!=RoiError::None){result.error=prepared.error;return result;}
            result.values.resize(bytes);
            for (std::size_t i=0;i<bytes;++i) {
                const auto offset=request.voxelOffset+i;
                if ((i==0 || offset%request.region.size[0]==0)
                    && request.getCancelled && request.getCancelled()) {
                    result.values.clear(); result.error=RoiError::Cancelled; return result;
                }
                const std::size_t x=offset%request.region.size[0];
                const std::size_t y=(offset/request.region.size[0])%request.region.size[1];
                const std::size_t z=offset/(request.region.size[0]*request.region.size[1]);
                Point index{static_cast<double>(grid.extent[0])+static_cast<double>(request.region.offset[0]+x),
                    static_cast<double>(grid.extent[2])+static_cast<double>(request.region.offset[1]+y),
                    static_cast<double>(grid.extent[4])+static_cast<double>(request.region.offset[2]+z)};
                result.values[i]=prepared.query->GetContains(Transform(m_indexMatrix,index))?1:0;
            }
            if (request.getCancelled && request.getCancelled()) { result.values.clear(); result.error=RoiError::Cancelled; return result; }
            result.error=RoiError::None;
            result.nextOffset=request.voxelOffset+bytes;
            result.isComplete=result.nextOffset==count;
        } catch (const std::bad_alloc&) { result.values.clear(); result.error=RoiError::TooLarge; }
        catch (...) { result.values.clear(); result.error=RoiError::Cancelled; }
        return result;
    }
private:
    struct Prepared final { Matrix inverse=roiIdentityMatrix; Point normal{}; DataSnapshot mask; };
    DataRevisionRef m_revision;
    RoiDefinition m_definition;
    class Query final : public RoiQuery {
    public:
        explicit Query(std::shared_ptr<const RoiView> view):m_view(std::move(view))
        {if(m_view->m_definition.nodes.size()>m_local.size())m_scratch.resize(m_view->m_definition.nodes.size());}
        bool GetContains(const Point& point) const noexcept override
        {return m_view->GetContains(point,m_scratch.empty()?m_local.data():m_scratch.data());}
    private:
        std::shared_ptr<const RoiView> m_view;
        mutable std::array<std::uint8_t,128> m_local{};
        mutable std::vector<std::uint8_t> m_scratch;
    };
    std::vector<Prepared> m_prepared;
    std::vector<DataRevisionRef> m_dependencies;
    std::shared_ptr<const ImageGrid3DPayload> m_image;
    Matrix m_indexMatrix=roiIdentityMatrix;
    Matrix m_sourceToIndex=roiIdentityMatrix;
    std::array<double,6> m_bounds{};
    RoiPlanesResult m_planes;
};
} // namespace

RoiError RoiEvaluator::GetDefinitionError(const RoiDefinition& definition) noexcept
{
    try {
    if (!GetDataRevisionRefValid(definition.source) || definition.nodes.empty()) return RoiError::InvalidRequest;
    if (definition.nodes.size()>roiNodeLimit) return RoiError::TooLarge;
    std::vector<std::uint8_t> reachable(definition.nodes.size());
    for (std::size_t i=0;i<definition.nodes.size();++i) {
        const auto& n=definition.nodes[i];
        const auto& p=n.primitive;
        const bool binary=n.kind==RoiNodeKind::Union || n.kind==RoiNodeKind::Intersection || n.kind==RoiNodeKind::Difference;
        if (binary) {
            if (n.left>=i || n.right>=i || !GetPrimitiveDefault(p)) return RoiError::InvalidGeometry;
        } else {
            if (n.left!=0 || n.right!=0) return RoiError::InvalidGeometry;
            if (n.kind==RoiNodeKind::Primitive) {
                if (!GetFinite(p.localToSource) || !GetFinite(p.origin) || !GetFinite(p.normal)
                    || !std::isfinite(p.radius) || !std::isfinite(p.height)) return RoiError::InvalidGeometry;
                if (p.boundaryPolicy!=RoiBoundaryPolicy::Closed && p.boundaryPolicy!=RoiBoundaryPolicy::CropV1) return RoiError::UnsupportedRoi;
                if (p.shape!=RoiShape::Sphere && p.shape!=RoiShape::Cylinder && p.shape!=RoiShape::Brush && (p.radius!=0 || p.height!=0)) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::Contour && p.shape!=RoiShape::ProjectedContour && !p.contours.empty()) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::Brush && !p.path.empty()) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::VoxelSet && p.voxels) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::VoxelSet && p.shape!=RoiShape::MaskReference && !p.labels.empty()) return RoiError::InvalidGeometry;
                if (std::any_of(p.labels.begin(),p.labels.end(),[](auto label){return label==0;})) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::ProjectedContour && (p.sourceToView!=roiIdentityMatrix || p.viewToClip!=roiIdentityMatrix)) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::Contour && p.shape!=RoiShape::ProjectedContour && p.shape!=RoiShape::Brush && p.shape!=RoiShape::EllipticCylinder && p.depth!=std::array<double,2>{-0.5,0.5}) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::HalfSpace && p.shape!=RoiShape::Sphere && p.shape!=RoiShape::Cylinder
                    && (p.origin!=Point{0,0,0} || p.normal!=Point{0,0,1})) return RoiError::InvalidGeometry;
                if (p.shape!=RoiShape::Box && p.shape!=RoiShape::HalfSpace && p.shape!=RoiShape::Sphere && p.shape!=RoiShape::Cylinder
                    && p.boundaryPolicy!=RoiBoundaryPolicy::Closed) return RoiError::InvalidGeometry;
                if (p.shape==RoiShape::Box) {
                    Matrix inverse;
                    if (!Invert(p.localToSource,inverse) || p.mask || p.origin!=Point{0,0,0} || p.normal!=Point{0,0,1}) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::HalfSpace) {
                    const double length=std::hypot(p.normal[0],p.normal[1],p.normal[2]);
                    if (!(length>0) || !std::isfinite(length) || p.mask || p.localToSource!=roiIdentityMatrix) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::Sphere || p.shape==RoiShape::Cylinder) {
                    const double length=std::hypot(p.normal[0],p.normal[1],p.normal[2]);
                    if (p.mask || p.localToSource!=roiIdentityMatrix || !(p.radius>0)) return RoiError::InvalidGeometry;
                    if (p.shape==RoiShape::Sphere ? (p.height!=0 || p.normal!=Point{0,0,1})
                        : (!(p.height>0) || !(length>0) || !std::isfinite(length))) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::MaskReference) {
                    if (p.boundaryPolicy!=RoiBoundaryPolicy::Closed || !p.mask || !GetDataRevisionRefValid(*p.mask) || p.localToSource!=roiIdentityMatrix
                        || p.origin!=Point{0,0,0} || p.normal!=Point{0,0,1}) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::Ellipsoid || p.shape==RoiShape::EllipticCylinder || p.shape==RoiShape::Contour || p.shape==RoiShape::Brush) {
                    Matrix inverse;
                    if (!Invert(p.localToSource,inverse) || p.mask || !GetFinite(p.depth) || p.depth[0]>p.depth[1]
                        || ((p.shape==RoiShape::Contour || p.shape==RoiShape::EllipticCylinder || (p.shape==RoiShape::Brush && p.height>0)) && p.depth[0]==p.depth[1])) return RoiError::InvalidGeometry;
                    if (p.shape==RoiShape::Brush) {
                        if(p.height>0)for(int a=0;a<3;++a)for(int b=0;b<3;++b){double dot=0;for(int r=0;r<3;++r)dot+=p.localToSource[r*4+a]*p.localToSource[r*4+b];if(std::abs(dot-(a==b?1.:0.))>1e-9)return RoiError::InvalidGeometry;}
                        if (!(p.radius>0) || p.height<0 || p.path.empty()) return RoiError::InvalidGeometry;
                        for (const auto& point:p.path) if (!GetFinite(point)) return RoiError::InvalidGeometry;
                    } else if (p.shape==RoiShape::Contour) {
                        if (p.contours.empty()) return RoiError::InvalidGeometry;
                        for (const auto& ring:p.contours) if(!GetRingValid(ring)) return RoiError::InvalidGeometry;
                    }
                } else if (p.shape==RoiShape::ProjectedContour) {
                    Matrix inverse;
                    if (p.mask || p.localToSource!=roiIdentityMatrix || !Invert(p.sourceToView,inverse) || !GetProjectiveValid(p.viewToClip) || !GetFinite(p.depth)
                        || p.depth[0]<0 || p.depth[0]>=p.depth[1] || p.contours.empty()) return RoiError::InvalidGeometry;
                    for (const auto& ring:p.contours) if(!GetRingValid(ring)) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::VoxelSet) {
                    if (!p.voxels || p.mask || p.localToSource!=roiIdentityMatrix) return RoiError::InvalidGeometry;
                } else return RoiError::UnsupportedRoi;
            } else if ((n.kind!=RoiNodeKind::Empty && n.kind!=RoiNodeKind::SourceDomain) || !GetPrimitiveDefault(p)) return RoiError::InvalidGeometry;
        }
    }
    reachable[definition.nodes.size()-1]=true;
    for (std::size_t i=definition.nodes.size();i-->0;) {
        if (!reachable[i]) return RoiError::InvalidGeometry;
        const auto& n=definition.nodes[i];
        if (n.kind==RoiNodeKind::Union || n.kind==RoiNodeKind::Intersection || n.kind==RoiNodeKind::Difference)
            reachable[n.left]=reachable[n.right]=true;
    }
    return RoiError::None;
    } catch (...) { return RoiError::TooLarge; }
}

std::vector<DataInputRef> RoiEvaluator::GetInputs(const RoiDefinition& definition)
{
    std::vector<DataInputRef> inputs{{"source-data",definition.source}};
    std::set<DataRevisionRef> masks;
    for (const auto& node:definition.nodes)
        if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::MaskReference && node.primitive.mask)
            masks.insert(*node.primitive.mask);
    for (const auto& mask:masks) inputs.push_back({"mask-"+std::to_string(inputs.size()-1),mask});
    return inputs;
}

RoiError RoiEvaluator::GetRelationsError(const DataRevision& roi,const DataLookup& getData,bool checkProvenance)
{
    const auto* payload=dynamic_cast<const RoiGeometryPayload*>(roi.payload.get());
    if (!payload || roi.type!=DataTypes::roiGeometry) return RoiError::UnsupportedRoi;
    const auto& definition=payload->GetDefinition();
    const auto local=GetDefinitionError(definition);
    if (local!=RoiError::None) return local;
    const auto expected=GetInputs(definition);
    std::set<std::string> roles;
    for (const auto& input:roi.inputs) {
        if (!roles.insert(input.role).second) return RoiError::InvalidRequest;
        const auto found=std::find_if(expected.begin(),expected.end(),[&](const DataInputRef& item){return item.role==input.role && item.source==input.source;});
        if (found==expected.end()) {
            if (input.role=="operation-mesh" || input.role.rfind("operation-mesh-",0)==0) {
                if(!checkProvenance)continue;
                const auto mesh=getData(input.source);
                if (!mesh || mesh->type!=DataTypes::surfaceMesh) return RoiError::MissingInput;
                continue;
            }
            if (input.role!="copy-source" && input.role.rfind("operation-input-",0)!=0) return RoiError::InvalidRequest;
            if(!checkProvenance)continue;
            const auto copy=getData(input.source);
            const auto* copyPayload=copy ? dynamic_cast<const RoiGeometryPayload*>(copy->payload.get()):nullptr;
            if (!copyPayload || (input.role=="copy-source" && copyPayload->GetDefinition().source!=definition.source)) return RoiError::SourceMismatch;
        }
    }
    for (const auto& input:expected) if (roles.count(input.role)==0) return RoiError::MissingInput;
    const auto source=getData(definition.source);
    if (!source) return RoiError::MissingInput;
    const auto* image=dynamic_cast<const ImageGrid3DPayload*>(source->payload.get());
    const auto* mesh=dynamic_cast<const SurfaceMeshPayload*>(source->payload.get());
    if ((!image || source->type!=DataTypes::imageGrid3D) && (!mesh || source->type!=DataTypes::surfaceMesh)) return RoiError::SourceMismatch;
    if (image) {
        Matrix inverse;
        if (!image->GetValid() || !Invert(GetIndexMatrix(image->GetGeometry()),inverse)
            || !GetFinite(GetGridBounds(GetIndexMatrix(image->GetGeometry()),image->GetGeometry().extent))) return RoiError::InvalidGeometry;
    } else if (!mesh->GetValid() || mesh->GetVertices().empty()) return RoiError::InvalidGeometry;
    for (const auto& node:definition.nodes) if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::VoxelSet) {
        if (!image) return RoiError::SourceMismatch;
        const auto& extent=image->GetGeometry().extent;
        for (const auto& run:node.primitive.voxels->GetRuns())
            for (int axis=0;axis<3;++axis)
                if (run.first[axis]<extent[axis*2] || (axis==0?run.last:run.first[axis])>extent[axis*2+1]) return RoiError::InvalidGeometry;
    }
    for (std::size_t i=1;i<expected.size();++i) {
        const auto mask=getData(expected[i].source);
        const auto* grid=GetMaskGrid(mask);
        if (!mask || !grid) return RoiError::MissingInput;
        if (!image || !GetSameGrid(image->GetGeometry(),*grid)) return RoiError::SourceMismatch;
        const auto* binary=dynamic_cast<const BinaryMask3DPayload*>(mask->payload.get());
        const auto* labels=dynamic_cast<const LabelMap3DPayload*>(mask->payload.get());
        if ((binary && (!binary->GetValid() || mask->type!=DataTypes::binaryMask3D))
            || (labels && (!labels->GetValid() || mask->type!=DataTypes::labelMap3D))) return RoiError::InvalidGeometry;
    }
    return RoiError::None;
}

RoiReadResult RoiEvaluator::GetRoi(const DataGraphSnapshot& graph,const DataRevisionRef& roiRef,const DataRevisionRef& sourceRef)
{
    RoiReadResult result;
    if (!graph.view) return result;
    const auto lookup=[&graph](const DataRevisionRef& ref){return graph.view->GetData(ref);};
    const auto roi=lookup(roiRef);
    if (!roi) { result.error=RoiError::MissingInput; return result; }
    try {
        result.error=GetRelationsError(*roi,lookup,false);
        if (result.error!=RoiError::None) return result;
        const auto& definition=dynamic_cast<const RoiGeometryPayload&>(*roi->payload).GetDefinition();
        if (definition.source!=sourceRef) { result.error=RoiError::SourceMismatch; return result; }
        result.roi=std::make_shared<const RoiView>(roi,lookup(sourceRef),lookup);
    } catch (const std::bad_alloc&) { result.error=RoiError::TooLarge; }
    catch (...) { result.error=RoiError::InvalidGeometry; }
    return result;
}

RoiReadResult RoiEvaluator::GetPreview(const DataGraphSnapshot& graph,const RoiDefinition& definition)
{
    RoiReadResult result;if (!graph.view) return result;
    try {
        const auto candidate=std::make_shared<const DataRevision>(DataRevision{{},DataTypes::roiGeometry,
            GetInputs(definition),std::make_shared<const RoiGeometryPayload>(definition),{}});
        const auto lookup=[&](const DataRevisionRef& ref){return graph.view->GetData(ref);};
        result.error=GetRelationsError(*candidate,lookup);
        if (result.error==RoiError::None) result.roi=std::make_shared<const RoiView>(candidate,lookup(definition.source),lookup);
    } catch (const std::bad_alloc&) {result.error=RoiError::TooLarge;}
    catch (...) {result.error=RoiError::InvalidGeometry;}
    return result;
}
