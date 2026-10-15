#include "AlignmentSection.h"
#include "AlignmentMath.h"
#include "AlignmentAllocation.h"
#include <map>
#include <set>
#include <numeric>

namespace {
using namespace AlignmentMath;
using namespace AlignmentMemory;
using Key=std::array<std::uint64_t,2>;
template<class T> using VectorList=std::vector<T,AccountingAllocator<T>>;
template<class K,class V> using Map=std::map<K,V,std::less<K>,AccountingAllocator<std::pair<const K,V>>>;
struct Node final {
    Key edge{}; Vec point{}; double fraction=0;
    std::size_t parent=0,degree=0; std::array<std::size_t,2> segments{};
};
struct Segment final {
    std::array<std::size_t,2> nodes{};
    std::uint64_t triangle=0;
    unsigned incidences=1;
    int onEdgeSide=0;
    bool isValid=false;
};
struct Component final {
    std::size_t root=0,segments=0;
    bool closed=true;
    double distance=std::numeric_limits<double>::infinity();
};
void Require(bool value,const char *message) { if (!value) throw std::invalid_argument(message); }
void Check(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work)!=AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Section extraction cancelled.");
}
Vec Vertex(const SurfaceMeshPayload &mesh,std::uint64_t id) {
    const auto &v=mesh.GetPointValues(); return {v[3*id],v[3*id+1],v[3*id+2]};
}
std::size_t Root(VectorList<Node> &nodes,std::size_t id) {
    auto root=id; while (nodes[root].parent!=root) root=nodes[root].parent;
    while (nodes[id].parent!=id) { const auto next=nodes[id].parent; nodes[id].parent=root; id=next; }
    return root;
}
double DistanceSquared(const Vec &point,const Vec &a,const Vec &b) {
    const auto d=b-a; const double t=std::clamp((point-a).dot(d)/d.dot(d),0.0,1.0);
    const auto q=point-a-t*d; return q.dot(q);
}
const MeshField *FaceValidity(const AlignmentWork &work) {
    const MeshField *result=nullptr;
    for (const auto &a:work.mesh->GetCellFields()) if (a.name=="measurement.valid") {
        Require(!result && a.componentCount==1 && a.values.size()==work.mesh->GetTriangleIndices().size()/3,"Invalid face validity schema."); result=&a;
    }
    Require(result || !work.recipe.requiresMeasurementQuality,"Section requires published face validity for measurement-quality input.");
    return result;
}
std::array<double,3> Barycentric(const Node &node,const std::array<std::uint64_t,3> &triangle) {
    std::array<double,3> weights{};
    for (std::size_t j=0;j<3;++j) {
        if (triangle[j]==node.edge[0]) weights[j]+=1-node.fraction;
        if (triangle[j]==node.edge[1]) weights[j]+=node.fraction;
    }
    Require(std::abs(weights[0]+weights[1]+weights[2]-1)<1e-10,"Section provenance is not on its source triangle."); return weights;
}
// 使用平面局部坐标，先按包围盒筛选，再拒绝非相邻段相交/接触及相邻段回折。
void CheckSimple(const AlignmentWork &work,const VectorList<Node> &nodes,const VectorList<std::size_t> &order,
                 const Vec &normal,double tolerance,AllocationAccounting &budget) {
    struct Edge final { std::size_t index; double x0,x1,y0,y1; };
    const auto u=Tangent(normal),v=normal.cross(u),origin=nodes[order[0]].point;
    VectorList<std::array<double,2>> p(0,AccountingAllocator<std::array<double,2>>{&budget}); p.reserve(order.size());
    VectorList<Edge> edges(0,AccountingAllocator<Edge>{&budget}); edges.reserve(order.size());
    for (auto id:order) { const auto d=nodes[id].point-origin; p.push_back({d.dot(u),d.dot(v)}); }
    double twiceArea=0;
    for (std::size_t i=0;i<p.size();++i) {
        if (i%256==0) Check(work);
        const auto &a=p[i],&b=p[(i+1)%p.size()]; twiceArea+=a[0]*b[1]-a[1]*b[0];
        edges.push_back({i,std::min(a[0],b[0]),std::max(a[0],b[0]),std::min(a[1],b[1]),std::max(a[1],b[1])});
    }
    Require(std::isfinite(twiceArea) && std::abs(twiceArea)>tolerance*tolerance,"Section loop has zero or unresolved area.");
    std::size_t checks=0;
    std::sort(edges.begin(),edges.end(),[&](const auto &a,const auto &b) { if (++checks%256==0) Check(work); return a.x0==b.x0 ? a.index<b.index : a.x0<b.x0; });
    const auto orientation=[&](const auto &a,const auto &b,const auto &c) {
        const double cross=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        const double limit=tolerance*std::hypot(b[0]-a[0],b[1]-a[1]); return cross>limit ? 1 : cross< -limit ? -1 : 0;
    };
    for (std::size_t i=0;i<edges.size();++i) for (std::size_t j=i+1;j<edges.size() && edges[j].x0<=edges[i].x1+tolerance;++j) {
        if (++checks%256==0) Check(work);
        const auto a=edges[i].index,b=edges[j].index;
        if (edges[j].y0>edges[i].y1+tolerance || edges[i].y0>edges[j].y1+tolerance) continue;
        if ((a+1)%p.size()==b || (b+1)%p.size()==a) {
            const auto first=(a+1)%p.size()==b ? a : b;
            const auto &x=p[first],&y=p[(first+1)%p.size()],&z=p[(first+2)%p.size()];
            Require(orientation(x,y,z)!=0 || (y[0]-x[0])*(z[0]-y[0])+(y[1]-x[1])*(z[1]-y[1])>=0,"Section contains a backtracking edge.");
            continue;
        }
        const auto &a0=p[a],&a1=p[(a+1)%p.size()],&b0=p[b],&b1=p[(b+1)%p.size()];
        Require(orientation(a0,a1,b0)*orientation(a0,a1,b1)>0 || orientation(b0,b1,a0)*orientation(b0,b1,a1)>0,
                "Section loop intersects or touches itself.");
    }
}
} // namespace

bool AlignmentSection::GetValid(const InspectionSection &s) {
    return GetDataRevisionRefValid(s.pinnedMesh) && Finite(Vector(s.planePoint)) && GetUnitDirectionValid(s.planeNormal) &&
        Positive(s.sampleSpacing) && Positive(s.planeTolerance) && s.planeTolerance<s.sampleSpacing/4 &&
        Positive(s.maxSelectionDistance) && (!s.selectionPoint || Finite(Vector(*s.selectionPoint)));
}
AlignmentSectionData AlignmentSection::Build(const AlignmentWork &work,const InspectionSection &s,const AlignmentMatrix &pose) {
    Check(work); Require(GetValid(s) && Rigid(pose) && work.mesh && work.mesh->GetValid() && s.pinnedMesh==work.input.mesh,"Invalid section input or mesh revision.");
    const auto inverse=Inverse(pose); const auto origin=Transform(inverse,Vector(s.planePoint)),normal=Unit(Rotate(inverse,Vector(s.planeNormal)));
    const auto &mesh=*work.mesh; const auto &tri=mesh.GetTriangleIndices(); const auto *validity=FaceValidity(work);
    AllocationAccounting budget{std::numeric_limits<std::size_t>::max(),0};
    budget.Acquire(4096);
    // MSVC Debug 的 allocator-only 构造标为 noexcept，但会分配迭代器代理。
    // 使用 count=0 构造，让预算耗尽正常抛出而非调用 terminate。
    VectorList<Node> nodes(0,AccountingAllocator<Node>{&budget});
    VectorList<Segment> segments(0,AccountingAllocator<Segment>{&budget});
    Map<Key,std::size_t> nodeIds{std::less<Key>{},AccountingAllocator<std::pair<const Key,std::size_t>>{&budget}};
    Map<Key,std::size_t> segmentIds{std::less<Key>{},AccountingAllocator<std::pair<const Key,std::size_t>>{&budget}};
    std::set<std::uint64_t,std::less<std::uint64_t>,AccountingAllocator<std::uint64_t>> seen{std::less<std::uint64_t>{},AccountingAllocator<std::uint64_t>{&budget}};
    const auto nodeAt=[&](Key key) {
        if (key[1]<key[0]) std::swap(key[0],key[1]);
        const auto found=nodeIds.find(key); if (found!=nodeIds.end()) return found->second;
        const auto a=Vertex(mesh,key[0]),b=Vertex(mesh,key[1]); double fraction=0;
        if (key[0]!=key[1]) { const double da=(a-origin).dot(normal),db=(b-origin).dot(normal); fraction=da/(da-db); }
        auto point=a+fraction*(b-a); point-=normal*(point-origin).dot(normal);
        const auto id=nodes.size(); Node node; node.edge=key; node.point=point; node.fraction=fraction; node.parent=id;
        nodes.push_back(node); nodeIds.emplace(key,id); return id;
    };
    const auto count=s.triangles.empty() ? tri.size()/3 : s.triangles.size();
    for (std::size_t i=0;i<count;++i) {
        if (i%256==0) Check(work);
        const auto t=s.triangles.empty() ? i : s.triangles[i]; Require(t<tri.size()/3,"Section triangle index out of bounds.");
        if (!s.triangles.empty()) Require(seen.insert(t).second,"Duplicate section triangle.");
        const std::array<std::uint64_t,3> ids{tri[3*t],tri[3*t+1],tri[3*t+2]};
        std::array<int,3> side{}; unsigned zeros=0; bool positive=false,negative=false;
        for (std::size_t j=0;j<3;++j) {
            const double d=(Vertex(mesh,ids[j])-origin).dot(normal); Require(std::isfinite(d),"Section distance overflows.");
            side[j]=d>s.planeTolerance ? 1 : d< -s.planeTolerance ? -1 : 0; zeros+=side[j]==0 ? 1 : 0;
            positive=positive || side[j]>0; negative=negative || side[j]<0;
        }
        Require(zeros!=3,"Section intersects a coplanar face; a unique contour is unavailable.");
        if (zeros<2 && !(positive && negative)) continue;
        const auto cross=(Vertex(mesh,ids[1])-Vertex(mesh,ids[0])).cross(Vertex(mesh,ids[2])-Vertex(mesh,ids[0]));
        Require(Finite(cross) && cv::norm(cross)>0,"Section intersects a degenerate face.");
        std::array<std::size_t,2> ends{}; std::size_t endCount=0;
        for (std::size_t j=0;j<3;++j) if (!side[j]) ends[endCount++]=nodeAt({ids[j],ids[j]});
        for (std::size_t j=0;j<3;++j) if (side[j]*side[(j+1)%3]<0) ends[endCount++]=nodeAt({ids[j],ids[(j+1)%3]});
        Require(endCount==2 && ends[0]!=ends[1] && cv::norm(nodes[ends[0]].point-nodes[ends[1]].point)>s.planeTolerance,"Section segment is unresolved at the declared tolerance.");
        bool valid=true; for (auto id:ids) valid=AlignmentGeometryFit::GetPointUsable(work,static_cast<std::size_t>(id)) && valid;
        if (validity) { Require(validity->values[t]==0 || validity->values[t]==1,"Invalid face validity value."); valid=valid && validity->values[t]==1; }
        const int onEdgeSide=zeros==2 ? side[0]+side[1]+side[2] : 0;
        const Key key{std::min(ends[0],ends[1]),std::max(ends[0],ends[1])}; const auto existing=segmentIds.find(key);
        if (existing!=segmentIds.end()) {
            auto &segment=segments[existing->second];
            Require(onEdgeSide && segment.onEdgeSide && segment.incidences==1 && onEdgeSide==-segment.onEdgeSide,"Duplicate, tangent or non-manifold section edge.");
            ++segment.incidences; segment.isValid=segment.isValid && valid; continue;
        }
        const auto id=segments.size(); segments.push_back({ends,t,1,onEdgeSide,valid}); segmentIds.emplace(key,id);
        for (auto endpoint:ends) { auto &node=nodes[endpoint]; if (node.degree<2) node.segments[node.degree]=id; ++node.degree; }
        const auto a=Root(nodes,ends[0]),b=Root(nodes,ends[1]); if (a!=b) nodes[b].parent=a;
    }
    Require(!segments.empty(),"Section plane has no contour.");
    Map<std::size_t,std::size_t> componentIds{std::less<std::size_t>{},AccountingAllocator<std::pair<const std::size_t,std::size_t>>{&budget}};
    VectorList<Component> components(0,AccountingAllocator<Component>{&budget});
    for (std::size_t i=0;i<nodes.size();++i) {
        if (i%256==0) Check(work); const auto root=Root(nodes,i);
        auto found=componentIds.find(root); if (found==componentIds.end()) { found=componentIds.emplace(root,components.size()).first; components.push_back({root}); }
        components[found->second].closed=components[found->second].closed && nodes[i].degree==2;
    }
    const auto selection=s.selectionPoint ? Transform(inverse,Vector(*s.selectionPoint)) : Vec{};
    for (const auto &segment:segments) {
        auto &c=components[componentIds.at(Root(nodes,segment.nodes[0]))]; ++c.segments;
        if (segment.onEdgeSide && segment.incidences!=2) c.closed=false;
        if (s.selectionPoint) c.distance=std::min(c.distance,DistanceSquared(selection,nodes[segment.nodes[0]].point,nodes[segment.nodes[1]].point));
    }
    std::size_t selected=0;
    if (s.selectionPoint) {
        double best=std::numeric_limits<double>::infinity(),second=best;
        for (std::size_t i=0;i<components.size();++i) {
            const double d=std::sqrt(components[i].distance);
            if (d<best) { second=best; best=d; selected=i; } else second=std::min(second,d);
        }
        Require(best<=s.maxSelectionDistance && second-best>s.planeTolerance,"Section selection is too far or ambiguous.");
    } else Require(components.size()==1,"Multiple section components require an explicit selection point.");
    Require(components[selected].closed,"Selected section is open, branched or on an unsupported boundary.");
    std::size_t first=nodes.size();
    const auto less=[&](std::size_t a,std::size_t b) { return Point(nodes[a].point)<Point(nodes[b].point); };
    for (std::size_t i=0;i<nodes.size();++i) if (Root(nodes,i)==components[selected].root && (first==nodes.size() || less(i,first))) first=i;
    const auto nextNode=[&](std::size_t segment,std::size_t node) { return segments[segment].nodes[0]==node ? segments[segment].nodes[1] : segments[segment].nodes[0]; };
    auto edge=nodes[first].segments[0]; if (less(nextNode(nodes[first].segments[1],first),nextNode(edge,first))) edge=nodes[first].segments[1];
    VectorList<std::size_t> order(0,AccountingAllocator<std::size_t>{&budget}),path(0,AccountingAllocator<std::size_t>{&budget});
    auto node=first; double perimeter=0;
    do {
        Check(work); Require(segments[edge].isValid,"Selected section crosses invalid measurement faces.");
        order.push_back(node); path.push_back(edge); const auto next=nextNode(edge,node);
        perimeter+=cv::norm(nodes[next].point-nodes[node].point); node=next;
        edge=nodes[node].segments[0]==edge ? nodes[node].segments[1] : nodes[node].segments[0];
        Require(order.size()<=components[selected].segments,"Section traversal did not close.");
    } while (node!=first);
    Require(order.size()==components[selected].segments && order.size()>=3 && Positive(perimeter),"Section loop is invalid.");
    CheckSimple(work,nodes,order,normal,s.planeTolerance,budget);
    const double requested=std::max(8.0,std::ceil(perimeter/s.sampleSpacing));
    Require(std::isfinite(requested),"Section sample count overflows.");
    if (requested>static_cast<double>(std::numeric_limits<int>::max()/2)) throw std::length_error("Section sample count exceeds backend index representation.");
    const auto samples=static_cast<std::size_t>(requested);
    if (samples>(budget.limit-budget.used)/(1024+sizeof(InspectionSectionSample))) throw std::length_error("Section fit storage size accounting overflows.");
    budget.Acquire(samples*(1024+sizeof(InspectionSectionSample)));
    AlignmentSectionData output; output.samples.points.reserve(samples);
    auto trace=std::make_shared<InspectionSectionTrace>(); trace->samples.reserve(samples);
    trace->sourcePlanePoint=Point(origin); trace->sourcePlaneNormal=Point(normal); trace->components=components.size();
    trace->sourceTriangleCount=tri.size()/3;
    trace->closedLoops=std::count_if(components.begin(),components.end(),[](const auto &c) { return c.closed; });
    trace->selectedSegments=path.size(); trace->perimeter=perimeter; trace->sampleSpacing=perimeter/samples;
    std::size_t segmentIndex=0; double begin=0;
    for (std::size_t i=0;i<samples;++i) {
        if (i%256==0) Check(work); const double distance=trace->sampleSpacing*i;
        double length=cv::norm(nodes[order[(segmentIndex+1)%order.size()]].point-nodes[order[segmentIndex]].point);
        while (segmentIndex+1<path.size() && begin+length<distance) { begin+=length; ++segmentIndex; length=cv::norm(nodes[order[(segmentIndex+1)%order.size()]].point-nodes[order[segmentIndex]].point); }
        const double fraction=std::clamp((distance-begin)/length,0.0,1.0); const auto triangle=segments[path[segmentIndex]].triangle;
        const std::array<std::uint64_t,3> ids{tri[3*triangle],tri[3*triangle+1],tri[3*triangle+2]};
        const auto a=Barycentric(nodes[order[segmentIndex]],ids),b=Barycentric(nodes[order[(segmentIndex+1)%order.size()]],ids);
        InspectionSectionSample sample; sample.triangle=triangle; Vec point{};
        for (std::size_t j=0;j<3;++j) { sample.barycentric[j]=a[j]+fraction*(b[j]-a[j]); point+=sample.barycentric[j]*Vertex(mesh,ids[j]); }
        sample.planeCorrection=(point-origin).dot(normal);
        Require(std::isfinite(sample.planeCorrection) && std::abs(sample.planeCorrection)<=s.planeTolerance*2,"Section plane precision exceeds its tolerance.");
        trace->maxPlaneCorrection=std::max(trace->maxPlaneCorrection,std::abs(sample.planeCorrection));
        output.samples.points.push_back(Point(point-sample.planeCorrection*normal)); trace->samples.push_back(sample);
    }
    output.trace=std::move(trace); return output;
}
