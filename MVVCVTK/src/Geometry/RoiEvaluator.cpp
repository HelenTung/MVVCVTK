#include "Geometry/RoiEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

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
        && p.origin==Point{0,0,0} && p.normal==Point{0,0,1} && !p.mask;
}

const GridGeometry3D* GetMaskGrid(const DataSnapshot& data) noexcept
{
    if (!data) return nullptr;
    if (const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get())) return &mask->GetGeometry();
    if (const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get())) return &labels->GetGeometry();
    return nullptr;
}

bool GetMaskValue(const DataSnapshot& data, std::size_t index) noexcept
{
    if (const auto* mask=dynamic_cast<const BinaryMask3DPayload*>(data->payload.get()))
        return (*mask->GetValues())[index]!=0;
    const auto* labels=dynamic_cast<const LabelMap3DPayload*>(data->payload.get());
    return labels && std::visit([index](const auto& values) { return (*values)[index]!=0; },labels->GetValues());
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

class RoiView final : public RoiReadView {
public:
    RoiView(DataSnapshot roi, DataSnapshot source, const RoiEvaluator::DataLookup& getData)
        : m_revision(roi->self), m_definition(dynamic_cast<const RoiGeometryPayload&>(*roi->payload).GetDefinition())
    {
        m_dependencies.push_back(m_definition.source);
        m_dependencies.push_back(m_revision);
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
        m_planes.error=RoiError::None;
        for (std::size_t i=0;i<m_definition.nodes.size();++i) {
            const auto& node=m_definition.nodes[i];
            auto& prepared=m_prepared[i];
            if (node.kind==RoiNodeKind::Primitive) {
                const auto& p=node.primitive;
                if (p.shape==RoiShape::Box) {
                    if (!Invert(p.localToSource,prepared.inverse)) throw std::runtime_error("Invalid box matrix.");
                    AddBoxPlanes(m_planes.planes,prepared.inverse,{-1,1,-1,1,-1,1});
                } else if (p.shape==RoiShape::HalfSpace) {
                    const auto length=std::hypot(p.normal[0],p.normal[1],p.normal[2]);
                    prepared.normal={p.normal[0]/length,p.normal[1]/length,p.normal[2]/length};
                    m_planes.planes.push_back({p.origin,prepared.normal});
                } else {
                    prepared.mask=getData(*p.mask);
                    if (std::find(m_dependencies.begin(),m_dependencies.end(),*p.mask)==m_dependencies.end())
                        m_dependencies.push_back(*p.mask);
                    m_planes.error=RoiError::UnsupportedRoi;
                }
            } else if (node.kind!=RoiNodeKind::Intersection && node.kind!=RoiNodeKind::SourceDomain) {
                m_planes.error=RoiError::UnsupportedRoi;
            }
        }
        if (m_planes.error!=RoiError::None) m_planes.planes.clear();
        // 仅作保守读取范围；精确选择始终使用表达式谓词。
        std::array<std::array<double,6>,roiNodeLimit> bounds{};
        for (std::size_t i=0;i<m_definition.nodes.size();++i) {
            const auto& node=m_definition.nodes[i];
            auto& box=bounds[i]; box=m_bounds;
            if (node.kind==RoiNodeKind::Empty) box={1,0,1,0,1,0};
            if (node.kind==RoiNodeKind::Primitive && node.primitive.shape==RoiShape::Box)
                box=GetGridBounds(node.primitive.localToSource,{-1,1,-1,1,-1,1});
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
    RoiPolygonResult GetClippedTriangle(const std::array<Point,3>& triangle,std::size_t maxBytes) const override
    {
        RoiPolygonResult result;
        if (m_planes.error!=RoiError::None) return result;
        for (const auto& p:triangle) if (!GetFinite(p)) { result.error=RoiError::InvalidGeometry; return result; }
        result.requiredBytes=(m_planes.planes.size()+3)*sizeof(Point)*2;
        if (result.requiredBytes>std::min(maxBytes,roiCopyLimit)) { result.error=RoiError::TooLarge; return result; }
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
                        const double t=previousDistance/(previousDistance-currentDistance);
                        next.push_back({previous[0]+(point[0]-previous[0])*t,
                            previous[1]+(point[1]-previous[1])*t,previous[2]+(point[2]-previous[2])*t});
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
        if (m_planes.error!=RoiError::None || !GetContains(p)) return NAN;
        double distance=INFINITY;
        for (const auto& plane:m_planes.planes) {
            const Point offset{p[0]-plane.origin[0],p[1]-plane.origin[1],p[2]-plane.origin[2]};
            distance=std::min(distance,std::abs(Dot(plane.normal,offset)));
        }
        return distance;
    }
    bool GetContains(const Point& p) const noexcept override
    {
        if (!GetFinite(p)) return false;
        Point index{};
        if (m_image) {
            index=Transform(m_sourceToIndex,p);
            const auto& extent=m_image->GetGeometry().extent;
            for (int a=0;a<3;++a) if (index[a]<extent[a*2]-1e-9 || index[a]>extent[a*2+1]+1e-9) return false;
        } else {
            for (int a=0;a<3;++a) if (p[a]<m_bounds[a*2] || p[a]>m_bounds[a*2+1]) return false;
        }
        bool values[roiNodeLimit]{};
        for (std::size_t i=0;i<m_definition.nodes.size();++i) {
            const auto& node=m_definition.nodes[i];
            switch (node.kind) {
            case RoiNodeKind::Empty: values[i]=false; break;
            case RoiNodeKind::SourceDomain: values[i]=true; break;
            case RoiNodeKind::Union: values[i]=values[node.left]||values[node.right]; break;
            case RoiNodeKind::Intersection: values[i]=values[node.left]&&values[node.right]; break;
            case RoiNodeKind::Difference: values[i]=values[node.left]&&!values[node.right]; break;
            case RoiNodeKind::Primitive: {
                const auto& primitive=node.primitive;
                if (primitive.shape==RoiShape::Box) {
                    const auto local=Transform(m_prepared[i].inverse,p);
                    values[i]=GetFinite(local) && std::abs(local[0])<=1+1e-12
                        && std::abs(local[1])<=1+1e-12 && std::abs(local[2])<=1+1e-12;
                } else if (primitive.shape==RoiShape::HalfSpace) {
                    values[i]=Dot(m_prepared[i].normal,{p[0]-primitive.origin[0],p[1]-primitive.origin[1],p[2]-primitive.origin[2]})>=0;
                } else {
                    const auto& grid=m_image->GetGeometry();
                    std::size_t offset=0, stride=1;
                    for (int a=0;a<3;++a) {
                        const double nearest=std::floor(index[a]+0.5);
                        if (nearest<grid.extent[a*2] || nearest>grid.extent[a*2+1]) return false;
                        offset+=static_cast<std::size_t>(nearest-grid.extent[a*2])*stride;
                        stride*=static_cast<std::size_t>(grid.dimensions[a]);
                    }
                    values[i]=GetMaskValue(m_prepared[i].mask,offset);
                }
                break;
            }
            }
        }
        return values[m_definition.nodes.size()-1];
    }
    RoiPointsResult GetPoints(const std::vector<Point>& points,std::size_t maxBytes) const override
    {
        RoiPointsResult result;
        result.requiredBytes=points.size();
        if (points.size()>std::min(maxBytes,roiCopyLimit)) { result.error=RoiError::TooLarge; return result; }
        if (std::any_of(points.begin(),points.end(),[](const Point& p){return !GetFinite(p);})) return result;
        try {
            result.values.reserve(points.size());
            for (const auto& p:points) result.values.push_back(GetContains(p)?1:0);
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
        if (request.voxelOffset>count || request.maxBytes==0) return result;
        const auto bytes=std::min({count-request.voxelOffset,request.maxBytes,roiCopyLimit});
        result.requiredBytes=bytes;
        try {
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
                result.values[i]=GetContains(Transform(m_indexMatrix,index))?1:0;
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
    if (!GetDataRevisionRefValid(definition.source) || definition.nodes.empty()) return RoiError::InvalidRequest;
    if (definition.nodes.size()>roiNodeLimit) return RoiError::TooLarge;
    std::size_t depths[roiNodeLimit]{};
    bool reachable[roiNodeLimit]{};
    for (std::size_t i=0;i<definition.nodes.size();++i) {
        const auto& n=definition.nodes[i];
        const auto& p=n.primitive;
        const bool binary=n.kind==RoiNodeKind::Union || n.kind==RoiNodeKind::Intersection || n.kind==RoiNodeKind::Difference;
        if (binary) {
            if (n.left>=i || n.right>=i || !GetPrimitiveDefault(p)) return RoiError::InvalidGeometry;
            depths[i]=1+std::max(depths[n.left],depths[n.right]);
        } else {
            if (n.left!=0 || n.right!=0) return RoiError::InvalidGeometry;
            depths[i]=1;
            if (n.kind==RoiNodeKind::Primitive) {
                if (!GetFinite(p.localToSource) || !GetFinite(p.origin) || !GetFinite(p.normal)) return RoiError::InvalidGeometry;
                if (p.shape==RoiShape::Box) {
                    Matrix inverse;
                    if (!Invert(p.localToSource,inverse) || p.mask || p.origin!=Point{0,0,0} || p.normal!=Point{0,0,1}) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::HalfSpace) {
                    const double length=std::hypot(p.normal[0],p.normal[1],p.normal[2]);
                    if (!(length>0) || !std::isfinite(length) || p.mask || p.localToSource!=roiIdentityMatrix) return RoiError::InvalidGeometry;
                } else if (p.shape==RoiShape::MaskReference) {
                    if (!p.mask || !GetDataRevisionRefValid(*p.mask) || p.localToSource!=roiIdentityMatrix
                        || p.origin!=Point{0,0,0} || p.normal!=Point{0,0,1}) return RoiError::InvalidGeometry;
                } else return RoiError::UnsupportedRoi;
            } else if ((n.kind!=RoiNodeKind::Empty && n.kind!=RoiNodeKind::SourceDomain) || !GetPrimitiveDefault(p)) return RoiError::InvalidGeometry;
        }
        if (depths[i]>roiDepthLimit) return RoiError::TooLarge;
    }
    reachable[definition.nodes.size()-1]=true;
    for (std::size_t i=definition.nodes.size();i-->0;) {
        if (!reachable[i]) return RoiError::InvalidGeometry;
        const auto& n=definition.nodes[i];
        if (n.kind==RoiNodeKind::Union || n.kind==RoiNodeKind::Intersection || n.kind==RoiNodeKind::Difference)
            reachable[n.left]=reachable[n.right]=true;
    }
    return RoiError::None;
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

RoiError RoiEvaluator::GetRelationsError(const DataRevision& roi,const DataLookup& getData)
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
            if (input.role!="copy-source") return RoiError::InvalidRequest;
            const auto copy=getData(input.source);
            const auto* copyPayload=copy ? dynamic_cast<const RoiGeometryPayload*>(copy->payload.get()):nullptr;
            if (!copyPayload || copyPayload->GetDefinition().source!=definition.source) return RoiError::SourceMismatch;
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
        result.error=GetRelationsError(*roi,lookup);
        if (result.error!=RoiError::None) return result;
        const auto& definition=dynamic_cast<const RoiGeometryPayload&>(*roi->payload).GetDefinition();
        if (definition.source!=sourceRef) { result.error=RoiError::SourceMismatch; return result; }
        result.roi=std::make_shared<const RoiView>(roi,lookup(sourceRef),lookup);
    } catch (const std::bad_alloc&) { result.error=RoiError::TooLarge; }
    catch (...) { result.error=RoiError::InvalidGeometry; }
    return result;
}
