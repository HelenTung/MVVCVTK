#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <locale>
#include <cmath>
#include <cstring>
#include <climits>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <mutex>
#include <numeric>

namespace {
// 邻域及形态学中间 halo 可越过 int 端点；只有最终域内载荷才收窄。
using Index=std::array<std::int64_t,3>;
using Point=std::array<double,3>;
using Matrix=std::array<double,16>;
void CheckCancel(const RoiOperationRequest& request);
// X 连续区间按行保存；稠密选区不再为每个体素分配一个哈希节点。
class Cells final {
public:
    using Runs=std::vector<std::pair<std::int64_t,std::int64_t>>;
    using Rows=std::map<std::array<std::int64_t,2>,Runs>;
    struct Span {Index first;std::int64_t last;};
    const Rows& GetRows()const noexcept{return m_rows;}
    const Runs& GetRow(const std::array<std::int64_t,2>& row)const{
        static const Runs empty;const auto found=m_rows.find(row);return found==m_rows.end()?empty:found->second;
    }
    void InsertRun(const Index& first,std::int64_t last){
        auto& runs=m_rows[{first[1],first[2]}];std::int64_t low=first[0],high=last;
        auto begin=std::lower_bound(runs.begin(),runs.end(),low,[](const auto& run,std::int64_t x){return run.second+1<x;});
        auto end=begin;std::size_t removed=0;
        while(end!=runs.end() && std::int64_t(end->first)<=std::int64_t(high)+1){
            low=std::min(low,end->first);high=std::max(high,end->second);removed+=std::size_t(std::int64_t(end->second)-end->first+1);++end;}
        const auto added=std::size_t(std::int64_t(high)-low+1);
        if(added>std::numeric_limits<std::size_t>::max()-(m_count-removed))throw std::length_error("ROI member count overflows.");
        begin=runs.erase(begin,end);runs.insert(begin,{low,high});m_count=m_count-removed+added;
    }
    std::optional<Span> TakeRun(const std::array<std::int64_t,2>& key,std::int64_t low,std::int64_t high){
        const auto row=m_rows.find(key);if(row==m_rows.end())return {};
        auto& runs=row->second;const auto run=std::lower_bound(runs.begin(),runs.end(),low,[](const auto& run,std::int64_t x){return run.second<x;});
        if(run==runs.end() || run->first>high)return {};
        const Span taken{{run->first,key[0],key[1]},run->second};m_count-=std::size_t(std::int64_t(run->second)-run->first+1);
        runs.erase(run);if(runs.empty())m_rows.erase(row);return taken;
    }
    class Iterator final {
    public:
        using iterator_category=std::input_iterator_tag;
        using value_type=Index;using difference_type=std::ptrdiff_t;using pointer=void;using reference=Index;
        Iterator(Rows::const_iterator row,Rows::const_iterator end):m_row(row),m_end(end)
        {if(m_row!=m_end)m_x=m_row->second.front().first;}
        Index operator*()const{return {m_x,m_row->first[0],m_row->first[1]};}
        Iterator& operator++(){const auto& run=m_row->second[m_run];if(m_x<run.second)++m_x;
            else if(++m_run<m_row->second.size())m_x=m_row->second[m_run].first;
            else{++m_row;m_run=0;if(m_row!=m_end)m_x=m_row->second.front().first;}return *this;}
        bool operator!=(const Iterator& other)const{return m_row!=other.m_row || (m_row!=m_end && (m_run!=other.m_run || m_x!=other.m_x));}
    private:Rows::const_iterator m_row,m_end;std::size_t m_run=0;std::int64_t m_x=0;
    };
    Iterator begin()const{return {m_rows.begin(),m_rows.end()};}
    Iterator end()const{return {m_rows.end(),m_rows.end()};}
    std::size_t size()const noexcept{return m_count;}
    bool empty()const noexcept{return !m_count;}
    bool count(const Index& i)const{
        const auto row=m_rows.find({i[1],i[2]});if(row==m_rows.end())return false;
        const auto& runs=row->second;auto next=std::upper_bound(runs.begin(),runs.end(),i[0],[](std::int64_t x,const auto& run){return x<run.first;});
        return next!=runs.begin() && std::prev(next)->second>=i[0];
    }
    bool insert(const Index& i){
        auto& runs=m_rows[{i[1],i[2]}];auto next=std::upper_bound(runs.begin(),runs.end(),i[0],[](std::int64_t x,const auto& run){return x<run.first;});
        if(next!=runs.begin()){
            auto previous=std::prev(next);if(previous->second>=i[0])return false;
            if(std::int64_t(previous->second)+1==i[0]){previous->second=i[0];
                if(next!=runs.end() && std::int64_t(i[0])+1==next->first){previous->second=next->second;runs.erase(next);}++m_count;return true;}
        }
        if(next!=runs.end() && std::int64_t(i[0])+1==next->first)next->first=i[0];
        else runs.insert(next,{i[0],i[0]});++m_count;return true;
    }
    bool erase(const Index& i){
        const auto row=m_rows.find({i[1],i[2]});if(row==m_rows.end())return false;auto& runs=row->second;
        auto next=std::upper_bound(runs.begin(),runs.end(),i[0],[](std::int64_t x,const auto& run){return x<run.first;});
        if(next==runs.begin())return false;auto run=std::prev(next);if(run->second<i[0])return false;
        if(run->first==run->second)runs.erase(run);
        else if(run->first==i[0])++run->first;
        else if(run->second==i[0])--run->second;
        else{const auto high=run->second;run->second=i[0]-1;runs.insert(next,{i[0]+1,high});}
        if(runs.empty())m_rows.erase(row);--m_count;return true;
    }
    template<class Predicate> void Keep(Predicate predicate){
        Cells kept;for(const auto& i:*this)if(predicate(i))kept.insert(i);*this=std::move(kept);
    }
private:Rows m_rows;std::size_t m_count=0;
};
struct Failure final { RoiError error; const char* message; };
void Require(bool condition,RoiError error,const char* message) {if (!condition) throw Failure{error,message};}
void CheckCancel(const RoiOperationRequest& request) {
    if(std::chrono::steady_clock::now()>=request.deadline)throw Failure{RoiError::DeadlineExceeded,"ROI operation deadline exceeded."};
    try {if (request.getCancelled && request.getCancelled()) throw Failure{RoiError::Cancelled,"ROI operation cancelled."};}
    catch (const Failure&) {throw;} catch (...) {throw Failure{RoiError::Cancelled,"ROI cancellation callback failed."};}
}
bool Insert(Cells& cells,const Index& index,const RoiOperationRequest&){return cells.insert(index);}
Point GetPoint(const GridGeometry3D& g,const Index& i) {
    Point p=g.origin;for (int r=0;r<3;++r) for (int a=0;a<3;++a) p[r]+=g.direction[r*3+a]*g.spacing[a]*i[a];return p;
}
Point Transform(const Matrix& m,const Point& p) {
    return {m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3],m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11]};
}
Matrix Invert(const Matrix& m) {
    Require(std::all_of(m.begin(),m.end(),[](double x){return std::isfinite(x);}) && m[12]==0 && m[13]==0 && m[14]==0 && m[15]==1,RoiError::InvalidGeometry,"ROI transform must be affine.");
    Matrix result=roiIdentityMatrix; double a[3][6]{};
    for (int r=0;r<3;++r) {for (int c=0;c<3;++c) a[r][c]=m[r*4+c];a[r][r+3]=1;}
    for (int c=0;c<3;++c) {
        int pivot=c;for (int r=c+1;r<3;++r) if (std::abs(a[r][c])>std::abs(a[pivot][c])) pivot=r;
        Require(std::abs(a[pivot][c])>std::numeric_limits<double>::min(),RoiError::InvalidGeometry,"ROI transform is singular.");
        for (int j=0;j<6;++j) std::swap(a[pivot][j],a[c][j]);
        const double d=a[c][c];for (int j=0;j<6;++j) a[c][j]/=d;
        for (int r=0;r<3;++r) if (r!=c) {const double f=a[r][c];for (int j=0;j<6;++j) a[r][j]-=f*a[c][j];}
    }
    for (int r=0;r<3;++r) {for (int c=0;c<3;++c) result[r*4+c]=a[r][c+3];result[r*4+3]=-(result[r*4]*m[3]+result[r*4+1]*m[7]+result[r*4+2]*m[11]);}
    return result;
}
Matrix Multiply(const Matrix& a,const Matrix& b){Matrix result{};for(int r=0;r<4;++r)for(int c=0;c<4;++c)for(int k=0;k<4;++k)result[r*4+c]+=a[r*4+k]*b[k*4+c];return result;}
RoiDefinition TransformAnalytic(const RoiDefinition& definition,const ImageGrid3DPayload& image,DataRevisionRef target,const Matrix& matrix)
{
    if(target!=definition.source && image.GetValidityMask())Require(std::all_of(image.GetValidityMask()->begin(),image.GetValidityMask()->end(),[](auto value){return value!=0;}),RoiError::UnsupportedRoi,"Copying a non-rectangular source domain requires explicit resampling.");
    const auto inverse=Invert(matrix);const auto& grid=image.GetGeometry();
    Matrix index=roiIdentityMatrix;for(int r=0;r<3;++r){for(int a=0;a<3;++a)index[r*4+a]=grid.direction[r*3+a]*grid.spacing[a];index[r*4+3]=grid.origin[r];}
    const auto toIndex=Invert(index);RoiDefinition domain{definition.source,{}};
    for(int a=0;a<3;++a)for(int side=0;side<2;++side){
        RoiNode node;node.primitive.shape=RoiShape::HalfSpace;Point p{};p[a]=grid.extent[a*2+side];node.primitive.origin=Transform(index,p);
        for(int r=0;r<3;++r)node.primitive.normal[r]=(side?-1.:1.)*toIndex[a*4+r];
        const auto left=static_cast<std::uint32_t>(domain.nodes.size()-1);domain.nodes.push_back(node);
        if(domain.nodes.size()>1)domain.nodes.push_back({RoiNodeKind::Intersection,{},left,static_cast<std::uint32_t>(domain.nodes.size()-1)});
    }
    RoiDefinition transformed{target,{}};std::vector<std::uint32_t> mapping;
    const auto append=[&](const RoiDefinition& d){const auto base=static_cast<std::uint32_t>(transformed.nodes.size());for(auto node:d.nodes){if(node.kind==RoiNodeKind::Union || node.kind==RoiNodeKind::Intersection || node.kind==RoiNodeKind::Difference){node.left+=base;node.right+=base;}transformed.nodes.push_back(std::move(node));}return static_cast<std::uint32_t>(transformed.nodes.size()-1);};
    for(auto node:definition.nodes){
        if(node.kind==RoiNodeKind::SourceDomain && target!=definition.source){mapping.push_back(append(domain));continue;}
        if(node.kind==RoiNodeKind::Union || node.kind==RoiNodeKind::Intersection || node.kind==RoiNodeKind::Difference){node.left=mapping[node.left];node.right=mapping[node.right];}
        transformed.nodes.push_back(std::move(node));mapping.push_back(static_cast<std::uint32_t>(transformed.nodes.size()-1));
    }
    if(target!=definition.source){const auto root=static_cast<std::uint32_t>(transformed.nodes.size()-1),domainRoot=append(domain);transformed.nodes.push_back({RoiNodeKind::Intersection,{},root,domainRoot});}
    double scale=0;for(int r=0;r<3;++r)scale+=matrix[r*4]*matrix[r*4];scale=std::sqrt(scale);bool uniform=scale>0;
    for(int a=0;a<3;++a)for(int b=0;b<3;++b){double dot=0;for(int r=0;r<3;++r)dot+=matrix[r*4+a]*matrix[r*4+b];if(std::abs(dot-(a==b?scale*scale:0))>1e-9*std::max(1.,scale*scale))uniform=false;}
    for(auto& node:transformed.nodes)if(node.kind==RoiNodeKind::Primitive){auto& p=node.primitive;
        Require(p.shape!=RoiShape::MaskReference && p.shape!=RoiShape::VoxelSet,RoiError::UnsupportedRoi,"Voxel ROI transforms require explicit resampling.");
        if(p.shape==RoiShape::Sphere){Matrix sphere=roiIdentityMatrix;for(int a=0;a<3;++a){sphere[a*4+a]=p.radius;sphere[a*4+3]=p.origin[a];}p={};p.shape=RoiShape::Ellipsoid;p.localToSource=Multiply(matrix,sphere);}
        else if(p.shape==RoiShape::Box || p.shape==RoiShape::Ellipsoid || p.shape==RoiShape::EllipticCylinder || p.shape==RoiShape::Contour)p.localToSource=Multiply(matrix,p.localToSource);
        else if(p.shape==RoiShape::HalfSpace){const auto normal=p.normal;p.origin=Transform(matrix,p.origin);for(int r=0;r<3;++r)p.normal[r]=inverse[r]*normal[0]+inverse[4+r]*normal[1]+inverse[8+r]*normal[2];}
        else if(p.shape==RoiShape::ProjectedContour){const double before=std::hypot(p.sourceToView[8],p.sourceToView[9],p.sourceToView[10]);p.sourceToView=Multiply(p.sourceToView,inverse);const double after=std::hypot(p.sourceToView[8],p.sourceToView[9],p.sourceToView[10]);p.depth[0]*=before/after;p.depth[1]*=before/after;}
        else if(p.shape==RoiShape::Cylinder || p.shape==RoiShape::Brush){Require(uniform,RoiError::UnsupportedRoi,"Non-uniform cylinder/brush transforms require explicit resampling.");
            p.radius*=scale;if(p.shape==RoiShape::Cylinder){p.height*=scale;p.origin=Transform(matrix,p.origin);const auto normal=p.normal;for(int r=0;r<3;++r)p.normal[r]=matrix[r*4]*normal[0]+matrix[r*4+1]*normal[1]+matrix[r*4+2]*normal[2];}
            else{for(auto& point:p.path)point=Transform(matrix,point);if(p.height>0){p.localToSource=Multiply(matrix,p.localToSource);for(int r=0;r<3;++r)for(int a=0;a<3;++a)p.localToSource[r*4+a]/=scale;p.depth[0]*=scale;p.depth[1]*=scale;}}
        }
    }
    return transformed;
}
double GetVoxelVolume(const GridGeometry3D& g) {
    const auto& d=g.direction;
    return std::abs(d[0]*(d[4]*d[8]-d[5]*d[7])-d[1]*(d[3]*d[8]-d[5]*d[6])+d[2]*(d[3]*d[7]-d[4]*d[6]))*g.spacing[0]*g.spacing[1]*g.spacing[2];
}
bool GetInGrid(const GridGeometry3D& g,const Index& i) {
    for (int a=0;a<3;++a) if (i[a]<g.extent[a*2] || i[a]>g.extent[a*2+1]) return false;return true;
}
std::size_t GetOffset(const GridGeometry3D& g,const Index& i) {
    return static_cast<std::size_t>(i[0]-g.extent[0])+static_cast<std::size_t>(g.dimensions[0])*(static_cast<std::size_t>(i[1]-g.extent[2])+static_cast<std::size_t>(g.dimensions[1])*static_cast<std::size_t>(i[2]-g.extent[4]));
}
bool GetValidCell(const ImageGrid3DPayload& image,const Index& i) {
    return GetInGrid(image.GetGeometry(),i) && (!image.GetValidityMask() || (*image.GetValidityMask())[GetOffset(image.GetGeometry(),i)]!=0);
}
std::optional<std::array<int,6>> GetCellExtent(const GridGeometry3D& g,const std::optional<std::array<double,6>>& bounds) {
    auto extent=g.extent;
    if(bounds){
        for(int a=0;a<3;++a)if((*bounds)[a*2]>(*bounds)[a*2+1])return {};
        Matrix matrix=roiIdentityMatrix;for(int r=0;r<3;++r){for(int a=0;a<3;++a)matrix[r*4+a]=g.direction[r*3+a]*g.spacing[a];matrix[r*4+3]=g.origin[r];}
        const auto inverse=Invert(matrix);std::array<double,6> indexed{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        for(unsigned k=0;k<8;++k){const auto p=Transform(inverse,{(*bounds)[(k&1)?1:0],(*bounds)[(k&2)?3:2],(*bounds)[(k&4)?5:4]});for(int a=0;a<3;++a){indexed[a*2]=std::min(indexed[a*2],p[a]);indexed[a*2+1]=std::max(indexed[a*2+1],p[a]);}}
        for(int a=0;a<3;++a){const double low=std::max(double(extent[a*2]),std::ceil(indexed[a*2]-1e-9)),high=std::min(double(extent[a*2+1]),std::floor(indexed[a*2+1]+1e-9));if(low>high)return {};extent[a*2]=static_cast<int>(low);extent[a*2+1]=static_cast<int>(high);}
    }
    return extent;
}
template<class F> void ForCells(const ImageGrid3DPayload& image,const RoiOperationRequest& request,F visit,
    std::optional<std::array<double,6>> bounds={}) {
    const auto range=GetCellExtent(image.GetGeometry(),bounds);if(!range)return;const auto& extent=*range;
    for (std::int64_t z=extent[4];z<=extent[5];++z) for (std::int64_t y=extent[2];y<=extent[3];++y) {
        CheckCancel(request);
        for (std::int64_t x=extent[0];x<=extent[1];++x) {if(!((x-extent[0])%1024))CheckCancel(request);Index i{static_cast<int>(x),static_cast<int>(y),static_cast<int>(z)};if (GetValidCell(image,i)) visit(i);}
    }
}
RoiReadSnapshot GetRead(const DataGraphSnapshot& graph,const DataRevisionRef& ref,const DataRevisionRef& source) {
    auto read=RoiEvaluator::GetRoi(graph,ref,source);Require(read.error==RoiError::None && read.roi,read.error,"ROI input cannot be read.");return read.roi;
}
RoiDefinition GetDefinition(const DataGraphSnapshot& graph,const DataRevisionRef& ref,const DataRevisionRef& source) {
    return GetRead(graph,ref,source)->GetDefinition();
}
RoiDefinition Combine(RoiDefinition a,const RoiDefinition& b,RoiNodeKind kind) {
    Require(a.source==b.source,RoiError::SourceMismatch,"ROI operands use different sources.");
    Require(!a.nodes.empty() && !b.nodes.empty() && a.nodes.size()+b.nodes.size()<roiNodeLimit,RoiError::TooLarge,"ROI node indices overflow.");
    const auto left=static_cast<std::uint32_t>(a.nodes.size()-1),base=static_cast<std::uint32_t>(a.nodes.size());
    for (auto node:b.nodes) {
        if (node.kind==RoiNodeKind::Union || node.kind==RoiNodeKind::Intersection || node.kind==RoiNodeKind::Difference) {node.left+=base;node.right+=base;}
        a.nodes.push_back(std::move(node));
    }
    a.nodes.push_back({kind,{},left,static_cast<std::uint32_t>(a.nodes.size()-1)});return a;
}
RoiDefinition GetVoxelDefinition(DataRevisionRef source,std::shared_ptr<const RoiVoxelSet> voxels,std::vector<std::uint64_t> labels={}) {
    RoiNode node;node.primitive.shape=RoiShape::VoxelSet;node.primitive.voxels=std::move(voxels);node.primitive.labels=std::move(labels);return {source,{std::move(node)}};
}
RoiVoxelRun GetVoxelRun(const Cells::Span& span,std::uint32_t label) {
    for(const auto value:span.first)Require(value>=INT_MIN && value<=INT_MAX,RoiError::TooLarge,"ROI output index overflow.");
    Require(span.last>=span.first[0] && span.last<=INT_MAX,RoiError::TooLarge,"ROI output run overflow.");
    return {{int(span.first[0]),int(span.first[1]),int(span.first[2])},int(span.last),label};
}
RoiDefinition GetVoxelDefinition(DataRevisionRef source,const Cells& cells) {
    std::vector<RoiVoxelRun> values;
    for(const auto& row:cells.GetRows())for(const auto& run:row.second)values.push_back(GetVoxelRun({{run.first,row.first[0],row.first[1]},run.second},1));
    return GetVoxelDefinition(source,std::make_shared<const RoiVoxelSet>(std::move(values)));
}
Cells GetCells(const ImageGrid3DPayload& image,const RoiReadSnapshot& read,const RoiOperationRequest& request) {
    const auto query=read->GetQuery();Require(query.error==RoiError::None && query.query,query.error,"ROI query preparation failed.");
    Cells cells;ForCells(image,request,[&](const Index& i){if (query.query->GetContains(GetPoint(image.GetGeometry(),i))) Insert(cells,i,request);},read->GetBounds());return cells;
}
std::vector<Index> GetNeighbours(RoiConnectivity connectivity) {
    Require(connectivity==RoiConnectivity::Faces || connectivity==RoiConnectivity::Full,RoiError::InvalidRequest,"Invalid ROI connectivity.");
    std::vector<Index> offsets;for (int z=-1;z<=1;++z) for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
        if ((x||y||z) && (connectivity==RoiConnectivity::Full || std::abs(x)+std::abs(y)+std::abs(z)==1)) offsets.push_back({x,y,z});return offsets;
}
Index Add(const Index& a,const Index& b) {
    // 输入 int 网格加最多两步 int 半径，临时索引始终在 int64_t 内。
    return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};
}
std::vector<Cells::Span> GetKernel(const GridGeometry3D& grid,const RoiMorphologyParams& params,const RoiOperationRequest& request) {
    Require(std::isfinite(params.radius) && params.radius>=0,RoiError::InvalidRequest,"ROI radius must be nonnegative.");
    Require(params.unit==RoiRadiusUnit::Millimetres || params.unit==RoiRadiusUnit::Voxels,RoiError::InvalidRequest,"Invalid ROI radius unit.");
    Matrix j=roiIdentityMatrix;for (int r=0;r<3;++r) for (int c=0;c<3;++c) j[r*4+c]=params.unit==RoiRadiusUnit::Voxels ? (r==c ? 1.0:0.0):grid.direction[r*3+c]*grid.spacing[c];
    const auto inverse=Invert(j);Index halo{};
    for (int a=0;a<3;++a) {const double size=std::ceil(params.radius*std::hypot(inverse[a*4],inverse[a*4+1],inverse[a*4+2]));Require(std::isfinite(size) && size<INT_MAX,RoiError::TooLarge,"ROI neighbourhood exceeds index range.");halo[a]=static_cast<int>(size);}
    std::vector<Cells::Span> kernel;
    // 固定 Y/Z 后，物理球沿 X 的交集是一个区间；在离散最小值两侧二分，完全沿用 hypot 包含规则。
    const long double a=j[0]*j[0]+j[4]*j[4]+j[8]*j[8];
    for(std::int64_t z=-halo[2];z<=halo[2];++z)for(std::int64_t y=-halo[1];y<=halo[1];++y) {
        CheckCancel(request);const auto p=Transform(j,{0.,double(y),double(z)});
        const long double b=j[0]*p[0]+j[4]*p[1]+j[8]*p[2];
        const auto inside=[&](std::int64_t x){const auto q=Transform(j,{double(x),double(y),double(z)});return std::hypot(q[0],q[1],q[2])<=params.radius;};
        const double center=static_cast<double>(-b/a);
        if(!std::isfinite(center)) {
            // 极端尺度使二次系数溢出/下溢时，按原度量扫描这一行，仍只保存连续区间。
            std::optional<int> first;
            for(std::int64_t x=-halo[0];x<=halo[0];++x) {
                if(!((x+halo[0])%1024))CheckCancel(request);
                if(inside(x)){if(!first)first=static_cast<int>(x);}
                else if(first){kernel.push_back({{*first,y,z},static_cast<int>(x-1)});first.reset();}
            }
            if(first)kernel.push_back({{*first,y,z},halo[0]});
            continue;
        }
        auto seed=static_cast<std::int64_t>(std::floor(std::clamp(center,-double(halo[0]),double(halo[0]))));
        if(!inside(seed)){if(seed==halo[0]||!inside(seed+1))continue;++seed;}
        std::int64_t low=-halo[0],high=seed;
        while(low<high){const auto mid=low+(high-low)/2;if(inside(mid))high=mid;else low=mid+1;}const auto first=low;
        low=seed;high=halo[0];
        while(low<high){const auto mid=low+(high-low+1)/2;if(inside(mid))low=mid;else high=mid-1;}
        kernel.push_back({{static_cast<int>(first),y,z},static_cast<int>(low)});
    }
    return kernel;
}
Cells Morph(const Cells& cells,const std::vector<Cells::Span>& kernel,bool dilate,const RoiOperationRequest& request) {
    Cells out;std::size_t visited=0;
    // 核仍按原物理度量生成；每个 X 区间整体平移/求交，不展开成员与核的笛卡尔积。
    if(dilate){for(const auto& row:cells.GetRows())for(const auto& run:row.second)for(const auto& d:kernel){
        if(!(visited++%1024))CheckCancel(request);
        const auto low=Add({run.first,row.first[0],row.first[1]},d.first),high=Add({run.second,row.first[0],row.first[1]},{d.last,d.first[1],d.first[2]});
        out.InsertRun(low,high[0]);}return out;}
    out=cells;
    for(const auto& d:kernel){
        if(out.empty())break;Cells intersection;
        for(const auto& row:out.GetRows())for(const auto& run:row.second){
            if(!(visited++%1024))CheckCancel(request);
            const auto low=Add({run.first,row.first[0],row.first[1]},d.first),high=Add({run.second,row.first[0],row.first[1]},{d.last,d.first[1],d.first[2]});
            const auto& shifted=cells.GetRow({low[1],low[2]});
            auto other=std::lower_bound(shifted.begin(),shifted.end(),low[0],[](const auto& r,std::int64_t x){return r.second<x;});
            for(;other!=shifted.end() && other->first<=high[0];++other){
                if(!(visited++%1024))CheckCancel(request);
                const auto first=std::max(std::int64_t(run.first),std::int64_t(other->first)-d.first[0]);
                const auto last=std::min(std::int64_t(run.second),std::int64_t(other->second)-d.last);
                if(first<=last)intersection.InsertRun({first,row.first[0],row.first[1]},last);
            }
        }out=std::move(intersection);
    }return out;
}
struct Component final {Index first;Cells cells;};
std::vector<Component> GetComponents(Cells remaining,RoiConnectivity connectivity,const RoiOperationRequest& request) {
    Require(connectivity==RoiConnectivity::Faces || connectivity==RoiConnectivity::Full,RoiError::InvalidRequest,"Invalid ROI connectivity.");
    std::vector<Component> components;std::size_t visited=0;
    // 任取剩余行，提取时记录分量最小坐标；最后统一排序，避免每个孤岛扫描所有行。
    while(!remaining.empty()){
        CheckCancel(request);const auto seed=*remaining.begin();Component component{seed,{}};std::deque<Cells::Span> frontier;
        const auto append=[&](Cells::Span span){component.first=std::min(component.first,span.first);component.cells.InsertRun(span.first,span.last);frontier.push_back(span);};
        append(*remaining.TakeRun({seed[1],seed[2]},seed[0],seed[0]));
        while(!frontier.empty()){
            if(!(visited++%1024))CheckCancel(request);const auto span=frontier.front();frontier.pop_front();
            for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y){
                if((!y && !z) || (connectivity==RoiConnectivity::Faces && std::abs(y)+std::abs(z)!=1))continue;
                const auto next=Add(span.first,{0,y,z});const int margin=connectivity==RoiConnectivity::Full?1:0;
                while(const auto joined=remaining.TakeRun({next[1],next[2]},std::int64_t(span.first[0])-margin,std::int64_t(span.last)+margin))append(*joined);
            }
        }components.push_back(std::move(component));
    }
    std::sort(components.begin(),components.end(),[&](const auto& a,const auto& b){if(!(visited++%1024))CheckCancel(request);return a.first<b.first;});
    CheckCancel(request);return components;
}
template<class T> double ReadValue(const std::uint8_t* bytes) {T value;std::memcpy(&value,bytes,sizeof(T));return static_cast<double>(value);}
double GetGray(const ImageGrid3DPayload& image,const Index& i) {
    const auto* bytes=image.GetValues()->data()+GetOffset(image.GetGeometry(),i)*GetImageValueBytes(image.GetValueType());
    switch (image.GetValueType()) {
    case ImageValueType::Int8:return ReadValue<std::int8_t>(bytes);case ImageValueType::UInt8:return ReadValue<std::uint8_t>(bytes);
    case ImageValueType::Int16:return ReadValue<std::int16_t>(bytes);case ImageValueType::UInt16:return ReadValue<std::uint16_t>(bytes);
    case ImageValueType::Int32:return ReadValue<std::int32_t>(bytes);case ImageValueType::UInt32:return ReadValue<std::uint32_t>(bytes);
    case ImageValueType::Int64:return ReadValue<std::int64_t>(bytes);case ImageValueType::UInt64:return ReadValue<std::uint64_t>(bytes);
    case ImageValueType::Float32:return ReadValue<float>(bytes);case ImageValueType::Float64:return ReadValue<double>(bytes);
    default:throw Failure{RoiError::UnsupportedRoi,"Unsupported ROI gray scalar type."};
    }
}
Point Subtract(Point a,const Point& b) {for (int i=0;i<3;++i) a[i]-=b[i];return a;}
double Dot(const Point& a,const Point& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Point Cross(const Point& a,const Point& b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double GetTriangleDistance(const Point& p,const Point& a,const Point& b,const Point& c) {
    // 最近点保留在原三角形内，退化面片按三条边处理。
    const auto ab=Subtract(b,a),ac=Subtract(c,a),ap=Subtract(p,a);const auto normal=Cross(ab,ac);
    const double area=Dot(normal,normal),d1=Dot(ab,ap),d2=Dot(ac,ap);
    if (area>0) {
        const double v=(Dot(ac,ac)*d1-Dot(ab,ac)*d2)/area,w=(Dot(ab,ab)*d2-Dot(ab,ac)*d1)/area;
        if (v>=0 && w>=0 && v+w<=1) return std::abs(Dot(ap,normal))/std::sqrt(area);
    }
    double result=INFINITY;for (const auto& edge:std::array<std::array<Point,2>,3>{{{a,b},{b,c},{c,a}}}) {
        const auto d=Subtract(edge[1],edge[0]),q=Subtract(p,edge[0]);const double t=Dot(d,d)>0 ? std::clamp(Dot(q,d)/Dot(d,d),0.0,1.0):0;
        const Point r{q[0]-t*d[0],q[1]-t*d[1],q[2]-t*d[2]};result=std::min(result,std::sqrt(Dot(r,r)));
    }return result;
}
} // namespace

class SurfaceMeshPayload::RoiMeshIndex final {
    struct Node final {std::array<double,6> bounds;std::size_t first=0,count=0,left=0,right=0;};
public:
    static std::shared_ptr<const RoiMeshIndex> Get(const DataSnapshot& data,const RoiMeshParams& params,const RoiOperationRequest& request){
        const auto& mesh=dynamic_cast<const SurfaceMeshPayload&>(*data->payload);
        const auto& owner=*mesh.m_mesh;
        {const std::lock_guard<std::mutex> lock(owner.roiIndexMutex);
            if(owner.roiIndex && owner.roiIndexTransform==params.meshToSource)return owner.roiIndex;}
        auto prepared=std::make_shared<const RoiMeshIndex>(mesh,params.meshToSource,request);
        {const std::lock_guard<std::mutex> lock(owner.roiIndexMutex);owner.roiIndexTransform=params.meshToSource;owner.roiIndex=prepared;}
        return prepared;
    }
    RoiMeshIndex(const SurfaceMeshPayload& mesh,const Matrix& matrix,const RoiOperationRequest& request)
        :m_triangles(mesh.GetTriangles()){
        m_bounds={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        m_points.reserve(mesh.GetVertices().size()/3);
        for(std::size_t i=0;i<mesh.GetVertices().size();i+=3){if(i%768==0)CheckCancel(request);
            const auto p=Transform(matrix,{mesh.GetVertices()[i],mesh.GetVertices()[i+1],mesh.GetVertices()[i+2]});
            Require(std::all_of(p.begin(),p.end(),[](double value){return std::isfinite(value);}),RoiError::InvalidGeometry,"ROI transformed mesh is not finite.");
            m_points.push_back(p);for(int a=0;a<3;++a){m_bounds[a*2]=std::min(m_bounds[a*2],p[a]);m_bounds[a*2+1]=std::max(m_bounds[a*2+1],p[a]);}}
        const auto& triangles=mesh.GetTriangles();std::map<std::pair<std::uint64_t,std::uint64_t>,std::size_t> edges;
        for(std::size_t i=0;i<triangles.size();i+=3){if(i%768==0)CheckCancel(request);
            const auto normal=Cross(Subtract(m_points[triangles[i+1]],m_points[triangles[i]]),Subtract(m_points[triangles[i+2]],m_points[triangles[i]]));
            Require(Dot(normal,normal)>0 && std::isfinite(Dot(normal,normal)),RoiError::InvalidGeometry,"ROI mesh contains a degenerate face.");
            for(int e=0;e<3;++e){auto a=triangles[i+e],b=triangles[i+(e+1)%3];if(a>b)std::swap(a,b);++edges[{a,b}];}}
        m_closed=!triangles.empty() && std::all_of(edges.begin(),edges.end(),[](const auto& e){return e.second==2;});
        m_order.resize(triangles.size()/3);std::iota(m_order.begin(),m_order.end(),0);
        if(!m_order.empty())Build(0,m_order.size(),request);
    }
    const std::array<double,6>& GetBounds()const noexcept{return m_bounds;}
    bool GetClosed()const noexcept{return m_closed;}
    double GetDistance(const Point& point,const RoiOperationRequest& request)const{double distance=INFINITY;if(!m_nodes.empty())Distance(0,point,distance,request);return distance;}
    void GetHits(const Point& point,const Point& ray,std::vector<double>& hits,const RoiOperationRequest& request)const{hits.clear();if(!m_nodes.empty())Hits(0,point,ray,hits,request);}
private:
    std::array<Point,3> Triangle(std::size_t id)const{return {m_points[m_triangles[id*3]],m_points[m_triangles[id*3+1]],m_points[m_triangles[id*3+2]]};}
    std::size_t Build(std::size_t first,std::size_t count,const RoiOperationRequest& request){
        CheckCancel(request);Node node;node.bounds={INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};node.first=first;node.count=count;
        for(std::size_t k=first;k<first+count;++k){if(k%1024==0)CheckCancel(request);for(const auto& p:Triangle(m_order[k]))for(int a=0;a<3;++a){node.bounds[a*2]=std::min(node.bounds[a*2],p[a]);node.bounds[a*2+1]=std::max(node.bounds[a*2+1],p[a]);}}
        const auto id=m_nodes.size();m_nodes.push_back(node);
        if(count>8){int axis=0;for(int a=1;a<3;++a)if(node.bounds[a*2+1]-node.bounds[a*2]>node.bounds[axis*2+1]-node.bounds[axis*2])axis=a;
            const auto middle=first+count/2;std::nth_element(m_order.begin()+first,m_order.begin()+middle,m_order.begin()+first+count,[&](auto a,auto b){const auto x=Triangle(a),y=Triangle(b);return x[0][axis]/3+x[1][axis]/3+x[2][axis]/3<y[0][axis]/3+y[1][axis]/3+y[2][axis]/3;});
            const auto left=Build(first,middle-first,request),right=Build(middle,first+count-middle,request);m_nodes[id].left=left;m_nodes[id].right=right;m_nodes[id].count=0;}
        return id;
    }
    static double BoxDistance(const std::array<double,6>& box,const Point& p){double sum=0;for(int a=0;a<3;++a){const double delta=std::max({box[a*2]-p[a],0.,p[a]-box[a*2+1]});sum+=delta*delta;}return std::sqrt(sum);}
    void Distance(std::size_t id,const Point& p,double& distance,const RoiOperationRequest& request)const{
        const auto& node=m_nodes[id];if(BoxDistance(node.bounds,p)>distance)return;CheckCancel(request);
        if(node.count){for(std::size_t k=node.first;k<node.first+node.count;++k){const auto t=Triangle(m_order[k]);distance=std::min(distance,GetTriangleDistance(p,t[0],t[1],t[2]));}}
        else{auto first=node.left,second=node.right;if(BoxDistance(m_nodes[first].bounds,p)>BoxDistance(m_nodes[second].bounds,p))std::swap(first,second);Distance(first,p,distance,request);Distance(second,p,distance,request);}
    }
    void Hits(std::size_t id,const Point& p,const Point& ray,std::vector<double>& hits,const RoiOperationRequest& request)const{
        const auto& node=m_nodes[id];double low=0,high=INFINITY;
        for(int a=0;a<3;++a){const double pad=64*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(node.bounds[a*2]),std::abs(node.bounds[a*2+1])});
            double near=(node.bounds[a*2]-pad-p[a])/ray[a],far=(node.bounds[a*2+1]+pad-p[a])/ray[a];if(near>far)std::swap(near,far);low=std::max(low,near);high=std::min(high,far);if(low>high)return;}
        CheckCancel(request);
        if(!node.count){Hits(node.left,p,ray,hits,request);Hits(node.right,p,ray,hits,request);return;}
        for(std::size_t k=node.first;k<node.first+node.count;++k){const auto triangle=Triangle(m_order[k]);const auto& a=triangle[0];const auto ab=Subtract(triangle[1],a),ac=Subtract(triangle[2],a),h=Cross(ray,ac);const double det=Dot(ab,h);
            if(std::abs(det)<=1e-14)continue;const auto q=Subtract(p,a);const double u=Dot(q,h)/det;if(u<0 || u>1)continue;
            const auto cross=Cross(q,ab);const double v=Dot(ray,cross)/det,t=Dot(ac,cross)/det;if(v>=0 && u+v<=1 && t>0)hits.push_back(t);}
    }
    const std::vector<std::uint64_t>& m_triangles;
    std::vector<Point> m_points;std::vector<std::size_t> m_order;std::vector<Node> m_nodes;std::array<double,6> m_bounds;bool m_closed=false;
};

namespace {
std::string RevisionText(const DataRevisionRef& ref){std::ostringstream text;text<<std::hex<<std::setfill('0');for(auto byte:ref.entityId.bytes)text<<std::setw(2)<<unsigned(byte);text<<':'<<std::dec<<ref.generation;return text.str();}
DataProvenance GetOperationProvenance(const RoiOperationRequest& request)
{
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(17);
    text<<"source="<<RevisionText(request.source)<<";target="<<(request.target?RevisionText(*request.target):"new")<<";output="<<int(request.output)<<";";
    std::string operation;const auto values=[&](const auto& list){for(const auto& value:list)text<<value<<',';};
    std::visit([&](const auto& p){using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,RoiBooleanParams>){operation="boolean";text<<int(p.operation);for(const auto& operand:p.operands)text<<';'<<RevisionText(operand);}
        else if constexpr(std::is_same_v<T,RoiMorphologyParams>){operation="morphology";text<<int(p.operation)<<';'<<p.radius<<';'<<int(p.unit);}
        else if constexpr(std::is_same_v<T,RoiComponentParams>){operation="components";text<<int(p.operation)<<';'<<int(p.connectivity)<<';'<<p.minimumVolume<<';'<<p.maximumHoleVolume<<';'<<p.keepInput<<';';values(p.labels);}
        else if constexpr(std::is_same_v<T,RoiThresholdParams>){operation=p.mode==RoiThresholdMode::SeedGrow?"seed-grow":"threshold";values(p.range);text<<';'<<int(p.connectivity)<<';';for(const auto& seed:p.seeds){values(seed);text<<';';}}
        else if constexpr(std::is_same_v<T,RoiInvertParams>){operation="invert";text<<(p.domain?RevisionText(*p.domain):"source-domain");}
        else if constexpr(std::is_same_v<T,RoiClearParams>)operation="clear";
        else if constexpr(std::is_same_v<T,RoiTransformParams>){operation=p.rasterize?"transform-resample":"transform-analytic";values(p.sourceToTarget);text<<';'<<(p.targetSource?RevisionText(*p.targetSource):"same-source");}
        else {operation="mesh-selection";text<<int(p.mode)<<';'<<p.inward<<';'<<p.outward<<';'<<p.hasOrientation<<';';values(p.meshToSource);text<<';'<<RevisionText(p.mesh);}
    },request.params);
    return {"Host.Roi",operation,"3",text.str()};
}
std::uint64_t HashDefinition(const RoiDefinition& d){
    std::uint64_t h=1469598103934665603ULL;
    const auto hash=[&](const auto& value){const auto* bytes=reinterpret_cast<const unsigned char*>(&value);for(std::size_t i=0;i<sizeof(value);++i){h^=bytes[i];h*=1099511628211ULL;}};
    for(auto byte:d.source.entityId.bytes)hash(byte);hash(d.source.generation);
    for(const auto& n:d.nodes){hash(n.kind);hash(n.left);hash(n.right);const auto& p=n.primitive;hash(p.shape);hash(p.boundaryPolicy);hash(p.radius);hash(p.height);hash(p.depth);hash(p.origin);hash(p.normal);hash(p.localToSource);hash(p.sourceToView);hash(p.viewToClip);
        if(p.mask){for(auto byte:p.mask->entityId.bytes)hash(byte);hash(p.mask->generation);}hash(p.labels.size());for(auto label:p.labels)hash(label);
        hash(p.contours.size());for(const auto& ring:p.contours){hash(ring.size());for(const auto& point:ring)hash(point);}hash(p.path.size());for(const auto& point:p.path)hash(point);
        const auto identity=reinterpret_cast<std::uintptr_t>(p.voxels.get());hash(identity);
    }return h;
}
std::string PreparedSignature(const RoiOperationRequest& request,const RoiOperationResult& result){
    // 输出身份不参与几何计算；输入、算法参数和候选几何仍全部绑定。
    auto geometryRequest=request;geometryRequest.output=RoiOutputMode::CreateNew;
    const auto provenance=GetOperationProvenance(geometryRequest);std::ostringstream text;text<<provenance.operationId<<';'<<provenance.canonicalParameters<<';';
    if(request.input)text<<HashDefinition(*request.input);text<<';';
    for(const auto& roi:result.rois)text<<HashDefinition(roi.definition)<<',';text<<';';for(auto count:result.componentVoxelCounts)text<<count<<',';
    return text.str();
}

}
RoiOperationResult RoiService::GetOperation(const DataGraphSnapshot& graph,const RoiOperationRequest& requested)
{
    RoiOperationResult result;
    try {
        const auto& request=requested;
        CheckCancel(request);Require(graph.view!=nullptr,RoiError::Unavailable,"ROI graph is unavailable.");
        const auto source=graph.view->GetData(request.source);Require(source!=nullptr,RoiError::MissingInput,"ROI source is missing.");
        const auto* image=dynamic_cast<const ImageGrid3DPayload*>(source->payload.get());
        RoiReadSnapshot target;if (request.target) target=GetRead(graph,*request.target,request.source);
        if (request.input) {
            Require(request.input->source==request.source,RoiError::SourceMismatch,"ROI preview uses a different source.");
            auto preview=RoiEvaluator::GetPreview(graph,*request.input);
            Require(preview.error==RoiError::None && preview.roi,preview.error,"ROI preview cannot be read.");target=std::move(preview.roi);
        }
        const auto targetQuery=target?target->GetQuery():RoiQueryResult{};
        if(target)Require(targetQuery.error==RoiError::None && targetQuery.query,targetQuery.error,"ROI query preparation failed.");
        RoiMetadata metadata=request.metadata;
        if (metadata.name.empty() && request.target) {const auto descriptor=GetDescriptor(graph,*request.target);if (descriptor) metadata=descriptor->metadata;}
        if (metadata.name.empty()) metadata.name="ROI";
        auto add=[&](RoiDefinition definition,std::string suffix={}) {
            auto named=metadata;
            if(named.name.size()+suffix.size()>256){
                const auto length=256-suffix.size();auto end=std::min(length,named.name.size());
                while(end && end<named.name.size() && (static_cast<unsigned char>(named.name[end])&0xc0)==0x80)--end;
                named.name.resize(end);
            }
            named.name+=suffix;result.rois.push_back({{}, {},std::move(definition),std::move(named),0});
        };
        std::visit([&](const auto& params) {
            using T=std::decay_t<decltype(params)>;
            if constexpr (std::is_same_v<T,RoiBooleanParams>) {
                Require(!params.operands.empty() || target,RoiError::InvalidRequest,"ROI operands are empty.");
                const auto kind=params.operation==RoiBooleanOperation::Union ? RoiNodeKind::Union:params.operation==RoiBooleanOperation::Intersection ? RoiNodeKind::Intersection:RoiNodeKind::Difference;
                Require(params.operation==RoiBooleanOperation::Union || params.operation==RoiBooleanOperation::Intersection || params.operation==RoiBooleanOperation::Difference,RoiError::InvalidRequest,"Invalid ROI boolean operation.");
                RoiDefinition definition=target ? target->GetDefinition():GetDefinition(graph,params.operands.front(),request.source);
                for (std::size_t i=target ? 0:1;i<params.operands.size();++i) definition=Combine(std::move(definition),GetDefinition(graph,params.operands[i],request.source),kind);
                add(std::move(definition));
            } else if constexpr (std::is_same_v<T,RoiInvertParams>) {
                Require(target!=nullptr,RoiError::MissingInput,"ROI target is required.");
                RoiDefinition domain=params.domain ? GetDefinition(graph,*params.domain,request.source):RoiDefinition{request.source,{{RoiNodeKind::SourceDomain}}};
                add(Combine(std::move(domain),target->GetDefinition(),RoiNodeKind::Difference));
            } else if constexpr (std::is_same_v<T,RoiClearParams>) {
                add({request.source,{{RoiNodeKind::Empty}}});
            } else {
                Require(image && image->GetValid(),RoiError::UnsupportedRoi,"This ROI operation requires a valid image grid.");
                const auto& grid=image->GetGeometry();
                if constexpr (std::is_same_v<T,RoiMorphologyParams>) {
                    Require(target!=nullptr,RoiError::MissingInput,"ROI target is required.");
                    auto cells=GetCells(*image,target,request);const auto kernel=GetKernel(grid,params,request);
                    // 域外非成员延拓；两步中间结果不裁剪，避免源边界及块接缝改变闭合结果。
                    switch (params.operation) {
                    case RoiMorphologyOperation::Dilate:cells=Morph(cells,kernel,true,request);break;
                    case RoiMorphologyOperation::Erode:cells=Morph(cells,kernel,false,request);break;
                    case RoiMorphologyOperation::Open:cells=Morph(Morph(cells,kernel,false,request),kernel,true,request);break;
                    case RoiMorphologyOperation::Close:cells=Morph(Morph(cells,kernel,true,request),kernel,false,request);break;
                    default:throw Failure{RoiError::InvalidRequest,"Invalid ROI morphology operation."};
                    }
                    cells.Keep([&](const Index& i){return GetValidCell(*image,i);});
                    add(GetVoxelDefinition(request.source,cells));
                } else if constexpr (std::is_same_v<T,RoiComponentParams>) {
                    Require(target!=nullptr,RoiError::MissingInput,"ROI target is required.");
                    Require(std::isfinite(params.minimumVolume) && params.minimumVolume>=0 && std::isfinite(params.maximumHoleVolume) && params.maximumHoleVolume>=0,RoiError::InvalidRequest,"Invalid ROI component volume.");
                    auto cells=GetCells(*image,target,request);const auto volume=GetVoxelVolume(grid);
                    Require(volume>0 && std::isfinite(volume),RoiError::InvalidGeometry,"Invalid ROI voxel volume.");
                    if (params.operation==RoiComponentOperation::FillHoles) {
                        std::array<int,6> extent=grid.extent;
                        if(cells.empty()){add(GetVoxelDefinition(request.source,cells));return;}
                        for(int a=0;a<3;++a){std::int64_t low=INT_MAX,high=INT_MIN;for(const auto& i:cells){low=std::min(low,i[a]);high=std::max(high,i[a]);}extent[a*2]=static_cast<int>(std::max(std::int64_t(grid.extent[a*2]),low-1));extent[a*2+1]=static_cast<int>(std::min(std::int64_t(grid.extent[a*2+1]),high+1));}
                        const auto inside=[&](const Index& i){for(int a=0;a<3;++a)if(i[a]<extent[a*2] || i[a]>extent[a*2+1])return false;return true;};
                        Cells background;
                        for(std::int64_t z=extent[4];z<=extent[5];++z)for(std::int64_t y=extent[2];y<=extent[3];++y){CheckCancel(request);for(std::int64_t x=extent[0];x<=extent[1];++x){Index i{int(x),int(y),int(z)};if(GetValidCell(*image,i) && !cells.count(i))Insert(background,i,request);}}
                        const auto components=GetComponents(std::move(background),params.connectivity,request);
                        const auto neighbours=GetNeighbours(params.connectivity);
                        std::size_t visited=0;
                        for (const auto& component:components) {
                            bool exterior=false;for (const auto& i:component.cells) {if(!(visited++%1024))CheckCancel(request);for (const auto& d:neighbours) {const auto next=Add(i,d);if (!inside(next) || !GetValidCell(*image,next)) {exterior=true;break;}}if (exterior) break;}
                            if (!exterior && (!params.maximumHoleVolume || component.cells.size()*volume<=params.maximumHoleVolume)) for(const auto& i:component.cells){if(!(visited++%1024))CheckCancel(request);Insert(cells,i,request);}
                        }add(GetVoxelDefinition(request.source,cells));
                    } else {
                        const auto components=GetComponents(std::move(cells),params.connectivity,request);
                        std::size_t largest=0;for (std::size_t i=0;i<components.size();++i) {result.componentVoxelCounts.push_back(components[i].cells.size());if (components[i].cells.size()>components[largest].cells.size()) largest=i;}
                        std::vector<RoiVoxelRun> values;std::vector<std::uint32_t> selected;
                        Require(components.size()<UINT32_MAX,RoiError::TooLarge,"ROI component labels overflow.");
                        for (std::size_t i=0;i<components.size();++i) {
                            const auto label=static_cast<std::uint32_t>(i+1);
                            bool keep=components[i].cells.size()*volume>=params.minimumVolume;
                            if (params.operation==RoiComponentOperation::KeepLargest) keep=i==largest;
                            else if (params.operation==RoiComponentOperation::KeepSelected) keep=std::find(params.labels.begin(),params.labels.end(),label)!=params.labels.end();
                            else Require(params.operation==RoiComponentOperation::RemoveSmall || params.operation==RoiComponentOperation::Split,RoiError::InvalidRequest,"Invalid ROI component operation.");
                            if (keep) {selected.push_back(label);for(const auto& row:components[i].cells.GetRows())for(const auto& run:row.second)values.push_back(GetVoxelRun({{run.first,row.first[0],row.first[1]},run.second},label));}
                        }
                        auto shared=std::make_shared<const RoiVoxelSet>(std::move(values));
                        if (params.operation==RoiComponentOperation::Split) for (auto label:selected) add(GetVoxelDefinition(request.source,shared,{label})," / "+std::to_string(label));
                        else add(GetVoxelDefinition(request.source,std::move(shared)));
                    }
                } else if constexpr (std::is_same_v<T,RoiThresholdParams>) {
                    Require((params.mode==RoiThresholdMode::Threshold && params.seeds.empty()) || (params.mode==RoiThresholdMode::SeedGrow && !params.seeds.empty()),RoiError::InvalidRequest,"Seed grow needs at least one explicit seed; threshold has no seeds.");
                    Require(image->GetComponentCount()==1 && std::isfinite(params.range[0]) && std::isfinite(params.range[1]) && params.range[0]<=params.range[1],RoiError::InvalidRequest,"Invalid ROI gray range.");
                    Cells eligible;ForCells(*image,request,[&](const Index& i){if(target && !targetQuery.query->GetContains(GetPoint(grid,i)))return;const double v=GetGray(*image,i);Require(std::isfinite(v),RoiError::InvalidGeometry,"ROI gray domain contains undefined scalar values.");
                        // 保留全允许域的有限灰度校验；生长不再保存整域 eligible 副本。
                        if(params.mode==RoiThresholdMode::Threshold && v>=params.range[0] && v<=params.range[1])Insert(eligible,i,request);},target?std::optional<std::array<double,6>>(target->GetBounds()):std::nullopt);
                    if (params.mode==RoiThresholdMode::SeedGrow) {
                        Matrix j=roiIdentityMatrix;for (int r=0;r<3;++r) {for (int c=0;c<3;++c) j[r*4+c]=grid.direction[r*3+c]*grid.spacing[c];j[r*4+3]=grid.origin[r];}const auto inverse=Invert(j);
                        const auto domain=GetCellExtent(grid,target?std::optional<std::array<double,6>>(target->GetBounds()):std::nullopt);
                        const auto allowed=[&](const Index& i){if(!domain)return false;for(int axis=0;axis<3;++axis)if(i[axis]<(*domain)[axis*2] || i[axis]>(*domain)[axis*2+1])return false;
                            if(!GetValidCell(*image,i) || (target && !targetQuery.query->GetContains(GetPoint(grid,i))))return false;
                            const double value=GetGray(*image,i);return value>=params.range[0] && value<=params.range[1];};
                        Cells grown;std::deque<Index> queue;for (const auto& seed:params.seeds) {
                            const auto p=Transform(inverse,seed);Index i{};for (int a=0;a<3;++a) {const auto rounded=std::floor(p[a]+0.5);Require(std::isfinite(rounded) && rounded>=INT_MIN && rounded<=INT_MAX,RoiError::InvalidGeometry,"ROI seed is invalid.");i[a]=static_cast<std::int64_t>(rounded);}
                            Require(allowed(i),RoiError::InvalidRequest,"ROI seed is outside the permitted gray domain.");if(Insert(grown,i,request))queue.push_back(i);
                        }
                        const auto neighbours=GetNeighbours(params.connectivity);std::size_t visited=0;
                        while(!queue.empty()){if(!(visited++%1024))CheckCancel(request);const auto current=queue.front();queue.pop_front();
                            for(const auto& d:neighbours){const auto i=Add(current,d);if(!grown.count(i) && allowed(i)){Insert(grown,i,request);queue.push_back(i);}}}
                        eligible=std::move(grown);
                    }add(GetVoxelDefinition(request.source,eligible));
                } else if constexpr (std::is_same_v<T,RoiTransformParams>) {
                    Require(target!=nullptr,RoiError::MissingInput,"ROI target is required.");const auto inverse=Invert(params.sourceToTarget);
                    const auto targetRef=params.targetSource.value_or(request.source);
                    const auto targetData=graph.view->GetData(targetRef);
                    const auto* targetImage=targetData ? dynamic_cast<const ImageGrid3DPayload*>(targetData->payload.get()):nullptr;
                    Require(targetImage && targetImage->GetValid(),RoiError::SourceMismatch,"ROI target source is not an image.");
                    if(!params.rasterize)add(TransformAnalytic(target->GetDefinition(),*image,targetRef,params.sourceToTarget));
                    else{Cells cells;ForCells(*targetImage,request,[&](const Index& i){if (targetQuery.query->GetContains(Transform(inverse,GetPoint(targetImage->GetGeometry(),i)))) Insert(cells,i,request);});add(GetVoxelDefinition(targetRef,cells));}
                } else if constexpr (std::is_same_v<T,RoiMeshParams>) {
                    Require(std::isfinite(params.inward) && std::isfinite(params.outward) && params.inward>=0 && params.outward>=0,RoiError::InvalidRequest,"Invalid ROI surface layer thickness.");
                    const auto data=graph.view->GetData(params.mesh);const auto* mesh=data ? dynamic_cast<const SurfaceMeshPayload*>(data->payload.get()):nullptr;
                    Require(mesh && mesh->GetValid(),RoiError::MissingInput,"ROI surface mesh is missing.");(void)Invert(params.meshToSource);
                    const auto spatial=SurfaceMeshPayload::RoiMeshIndex::Get(data,params,request);auto bounds=spatial->GetBounds();
                    const double margin=params.mode==RoiMeshMode::Layer?std::max(params.inward,params.outward):0;for(int a=0;a<3;++a){bounds[a*2]-=margin;bounds[a*2+1]+=margin;}
                    const bool closed=spatial->GetClosed();
                    Require(params.mode==RoiMeshMode::Interior || params.mode==RoiMeshMode::Layer,RoiError::InvalidRequest,"Invalid ROI mesh mode.");
                    Require((params.mode!=RoiMeshMode::Interior && !params.hasOrientation) || closed,RoiError::InvalidGeometry,"An open surface cannot define a reliable interior.");
                    Cells cells;const Point ray{1,0.3713906763541037,0.529173652};std::vector<double> hits;
                    ForCells(*image,request,[&](const Index& index){
                        const auto p=GetPoint(grid,index);const double distance=spatial->GetDistance(p,request);spatial->GetHits(p,ray,hits,request);
                        std::sort(hits.begin(),hits.end());hits.erase(std::unique(hits.begin(),hits.end(),[](double a,double b){return std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)});}),hits.end());
                        const bool inside=hits.size()%2!=0 || distance==0;
                        const bool selected=params.mode==RoiMeshMode::Interior ? inside:distance<=(params.hasOrientation ? (inside ? params.inward:params.outward):std::max(params.inward,params.outward));
                        if (selected && (!target || targetQuery.query->GetContains(p))) Insert(cells,index,request);
                    },bounds);add(GetVoxelDefinition(request.source,cells));
                }
            }
        },request.params);
        CheckCancel(request);result.error=RoiError::None;result.preparedSignature=PreparedSignature(request,result);result.provenance=GetOperationProvenance(request);
    } catch (const Failure& e) {result.error=e.error;result.message=e.message;result.rois.clear();}
    catch (const std::bad_alloc&) {result.error=RoiError::TooLarge;result.message="ROI allocation failed.";result.rois.clear();}
    catch (...) {result.error=RoiError::InvalidRequest;result.message="ROI operation failed.";result.rois.clear();}
    return result;
}

RoiOperationResult RoiService::SetOperation(const RoiOperationRequest& request)
{
    return SetOperationCommit(request,GetOperation(m_store.GetDataGraph(),request));
}
RoiOperationResult RoiService::SetOperationCommit(const RoiOperationRequest& request,RoiOperationResult result)
{
    if (result.error!=RoiError::None) return result;
    try {
        CheckCancel(request);Require(!result.preparedSignature.empty() && result.preparedSignature==PreparedSignature(request,result),RoiError::InvalidRequest,"Prepared ROI no longer matches the operation input/parameters.");const auto graph=m_store.GetDataGraph();
        const auto binding=graph.view->GetDataBinding(roiCatalogBinding).value_or(DataBinding{std::string(roiCatalogBinding)});
        const auto catalogData=binding.target ? graph.view->GetData(*binding.target):DataSnapshot{};
        const auto* catalog=catalogData ? dynamic_cast<const RoiCatalogPayload*>(catalogData->payload.get()):nullptr;
        Require(!binding.target || catalog,RoiError::MissingInput,"ROI catalog cannot be read.");
        auto entries=catalog ? catalog->GetEntries():std::vector<RoiCatalogEntry>{};
        const auto* components=std::get_if<RoiComponentParams>(&request.params);
        const bool split=components && components->operation==RoiComponentOperation::Split;
        const bool update=request.output==RoiOutputMode::UpdateTarget && !split;
        Require(request.output==RoiOutputMode::UpdateTarget || request.output==RoiOutputMode::CreateNew,RoiError::InvalidRequest,"Invalid ROI output mode.");
        auto selected=entries.end();
        if (update || (split && !components->keepInput)) {
            Require(request.target.has_value(),RoiError::MissingInput,"ROI update target is required.");
            selected=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.geometry.entityId==request.target->entityId;});
            Require(selected!=entries.end(),RoiError::MissingInput,"ROI target is no longer in the catalog.");
            Require(selected->geometry==*request.target,RoiError::RevisionConflict,"ROI target changed during the operation.");
        }
        DataTransaction tx;
        if (request.expectedSourceBinding) {
            const auto& b=*request.expectedSourceBinding;Require(b.target==request.source,RoiError::InvalidRequest,"ROI source binding does not match the operation.");
            DataExpectation e;e.kind=DataExpectationKind::Binding;e.binding=b.name;e.expectedBindingRevision=b.revision;e.isTargetChecked=true;e.expectedTarget=b.target;tx.expectations.push_back(std::move(e));
        }
        if (split && !components->keepInput) {DataExpectation e;e.entityId=request.target->entityId;e.expectedGeneration=request.target->generation;tx.expectations.push_back(e);entries.erase(selected);}
        for (auto& roi:result.rois) {
            Require(!roi.metadata.name.empty() && roi.metadata.name.size()<=256 && roi.metadata.group.size()<=256
                && roi.metadata.description.size()<=4096,RoiError::InvalidRequest,"ROI output metadata is invalid.");
            const DataRevisionRef ref=update ? DataRevisionRef{request.target->entityId,request.target->generation+1}:DataRevisionRef{m_store.CreateDataEntityId(),1};
            Require(ref.generation!=0,RoiError::TooLarge,"ROI generation exhausted.");
            if (update) Require(roi.definition.source==request.source,RoiError::SourceMismatch,"Cross-source copying requires a new ROI object.");
            const auto* transform=std::get_if<RoiTransformParams>(&request.params);
            Require(roi.definition.source==(transform ? transform->targetSource.value_or(request.source):request.source),RoiError::SourceMismatch,"ROI candidate source differs from the operation.");
            auto inputs=RoiEvaluator::GetInputs(roi.definition);
            if (request.target) inputs.push_back({"operation-input-0",*request.target});
            if (const auto* boolean=std::get_if<RoiBooleanParams>(&request.params)) for (std::size_t i=0;i<boolean->operands.size();++i) inputs.push_back({"operation-input-"+std::to_string(i+1),boolean->operands[i]});
            if (const auto* mesh=std::get_if<RoiMeshParams>(&request.params)) inputs.push_back({"operation-mesh",mesh->mesh});
            if (const auto* invert=std::get_if<RoiInvertParams>(&request.params);invert && invert->domain)inputs.push_back({"operation-input-domain",*invert->domain});
            tx.outputs.push_back({ref.entityId,ref.generation-1,DataTypes::roiGeometry,std::move(inputs),std::make_shared<const RoiGeometryPayload>(roi.definition),GetOperationProvenance(request)});
            if (update) *selected={ref,roi.metadata};else entries.push_back({ref,roi.metadata});
            roi.revision=roi.currentRevision=ref;roi.catalogRevision=binding.revision+1;
        }
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.geometry.entityId<b.geometry.entityId;});
        const DataRevisionRef catalogRef=catalogData ? DataRevisionRef{catalogData->self.entityId,catalogData->self.generation+1}:DataRevisionRef{m_store.CreateDataEntityId(),1};
        Require(catalogRef.generation!=0,RoiError::TooLarge,"ROI catalog generation exhausted.");std::vector<DataInputRef> inputs;
        for (std::size_t i=0;i<entries.size();++i) inputs.push_back({"roi-"+std::to_string(i),entries[i].geometry});
        tx.outputs.push_back({catalogRef.entityId,catalogRef.generation-1,DataTypes::roiCatalog,std::move(inputs),std::make_shared<const RoiCatalogPayload>(std::move(entries)),DataProvenance{"Host.Roi","set-catalog","1",{}}});
        for (auto& draft:tx.outputs) {
            bool explicitScope=false;for (const auto& input:draft.inputs) {
                const auto data=graph.view->GetData(input.source);if (GetInputTransient(data)) explicitScope=true;
                for (const auto& staged:tx.outputs) if (staged.entityId==input.source.entityId && staged.expectedGeneration+1==input.source.generation && staged.lifetimeScope) explicitScope=true;
            }if (explicitScope) {draft.lifetimeScope=m_store.CreateDataEntityId();draft.lifetimePolicy=DataLifetimePolicy::ExplicitRetirement;}
        }
        tx.bindings.push_back({std::string(roiCatalogBinding),binding.revision,true,binding.target,catalogRef});CheckCancel(request);
        const auto committed=m_store.SetDataCommit(std::move(tx));
        Require(committed.status==DataCommitStatus::Succeeded,committed.failureReason==DataCommitFailure::ExpectationFailed ? RoiError::RevisionConflict:RoiError::CommitFailed,"ROI batch transaction was rejected.");result.commitId=committed.commitId;
    } catch (const Failure& e) {result.error=e.error;result.message=e.message;result.rois.clear();}
    catch (const std::bad_alloc&) {result.error=RoiError::TooLarge;result.message="ROI commit allocation failed.";result.rois.clear();}
    catch (...) {result.error=RoiError::CommitFailed;result.message="ROI operation commit failed.";result.rois.clear();}
    return result;
}
