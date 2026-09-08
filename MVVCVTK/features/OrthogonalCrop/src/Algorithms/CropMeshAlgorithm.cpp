#include "Algorithms/CropMeshAlgorithm.h"
#include "Algorithms/CropMeshExact.h"
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkTriangleFilter.h>
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace {
using N=CropMeshExact::Number;
using EP=CropMeshExact::Point;
using P=CropVectorDouble3Array;
using CropMeshExact::Failure;
using CropMeshExact::Dot;
using CropMeshExact::Sub;
using CropMeshExact::Scale;
using CropMeshExact::Values;
using CropMeshExact::Radial;
struct Vertex final { double u=0,v=0; P point{}; };
using Polygon=std::vector<Vertex>;
struct Triangle final {std::array<Vertex,3> vertices;std::uint32_t depth=0;};
struct Face final {P normal{},center{};double translation=0,offset=0;bool closed=true;};
P Difference(const P& a,const P& b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
double DotD(const P& a,const P& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
double Length(const P& a) {return std::hypot(a[0],a[1],a[2]);}
P RadialD(const P& p,const P& axis) {const double t=DotD(p,axis);return {p[0]-t*axis[0],p[1]-t*axis[1],p[2]-t*axis[2]};}
std::vector<Face> Faces(const CropGeometry& geometry)
{
    const auto& op=geometry.GetOperation();std::vector<Face> result;
    if(op.geometryType==CropShape::Box) {
        const auto& m=geometry.GetBoxInverse();result.reserve(6);
        for(int row=0;row<3;++row)for(double sign:{-1.0,1.0})
            result.push_back({{sign*m[row*4],sign*m[row*4+1],sign*m[row*4+2]},{},sign*m[row*4+3],1+1e-6,true});
    } else if(op.geometryType==CropShape::Plane) {
        const auto& n=op.planeNormalInInputModel;
        result.push_back({{-n[0],-n[1],-n[2]},op.planeCenterInInputModel,0,0,false});
    } else if(op.geometryType==CropShape::Cylinder) {
        const auto& n=op.axisInInputModel;
        for(double sign:{-1.0,1.0})result.push_back({{sign*n[0],sign*n[1],sign*n[2]},op.centerInInputModel,0,op.height/2,true});
    }
    return result;
}

class Engine final {
public:
    Engine(vtkPolyData* source,const CropBuildParams& params,const std::vector<CropGeometry>& geometry,
        const std::function<bool()>& stop,std::uint64_t& operation)
        :m_source(source),m_params(params),m_geometry(geometry),m_stop(stop),m_operation(operation)
    {
        m_limit=params.availableRamBytes?params.availableRamBytes:512ULL*1024*1024;
        if(!std::isfinite(params.meshTolerance)||params.meshTolerance<=0||!params.maxCells
            ||!params.maxDepth||params.maxDepth>128||std::fegetround()!=FE_TONEAREST)
            throw Failure{CropFailure::BadInput,"Mesh tolerance, cell/depth limits or floating-point mode is invalid."};
        m_fixed=16ULL*1024*1024;
        const auto memory=static_cast<std::uint64_t>(source->GetActualMemorySize());
        if(memory>std::numeric_limits<std::size_t>::max()/5120)throw Failure{CropFailure::LowRam,"Mesh input size overflows."};
        AddBytes(m_fixed,static_cast<std::size_t>(memory)*1024*5); // VTK reports KiB; reserve five coexisting input representations.
        std::size_t pointComponents=0,cellComponents=0;
        for(auto* data:{static_cast<vtkDataSetAttributes*>(source->GetPointData()),static_cast<vtkDataSetAttributes*>(source->GetCellData())}) {
            for(int i=0;i<data->GetNumberOfArrays();++i) {
                auto* array=vtkDataArray::SafeDownCast(data->GetAbstractArray(i));
                if(!array||!array->GetName()||array->GetNumberOfComponents()<=0
                    ||array->GetNumberOfTuples()!=(data==source->GetPointData()?source->GetNumberOfPoints():source->GetNumberOfCells()))
                    throw Failure{CropFailure::BadInput,"Mesh attributes must be named numeric arrays."};
                auto& components=data==source->GetPointData()?pointComponents:cellComponents;
                AddBytes(components,static_cast<std::size_t>(array->GetNumberOfComponents()));
                AddBytes(m_fixed,4096+4*std::char_traits<char>::length(array->GetName()));
            }
        }
        const auto multiply=[](std::size_t a,std::size_t b) {
            if(b&&a>std::numeric_limits<std::size_t>::max()/b)throw Failure{CropFailure::LowRam,"Mesh canonical layout overflows."};
            return a*b;
        };
        std::size_t pointBytes=24,cellBytes=32;
        AddBytes(pointBytes,multiply(pointComponents,8));AddBytes(cellBytes,multiply(cellComponents,8));
        std::size_t triangleUpper=0;
        for(auto* cells:{source->GetPolys(),source->GetStrips()})
            if(cells)AddBytes(triangleUpper,static_cast<std::size_t>(cells->GetNumberOfConnectivityIds()));
        std::size_t canonical=multiply(static_cast<std::size_t>(source->GetNumberOfPoints()),pointBytes);
        AddBytes(canonical,multiply(triangleUpper,cellBytes));
        // Numeric attributes become doubles. A byte-sized source array must be
        // budgeted at the canonical size before either factory allocates it.
        AddBytes(m_fixed,multiply(canonical,4));
        AddBytes(m_fixed,multiply(geometry.size(),4096));
        m_triangleBytes=128;
        if(pointComponents>std::numeric_limits<std::size_t>::max()/24||cellComponents>std::numeric_limits<std::size_t>::max()/8)
            throw Failure{CropFailure::LowRam,"Mesh attribute layout overflows."};
        AddBytes(m_triangleBytes,pointComponents*24);AddBytes(m_triangleBytes,cellComponents*8);
        if(m_triangleBytes>std::numeric_limits<std::size_t>::max()/8)throw Failure{CropFailure::LowRam,"Mesh output size overflows."};
        m_triangleBytes*=8; // VTK capacity growth, isolated staging, triangulation, formal arrays and prepared bridge coexist.
        Memory(0);
        for(const auto& item:geometry)m_faces.push_back(Faces(item));
        m_output=vtkSmartPointer<vtkPolyData>::New();m_points=vtkSmartPointer<vtkPoints>::New();
        m_points->SetDataTypeToDouble();m_cells=vtkSmartPointer<vtkCellArray>::New();
        m_output->SetPoints(m_points);m_output->SetPolys(m_cells);
    }

    CropMaterializationCandidate Run()
    {
        CheckStop();
        // The trusted worker factory validates lossless attributes and isolates VTK traversal state.
        const auto input=VtkPreparedDataView::BuildDataView(m_source);
        if(!input||!input->mesh||!input->mesh->mesh)throw Failure{CropFailure::BadInput,"Mesh input cannot be frozen without attribute or geometry loss."};
        m_input=input->mesh->mesh;
        m_output->GetPointData()->InterpolateAllocate(m_input->GetPointData());
        m_output->GetCellData()->CopyAllocate(m_input->GetCellData());
        vtkNew<vtkIdList> ids;
        for(vtkIdType cell=0;cell<m_input->GetNumberOfCells();++cell) {
            CheckStop();m_input->GetCellPoints(cell,ids);
            if(ids->GetNumberOfIds()!=3)throw Failure{CropFailure::BadInput,"Mesh triangulation failed."};
            m_cell=cell;m_ids=ids;
            double magnitude=0;
            for(int i=0;i<3;++i) {
                m_input->GetPoint(ids->GetId(i),m_root[i].data());
                for(double value:m_root[i]) {
                    if(!std::isfinite(value))throw Failure{CropFailure::BadInput,"Mesh coordinates must be finite."};
                    if(std::abs(value)>std::numeric_limits<float>::max())throw Failure{CropFailure::PrecisionNotMet,"Mesh coordinates exceed the preview coordinate range."};
                    magnitude=std::max(magnitude,std::abs(value));
                }
            }
            const auto a=Sub(Values(m_root[1]),Values(m_root[0])),b=Sub(Values(m_root[2]),Values(m_root[0]));
            const std::array<N,3> cross{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
            if(!cross[0].Sign()&&!cross[1].Sign()&&!cross[2].Sign())continue;
            m_positionError=std::nextafter((256+64.0*m_geometry.size()+16.0*m_params.maxDepth)
                *std::numeric_limits<double>::epsilon()*magnitude,std::numeric_limits<double>::infinity());
            m_stack.push_back({{Make(0,0),Make(1,0),Make(0,1)},0});
            while(!m_stack.empty()) {
                CheckStop();if(m_visited++>=m_params.maxCells)throw Failure{CropFailure::ResourceLimit,"Mesh cell budget was exhausted."};
                Triangle triangle=std::move(m_stack.back());m_stack.pop_back();
                Process(triangle);
            }
        }
        CheckStop();
        if(!m_emitted)throw Failure{CropFailure::EmptyResult,"Crop mesh contains no retained surface area."};
        m_output->Squeeze();
        CropMaterializationCandidate result;result.isSucceeded=true;result.sourceRevision=m_params.sourceRevision;
        result.nodeCount=m_params.nodeCount;
        for(const auto& geometry:m_geometry)result.operations.push_back(geometry.GetOperation());
        result.polyData=std::move(m_output);result.meshTriangleCount=m_emitted;
        result.meshErrorBound=m_errorBound;result.meshAreaErrorBound=m_areaBound;
        result.message="Mesh clipping completed within the declared spatial error bound.";
        return result;
    }
private:
    static void AddBytes(std::size_t& total,std::size_t count) {
        if(count>std::numeric_limits<std::size_t>::max()-total)throw Failure{CropFailure::LowRam,"Mesh memory accounting overflows."};total+=count;
    }
    void Memory(std::size_t temporary) const {
        std::size_t required=m_fixed;
        if(m_emitted>std::numeric_limits<std::size_t>::max()/m_triangleBytes)throw Failure{CropFailure::LowRam,"Mesh output size overflows."};
        AddBytes(required,m_emitted*m_triangleBytes);AddBytes(required,temporary);
        if(required>m_limit)throw Failure{CropFailure::LowRam,"Mesh preparation exceeds the available memory budget."};
    }
    void CheckStop() const {
        bool stopped=false;try {stopped=m_stop&&m_stop();}catch(...){stopped=true;}
        if(stopped)throw Failure{CropFailure::Cancelled,"Mesh clipping was cancelled."};
    }
    EP Coordinates(const Vertex& vertex) const {
        const auto first=Values(m_root[0]);
        const auto u=Scale(Sub(Values(m_root[1]),first),N(vertex.u));
        const auto v=Scale(Sub(Values(m_root[2]),first),N(vertex.v));
        return {first[0]+u[0]+v[0],first[1]+u[1]+v[1],first[2]+u[2]+v[2]};
    }
    Vertex Make(double u,double v) const {
        if(!std::isfinite(u)||!std::isfinite(v))CropMeshExact::Precision();
        Vertex value;value.u=std::clamp(u,0.0,1.0);value.v=std::clamp(v,0.0,1.0);
        if((N(value.u)+N(value.v)-N(1)).Sign()>0) {
            value.v=1-value.u;
            while((N(value.u)+N(value.v)-N(1)).Sign()>0)value.v=std::nextafter(value.v,0.0);
        }
        const auto p=Coordinates(value);for(int i=0;i<3;++i)value.point[i]=p[i].Value();
        return value;
    }
    Vertex Mix(const Vertex& a,const Vertex& b,double t) const {t=std::clamp(t,0.0,1.0);return Make(a.u*(1-t)+b.u*t,a.v*(1-t)+b.v*t);}
    Vertex Center(const Polygon& polygon) const {
        double u=0,v=0;for(const auto& point:polygon){u+=point.u;v+=point.v;}
        return Make(u/polygon.size(),v/polygon.size());
    }
    N Distance(const EP& point,const Face& face) const {return Dot(Sub(point,Values(face.center)),Values(face.normal))+N(face.translation)-N(face.offset);}
    EP Relative(const EP& point,const CropGeometry& geometry) const {
        const auto& op=geometry.GetOperation();auto q=Sub(point,Values(op.centerInInputModel));
        return op.geometryType==CropShape::Cylinder?Radial(q,op.axisInInputModel):q;
    }
    N RadiusSign(const EP& point,const CropGeometry& geometry) const {const auto q=Relative(point,geometry);return Dot(q,q)-N(geometry.GetOperation().radius)*N(geometry.GetOperation().radius);}
    bool Inside(const EP& point,std::size_t index) const {
        for(const auto& face:m_faces[index]) {const int sign=Distance(point,face).Sign();if(sign>0||(!face.closed&&sign==0))return false;}
        const auto shape=m_geometry[index].GetOperation().geometryType;
        return (shape!=CropShape::Sphere&&shape!=CropShape::Cylinder)||RadiusSign(point,m_geometry[index]).Sign()<=0;
    }
    bool Kept(const Vertex& vertex,std::size_t index) const {
        const bool inside=Inside(Coordinates(vertex),index);
        return m_geometry[index].GetOperation().removalMode==CropRemovalMode::KeepInside?inside:!inside;
    }
    bool Interior(const Vertex& vertex) const {
        const auto point=Coordinates(vertex);
        for(std::size_t index=0;index<m_geometry.size();++index) {
            const auto& op=m_geometry[index].GetOperation();
            if(!Kept(vertex,index))return false;
            if(op.removalMode==CropRemovalMode::RemoveInside&&op.geometryType!=CropShape::Plane)continue;
            for(const auto& face:m_faces[index]) {
                if(Distance(point,face).Sign()!=0)continue;
                bool constant=true;for(const auto& p:m_root)constant=constant&&Distance(Values(p),face).Sign()==0;
                if(!constant)return false;
            }
            if((op.geometryType==CropShape::Sphere||op.geometryType==CropShape::Cylinder)&&RadiusSign(point,m_geometry[index]).Sign()>=0)return false;
        }
        return true;
    }
    static bool Same(const Vertex& a,const Vertex& b) {return a.u==b.u&&a.v==b.v;}
    void Append(Polygon& polygon,const Vertex& point) const {if(polygon.empty()||!Same(polygon.back(),point))polygon.push_back(point);}
    Polygon ClipFace(const Polygon& polygon,const Face& face,bool keepInside) const {
        Polygon out;if(polygon.empty())return out;Memory((polygon.size()+1)*sizeof(Vertex)*8);
        out.reserve(polygon.size()+1);
        auto before=polygon.back();auto distanceBefore=Distance(Coordinates(before),face);
        const auto kept=[&](const N& value){const bool in=value.Sign()<0||(face.closed&&value.Sign()==0);return keepInside?in:!in;};
        bool wasKept=kept(distanceBefore);
        for(const auto& current:polygon) {
            const auto distance=Distance(Coordinates(current),face);const bool isKept=kept(distance);
            if(wasKept!=isKept) {
                const double denominator=(distanceBefore-distance).Value();
                if(denominator==0||!std::isfinite(denominator))CropMeshExact::Precision();
                Append(out,Mix(before,current,distanceBefore.Value()/denominator));
            }
            if(isKept)Append(out,current);
            before=current;distanceBefore=distance;wasKept=isKept;
        }
        if(out.size()>1&&Same(out.front(),out.back()))out.pop_back();return out;
    }
    Vertex Nearest(const Polygon& polygon,std::size_t index) const {
        const auto& op=m_geometry[index].GetOperation();
        auto transform=[&](const P& p){auto q=Difference(p,op.centerInInputModel);return op.geometryType==CropShape::Cylinder?RadialD(q,op.axisInInputModel):q;};
        Vertex best=polygon.front();double bestValue=std::numeric_limits<double>::infinity();
        const auto consider=[&](const Vertex& candidate){const auto q=transform(candidate.point);const auto value=DotD(q,q);if(std::isfinite(value)&&value<bestValue){best=candidate;bestValue=value;}};
        for(std::size_t i=0;i<polygon.size();++i) {
            const auto& a=polygon[i];const auto& b=polygon[(i+1)%polygon.size()];consider(a);
            const auto q=transform(a.point),e=Difference(transform(b.point),q);const double norm=DotD(e,e);
            if(norm>0)consider(Mix(a,b,-DotD(q,e)/norm));
        }
        for(std::size_t i=1;i+1<polygon.size();++i) {
            const auto& a=polygon[0];const auto& b=polygon[i];const auto& c=polygon[i+1];
            const auto d=transform(a.point),u=Difference(transform(b.point),d),v=Difference(transform(c.point),d);
            const double aa=DotD(u,u),bb=DotD(u,v),cc=DotD(v,v),du=DotD(d,u),dv=DotD(d,v);
            const double determinant=std::fma(aa,cc,-bb*bb);
            if(determinant>0) {
                const double x=(bb*dv-cc*du)/determinant,y=(bb*du-aa*dv)/determinant;
                if(x>=0&&y>=0&&x+y<=1)consider(Make(a.u+x*(b.u-a.u)+y*(c.u-a.u),a.v+x*(b.v-a.v)+y*(c.v-a.v)));
            }
        }
        return best;
    }
    bool OutsideRadius(const Polygon& polygon,std::size_t index,const Vertex& candidate) const {
        const auto q=Relative(Coordinates(candidate),m_geometry[index]);
        const auto f=Dot(q,q)-N(m_geometry[index].GetOperation().radius)*N(m_geometry[index].GetOperation().radius);
        bool supported=true;
        for(const auto& vertex:polygon) {
            const auto delta=Sub(Relative(Coordinates(vertex),m_geometry[index]),q);
            // Convex quadratic tangent gives a rigorous lower bound over the whole polygon.
            supported=supported&&(f+N(2)*Dot(q,delta)).Sign()>=0;
        }
        if(supported)return true;
        // Certify all constrained quadratic minima with expansion signs. No rounded
        // closest point is used to decide a tangent or a very narrow intersection.
        const auto radius=N(m_geometry[index].GetOperation().radius);
        const auto relative=[&](const Vertex& vertex){return Relative(Coordinates(vertex),m_geometry[index]);};
        for(std::size_t i=1;i+1<polygon.size();++i) {
            const std::array<EP,3> points{relative(polygon[0]),relative(polygon[i]),relative(polygon[i+1])};
            for(int j=0;j<3;++j) {
                const auto& d=points[j];const auto e=Sub(points[(j+1)%3],d);
                const auto aa=Dot(e,e),b=Dot(d,e),c=Dot(d,d)-radius*radius;
                if(c.Sign()<0)return false;
                if(aa.Sign()>0&&b.Sign()<0&&(b+aa).Sign()>0&&(aa*c-b*b).Sign()<0)return false;
            }
            const auto& d=points[0];const auto u=Sub(points[1],d),v=Sub(points[2],d);
            const auto a=Dot(u,u),b=Dot(u,v),c=Dot(v,v),du=Dot(d,u),dv=Dot(d,v);
            const auto determinant=a*c-b*b;
            if(determinant.Sign()>0) {
                const auto x=b*dv-c*du,y=b*du-a*dv;
                if(x.Sign()>=0&&y.Sign()>=0&&(determinant-x-y).Sign()>=0
                    &&((Dot(d,d)-radius*radius)*determinant+du*x+dv*y).Sign()<0)return false;
            }
        }
        return true;
    }
    int Classify(const Triangle& triangle,std::size_t index,const Polygon& polygon,double radius,const Vertex& center) const {
        const auto& geometry=m_geometry[index];const auto& op=geometry.GetOperation();
        // All accept/reject decisions use exact signs of the canonical binary64
        // recipe. A heuristic distance margin must never certify a surface cell.
        int insideClass=0;
        if(!insideClass) {
            bool allInside=true;for(const auto& point:triangle.vertices)allInside=allInside&&Inside(Coordinates(point),index);
            if(allInside)insideClass=1;
            for(const auto& face:m_faces[index]) {
                bool outside=true;bool positive=false;
                for(const auto& point:triangle.vertices) {
                    const auto sign=Distance(Coordinates(point),face).Sign();outside=outside&&sign>=0;positive=positive||sign>0;
                }
                if(outside&&(!face.closed||positive))insideClass=-1;
            }
            if(!insideClass&&(op.geometryType==CropShape::Sphere||op.geometryType==CropShape::Cylinder)
                &&OutsideRadius(polygon,index,Nearest(polygon,index)))insideClass=-1;
        }
        return op.removalMode==CropRemovalMode::KeepInside?insideClass:-insideClass;
    }
    std::vector<double> Roots(const Vertex& a,const Vertex& b,std::size_t index) const {
        std::vector<double> roots{0,1};const auto start=Coordinates(a),end=Coordinates(b);
        const auto add=[&](double t){if(std::isfinite(t)&&t>0&&t<1)roots.push_back(t);};
        for(const auto& face:m_faces[index]) {
            const auto first=Distance(start,face),last=Distance(end,face);const auto divisor=first-last;
            if(divisor.Sign())add(first.Value()/divisor.Value());
        }
        const auto shape=m_geometry[index].GetOperation().geometryType;
        if(shape==CropShape::Sphere||shape==CropShape::Cylinder) {
            const auto d=Relative(start,m_geometry[index]),e=Sub(Relative(end,m_geometry[index]),d);
            const auto aa=Dot(e,e),bb=N(2)*Dot(d,e),cc=Dot(d,d)-N(m_geometry[index].GetOperation().radius)*N(m_geometry[index].GetOperation().radius);
            if(aa.Sign()) {
                const auto discriminant=bb*bb-N(4)*aa*cc;
                if(discriminant.Sign()>=0) {
                    const double disc=discriminant.Value();if(disc<0||!std::isfinite(disc))CropMeshExact::Precision();
                    const double q=-0.5*(bb.Value()+std::copysign(std::sqrt(disc),bb.Value()));
                    if(q!=0){add(q/aa.Value());add(cc.Value()/q);}
                    else add(-bb.Value()/(2*aa.Value()));
                }
            } else if(bb.Sign())add(-cc.Value()/bb.Value());
        }
        std::sort(roots.begin(),roots.end());roots.erase(std::unique(roots.begin(),roots.end()),roots.end());return roots;
    }
    bool EdgeHasInterior(const Vertex& a,const Vertex& b,std::size_t index,Vertex& witness) const {
        struct Ratio final {N n,d;};
        const auto compare=[](const Ratio& x,const Ratio& y){return (x.n*y.d-y.n*x.d).Sign();};
        Ratio low{N(0),N(1)},high{N(1),N(1)};
        const auto first=Coordinates(a),last=Coordinates(b);
        for(const auto& face:m_faces[index]) {
            const auto start=Distance(first,face),slope=Distance(last,face)-start;
            if(!slope.Sign()) {if(start.Sign()>0)return false;continue;}
            Ratio root{-start,slope};if(root.d.Sign()<0){root.n=-root.n;root.d=-root.d;}
            if(slope.Sign()>0){if(compare(root,high)<0)high=root;}
            else if(compare(root,low)>0)low=root;
        }
        if(compare(low,high)>=0)return false;
        const auto d=Relative(first,m_geometry[index]),e=Sub(Relative(last,m_geometry[index]),d);
        const auto aa=Dot(e,e),bb=Dot(d,e),cc=Dot(d,d)-N(m_geometry[index].GetOperation().radius)*N(m_geometry[index].GetOperation().radius);
        Ratio minimum=low;
        if(aa.Sign()>0) {
            Ratio stationary{-bb,aa};minimum=compare(stationary,low)<0?low:compare(stationary,high)>0?high:stationary;
        } else if(bb.Sign()<0)minimum=high;
        if((aa*minimum.n*minimum.n+N(2)*bb*minimum.n*minimum.d+cc*minimum.d*minimum.d).Sign()>=0)return false;
        witness=Mix(a,b,minimum.n.Value()/minimum.d.Value());return true;
    }
    Vertex Boundary(const Vertex& a,const Vertex& b,std::size_t index,bool first) const {
        double low=0,high=1;
        // Analytic roots are hints only. Exact endpoint classifications bracket
        // the unique crossing; a rounded root never decides topology.
        for(const auto candidate:Roots(a,b,index))if(candidate>low&&candidate<high) {
            if(Kept(Mix(a,b,candidate),index)==first)low=candidate;else high=candidate;
        }
        for(int step=0;step<64;++step) {
            const double middle=low+(high-low)*0.5;
            if(middle==low||middle==high)break;
            if(Kept(Mix(a,b,middle),index)==first)low=middle;else high=middle;
        }
        return Mix(a,b,low+(high-low)*0.5);
    }
    bool ClipCurve(const Polygon& polygon,std::size_t index,Polygon& out,Vertex& seed,bool& hasSeed) const {
        if(polygon.empty())return true;
        bool allKept=true,anyKept=false;std::size_t runs=0;
        bool previous=Kept(polygon.back(),index);
        for(const auto& point:polygon){const bool kept=Kept(point,index);allKept=allKept&&kept;anyKept=anyKept||kept;if(kept&&!previous)++runs;previous=kept;}
        if(m_geometry[index].GetOperation().removalMode==CropRemovalMode::RemoveInside&&runs>1)return false;
        if(allKept||!anyKept) {
            auto nearestPolygon=polygon;
            if(m_geometry[index].GetOperation().geometryType==CropShape::Cylinder)
                for(const auto& face:m_faces[index])nearestPolygon=ClipFace(nearestPolygon,face,true);
            if(nearestPolygon.size()>=3){seed=Nearest(nearestPolygon,index);hasSeed=true;}
            if(allKept) {
                const bool keep=m_geometry[index].GetOperation().removalMode==CropRemovalMode::KeepInside;
                if(keep||OutsideRadius(polygon,index,Nearest(polygon,index))){out=polygon;return true;}
                return false; // a hole cannot be discarded by a vertex-only test.
            }
            return false; // an interior kept island requires a seed or further subdivision.
        }
        out.reserve(polygon.size()+2);
        for(std::size_t i=0;i<polygon.size();++i) {
            const auto& a=polygon[i];const auto& b=polygon[(i+1)%polygon.size()];
            const bool first=Kept(a,index),last=Kept(b,index);
            if(first==last&&!Inside(Coordinates(a),index)&&!Inside(Coordinates(b),index)
                &&EdgeHasInterior(a,b,index,seed)) {hasSeed=true;return false;}
            if(first)Append(out,a);
            if(first!=last)Append(out,Boundary(a,b,index,first));
        }
        if(out.size()>1&&Same(out.front(),out.back()))out.pop_back();return true;
    }
    bool ClipLeaf(const Triangle& triangle,const std::vector<int>& classes,std::vector<Polygon>& result,Vertex& seed,bool& hasSeed) const {
        result={Polygon(triangle.vertices.begin(),triangle.vertices.end())};
        for(std::size_t index=0;index<m_geometry.size();++index) {
            if(classes[index]>0)continue;
            const auto& op=m_geometry[index].GetOperation();std::vector<Polygon> next;
            for(const auto& polygon:result) {
                if(op.geometryType==CropShape::Box&&op.removalMode==CropRemovalMode::RemoveInside) {
                    Polygon remaining=polygon;
                    for(const auto& face:m_faces[index]) {
                        auto outside=ClipFace(remaining,face,false);if(outside.size()>=3)next.push_back(std::move(outside));
                        remaining=ClipFace(remaining,face,true);if(remaining.size()<3)break;
                    }
                } else if(op.geometryType==CropShape::Box||op.geometryType==CropShape::Plane) {
                    Polygon clipped=polygon;
                    for(const auto& face:m_faces[index])clipped=ClipFace(clipped,face,op.removalMode==CropRemovalMode::KeepInside);
                    if(clipped.size()>=3)next.push_back(std::move(clipped));
                } else {
                    Polygon clipped;if(!ClipCurve(polygon,index,clipped,seed,hasSeed))return false;
                    if(clipped.size()>=3)next.push_back(std::move(clipped));
                }
                if(next.size()>m_params.maxCells)throw Failure{CropFailure::ResourceLimit,"Mesh boundary component budget was exhausted."};
                std::size_t bytes=0;for(const auto& p:next)AddBytes(bytes,p.capacity()*sizeof(Vertex)*4);Memory(bytes);
            }
            result=std::move(next);if(result.empty())return false;
        }
        return true;
    }
    double Area(const std::array<Vertex,3>& triangle) const {
        const auto u=Difference(triangle[1].point,triangle[0].point),v=Difference(triangle[2].point,triangle[0].point);
        return 0.5*std::hypot(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]);
    }
    bool Nonzero(const std::array<Vertex,3>& triangle) const {
        const auto& a=triangle[0];const auto& b=triangle[1];const auto& c=triangle[2];
        return ((N(b.u)-N(a.u))*(N(c.v)-N(a.v))-(N(b.v)-N(a.v))*(N(c.u)-N(a.u))).Sign()!=0;
    }
    void Emit(const std::array<Vertex,3>& triangle,double error) {
        if(!Nonzero(triangle))return;
        if(!(Area(triangle)>0))throw Failure{CropFailure::PrecisionNotMet,"A retained mesh patch cannot be represented without degeneracy."};
        if(m_emitted>=m_params.maxCells||m_emitted>=static_cast<std::size_t>(std::numeric_limits<vtkIdType>::max()/3))
            throw Failure{CropFailure::ResourceLimit,"Mesh output cell budget was exhausted."};
        for(const auto& vertex:triangle) {
            const auto delta=Sub(Coordinates(vertex),Values(vertex.point));
            const double actual=std::hypot(delta[0].Value(),delta[1].Value(),delta[2].Value());
            error=std::max(error,std::nextafter(actual*(1+16*std::numeric_limits<double>::epsilon()),std::numeric_limits<double>::infinity()));
        }
        if(error>m_params.meshTolerance)throw Failure{CropFailure::PrecisionNotMet,"Mesh output coordinates exceed the requested error bound."};
        ++m_emitted;Memory(m_stack.capacity()*sizeof(Triangle));
        vtkIdType outputIds[3];
        for(int i=0;i<3;++i) {
            outputIds[i]=m_points->InsertNextPoint(triangle[i].point.data());
            if(outputIds[i]<0)throw Failure{CropFailure::LowRam,"Mesh point allocation failed."};
            double weights[3]={1-triangle[i].u-triangle[i].v,triangle[i].u,triangle[i].v};
            m_output->GetPointData()->InterpolatePoint(m_input->GetPointData(),outputIds[i],m_ids,weights);
            if(auto* normals=m_output->GetPointData()->GetNormals()) {
                double n[3];normals->GetTuple(outputIds[i],n);const double length=std::hypot(n[0],n[1],n[2]);
                if(length>0&&std::isfinite(length))for(double& value:n)value/=length;
                normals->SetTuple(outputIds[i],n);
            }
        }
        const auto cell=m_cells->InsertNextCell(3,outputIds);
        if(cell<0)throw Failure{CropFailure::LowRam,"Mesh cell allocation failed."};
        m_output->GetCellData()->CopyData(m_input->GetCellData(),m_cell,cell);
        m_errorBound=std::max(m_errorBound,error);
    }
    void Split(const Triangle& triangle,const Vertex& seed,bool hasSeed) {
        if(triangle.depth>=m_params.maxDepth)throw Failure{CropFailure::PrecisionNotMet,"Mesh connectivity or boundary precision could not be certified within maxDepth."};
        // A strict interior seed reveals islands/holes which edge signs cannot detect.
        const auto& v=triangle.vertices;
        const auto orient=[](const Vertex& a,const Vertex& b,const Vertex& c){return ((N(b.u)-N(a.u))*(N(c.v)-N(a.v))-(N(b.v)-N(a.v))*(N(c.u)-N(a.u))).Sign();};
        const int sign=orient(v[0],v[1],v[2]);
        const bool interior=sign&&orient(v[0],v[1],seed)==sign&&orient(v[1],v[2],seed)==sign&&orient(v[2],v[0],seed)==sign;
        if(hasSeed&&interior) {
            for(int i=0;i<3;++i)m_stack.push_back({{v[i],v[(i+1)%3],seed},triangle.depth+1});
        } else {
            int edge=0;double longest=-1;
            for(int i=0;i<3;++i){const double length=Length(Difference(v[i].point,v[(i+1)%3].point));if(length>longest){edge=i;longest=length;}}
            auto middle=Mix(v[edge],v[(edge+1)%3],0.5);
            if(hasSeed)for(int i=0;i<3;++i) {
                const auto& a=v[i];const auto& b=v[(i+1)%3];
                if(!orient(a,b,seed)&&!Same(a,seed)&&!Same(b,seed)
                    &&seed.u>=std::min(a.u,b.u)&&seed.u<=std::max(a.u,b.u)
                    &&seed.v>=std::min(a.v,b.v)&&seed.v<=std::max(a.v,b.v)) {edge=i;middle=seed;break;}
            }
            if(Same(middle,v[edge])||Same(middle,v[(edge+1)%3]))CropMeshExact::Precision();
            m_stack.push_back({{v[edge],middle,v[(edge+2)%3]},triangle.depth+1});
            m_stack.push_back({{middle,v[(edge+1)%3],v[(edge+2)%3]},triangle.depth+1});
        }
        Memory(m_stack.capacity()*sizeof(Triangle));
    }
    void Process(const Triangle& triangle) {
        if(!Nonzero(triangle.vertices))return;
        const Polygon polygon(triangle.vertices.begin(),triangle.vertices.end());const auto center=Center(polygon);
        double radius=0,diameter=0;
        for(int i=0;i<3;++i){radius=std::max(radius,Length(Difference(triangle.vertices[i].point,center.point)));diameter=std::max(diameter,Length(Difference(triangle.vertices[i].point,triangle.vertices[(i+1)%3].point)));}
        std::vector<int> classes;classes.reserve(m_geometry.size());std::size_t activeCurves=0;bool activeRemoval=false,uncertain=false;
        for(std::size_t i=0;i<m_geometry.size();++i) {
            m_operation=m_geometry[i].GetOperation().operationIndex;
            const int classification=Classify(triangle,i,polygon,radius,center);
            if(classification<0)return;classes.push_back(classification);uncertain=uncertain||classification==0;
            const auto& op=m_geometry[i].GetOperation();
            if(classification==0&&(op.geometryType==CropShape::Sphere||op.geometryType==CropShape::Cylinder)) {
                ++activeCurves;activeRemoval=activeRemoval||op.removalMode==CropRemovalMode::RemoveInside;
            }
        }
        m_operation=0;
        if(!uncertain){Emit(triangle.vertices,0);return;}
        Vertex seed=center;bool hasSeed=false;
        if(m_positionError*8>=m_params.meshTolerance)throw Failure{CropFailure::PrecisionNotMet,"Mesh coordinate precision cannot meet the requested tolerance."};
        const double error=std::nextafter(diameter+8*m_positionError,std::numeric_limits<double>::infinity());
        if(error<=m_params.meshTolerance&&!(activeRemoval&&activeCurves>1)) {
            std::vector<Polygon> clipped;
            if(ClipLeaf(triangle,classes,clipped,seed,hasSeed)) {
                bool hasArea=false,allPatchesHaveInterior=true;
                for(const auto& patch:clipped) {
                    bool patchArea=false,patchInterior=Interior(Center(patch));
                    for(const auto& point:patch)patchInterior=patchInterior||Interior(point);
                    for(std::size_t i=1;i+1<patch.size();++i)patchArea=patchArea||Nonzero({patch[0],patch[i],patch[i+1]});
                    hasArea=hasArea||patchArea;
                    allPatchesHaveInterior=allPatchesHaveInterior&&(!patchArea||patchInterior);
                }
                if(hasArea&&allPatchesHaveInterior) {
                    for(const auto& patch:clipped)for(std::size_t i=1;i+1<patch.size();++i)Emit({patch[0],patch[i],patch[i+1]},error);
                    m_areaBound=std::nextafter(m_areaBound+Area(triangle.vertices)+8*diameter*m_positionError+8*m_positionError*m_positionError,std::numeric_limits<double>::infinity());
                    return;
                }
            }
        }
        Split(triangle,seed,hasSeed);
    }
    vtkPolyData* m_source;const CropBuildParams& m_params;const std::vector<CropGeometry>& m_geometry;
    const std::function<bool()>& m_stop;std::uint64_t& m_operation;
    vtkPolyData* m_input=nullptr;vtkIdList* m_ids=nullptr;vtkIdType m_cell=0;
    std::array<P,3> m_root{};std::vector<std::vector<Face>> m_faces;std::vector<Triangle> m_stack;
    vtkSmartPointer<vtkPolyData> m_output;vtkSmartPointer<vtkPoints> m_points;vtkSmartPointer<vtkCellArray> m_cells;
    std::size_t m_limit=0,m_fixed=0,m_triangleBytes=0,m_visited=0,m_emitted=0;
    double m_positionError=0,m_errorBound=0,m_areaBound=0;
};
}

CropMaterializationCandidate CropMeshAlgorithm::GetResult(vtkPolyData* mesh,const CropBuildParams& params,
    const std::vector<CropGeometry>& geometry,const std::function<bool()>& getStopRequested)
{
    std::uint64_t operation=0;
    CropMaterializationCandidate failure;failure.sourceRevision=params.sourceRevision;failure.operations=params.operations;failure.nodeCount=params.nodeCount;
    try {return Engine(mesh,params,geometry,getStopRequested,operation).Run();}
    catch(const Failure& error){failure.failureReason=error.reason;failure.message=error.message;failure.isCancelled=error.reason==CropFailure::Cancelled;}
    catch(const std::bad_alloc&){failure.failureReason=CropFailure::LowRam;failure.message="Mesh allocation failed.";}
    catch(...){failure.failureReason=CropFailure::WorkerFailed;failure.message="Mesh clipping failed before publication.";}
    failure.failureOperationIndex=operation;return failure;
}
