#include "Data/RoiService.h"
#include "Data/Internal/ReadBudget.h"
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
#include <unordered_set>

namespace {
using Index=std::array<int,3>;
using Point=std::array<double,3>;
using Matrix=std::array<double,16>;
struct IndexHash final {
    std::size_t operator()(const Index& p) const noexcept {
        std::size_t h=0;for (auto v:p) h^=std::hash<int>{}(v)+0x9e3779b9+(h<<6)+(h>>2);return h;
    }
};
using Cells=std::unordered_set<Index,IndexHash>;
struct Failure final { RoiError error; const char* message; };
void Require(bool condition,RoiError error,const char* message) {if (!condition) throw Failure{error,message};}
void CheckCancel(const RoiOperationRequest& request) {
    if(std::chrono::steady_clock::now()>=request.deadline)throw Failure{RoiError::DeadlineExceeded,"ROI operation deadline exceeded."};
    try {if (request.getCancelled && request.getCancelled()) throw Failure{RoiError::Cancelled,"ROI operation cancelled."};}
    catch (const Failure&) {throw;} catch (...) {throw Failure{RoiError::Cancelled,"ROI cancellation callback failed."};}
}
bool Insert(Cells& cells,const Index& index,const RoiOperationRequest& request){const bool added=cells.insert(index).second;Require(cells.size()<=GetReadBudget(request.maxWorkingBytes)/256,RoiError::TooLarge,"ROI working storage exceeds the resource budget.");return added;}
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
template<class F> void ForCells(const ImageGrid3DPayload& image,const RoiOperationRequest& request,F visit,
    std::optional<std::array<double,6>> bounds={}) {
    const auto& g=image.GetGeometry();
    auto extent=g.extent;
    if(bounds){
        for(int a=0;a<3;++a)if((*bounds)[a*2]>(*bounds)[a*2+1])return;
        Matrix matrix=roiIdentityMatrix;for(int r=0;r<3;++r){for(int a=0;a<3;++a)matrix[r*4+a]=g.direction[r*3+a]*g.spacing[a];matrix[r*4+3]=g.origin[r];}
        const auto inverse=Invert(matrix);std::array<double,6> indexed{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        for(unsigned k=0;k<8;++k){const auto p=Transform(inverse,{(*bounds)[(k&1)?1:0],(*bounds)[(k&2)?3:2],(*bounds)[(k&4)?5:4]});for(int a=0;a<3;++a){indexed[a*2]=std::min(indexed[a*2],p[a]);indexed[a*2+1]=std::max(indexed[a*2+1],p[a]);}}
        for(int a=0;a<3;++a){const double low=std::max(double(extent[a*2]),std::ceil(indexed[a*2]-1e-9)),high=std::min(double(extent[a*2+1]),std::floor(indexed[a*2+1]+1e-9));if(low>high)return;extent[a*2]=static_cast<int>(low);extent[a*2+1]=static_cast<int>(high);}
    }
    for (std::int64_t z=extent[4];z<=extent[5];++z) for (std::int64_t y=extent[2];y<=extent[3];++y) {
        CheckCancel(request);
        for (std::int64_t x=extent[0];x<=extent[1];++x) {Index i{static_cast<int>(x),static_cast<int>(y),static_cast<int>(z)};if (GetValidCell(image,i)) visit(i);}
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
RoiDefinition GetVoxelDefinition(DataRevisionRef source,const Cells& cells) {
    std::vector<RoiVoxel> values;values.reserve(cells.size());for (const auto& i:cells) values.push_back({i,1});
    return GetVoxelDefinition(source,std::make_shared<const RoiVoxelSet>(std::move(values)));
}
Cells GetCells(const ImageGrid3DPayload& image,const RoiReadSnapshot& read,const RoiOperationRequest& request) {
    Cells cells;ForCells(image,request,[&](const Index& i){if (read->GetContains(GetPoint(image.GetGeometry(),i))) Insert(cells,i,request);},read->GetBounds());return cells;
}
std::vector<Index> GetNeighbours(RoiConnectivity connectivity) {
    Require(connectivity==RoiConnectivity::Faces || connectivity==RoiConnectivity::Full,RoiError::InvalidRequest,"Invalid ROI connectivity.");
    std::vector<Index> offsets;for (int z=-1;z<=1;++z) for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
        if ((x||y||z) && (connectivity==RoiConnectivity::Full || std::abs(x)+std::abs(y)+std::abs(z)==1)) offsets.push_back({x,y,z});return offsets;
}
Index Add(const Index& a,const Index& b) {
    Index result{};for (int k=0;k<3;++k) {const auto n=static_cast<std::int64_t>(a[k])+b[k];Require(n>=INT_MIN && n<=INT_MAX,RoiError::TooLarge,"ROI index overflow.");result[k]=static_cast<int>(n);}return result;
}
std::vector<Index> GetKernel(const GridGeometry3D& grid,const RoiMorphologyParams& params,const RoiOperationRequest& request) {
    Require(std::isfinite(params.radius) && params.radius>=0,RoiError::InvalidRequest,"ROI radius must be nonnegative.");
    Require(params.unit==RoiRadiusUnit::Millimetres || params.unit==RoiRadiusUnit::Voxels,RoiError::InvalidRequest,"Invalid ROI radius unit.");
    Matrix j=roiIdentityMatrix;for (int r=0;r<3;++r) for (int c=0;c<3;++c) j[r*4+c]=params.unit==RoiRadiusUnit::Voxels ? (r==c ? 1.0:0.0):grid.direction[r*3+c]*grid.spacing[c];
    const auto inverse=Invert(j);Index halo{};
    for (int a=0;a<3;++a) {const double size=std::ceil(params.radius*std::hypot(inverse[a*4],inverse[a*4+1],inverse[a*4+2]));Require(std::isfinite(size) && size<INT_MAX,RoiError::TooLarge,"ROI neighbourhood exceeds index range.");halo[a]=static_cast<int>(size);}
    std::vector<Index> kernel;
    for (int z=-halo[2];z<=halo[2];++z) for (int y=-halo[1];y<=halo[1];++y) {CheckCancel(request);for (int x=-halo[0];x<=halo[0];++x) {
        const auto p=Transform(j,{static_cast<double>(x),static_cast<double>(y),static_cast<double>(z)});
        if (std::hypot(p[0],p[1],p[2])<=params.radius){Require(kernel.size()<request.maxWorkingBytes/sizeof(Index),RoiError::TooLarge,"ROI kernel exceeds the resource budget.");kernel.push_back({x,y,z});}
    }}return kernel;
}
Cells Morph(const Cells& cells,const std::vector<Index>& kernel,bool dilate,const RoiOperationRequest& request) {
    Cells out;std::size_t visited=0;
    for (const auto& i:cells) {
        if (!(visited++%1024)) CheckCancel(request);
        if (dilate) for (const auto& d:kernel) {if (!(visited++%1024)) CheckCancel(request);Insert(out,Add(i,d),request);}
        else {bool inside=true;for (const auto& d:kernel) {if (!(visited++%1024)) CheckCancel(request);if (!cells.count(Add(i,d))) {inside=false;break;}}if (inside) Insert(out,i,request);}
    }return out;
}
std::vector<std::vector<Index>> GetComponents(Cells remaining,RoiConnectivity connectivity,const RoiOperationRequest& request) {
    std::vector<Index> seeds(remaining.begin(),remaining.end());std::sort(seeds.begin(),seeds.end());
    const auto neighbours=GetNeighbours(connectivity);std::vector<std::vector<Index>> components;
    for (const auto& seed:seeds) if (remaining.erase(seed)) {
        std::vector<Index> component{seed};
        for (std::size_t cursor=0;cursor<component.size();++cursor) {
            if (!(cursor%1024)) CheckCancel(request);
            for (const auto& d:neighbours) {const auto next=Add(component[cursor],d);if (remaining.erase(next)) component.push_back(next);}
        }components.push_back(std::move(component));
    }return components;
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
}

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
    const auto provenance=GetOperationProvenance(request);std::ostringstream text;text<<provenance.operationId<<';'<<provenance.canonicalParameters<<';';
    if(request.input)text<<HashDefinition(*request.input);text<<';';
    for(const auto& roi:result.rois)text<<HashDefinition(roi.definition)<<',';text<<';';for(auto count:result.componentVoxelCounts)text<<count<<',';
    return text.str();
}

}
RoiOperationResult RoiService::GetOperation(const DataGraphSnapshot& graph,const RoiOperationRequest& requested)
{
    RoiOperationResult result;
    try {
        auto request=requested;request.maxWorkingBytes=GetReadBudget(request.maxWorkingBytes);
        CheckCancel(request);Require(graph.view!=nullptr,RoiError::Unavailable,"ROI graph is unavailable.");
        const auto source=graph.view->GetData(request.source);Require(source!=nullptr,RoiError::MissingInput,"ROI source is missing.");
        const auto* image=dynamic_cast<const ImageGrid3DPayload*>(source->payload.get());
        RoiReadSnapshot target;if (request.target) target=GetRead(graph,*request.target,request.source);
        if (request.input) {
            Require(request.input->source==request.source,RoiError::SourceMismatch,"ROI preview uses a different source.");
            auto preview=RoiEvaluator::GetPreview(graph,*request.input);
            Require(preview.error==RoiError::None && preview.roi,preview.error,"ROI preview cannot be read.");target=std::move(preview.roi);
        }
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
                    for (auto it=cells.begin();it!=cells.end();) if (!GetValidCell(*image,*it)) it=cells.erase(it);else ++it;
                    add(GetVoxelDefinition(request.source,cells));
                } else if constexpr (std::is_same_v<T,RoiComponentParams>) {
                    Require(target!=nullptr,RoiError::MissingInput,"ROI target is required.");
                    Require(std::isfinite(params.minimumVolume) && params.minimumVolume>=0 && std::isfinite(params.maximumHoleVolume) && params.maximumHoleVolume>=0,RoiError::InvalidRequest,"Invalid ROI component volume.");
                    auto cells=GetCells(*image,target,request);const auto volume=GetVoxelVolume(grid);
                    Require(volume>0 && std::isfinite(volume),RoiError::InvalidGeometry,"Invalid ROI voxel volume.");
                    if (params.operation==RoiComponentOperation::FillHoles) {
                        std::array<int,6> extent=grid.extent;
                        if(cells.empty()){add(GetVoxelDefinition(request.source,cells));return;}
                        for(int a=0;a<3;++a){int low=INT_MAX,high=INT_MIN;for(const auto& i:cells){low=std::min(low,i[a]);high=std::max(high,i[a]);}extent[a*2]=static_cast<int>(std::max(std::int64_t(grid.extent[a*2]),std::int64_t(low)-1));extent[a*2+1]=static_cast<int>(std::min(std::int64_t(grid.extent[a*2+1]),std::int64_t(high)+1));}
                        const auto inside=[&](const Index& i){for(int a=0;a<3;++a)if(i[a]<extent[a*2] || i[a]>extent[a*2+1])return false;return true;};
                        Cells background;
                        for(std::int64_t z=extent[4];z<=extent[5];++z)for(std::int64_t y=extent[2];y<=extent[3];++y){CheckCancel(request);for(std::int64_t x=extent[0];x<=extent[1];++x){Index i{int(x),int(y),int(z)};if(GetValidCell(*image,i) && !cells.count(i))Insert(background,i,request);}}
                        const auto components=GetComponents(std::move(background),params.connectivity,request);
                        const auto neighbours=GetNeighbours(params.connectivity);
                        for (const auto& component:components) {
                            bool exterior=false;for (const auto& i:component) {for (const auto& d:neighbours) if (!inside(Add(i,d)) || !GetValidCell(*image,Add(i,d))) {exterior=true;break;}if (exterior) break;}
                            if (!exterior && (!params.maximumHoleVolume || component.size()*volume<=params.maximumHoleVolume)) for(const auto& i:component)Insert(cells,i,request);
                        }add(GetVoxelDefinition(request.source,cells));
                    } else {
                        const auto components=GetComponents(std::move(cells),params.connectivity,request);
                        std::size_t largest=0;for (std::size_t i=0;i<components.size();++i) {result.componentVoxelCounts.push_back(components[i].size());if (components[i].size()>components[largest].size()) largest=i;}
                        std::vector<RoiVoxel> values;std::vector<std::uint32_t> selected;
                        Require(components.size()<UINT32_MAX,RoiError::TooLarge,"ROI component labels overflow.");
                        for (std::size_t i=0;i<components.size();++i) {
                            const auto label=static_cast<std::uint32_t>(i+1);
                            bool keep=components[i].size()*volume>=params.minimumVolume;
                            if (params.operation==RoiComponentOperation::KeepLargest) keep=i==largest;
                            else if (params.operation==RoiComponentOperation::KeepSelected) keep=std::find(params.labels.begin(),params.labels.end(),label)!=params.labels.end();
                            else Require(params.operation==RoiComponentOperation::RemoveSmall || params.operation==RoiComponentOperation::Split,RoiError::InvalidRequest,"Invalid ROI component operation.");
                            if (keep) {selected.push_back(label);for (const auto& index:components[i]) values.push_back({index,label});}
                        }
                        auto shared=std::make_shared<const RoiVoxelSet>(std::move(values));
                        if (params.operation==RoiComponentOperation::Split) for (auto label:selected) add(GetVoxelDefinition(request.source,shared,{label})," / "+std::to_string(label));
                        else add(GetVoxelDefinition(request.source,std::move(shared)));
                    }
                } else if constexpr (std::is_same_v<T,RoiThresholdParams>) {
                    Require((params.mode==RoiThresholdMode::Threshold && params.seeds.empty()) || (params.mode==RoiThresholdMode::SeedGrow && !params.seeds.empty()),RoiError::InvalidRequest,"Seed grow needs at least one explicit seed; threshold has no seeds.");
                    Require(image->GetComponentCount()==1 && std::isfinite(params.range[0]) && std::isfinite(params.range[1]) && params.range[0]<=params.range[1],RoiError::InvalidRequest,"Invalid ROI gray range.");
                    Cells eligible;ForCells(*image,request,[&](const Index& i){if(target && !target->GetContains(GetPoint(grid,i)))return;const double v=GetGray(*image,i);Require(std::isfinite(v),RoiError::InvalidGeometry,"ROI gray domain contains undefined scalar values.");if (v>=params.range[0] && v<=params.range[1]) Insert(eligible,i,request);},target?std::optional<std::array<double,6>>(target->GetBounds()):std::nullopt);
                    if (params.mode==RoiThresholdMode::SeedGrow) {
                        Matrix j=roiIdentityMatrix;for (int r=0;r<3;++r) {for (int c=0;c<3;++c) j[r*4+c]=grid.direction[r*3+c]*grid.spacing[c];j[r*4+3]=grid.origin[r];}const auto inverse=Invert(j);
                        Cells grown;std::vector<Index> queue;for (const auto& seed:params.seeds) {
                            const auto p=Transform(inverse,seed);Index i{};for (int a=0;a<3;++a) {Require(std::isfinite(p[a]) && p[a]>=INT_MIN && p[a]<INT_MAX,RoiError::InvalidGeometry,"ROI seed is invalid.");i[a]=static_cast<int>(std::floor(p[a]+0.5));}
                            Require(eligible.count(i)>0,RoiError::InvalidRequest,"ROI seed is outside the permitted gray domain.");if (Insert(grown,i,request)) queue.push_back(i);
                        }
                        const auto neighbours=GetNeighbours(params.connectivity);for (std::size_t k=0;k<queue.size();++k) {if (!(k%1024)) CheckCancel(request);for (const auto& d:neighbours) {const auto i=Add(queue[k],d);if (eligible.count(i) && Insert(grown,i,request)) queue.push_back(i);}}
                        eligible=std::move(grown);
                    }add(GetVoxelDefinition(request.source,eligible));
                } else if constexpr (std::is_same_v<T,RoiTransformParams>) {
                    Require(target!=nullptr,RoiError::MissingInput,"ROI target is required.");const auto inverse=Invert(params.sourceToTarget);
                    const auto targetRef=params.targetSource.value_or(request.source);
                    const auto targetData=graph.view->GetData(targetRef);
                    const auto* targetImage=targetData ? dynamic_cast<const ImageGrid3DPayload*>(targetData->payload.get()):nullptr;
                    Require(targetImage && targetImage->GetValid(),RoiError::SourceMismatch,"ROI target source is not an image.");
                    if(!params.rasterize)add(TransformAnalytic(target->GetDefinition(),*image,targetRef,params.sourceToTarget));
                    else{Cells cells;ForCells(*targetImage,request,[&](const Index& i){if (target->GetContains(Transform(inverse,GetPoint(targetImage->GetGeometry(),i)))) Insert(cells,i,request);});add(GetVoxelDefinition(targetRef,cells));}
                } else if constexpr (std::is_same_v<T,RoiMeshParams>) {
                    Require(std::isfinite(params.inward) && std::isfinite(params.outward) && params.inward>=0 && params.outward>=0,RoiError::InvalidRequest,"Invalid ROI surface layer thickness.");
                    const auto data=graph.view->GetData(params.mesh);const auto* mesh=data ? dynamic_cast<const SurfaceMeshPayload*>(data->payload.get()):nullptr;
                    Require(mesh && mesh->GetValid(),RoiError::MissingInput,"ROI surface mesh is missing.");(void)Invert(params.meshToSource);
                    Require(mesh->GetVertices().size()/3<=request.maxWorkingBytes/sizeof(Point) && mesh->GetTriangles().size()/3<=request.maxWorkingBytes/512,RoiError::TooLarge,"ROI mesh working storage exceeds the resource budget.");
                    std::array<double,6> bounds{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};std::vector<Point> points;points.reserve(mesh->GetVertices().size()/3);
                    for (std::size_t i=0;i<mesh->GetVertices().size();i+=3){if(i%768==0)CheckCancel(request);const auto point=Transform(params.meshToSource,{mesh->GetVertices()[i],mesh->GetVertices()[i+1],mesh->GetVertices()[i+2]});Require(std::all_of(point.begin(),point.end(),[](auto value){return std::isfinite(value);}),RoiError::InvalidGeometry,"ROI transformed mesh is not finite.");points.push_back(point);for(int a=0;a<3;++a){bounds[a*2]=std::min(bounds[a*2],point[a]);bounds[a*2+1]=std::max(bounds[a*2+1],point[a]);}}
                    const double margin=params.mode==RoiMeshMode::Layer?std::max(params.inward,params.outward):0;for(int a=0;a<3;++a){bounds[a*2]-=margin;bounds[a*2+1]+=margin;}
                    const auto& triangles=mesh->GetTriangles();std::map<std::pair<std::uint64_t,std::uint64_t>,std::size_t> edges;
                    for (std::size_t i=0;i<triangles.size();i+=3){if(i%768==0)CheckCancel(request);const auto cross=Cross(Subtract(points[triangles[i+1]],points[triangles[i]]),Subtract(points[triangles[i+2]],points[triangles[i]]));Require(Dot(cross,cross)>0 && std::isfinite(Dot(cross,cross)),RoiError::InvalidGeometry,"ROI mesh contains a degenerate face.");for (int e=0;e<3;++e) {auto a=triangles[i+e],b=triangles[i+(e+1)%3];if (a>b) std::swap(a,b);++edges[{a,b}];}}
                    const bool closed=!triangles.empty() && std::all_of(edges.begin(),edges.end(),[](const auto& e){return e.second==2;});
                    Require(params.mode==RoiMeshMode::Interior || params.mode==RoiMeshMode::Layer,RoiError::InvalidRequest,"Invalid ROI mesh mode.");
                    Require((params.mode!=RoiMeshMode::Interior && !params.hasOrientation) || closed,RoiError::InvalidGeometry,"An open surface cannot define a reliable interior.");
                    Cells cells;const Point ray{1,0.3713906763541037,0.529173652};
                    ForCells(*image,request,[&](const Index& index){
                        const auto p=GetPoint(grid,index);double distance=INFINITY;std::vector<double> hits;
                        for (std::size_t i=0;i<triangles.size();i+=3) {
                            if(i%768==0)CheckCancel(request);
                            const auto& a=points[triangles[i]];const auto& b=points[triangles[i+1]];const auto& c=points[triangles[i+2]];
                            distance=std::min(distance,GetTriangleDistance(p,a,b,c));const auto ab=Subtract(b,a),ac=Subtract(c,a),h=Cross(ray,ac);const double det=Dot(ab,h);
                            if (std::abs(det)<=1e-14) continue;const auto q=Subtract(p,a);const double u=Dot(q,h)/det;if (u<0 || u>1) continue;
                            const auto s=Cross(q,ab);const double v=Dot(ray,s)/det,t=Dot(ac,s)/det;if (v>=0 && u+v<=1 && t>0) hits.push_back(t);
                        }
                        std::sort(hits.begin(),hits.end());hits.erase(std::unique(hits.begin(),hits.end(),[](double a,double b){return std::abs(a-b)<=1e-10*std::max({1.0,std::abs(a),std::abs(b)});}),hits.end());
                        const bool inside=hits.size()%2!=0 || distance==0;
                        const bool selected=params.mode==RoiMeshMode::Interior ? inside:distance<=(params.hasOrientation ? (inside ? params.inward:params.outward):std::max(params.inward,params.outward));
                        if (selected && (!target || target->GetContains(p))) Insert(cells,index,request);
                    },bounds);add(GetVoxelDefinition(request.source,cells));
                }
            }
        },request.params);
        CheckCancel(request);result.error=RoiError::None;result.preparedSignature=PreparedSignature(request,result);
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
                const auto data=graph.view->GetData(input.source);if (data && data->lifetimePolicy==DataLifetimePolicy::ExplicitRetirement) explicitScope=true;
                for (const auto& staged:tx.outputs) if (staged.entityId==input.source.entityId && staged.lifetimePolicy==DataLifetimePolicy::ExplicitRetirement) explicitScope=true;
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
