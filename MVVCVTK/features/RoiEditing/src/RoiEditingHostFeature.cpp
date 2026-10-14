#include "Host/RoiEditingHostFeature.h"
#include "../../common/FeatureInputReads.h"
#include "FeatureSupport/WidgetBindings.h"
#include "FeatureSupport/RoutedWidgets.h"
#include "Data/DataPayloads.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Support/FeatureOverlayBase.h"
#include "Render/Support/SliceContourPlane.h"

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkRenderer.h>
#include <vtkCellPicker.h>
#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCubeSource.h>
#include <vtkSphereSource.h>
#include <vtkCylinderSource.h>
#include <vtkMath.h>
#include <vtkCutter.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkCompositePolyDataMapper.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkProperty.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <thread>
#include <utility>
#include <future>
#include <atomic>
#include <chrono>
#include <map>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <limits>

namespace {
struct RegionProduct final {
    std::shared_ptr<std::vector<vtkSmartPointer<vtkPolyData>>> blocks=std::make_shared<std::vector<vtkSmartPointer<vtkPolyData>>>();
    std::map<std::string,std::pair<RoiPlane,vtkSmartPointer<vtkPolyData>>> slices;
    bool isEmpty=false;
    bool has3D=false;
    bool isComplete=false;
};
using RegionMesh=std::shared_ptr<const RegionProduct>;
bool GetMatrixValid(const std::array<double,16>& m)
{
    if (!std::all_of(m.begin(),m.end(),[](double x){return std::isfinite(x);})
        || m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1) return false;
    vtkNew<vtkMatrix4x4> matrix; matrix->DeepCopy(m.data());
    const double determinant=matrix->Determinant();
    return std::isfinite(determinant) && determinant!=0;
}

bool SameDefinition(const RoiDefinition& a,const RoiDefinition& b)
{
    if(a.source!=b.source || a.nodes.size()!=b.nodes.size())return false;
    for(std::size_t i=0;i<a.nodes.size();++i){const auto& x=a.nodes[i];const auto& y=b.nodes[i];const auto& p=x.primitive;const auto& q=y.primitive;
        if(x.kind!=y.kind || x.left!=y.left || x.right!=y.right || p.shape!=q.shape || p.localToSource!=q.localToSource || p.origin!=q.origin || p.normal!=q.normal || p.mask!=q.mask || p.boundaryPolicy!=q.boundaryPolicy || p.radius!=q.radius || p.height!=q.height || p.contours!=q.contours || p.depth!=q.depth || p.sourceToView!=q.sourceToView || p.viewToClip!=q.viewToClip || p.path!=q.path || p.voxels!=q.voxels || p.labels!=q.labels)return false;
    }return true;
}
std::array<double,3> MapRoiPoint(vtkMatrix4x4* matrix,const std::array<double,3>& p)
{
    double input[4]{p[0],p[1],p[2],1},out[4]{};matrix->MultiplyPoint(input,out);return {out[0]/out[3],out[1]/out[3],out[2]/out[3]};
}
std::vector<std::array<double,3>> GetControlPoints(const RoiPrimitive& primitive)
{
    if(primitive.shape==RoiShape::Brush)return primitive.path;
    std::vector<std::array<double,3>> points;const double depth=(primitive.depth[0]+primitive.depth[1])*.5*(primitive.shape==RoiShape::ProjectedContour?std::hypot(primitive.sourceToView[8],primitive.sourceToView[9],primitive.sourceToView[10]):1);
    vtkNew<vtkMatrix4x4> plane;plane->DeepCopy(primitive.localToSource.data());
    vtkNew<vtkMatrix4x4> inverseView;inverseView->DeepCopy(primitive.sourceToView.data());inverseView->Invert();
    vtkNew<vtkMatrix4x4> inverseClip;inverseClip->DeepCopy(primitive.viewToClip.data());inverseClip->Invert();
    for(const auto& ring:primitive.contours)for(const auto& point:ring){
        if(primitive.shape==RoiShape::Contour)points.push_back(MapRoiPoint(plane,{point[0],point[1],depth}));
        else if(primitive.shape==RoiShape::ProjectedContour){const auto a=MapRoiPoint(inverseClip,{point[0],point[1],-1}),b=MapRoiPoint(inverseClip,{point[0],point[1],1});const double t=(-depth-a[2])/(b[2]-a[2]);std::array<double,3> view{};for(int k=0;k<3;++k)view[k]=a[k]+t*(b[k]-a[k]);const auto source=MapRoiPoint(inverseView,view);if(std::all_of(source.begin(),source.end(),[](double v){return std::isfinite(v);}))points.push_back(source);}
    }return points;
}

class RoiOverlay final : public FeatureOverlayBase {
public:
    explicit RoiOverlay(HostRenderViewRole role)
    {
        m_role=role;
        m_cube->SetBounds(-1,1,-1,1,-1,1);
        m_transformFilter->SetInputConnection(m_cube->GetOutputPort());
        m_transformFilter->SetTransform(m_transform);
        const int axis=role==HostRenderViewRole::TopDownSlice ? 2
            : role==HostRenderViewRole::FrontBackSlice ? 1 : role==HostRenderViewRole::LeftRightSlice ? 0:-1;
        if (axis>=0) {
            m_normal={}; m_normal[axis]=1; m_plane->SetNormal(m_normal.data());
            m_cutter->SetCutFunction(m_plane); m_cutter->SetInputConnection(m_transformFilter->GetOutputPort());
            m_mapper->SetInputConnection(m_cutter->GetOutputPort());
        } else m_mapper->SetInputConnection(m_transformFilter->GetOutputPort());
        m_mapper->ScalarVisibilityOff(); m_actor->SetMapper(m_mapper);
        m_actor->GetProperty()->SetRepresentationToWireframe(); m_actor->GetProperty()->SetColor(1,0.7,0.15);
        m_actor->GetProperty()->SetLighting(false); m_actor->GetProperty()->SetLineWidth(2);
        m_actor->PickableOff(); AttachProp(m_actor);
        m_regionMapper->SetInputDataObject(m_regionData);m_regionMapper->ScalarVisibilityOff();m_regionActor->SetMapper(m_regionMapper);
        m_regionActor->PickableOff();m_regionActor->SetVisibility(false);m_regionActor->GetProperty()->SetRepresentationToWireframe();
        m_regionActor->GetProperty()->SetLighting(false);m_regionActor->GetProperty()->SetLineWidth(2);
        if(axis<0)AttachProp(m_regionActor);
        m_handleMapper->SetInputData(m_handles);m_handleActor->SetMapper(m_handleMapper);m_handleActor->GetProperty()->SetColor(1,.9,.3);m_handleActor->GetProperty()->SetPointSize(7);m_handleActor->GetProperty()->SetLighting(false);m_handleActor->PickableOff();AttachProp(m_handleActor);
    }
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    void SetOverlayState(const FeatureOverlayState& state) override
    {
        (void)SliceContourPlane::SetPlane(*m_plane,state.cursor,m_normal,state.modelToWorld);
        Set3DPropsTransform(state.modelToWorld);
        if(m_regionRead){const auto plane=GetSlicePlane();if(plane.origin!=m_slicePlane.origin || plane.normal!=m_slicePlane.normal){m_sliceDirty=true;m_actor->SetVisibility(false);}}
    }
    RoiPlane GetSlicePlane() const {RoiPlane p;std::copy_n(m_plane->GetOrigin(),3,p.origin.begin());std::copy_n(m_plane->GetNormal(),3,p.normal.begin());return p;}
    void SetPrimitive(const RoiPrimitive& primitive,bool isVisible,bool isDraft=true)
    {
        m_regionRead.reset();m_sliceDirty=false;m_regionActor->SetVisibility(false);
        m_regionData->Initialize();m_regionBlocks.reset();m_regions.reset();m_blockCount=0;
        vtkNew<vtkMatrix4x4> matrix;
        if (primitive.shape==RoiShape::Box || primitive.shape==RoiShape::Ellipsoid) {
            matrix->DeepCopy(primitive.localToSource.data());
            if (primitive.shape==RoiShape::Box) m_transformFilter->SetInputConnection(m_cube->GetOutputPort());
            else {m_sphere->SetCenter(0,0,0);m_sphere->SetRadius(1);m_sphere->SetThetaResolution(48);m_sphere->SetPhiResolution(32);m_transformFilter->SetInputConnection(m_sphere->GetOutputPort());}
        } else if (primitive.shape==RoiShape::Sphere) {
            m_sphere->SetCenter(primitive.origin.data());m_sphere->SetRadius(primitive.radius);
            m_sphere->SetThetaResolution(48);m_sphere->SetPhiResolution(32);
            m_transformFilter->SetInputConnection(m_sphere->GetOutputPort());
        } else {
            auto axis=primitive.normal;vtkMath::Normalize(axis.data());
            std::array<double,3> u{},v{};vtkMath::Perpendiculars(axis.data(),u.data(),v.data(),0);
            // VTK 圆柱沿局部 Y 轴；这里只近似显示，公共 ROI 查询仍使用精确圆柱。
            for(int row=0;row<3;++row) {
                matrix->SetElement(row,0,u[row]);matrix->SetElement(row,1,axis[row]);
                matrix->SetElement(row,2,v[row]);matrix->SetElement(row,3,primitive.origin[row]);
            }
            m_cylinder->SetRadius(primitive.radius);m_cylinder->SetHeight(primitive.height);
            m_cylinder->SetResolution(64);m_cylinder->CappingOn();
            m_transformFilter->SetInputConnection(m_cylinder->GetOutputPort());
        }
        m_transform->SetMatrix(matrix); m_actor->SetVisibility(isVisible);
        m_actor->GetProperty()->SetColor(isDraft ? 1.0 : 0.1, isDraft ? 0.7 : 0.95, isDraft ? 0.15 : 1.0);
    }
    void SetHandles(const RoiPrimitive& primitive,bool visible){const auto controls=GetControlPoints(primitive);vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> cells;for(const auto& p:controls){const auto id=points->InsertNextPoint(p.data());cells->InsertNextCell(1,&id);}m_handles->SetPoints(points);m_handles->SetVerts(cells);m_handleActor->SetVisibility(visible && !controls.empty());}
    void SetColour(const std::array<double,3>& colour){m_colour=colour;m_actor->GetProperty()->SetColor(colour[0],colour[1],colour[2]);m_regionActor->GetProperty()->SetColor(colour[0],colour[1],colour[2]);}
    void SetRegion(const RegionMesh& regions,const RoiReadSnapshot& read,bool isVisible,bool isDraft,const std::string& viewId){
        m_actor->SetVisibility(false);
        const bool slice=m_role==HostRenderViewRole::TopDownSlice || m_role==HostRenderViewRole::FrontBackSlice || m_role==HostRenderViewRole::LeftRightSlice;
        if(slice){
            if(m_regionRead!=read){m_regionRead=read;m_sliceDirty=true;}
            const auto found=regions?regions->slices.find(viewId):std::map<std::string,std::pair<RoiPlane,vtkSmartPointer<vtkPolyData>>>::const_iterator{};
            const auto plane=GetSlicePlane();
            if(regions && found!=regions->slices.end() && found->second.first.origin==plane.origin && found->second.first.normal==plane.normal){m_mapper->SetInputData(found->second.second);m_slicePlane=plane;m_sliceDirty=false;}
            m_actor->SetVisibility(isVisible && !m_sliceDirty);const auto colour=m_colour.value_or(std::array<double,3>{isDraft?1.:.1,isDraft?.7:.95,isDraft?.15:1.});m_actor->GetProperty()->SetColor(colour[0],colour[1],colour[2]);return;
        }
        const auto blocks=regions?regions->blocks:nullptr;
        if(m_regionBlocks!=blocks || (blocks && blocks->size()<m_blockCount)){m_regionData->Initialize();m_blockCount=0;m_regionBlocks=blocks;}
        if(blocks)for(;m_blockCount<blocks->size();++m_blockCount){
            if(m_blockCount>=std::numeric_limits<unsigned>::max())throw RoiError::TooLarge;
            m_regionData->SetBlock(static_cast<unsigned>(m_blockCount),(*blocks)[m_blockCount]);}
        m_regions=regions;const auto colour=m_colour.value_or(std::array<double,3>{isDraft?1.:.1,isDraft?.7:.95,isDraft?.15:1.});
        m_regionActor->SetVisibility(isVisible && blocks && !blocks->empty());m_regionActor->GetProperty()->SetColor(colour[0],colour[1],colour[2]);
    }
    void SetRegionVisible(bool visible){m_actor->SetVisibility(visible && (!m_regionRead || !m_sliceDirty));m_regionActor->SetVisibility(visible && m_blockCount>0);}
    bool GetSliceDirty()const noexcept{return m_sliceDirty;}
    bool GetIsSlice()const noexcept{return m_role==HostRenderViewRole::TopDownSlice || m_role==HostRenderViewRole::FrontBackSlice || m_role==HostRenderViewRole::LeftRightSlice;}
    static vtkSmartPointer<vtkPolyData> BuildSlice(const RoiReadSnapshot& read,const DataSnapshot& source,const RoiPlane& slicePlane,const std::shared_ptr<std::atomic<bool>>& cancel){
        const auto image=source?std::dynamic_pointer_cast<const ImageGrid3DPayload>(source->payload):nullptr;if(!image)throw RoiError::UnsupportedRoi;
        const auto prepared=read->GetQuery();if(prepared.error!=RoiError::None || !prepared.query)throw prepared.error;const auto& query=prepared.query;const auto& grid=image->GetGeometry();
        const auto point=[&](const std::array<double,3>& i){auto p=grid.origin;for(int r=0;r<3;++r)for(int a=0;a<3;++a)p[r]+=grid.direction[r*3+a]*grid.spacing[a]*i[a];return p;};
        std::array<double,3> coefficient{};double constant=0;
        for(int r=0;r<3;++r){constant+=slicePlane.normal[r]*(grid.origin[r]-slicePlane.origin[r]);for(int a=0;a<3;++a)coefficient[a]+=slicePlane.normal[r]*grid.direction[r*3+a]*grid.spacing[a];}
        int axis=0;for(int a=1;a<3;++a)if(std::abs(coefficient[a])>std::abs(coefficient[axis]))axis=a;
        if(!std::isfinite(constant) || !std::all_of(coefficient.begin(),coefficient.end(),[](double value){return std::isfinite(value);}))throw RoiError::InvalidGeometry;
        if(coefficient[axis]==0)throw std::runtime_error("Invalid ROI slice plane.");const int u=(axis+1)%3,v=(axis+2)%3;
        const double halo=.5*(std::abs(coefficient[0])+std::abs(coefficient[1])+std::abs(coefficient[2]))/std::abs(coefficient[axis]);
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> lines;
        // 直接读取平面相交的体素层；不先生成整个三维边界。
        for(std::int64_t y=grid.extent[u*2];y<=grid.extent[u*2+1];++y)for(std::int64_t z=grid.extent[v*2];z<=grid.extent[v*2+1];++z){
            if(cancel->load())throw RoiError::Cancelled;
            const double center=-(constant+coefficient[u]*y+coefficient[v]*z)/coefficient[axis];const double low=std::max(double(grid.extent[axis*2]),std::ceil(center-halo)),high=std::min(double(grid.extent[axis*2+1]),std::floor(center+halo));
            if(low>high)continue;
            for(std::int64_t x=static_cast<std::int64_t>(low);x<=high;++x){std::array<double,3> index{};index[axis]=double(x);index[u]=double(y);index[v]=double(z);if(!query->GetContains(point(index)))continue;
                for(int faceAxis=0;faceAxis<3;++faceAxis)for(int side=0;side<2;++side){auto neighbour=index;neighbour[faceAxis]+=side?1:-1;if(query->GetContains(point(neighbour)))continue;
                    std::array<std::array<double,3>,4> corners;const int a=(faceAxis+1)%3,b=(faceAxis+2)%3;
                    for(int k=0;k<4;++k){auto i=index;i[faceAxis]+=side?.5:-.5;i[a]+=(k==1 || k==2)?.5:-.5;i[b]+=k>=2?.5:-.5;for(int n=0;n<3;++n)i[n]=std::clamp(i[n],double(grid.extent[n*2]),double(grid.extent[n*2+1]));corners[k]=i;}
                    std::vector<std::array<double,3>> intersections;
                    const auto distance=[&](const auto& i){return constant+coefficient[0]*i[0]+coefficient[1]*i[1]+coefficient[2]*i[2];};
                    for(int k=0;k<4;++k){const auto& first=corners[k];const auto& second=corners[(k+1)%4];const double d=distance(first),e=distance(second);if(d==0)intersections.push_back(first);if((d<0 && e>0)||(d>0 && e<0)){auto i=first;for(int n=0;n<3;++n)i[n]+=d/(d-e)*(second[n]-first[n]);intersections.push_back(i);}}
                    if(intersections.size()>=2){vtkIdType ids[2];for(int k=0;k<2;++k){const auto p=point(intersections[k]);ids[k]=points->InsertNextPoint(p.data());}lines->InsertNextCell(2,ids);}
                }
            }
        }
        auto mesh=vtkSmartPointer<vtkPolyData>::New();mesh->SetPoints(points);mesh->SetLines(lines);return mesh;
    }
private:
    HostRenderViewRole m_role=HostRenderViewRole::Primary3D;
    std::optional<std::array<double,3>> m_colour;
    RegionMesh m_regions;
    std::shared_ptr<std::vector<vtkSmartPointer<vtkPolyData>>> m_regionBlocks;
    std::size_t m_blockCount=0;
    vtkSmartPointer<vtkMultiBlockDataSet> m_regionData=vtkSmartPointer<vtkMultiBlockDataSet>::New();
    vtkSmartPointer<vtkCompositePolyDataMapper> m_regionMapper=vtkSmartPointer<vtkCompositePolyDataMapper>::New();
    vtkSmartPointer<vtkActor> m_regionActor=vtkSmartPointer<vtkActor>::New();
    RoiReadSnapshot m_regionRead;RoiPlane m_slicePlane;bool m_sliceDirty=false;
    vtkSmartPointer<vtkPolyData> m_handles=vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkPolyDataMapper> m_handleMapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    vtkSmartPointer<vtkActor> m_handleActor=vtkSmartPointer<vtkActor>::New();
    vtkSmartPointer<vtkCubeSource> m_cube=vtkSmartPointer<vtkCubeSource>::New();
    vtkSmartPointer<vtkSphereSource> m_sphere=vtkSmartPointer<vtkSphereSource>::New();
    vtkSmartPointer<vtkCylinderSource> m_cylinder=vtkSmartPointer<vtkCylinderSource>::New();
    vtkSmartPointer<vtkTransform> m_transform=vtkSmartPointer<vtkTransform>::New();
    vtkSmartPointer<vtkTransformPolyDataFilter> m_transformFilter=vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    vtkSmartPointer<vtkPlane> m_plane=vtkSmartPointer<vtkPlane>::New();
    std::array<double,3> m_normal{0,0,1};
    vtkSmartPointer<vtkCutter> m_cutter=vtkSmartPointer<vtkCutter>::New();
    vtkSmartPointer<vtkPolyDataMapper> m_mapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    vtkSmartPointer<vtkActor> m_actor=vtkSmartPointer<vtkActor>::New();
};
}

class RoiEditingHostFeature::Impl final {
public:
    explicit Impl(RoiEditingConfig config):m_config(std::move(config)) {}
    ~Impl() { if (ClearDraft()) {StopDisplay();ClearCommitted();} }
    struct Binding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureViewService> view;
        std::shared_ptr<RoiOverlay> overlay;
        std::string viewId;
    };
    struct Shown final {DataRevisionRef source;RoiReadSnapshot read;std::vector<Binding> bindings;RegionMesh mesh;bool isVisible=true;RoiError displayError=RoiError::None;};
    RoiError ShowRoi(const DataRevisionRef& ref,bool visible)
    {
        const auto graph=m_data->GetDataGraph();const auto data=m_data->GetData(graph,ref);
        const auto* geometry=data ? dynamic_cast<const RoiGeometryPayload*>(data->payload.get()):nullptr;
        if (!geometry) return RoiError::MissingInput;
        const auto resolved=m_data->GetRoi(graph,ref,geometry->GetDefinition().source);if (!resolved.roi) return resolved.error;
        for (auto it=m_shown.begin();it!=m_shown.end();) {
            if (it->first.entityId==ref.entityId && it->first!=ref) {for (auto& b:it->second.bindings) b.service->RemoveOverlay(b.overlay);it=m_shown.erase(it);}else ++it;
        }
        auto& shown=m_shown[ref];shown.read=resolved.roi;shown.source=resolved.roi->GetSource();shown.isVisible=visible;
        if (visible && GetSourceVisible(shown.source)) {
            const auto& definition=shown.read->GetDefinition();const auto& p=definition.nodes.front().primitive;
            const bool simple=definition.nodes.size()==1 && definition.nodes.front().kind==RoiNodeKind::Primitive
                && (p.shape==RoiShape::Box || p.shape==RoiShape::Sphere || p.shape==RoiShape::Cylinder || p.shape==RoiShape::Ellipsoid);
            const bool needsDisplay=!simple && (!shown.mesh || !shown.mesh->isComplete);
            if(!simple && !shown.mesh)shown.mesh=std::make_shared<const RegionProduct>();
            for (const auto& view:m_views->GetViews(m_config.targetViews)) {
                auto port=m_views->GetFeaturePort(view.id);auto overlays=m_views->GetOverlayPort(view.id);
                const auto stamp=port ? port->GetRenderInputStamp():std::nullopt;if (!overlays || !stamp || stamp->dataRevision!=shown.read->GetSource()) continue;
                const auto existing=std::find_if(shown.bindings.begin(),shown.bindings.end(),[&](const auto& b){return b.view==port;});
                if(existing!=shown.bindings.end()){if(!simple && shown.mesh){existing->overlay->SetRegion(shown.mesh,shown.read,true,false,view.id);(void)port->SetRenderNeeded();}continue;}
                auto overlay=std::make_shared<RoiOverlay>(view.role);
                if (simple) overlay->SetPrimitive(p,true,false);
                else if (shown.mesh) overlay->SetRegion(shown.mesh,shown.read,true,false,view.id);
                else {shown.displayError=RoiError::TooLarge;if(!m_state.hasDraft)m_state.displayError=shown.displayError;continue;}
                std::uint64_t hash=1469598103934665603ULL;for(auto byte:ref.entityId.bytes){hash^=byte;hash*=1099511628211ULL;}const double angle=double(hash%1048576)/1048576*6.283185307179586;overlay->SetColour({.55+.4*std::cos(angle),.55+.4*std::cos(angle+2.094395102393195),.55+.4*std::cos(angle+4.188790204786391)});
                shown.bindings.push_back({overlays,port,overlay,view.id});if (!overlays->AttachOverlay(overlay)) {overlays->RemoveOverlay(overlay);shown.bindings.pop_back();shown.displayError=RoiError::Unavailable;if(!m_state.hasDraft)m_state.displayError=shown.displayError;continue;}
                (void)port->SetRenderNeeded();
            }
            if(needsDisplay && !shown.bindings.empty())QueueDisplay(shown.read,ref);
        }
        if (!visible) {for (auto& b:shown.bindings) {b.service->RemoveOverlay(b.overlay);(void)b.view->SetRenderNeeded();}shown.bindings.clear();}
        return RoiError::None;
    }
    void SetMetadata(const RoiMetadata& metadata)
    {
        m_request.metadata=metadata;
        if(m_operation){m_operation->metadata=metadata;const auto* components=std::get_if<RoiComponentParams>(&m_operation->params);
            for(auto& roi:m_state.operation.rois){std::string suffix;
                if(components && components->operation==RoiComponentOperation::Split && !roi.definition.nodes.empty() && !roi.definition.nodes.front().primitive.labels.empty())suffix=" / "+std::to_string(roi.definition.nodes.front().primitive.labels.front());
                roi.metadata=metadata;if(roi.metadata.name.size()+suffix.size()>256){auto end=256-suffix.size();while(end && (static_cast<unsigned char>(roi.metadata.name[end])&0xc0)==0x80)--end;roi.metadata.name.resize(end);}roi.metadata.name+=suffix;
            }
        }
    }
    bool GetDraftCurrent()const{
        if(!m_state.hasDraft)return true;const auto lease=m_lease.lock();const auto stamp=m_reference?m_reference->GetRenderInputStamp():std::nullopt;
        if(!lease || !lease->GetIsActive() || !lease->GetIsOwnerThread() || !stamp || stamp->dataRevision!=m_request.definition.source)return false;
        if(m_request.expectedSourceBinding){const auto binding=m_data->GetDataBinding(m_data->GetDataGraph(),m_request.expectedSourceBinding->name);if(!binding || binding->revision!=m_request.expectedSourceBinding->revision || binding->target!=m_request.expectedSourceBinding->target)return false;}return true;
    }
    RoiError GetAccessError() const noexcept
    {
        if (m_owner!=std::thread::id{} && m_owner!=std::this_thread::get_id()) return RoiError::WrongThread;
        return m_state.isAttached && !m_isPublishing ? RoiError::None:RoiError::Unavailable;
    }
    static bool BuildRegion(const RoiReadSnapshot& read,const DataSnapshot& source,const std::shared_ptr<std::atomic<bool>>& cancel,
        const std::function<void(vtkSmartPointer<vtkPolyData>)>& publish,const std::function<void()>& onBlockComplete)
    {
        if (!read)throw RoiError::Unavailable;
        const auto* image=source ? dynamic_cast<const ImageGrid3DPayload*>(source->payload.get()):nullptr;
        if (!image)throw RoiError::UnsupportedRoi;
        const auto& grid=image->GetGeometry();
        const auto point=[&](const std::array<double,3>& index) {auto p=grid.origin;for (int r=0;r<3;++r) for (int a=0;a<3;++a) p[r]+=grid.direction[r*3+a]*grid.spacing[a]*index[a];return p;};
        bool isEmpty=true;
        const auto prepared=read->GetQuery();if(prepared.error!=RoiError::None)throw prepared.error;if(!prepared.query)throw RoiError::InvalidRequest;
        const auto bounds=read->GetBounds();auto extent=grid.extent;
        vtkNew<vtkMatrix4x4> inverse;for(int r=0;r<3;++r){for(int a=0;a<3;++a)inverse->SetElement(r,a,grid.direction[r*3+a]*grid.spacing[a]);inverse->SetElement(r,3,grid.origin[r]);}inverse->Invert();
        std::array<double,6> indexed{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        for(int a=0;a<3;++a)if(bounds[a*2]>bounds[a*2+1])return true;
        for(unsigned k=0;k<8;++k){const auto p=MapPoint(inverse,{bounds[(k&1)?1:0],bounds[(k&2)?3:2],bounds[(k&4)?5:4]});for(int a=0;a<3;++a){indexed[a*2]=std::min(indexed[a*2],p[a]);indexed[a*2+1]=std::max(indexed[a*2+1],p[a]);}}
        const auto& nodes=read->GetDefinition().nodes;
        const bool discrete=std::none_of(nodes.begin(),nodes.end(),[](const auto& node){return node.kind==RoiNodeKind::Primitive && node.primitive.shape!=RoiShape::MaskReference && node.primitive.shape!=RoiShape::VoxelSet;});
        for(int a=0;a<3;++a){const auto low=std::max(double(extent[a*2]),std::ceil(indexed[a*2])),high=std::min(double(extent[a*2+1]),std::floor(indexed[a*2+1]));if(low>high){if(!discrete)throw RoiError::UnsupportedRoi;return true;}extent[a*2]=int(low);extent[a*2+1]=int(high);}
        // 每块共享顶点，邻接成员从同一真实读取快照查询，不生成块缝。
        for(std::int64_t bz=extent[4];bz<=extent[5];bz+=16)for(std::int64_t by=extent[2];by<=extent[3];by+=16)for(std::int64_t bx=extent[0];bx<=extent[1];bx+=16){
            vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> faces;std::map<std::array<std::int64_t,3>,vtkIdType> vertices;
        for(std::int64_t z=bz;z<=std::min<std::int64_t>(bz+15,extent[5]);++z)for(std::int64_t y=by;y<=std::min<std::int64_t>(by+15,extent[3]);++y)for(std::int64_t x=bx;x<=std::min<std::int64_t>(bx+15,extent[1]);++x) {
            if(cancel->load())throw RoiError::Cancelled;
            const std::array<double,3> index{static_cast<double>(x),static_cast<double>(y),static_cast<double>(z)};
            const auto p=point(index);bool candidate=true;for (int a=0;a<3;++a) if (p[a]<bounds[a*2] || p[a]>bounds[a*2+1]) candidate=false;
            if (!candidate || !prepared.query->GetContains(p)) continue;
            for (int axis=0;axis<3;++axis) for (int side=0;side<2;++side) {
                auto neighbour=index;neighbour[axis]+=side ? 1:-1;if (prepared.query->GetContains(point(neighbour))) continue;
                vtkIdType ids[4];const int a=(axis+1)%3,b=(axis+2)%3;
                for (int corner=0;corner<4;++corner) {auto vertex=index;vertex[axis]+=side ? .5:-.5;vertex[a]+=(corner==1 || corner==2) ? .5:-.5;vertex[b]+=corner>=2 ? .5:-.5;
                    for(int a=0;a<3;++a)vertex[a]=std::clamp(vertex[a],double(grid.extent[a*2]),double(grid.extent[a*2+1]));
                    std::array<std::int64_t,3> key{};for(int n=0;n<3;++n)key[n]=static_cast<std::int64_t>(vertex[n]*2);
                    auto found=vertices.find(key);if(found==vertices.end()){const auto world=point(vertex);found=vertices.emplace(key,points->InsertNextPoint(world.data())).first;}ids[corner]=found->second;}
                faces->InsertNextCell(4,ids);
            }
        }
            if(faces->GetNumberOfCells()){auto mesh=vtkSmartPointer<vtkPolyData>::New();mesh->SetPoints(points);mesh->SetPolys(faces);isEmpty=false;publish(std::move(mesh));}
            onBlockComplete();
        }
        if(isEmpty && !discrete)throw RoiError::UnsupportedRoi;
        return isEmpty;
    }
    RoiError SetDefinition(RoiDefinition definition)
    {
        const auto preview=m_data->GetRoiPreview(m_data->GetDataGraph(),definition);
        if (preview.error!=RoiError::None || !preview.roi) return preview.error;
        m_request.definition=std::move(definition);m_state.draft=m_request.definition;m_state.preview=preview.roi;
        m_geometryChanged=m_history.empty() || !SameDefinition(m_request.definition,m_history.front().request.definition);
        ++m_state.previewRevision;m_geometryDirty=true;
        if(m_displayRunning && !m_displayRunning->ref)CancelDisplay();
        m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[](const auto& work){return !work.ref;}),m_displayQueue.end());
        try{if(!SetProjection()){m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;}}catch(...){m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;}
        return RoiError::None;
    }
    RoiError SetSelection(const RoiPrimitive& primitive)
    {
        if(m_operation && m_state.operation.rois.size()>1)return RoiError::InvalidRequest;
        RoiDefinition selection{m_request.definition.source,{{RoiNodeKind::Primitive,primitive}}};
        auto definition=selection;
        if (m_selection==RoiSelectionMode::Add || m_selection==RoiSelectionMode::Subtract || m_selection==RoiSelectionMode::Intersect) {
            definition=m_applied;if(definition.nodes.size()>roiNodeLimit-2)return RoiError::TooLarge;const auto left=static_cast<std::uint32_t>(definition.nodes.size()-1),right=static_cast<std::uint32_t>(definition.nodes.size());
            definition.nodes.push_back(selection.nodes.front());definition.nodes.push_back({m_selection==RoiSelectionMode::Add ? RoiNodeKind::Union:m_selection==RoiSelectionMode::Subtract ? RoiNodeKind::Difference:RoiNodeKind::Intersection,{},left,right});
        } else if (m_selection!=RoiSelectionMode::Replace) return RoiError::InvalidRequest;
        const auto previous=m_tool;m_tool=primitive;m_toolNodeIndex=definition.nodes.size()==1?0:definition.nodes.size()-2;const auto error=SetDefinition(std::move(definition));
        if(error==RoiError::None){++m_serial;if(m_cancel)m_cancel->store(true);m_pending.reset();m_operation.reset();m_operationApplied=false;m_saveSerial.reset();m_state.operation={};m_state.isComputing=false;m_state.isSaving=false;}
        if (error!=RoiError::None) m_tool=previous;
        return error;
    }
    struct EditState final {
        RoiRequest request;RoiPrimitive tool;std::size_t toolNodeIndex=0;
        RoiSelectionMode selection=RoiSelectionMode::Replace;
        std::optional<DataRevisionRef> updateTarget;
        std::optional<RoiOperationRequest> operation;
        RoiOperationResult candidate;bool operationApplied=false;
    };
    EditState GetEditState()const{return {m_request,m_tool,m_toolNodeIndex,m_selection,m_updateTarget,m_operation,m_state.operation,m_operationApplied};}
    RoiError SetEditState(const EditState& edit){
        const auto preview=m_data->GetRoiPreview(m_data->GetDataGraph(),edit.request.definition);if(preview.error!=RoiError::None)return preview.error;
        ++m_serial;if(m_cancel)m_cancel->store(true);m_pending.reset();m_saveSerial.reset();m_state.isSaving=false;m_state.isComputing=false;
        m_request=edit.request;m_tool=edit.tool;m_toolNodeIndex=edit.toolNodeIndex;m_selection=edit.selection;m_updateTarget=edit.updateTarget;m_operation=edit.operation;m_state.operation=edit.candidate;m_operationApplied=edit.operationApplied;
        m_applied=edit.request.definition;return SetDefinition(edit.request.definition);
    }
    RoiError GetOutputError(RoiOutputMode output)const noexcept{
        if(output!=RoiOutputMode::CreateNew && output!=RoiOutputMode::UpdateTarget)return RoiError::InvalidRequest;
        return output==RoiOutputMode::UpdateTarget && !m_updateTarget?RoiError::MissingInput:RoiError::None;
    }
    RoiError SetOutput(RoiOutputMode output){
        const auto error=GetOutputError(output);if(error!=RoiError::None)return error;
        const auto previous=m_operation?m_operation->output:m_request.action==RoiAction::Create?RoiOutputMode::CreateNew:RoiOutputMode::UpdateTarget;
        if(previous!=output){m_saveSerial.reset();m_state.isSaving=false;}
        m_request.action=output==RoiOutputMode::CreateNew?RoiAction::Create:RoiAction::SetGeometry;
        m_request.expectedRoi=output==RoiOutputMode::CreateNew?std::nullopt:m_updateTarget;
        if(m_operation)m_operation->output=output;if(m_pending)m_pending->output=output;
        auto& text=m_state.operation.provenance.canonicalParameters;const auto at=text.find(";output=");if(at!=std::string::npos){const auto end=text.find(';',at+1);text.replace(at+8,end-(at+8),std::to_string(int(output)));}
        return RoiError::None;
    }
    void AcceptOperation(){
        if(!m_operation)return;
        (void)SetOutput(m_operation->output);
        const auto add=[&](const DataRevisionRef& ref,const char* role){
            if(std::any_of(m_request.inputs.begin(),m_request.inputs.end(),[&](const auto& input){return input.source==ref;}))return;
            std::string name=role;name+=std::to_string(m_request.inputs.size());
            m_request.inputs.push_back({std::move(name),ref});};
        if(m_operation->target)add(*m_operation->target,"operation-input-");
        if(const auto* p=std::get_if<RoiBooleanParams>(&m_operation->params))for(const auto& ref:p->operands)add(ref,"operation-input-");
        if(const auto* p=std::get_if<RoiInvertParams>(&m_operation->params);p && p->domain)add(*p->domain,"operation-input-");
        if(const auto* p=std::get_if<RoiMeshParams>(&m_operation->params))add(p->mesh,"operation-mesh-");
        m_request.provenance=m_state.operation.provenance;
    }
    void SetHistory(){
        m_applied=m_request.definition;m_history.push_back(GetEditState());m_redo.clear();
        m_state.undoCount=m_history.size()-1;m_state.redoCount=0;
    }
    void ApplyCandidate(){
        AcceptOperation();
        const auto* components=m_operation?std::get_if<RoiComponentParams>(&m_operation->params):nullptr;
        m_operationApplied=components && components->operation==RoiComponentOperation::Split;
        if(!m_operationApplied){m_operation.reset();m_state.operation={};}
        SetHistory();
    }
    RoiError StartOperation(const RoiOperationRequest& request,bool begins)
    {
        if (!m_state.hasDraft || m_state.isDragging || (!begins && m_operationApplied)) return RoiError::InvalidRequest;
        if (begins || !m_operation){m_operationStart=m_request.definition;m_operationOrigin=GetEditState();}
        auto operation=request;operation.source=m_request.definition.source;operation.input=m_operationStart;
        operation.target=m_updateTarget;operation.metadata=m_request.metadata;operation.expectedSourceBinding=m_request.expectedSourceBinding;
        if (!operation.target) operation.output=RoiOutputMode::CreateNew;
        ++m_serial;if (m_cancel) m_cancel->store(true);m_pending=operation;m_operation=std::move(operation);m_operationApplied=false;
        m_saveSerial.reset();m_state.isComputing=true;m_state.isSaving=false;m_state.operation={};m_state.operation.error=RoiError::Pending;
        if(m_displayFuture.valid())CancelDisplay();
        StartPendingOperation();return m_state.isComputing?RoiError::Pending:m_state.error;
    }
    void StartPendingOperation()
    {
        if (m_future.valid() || m_displayFuture.valid() || !m_pending) return;
        const auto graph=m_data->GetDataGraph();const auto data=m_data;auto request=std::move(*m_pending);m_pending.reset();
        std::vector<DataRevisionRef> refs{request.source};const auto addRead=[&](const RoiReadSnapshot& read){if(read){const auto deps=read->GetDependencies();refs.insert(refs.end(),deps.begin(),deps.end());}};
        if(request.input)addRead(m_data->GetRoiPreview(graph,*request.input).roi);
        if(request.target){refs.push_back(*request.target);addRead(m_data->GetRoi(graph,*request.target,request.source).roi);}
        if(const auto* p=std::get_if<RoiBooleanParams>(&request.params))for(const auto& ref:p->operands){refs.push_back(ref);addRead(m_data->GetRoi(graph,ref,request.source).roi);}
        if(const auto* p=std::get_if<RoiInvertParams>(&request.params);p && p->domain){refs.push_back(*p->domain);addRead(m_data->GetRoi(graph,*p->domain,request.source).roi);}
        if(const auto* p=std::get_if<RoiMeshParams>(&request.params))refs.push_back(p->mesh);
        if(const auto* p=std::get_if<RoiTransformParams>(&request.params);p && p->targetSource)refs.push_back(*p->targetSource);
        auto started=FeatureInternal::StartInputReads(graph,refs,"roi-operation-reader");if(!started){m_state.isComputing=false;m_state.operation.error=m_state.error=RoiError::Unavailable;return;}
        const auto readers=std::make_shared<const FeatureInternal::InputReads>(std::move(*started));
        m_cancel=std::make_shared<std::atomic<bool>>(false);const auto cancel=m_cancel;const auto original=request.getCancelled;
        request.getCancelled=[cancel,original]{return cancel->load() || (original && original());};m_runningSerial=m_serial;
        std::packaged_task<RoiOperationResult()> task([data,graph,readers,request=std::move(request)]{return data->GetRoiOperation(graph,request);});
        m_future=task.get_future();const std::weak_ptr<FeatureHostControl> host=m_host;
        try {m_worker=std::thread([task=std::move(task),host]()mutable{task();if(const auto owner=host.lock())(void)owner->SendWorkAvailable();});}
        catch(...){m_future={};m_state.isComputing=false;m_state.error=RoiError::TooLarge;m_state.operation.error=m_state.error;}
    }
    void PollOperation()
    {
        if (m_future.valid() && m_future.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            RoiOperationResult candidate;try {candidate=m_future.get();} catch (...) {candidate.error=RoiError::CommitFailed;}
            if(m_worker.joinable())m_worker.join();
            if (m_runningSerial==m_serial) {
                m_state.isComputing=false;m_state.operation=std::move(candidate);SetMetadata(m_request.metadata);m_state.error=m_state.operation.error;
                if (m_state.operation.error==RoiError::None) {
                    RoiDefinition combined{m_request.definition.source,{{RoiNodeKind::Empty}}};
                    if (!m_state.operation.rois.empty()) combined=m_state.operation.rois.front().definition;
                    for (std::size_t i=1;i<m_state.operation.rois.size();++i) {
                        const auto left=static_cast<std::uint32_t>(combined.nodes.size()-1),base=static_cast<std::uint32_t>(combined.nodes.size());
                        for (auto node:m_state.operation.rois[i].definition.nodes) {if (node.kind==RoiNodeKind::Union || node.kind==RoiNodeKind::Intersection || node.kind==RoiNodeKind::Difference) {node.left+=base;node.right+=base;}combined.nodes.push_back(std::move(node));}
                        combined.nodes.push_back({RoiNodeKind::Union,{},left,static_cast<std::uint32_t>(combined.nodes.size()-1)});
                    }
                    if (combined.source==m_request.definition.source) {const auto error=SetDefinition(std::move(combined));if (error!=RoiError::None) m_state.error=error;}
                }
                if (m_saveSerial && *m_saveSerial==m_runningSerial) {m_saveSerial.reset();m_state.isSaving=false;if(m_state.error==RoiError::None) {const auto saved=CommitDraft();m_state.error=saved.error;}}
            }
            StartPendingOperation();StartDisplay();
        }
    }
    struct DisplayWork {RoiReadSnapshot read;DataSnapshot source;std::shared_ptr<const FeatureInternal::InputReads> readers;std::optional<DataRevisionRef> ref;std::uint64_t previewRevision=0;RegionMesh reuse;std::map<std::string,RoiPlane> planes;bool needs3D=false;};
    struct DisplayResult {RoiError error=RoiError::None;bool isEmpty=false;bool has3D=false;};
    struct DisplayBatch {std::string viewId;RoiPlane plane;vtkSmartPointer<vtkPolyData> mesh;};
    struct DisplayChannel {
        std::mutex mutex;std::condition_variable changed;std::deque<DisplayBatch> batches;
        std::map<std::string,RoiPlane> pendingPlanes;
    };
    void CancelDisplay(){
        if(m_displayCancel)m_displayCancel->store(true);
        if(m_displayChannel)m_displayChannel->changed.notify_all();
    }
    void QueueDisplay(const RoiReadSnapshot& read,std::optional<DataRevisionRef> ref={},RegionMesh reuse={})
    {
        if(!read)return;
        const auto matches=[&](const DisplayWork& work){
            if(work.ref!=ref || work.read!=read || (!ref && work.previewRevision!=m_state.previewRevision))return false;
            const auto& bindings=ref?m_shown.at(*ref).bindings:m_bindings;std::size_t count=0;
            for(const auto& binding:bindings)if(binding.overlay->GetIsSlice()){++count;const auto plane=binding.overlay->GetSlicePlane();const auto found=work.planes.find(binding.viewId);if(found==work.planes.end() || found->second.origin!=plane.origin || found->second.normal!=plane.normal)return false;}
            const bool needs3D=std::any_of(bindings.begin(),bindings.end(),[](const auto& binding){return !binding.overlay->GetIsSlice();});
            return count==work.planes.size() && needs3D==work.needs3D;
        };
        if(m_displayRunning && !m_displayCancel->load() && m_displayRunning->ref==ref && m_displayRunning->read==read
            && (ref || m_displayRunning->previewRevision==m_state.previewRevision)){
            const auto& bindings=ref?m_shown.at(*ref).bindings:m_bindings;
            // 平面更新由同一个 worker 在块边界优先处理，不取消正在构建的三维结果。
            std::lock_guard<std::mutex> lock(m_displayChannel->mutex);
            for(const auto& binding:bindings)if(binding.overlay->GetIsSlice()){
                const auto plane=binding.overlay->GetSlicePlane();const auto found=m_displayRunning->planes.find(binding.viewId);
                if(found==m_displayRunning->planes.end() || found->second.origin!=plane.origin || found->second.normal!=plane.normal){
                    m_displayRunning->planes[binding.viewId]=plane;m_displayChannel->pendingPlanes[binding.viewId]=plane;}
            }return;
        }
        if(!ref && std::any_of(m_displayQueue.begin(),m_displayQueue.end(),matches))return;
        if(!ref){m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[](const auto& w){return !w.ref;}),m_displayQueue.end());if(m_displayRunning && !m_displayRunning->ref)CancelDisplay();}
        else if((m_displayRunning && m_displayRunning->ref==ref) || std::any_of(m_displayQueue.begin(),m_displayQueue.end(),[&](const auto& w){return w.ref==ref;}))return;
        const auto graph=m_data->GetDataGraph();auto refs=read->GetDependencies();refs.push_back(read->GetSource());
        auto readers=FeatureInternal::StartInputReads(graph,refs,"roi-display-reader");if(!readers){if(ref)m_shown.at(*ref).displayError=RoiError::Unavailable;else{m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;}return;}
        DisplayWork work{read,m_data->GetData(graph,read->GetSource()),std::make_shared<const FeatureInternal::InputReads>(std::move(*readers)),ref,m_state.previewRevision,std::move(reuse),{}};
        const auto& bindings=ref?m_shown.at(*ref).bindings:m_bindings;
        for(const auto& binding:bindings)if(binding.overlay->GetIsSlice())work.planes.emplace(binding.viewId,binding.overlay->GetSlicePlane());
        work.needs3D=std::any_of(bindings.begin(),bindings.end(),[](const auto& binding){return !binding.overlay->GetIsSlice();});
        m_displayQueue.push_back(std::move(work));if(!ref)m_state.displayStatus=RoiDisplayStatus::Updating;StartDisplay();
    }
    void StartDisplay()
    {
        if(m_future.valid() || m_pending || m_displayFuture.valid() || m_displayQueue.empty())return;
        m_displayRunning=std::move(m_displayQueue.front());m_displayQueue.erase(m_displayQueue.begin());
        m_displayCancel=std::make_shared<std::atomic<bool>>(false);const auto cancel=m_displayCancel;const std::weak_ptr<FeatureHostControl> host=m_host;
        const auto work=*m_displayRunning;
        m_displayChannel=std::make_shared<DisplayChannel>();const auto channel=m_displayChannel;
        m_displayProduct=std::make_shared<RegionProduct>();
        if(work.reuse && work.reuse->has3D){if(work.reuse->blocks)m_displayProduct->blocks=work.reuse->blocks;m_displayProduct->has3D=true;m_displayProduct->isEmpty=work.reuse->isEmpty;}
        std::packaged_task<DisplayResult()> task([work,cancel,channel,host]{
            const auto publish=[&](DisplayBatch batch){
                {std::unique_lock<std::mutex> lock(channel->mutex);
                    channel->changed.wait(lock,[&]{return cancel->load() || channel->batches.size()<8;});
                    if(cancel->load())throw RoiError::Cancelled;
                    channel->batches.push_back(std::move(batch));}
                if(const auto owner=host.lock())(void)owner->SendWorkAvailable();
            };
            const auto updateSlices=[&]{std::map<std::string,RoiPlane> planes;
                {std::lock_guard<std::mutex> lock(channel->mutex);planes.swap(channel->pendingPlanes);}
                for(const auto& plane:planes)publish({plane.first,plane.second,RoiOverlay::BuildSlice(work.read,work.source,plane.second,cancel)});
            };
            try{
                // 切片分别交付；没有三维视图时不扫描/构建三维边界。
                for(const auto& plane:work.planes)publish({plane.first,plane.second,RoiOverlay::BuildSlice(work.read,work.source,plane.second,cancel)});
                if(work.needs3D){
                    if(work.reuse && work.reuse->has3D)return DisplayResult{RoiError::None,work.reuse->isEmpty,true};
                    const bool empty=BuildRegion(work.read,work.source,cancel,[&](auto mesh){publish({{},{},std::move(mesh)});},updateSlices);
                    updateSlices();
                    return DisplayResult{RoiError::None,empty,true};
                }
                // 切片未相交不代表正式区域为空。
                return DisplayResult{RoiError::None,work.reuse?work.reuse->isEmpty:false,work.reuse?work.reuse->has3D:false};
            }catch(RoiError error){return DisplayResult{error,{}};}catch(const std::bad_alloc&){return DisplayResult{RoiError::TooLarge,{}};}catch(...){return DisplayResult{RoiError::Unavailable,{}};}
        });
        m_displayFuture=task.get_future();
        try{m_worker=std::thread([task=std::move(task),host]()mutable{task();if(const auto owner=host.lock())(void)owner->SendWorkAvailable();});}
        catch(...){const auto ref=m_displayRunning->ref;m_displayFuture={};m_displayRunning.reset();
            if(ref)m_shown.at(*ref).displayError=RoiError::Unavailable;
            if(!ref || !m_state.hasDraft){m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;}}
    }
    void PollDisplay()
    {
      try {
        if(!m_displayFuture.valid() || !m_displayRunning)return;
        const auto work=*m_displayRunning;
        const auto current=[&]{
            if(m_displayCancel->load())return false;
            if(!work.ref)return m_state.hasDraft && work.previewRevision==m_state.previewRevision && work.read==m_state.preview && GetDraftCurrent();
            const auto found=m_shown.find(*work.ref);return found!=m_shown.end() && found->second.isVisible && found->second.read==work.read && GetSourceVisible(work.read->GetSource());
        };
        if(!current())CancelDisplay();
        bool changed=false,more=false;
        // 每 tick 至多四批；生产者最多保留八批，取消可唤醒等待中的生产者。
        for(unsigned count=0;count<4;++count){
            DisplayBatch batch;{std::lock_guard<std::mutex> lock(m_displayChannel->mutex);
                if(m_displayChannel->batches.empty())break;
                batch=std::move(m_displayChannel->batches.front());m_displayChannel->batches.pop_front();}
            m_displayChannel->changed.notify_one();
            if(!current())continue;
            if(batch.viewId.empty())m_displayProduct->blocks->push_back(std::move(batch.mesh));
            else m_displayProduct->slices[batch.viewId]={batch.plane,std::move(batch.mesh)};
            changed=true;
        }
        bool finished=false;
        {std::lock_guard<std::mutex> lock(m_displayChannel->mutex);more=!m_displayChannel->batches.empty();
            // 与生产者入队互斥地检查 EOF，避免检查空队列后又入队最后一批。
            finished=!more && m_displayFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready;}
        DisplayResult result;
        if(finished){try{result=m_displayFuture.get();}catch(...){result.error=RoiError::Unavailable;}
            if(m_worker.joinable())m_worker.join();
            if(current()){
                m_displayProduct->isComplete=result.error==RoiError::None;m_displayProduct->has3D=result.has3D;m_displayProduct->isEmpty=result.isEmpty;
                if(result.error!=RoiError::None && !m_displayProduct->has3D)m_displayProduct->blocks->clear();
                changed=true;
            }
        }
        if(changed && current()){
            if(work.ref){auto& shown=m_shown.at(*work.ref);shown.mesh=m_displayProduct;
                if(finished)shown.displayError=result.error;
                for(auto& binding:shown.bindings){const auto stamp=binding.view->GetRenderInputStamp();
                    if(stamp && stamp->dataRevision==shown.source){binding.overlay->SetRegion(shown.mesh,shown.read,shown.isVisible,false,binding.viewId);(void)binding.view->SetRenderNeeded();}}
                if(finished && result.error!=RoiError::None && !m_state.hasDraft){m_state.displayError=result.error;m_state.displayStatus=RoiDisplayStatus::Failed;}
            }else{m_draftMesh=m_displayProduct;
                if(finished){m_state.displayError=result.error;m_state.displayStatus=result.error!=RoiError::None?RoiDisplayStatus::Failed:result.isEmpty?RoiDisplayStatus::Empty:RoiDisplayStatus::Displayed;}
                try{(void)SetProjection();}catch(...){m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;CancelDisplay();}
            }
        }
        if(more && m_host)(void)m_host->SendWorkAvailable();
        if(finished){m_displayRunning.reset();m_displayChannel.reset();m_displayProduct.reset();StartPendingOperation();StartDisplay();}
      }catch(RoiError error){FailDisplay(error);}
       catch(const std::bad_alloc&){FailDisplay(RoiError::TooLarge);}
       catch(...){FailDisplay(RoiError::Unavailable);}
    }
    void FailDisplay(RoiError error){
        CancelDisplay();if(!m_displayRunning)return;const auto& work=*m_displayRunning;
        if(work.ref){const auto found=m_shown.find(*work.ref);if(found!=m_shown.end()){
            found->second.displayError=error;for(const auto& binding:found->second.bindings)if(!binding.overlay->GetIsSlice()){binding.overlay->SetRegionVisible(false);(void)binding.view->SetRenderNeeded();}}}
        else if(m_state.hasDraft && work.previewRevision==m_state.previewRevision){m_state.displayError=error;m_state.displayStatus=RoiDisplayStatus::Failed;
            for(const auto& binding:m_bindings)if(!binding.overlay->GetIsSlice()){binding.overlay->SetRegionVisible(false);(void)binding.view->SetRenderNeeded();}}
        if(!m_displayFuture.valid()){m_displayRunning.reset();m_displayChannel.reset();m_displayProduct.reset();}
    }
    std::optional<RoiDescriptor> GetDraftDescriptor(const DataGraphSnapshot& graph,const DataRevisionRef& ref) const
    {
        const auto binding=m_data->GetDataBinding(graph,roiCatalogBinding);const auto data=binding && binding->target?m_data->GetData(graph,*binding->target):DataSnapshot{};
        const auto* catalog=data?dynamic_cast<const RoiCatalogPayload*>(data->payload.get()):nullptr;if(!catalog)return {};
        const auto found=std::find_if(catalog->GetEntries().begin(),catalog->GetEntries().end(),[&](const auto& e){return e.geometry.entityId==ref.entityId;});if(found==catalog->GetEntries().end())return {};
        return RoiDescriptor{ref,found->geometry,m_request.definition,found->metadata,binding->revision};
    }
    RoiResult CommitDraft()
    {
        RoiResult result;result.error=RoiError::InvalidRequest;
        if (!m_state.hasDraft || m_state.isDragging) return result;
        if (m_state.isComputing) {m_saveSerial=m_serial;m_state.isSaving=true;result.error=RoiError::Pending;return result;}
        if(m_request.expectedSourceBinding){const auto current=m_data->GetDataBinding(m_data->GetDataGraph(),m_request.expectedSourceBinding->name);if(!current || current->revision!=m_request.expectedSourceBinding->revision || current->target!=m_request.expectedSourceBinding->target){result.error=RoiError::RevisionConflict;return result;}}
        m_isPublishing=true;
        std::vector<RoiDescriptor> outputs;
        try {
            if (m_operation) {
                if (m_state.operation.error!=RoiError::None) {m_isPublishing=false;result.error=m_state.operation.error;return result;}
                auto committed=m_data->SetRoiOperationCommit(*m_operation,m_state.operation);result.error=committed.error;result.commitId=committed.commitId;
                if (!committed.rois.empty()) result.roi=committed.rois.front();
                outputs=std::move(committed.rois);
            } else if(m_request.action==RoiAction::SetGeometry && m_request.expectedRoi && !m_geometryChanged){
                const auto graph=m_data->GetDataGraph();const auto descriptor=GetDraftDescriptor(graph,*m_request.expectedRoi);
                if(!descriptor || descriptor->currentRevision!=*m_request.expectedRoi)result.error=RoiError::RevisionConflict;
                else{auto request=m_request;request.action=RoiAction::SetMetadata;request.definition={};request.expectedSourceBinding.reset();request.inputs.clear();request.provenance.reset();
                    if(request.metadata.name.empty())request.metadata=descriptor->metadata;result=m_data->SetRoi(request);}
            }else result=m_data->SetRoi(m_request);
        } catch (...) {m_isPublishing=false;throw;}
        m_isPublishing=false;
        if (result.error==RoiError::None) {
            if (result.roi) m_state.committedRoi=result.roi->revision;
            if (!m_operation && result.roi) outputs.push_back(*result.roi);
            (void)ClearDraft();
            for(const auto& roi:outputs)try{(void)ShowRoi(roi.revision,true);}catch(...){m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;}
        }return result;
    }
    void QueueSliceUpdates(){
        if(m_state.hasDraft && m_draftMesh && std::any_of(m_bindings.begin(),m_bindings.end(),[](const auto& binding){return binding.overlay->GetSliceDirty();}))QueueDisplay(m_state.preview,{},m_draftMesh);
        for(const auto& item:m_shown)if(item.second.read && item.second.mesh && item.second.displayError==RoiError::None && std::any_of(item.second.bindings.begin(),item.second.bindings.end(),[](const auto& binding){return binding.overlay->GetSliceDirty();}))QueueDisplay(item.second.read,item.first,item.second.mesh);
    }
    void StopDisplay()
    {
        CancelDisplay();
        if(m_displayFuture.valid()){m_displayFuture.wait();try{(void)m_displayFuture.get();}catch(...){}}
        if(m_worker.joinable())m_worker.join();m_displayRunning.reset();m_displayChannel.reset();m_displayProduct.reset();
    }
    bool ClearDraft()
    {
        if (m_owner!=std::thread::id{} && m_owner!=std::this_thread::get_id()) return false;
        CancelDrawing(false);m_drawing.reset();
        ++m_serial;if (m_cancel) m_cancel->store(true);
        if (m_future.valid()) {m_future.wait();try {(void)m_future.get();} catch (...) {}}
        CancelDisplay();
        if(!m_displayFuture.valid()){if(m_worker.joinable())m_worker.join();m_displayRunning.reset();m_displayChannel.reset();m_displayProduct.reset();}
        m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[](const auto& w){return !w.ref;}),m_displayQueue.end());
        m_pending.reset();m_operation.reset();m_operationApplied=false;m_saveSerial.reset();m_state.operation={};m_state.isComputing=false;m_state.isSaving=false;
        if (m_widget) {
            m_widget->RemoveAllObservers(); m_widget->Off(); m_widget->SetInteractor(nullptr);
            m_widget=nullptr; m_representation=nullptr;
        }
        try { if (m_reference) (void)m_reference->SetInteracting({"roi-editing","box"},false); } catch (...) {}
        m_state.isDragging=false; m_nativeInputActive=false; m_isDragCancelled=false;
        for (auto& binding:m_bindings) {
            binding.service->RemoveOverlay(binding.overlay);
            try { (void)binding.view->SetRenderNeeded(); } catch (...) {}
        }
        m_bindings.clear(); m_state.hasDraft=false; m_state.draft.reset(); m_request={}; m_reference.reset(); m_lease.reset();
        m_state.preview.reset();m_draftReaders.reset();m_draftMesh.reset();m_history.clear();m_redo.clear();m_state.undoCount=m_state.redoCount=0;
        if(m_host)(void)m_host->SetInputOwner("roi-editing",false);
        return true;
    }
    void ClearCommitted()
    {
        for (auto& item:m_shown) for (auto& binding:item.second.bindings) {binding.service->RemoveOverlay(binding.overlay);try {(void)binding.view->SetRenderNeeded();}catch (...) {}}
        m_shown.clear();
        m_displayQueue.clear();
    }
    bool GetSourceVisible(const DataRevisionRef& ref) const
    {
        const auto data=m_data->GetData(m_data->GetDataGraph(),ref);if(!data)return false;
        if(GetDataEntityIdValid(data->lifetimeScope)){const auto lifetime=data->lifetime.lock();if(!lifetime || !lifetime->GetIsPublished())return false;}
        for(const auto& view:m_views->GetViews(m_config.targetViews)){const auto port=m_views->GetFeaturePort(view.id);const auto stamp=port?port->GetRenderInputStamp():std::nullopt;if(stamp && stamp->dataRevision==ref)return true;}return false;
    }
    void SetCommittedCurrent()
    {
        const auto graph=m_data->GetDataGraph();const auto binding=m_data->GetDataBinding(graph,roiCatalogBinding);
        const auto catalogData=binding && binding->target?m_data->GetData(graph,*binding->target):DataSnapshot{};
        const auto* catalog=catalogData?dynamic_cast<const RoiCatalogPayload*>(catalogData->payload.get()):nullptr;
        std::vector<DataRevisionRef> restore;
        for(auto it=m_shown.begin();it!=m_shown.end();){
            auto& item=it->second;const bool exists=catalog && std::any_of(catalog->GetEntries().begin(),catalog->GetEntries().end(),[&](const auto& e){return e.geometry.entityId==it->first.entityId;});
            const bool sourceVisible=GetSourceVisible(item.source);bool current=exists && sourceVisible;for(const auto& b:item.bindings){const auto stamp=b.view->GetRenderInputStamp();if(!stamp || stamp->dataRevision!=item.source)current=false;}
            if(!current){for(auto& b:item.bindings){b.service->RemoveOverlay(b.overlay);(void)b.view->SetRenderNeeded();}item.bindings.clear();}
            if(!exists){it=m_shown.erase(it);continue;}
            if(item.isVisible && sourceVisible && item.displayError==RoiError::None)for(const auto& view:m_views->GetViews(m_config.targetViews)){const auto port=m_views->GetFeaturePort(view.id);const auto stamp=port?port->GetRenderInputStamp():std::nullopt;if(stamp && stamp->dataRevision==item.source && std::none_of(item.bindings.begin(),item.bindings.end(),[&](const auto& b){return b.view==port;})){restore.push_back(it->first);break;}}
            if(!sourceVisible)item.read.reset();
            ++it;
        }
        m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[&](const auto& work){return work.ref && !GetSourceVisible(work.read->GetSource());}),m_displayQueue.end());
        if(m_displayRunning && m_displayRunning->ref && !GetSourceVisible(m_displayRunning->read->GetSource()))CancelDisplay();
        for(const auto& ref:restore)(void)ShowRoi(ref,true);
    }
    bool SetProjection()
    {
        if (!m_state.hasDraft || !m_reference) return false;
        const auto lease=m_lease.lock();
        if (!lease || !lease->GetIsActive() || !lease->GetIsOwnerThread()) return false;
        const auto matrix=m_reference->GetModelToWorld();
        if (!matrix || !GetMatrixValid(*matrix)) {
            m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;
            if(m_widget)m_widget->SetEnabled(0);
            for(auto& binding:m_bindings){binding.overlay->SetHandles(m_tool,false);binding.overlay->SetRegionVisible(false);(void)binding.view->SetRenderNeeded();}
            return false;
        }
        if (m_state.isDragging && *matrix!=m_displayMatrix) {
            m_state.isDragging=false; m_isDragCancelled=true;
            m_tool.localToSource=m_dragStart;(void)SetDefinition(m_dragDefinition);
            (void)m_reference->SetInteracting({"roi-editing","box"},false);
        }
        if(m_state.displayError==RoiError::Unavailable)m_geometryDirty=true;
        m_displayMatrix=*matrix;
        vtkNew<vtkMatrix4x4> display; display->DeepCopy(matrix->data());
        vtkNew<vtkMatrix4x4> box; box->DeepCopy(m_tool.localToSource.data());
        vtkNew<vtkMatrix4x4> world; vtkMatrix4x4::Multiply4x4(display,box,world);
        vtkNew<vtkTransform> transform; transform->SetMatrix(world);
        if (m_widget) {
            SetWidgetBindings();
            if (!m_state.isDragging) m_representation->SetTransform(transform);
            m_widget->SetEnabled(m_state.isVisible && m_toolNodeIndex<m_request.definition.nodes.size() && m_request.definition.nodes[m_toolNodeIndex].kind==RoiNodeKind::Primitive && m_request.definition.nodes[m_toolNodeIndex].primitive.shape==m_tool.shape && (m_tool.shape==RoiShape::Box || m_tool.shape==RoiShape::Ellipsoid) ? 1:0);
        }
        if (m_geometryDirty) {m_draftMesh=nullptr;m_state.displayError=RoiError::None;
            const auto bounds=m_state.preview->GetBounds();const bool emptyBounds=bounds[0]>bounds[1] || bounds[2]>bounds[3] || bounds[4]>bounds[5];
            if(emptyBounds){m_draftMesh=std::make_shared<const RegionProduct>(RegionProduct{{},{},true});m_state.displayStatus=RoiDisplayStatus::Empty;}
            else if (!(m_request.definition.nodes.size()==1 && m_request.definition.nodes[0].kind==RoiNodeKind::Primitive
                && (m_tool.shape==RoiShape::Box || m_tool.shape==RoiShape::Sphere || m_tool.shape==RoiShape::Cylinder || m_tool.shape==RoiShape::Ellipsoid))) {
                m_state.displayStatus=RoiDisplayStatus::Updating;QueueDisplay(m_state.preview);
            }else m_state.displayStatus=RoiDisplayStatus::Displayed;m_geometryDirty=false;
        }
        for (auto& binding:m_bindings) {
            binding.overlay->SetHandles(m_tool,m_state.isVisible);
            if (m_draftMesh) binding.overlay->SetRegion(m_draftMesh,m_state.preview,m_state.isVisible,true,binding.viewId);
            else if(m_state.displayStatus==RoiDisplayStatus::Displayed)binding.overlay->SetPrimitive(m_request.definition.nodes.front().primitive,m_state.isVisible);
            else binding.overlay->SetRegionVisible(false);
            (void)binding.view->SetRenderNeeded();
        }
        return true;
    }
    void SetWidgetBindings() {
        if(!m_widget||m_state.isDragging||m_nativeInputActive||!m_views)return;
        const auto input=m_views->GetInputView(m_config.referenceView);
        const auto host=m_host;if(!input||!host)return;
        FeatureSupport::SetWidgetBindings(m_widget,host->GetInputBindings(input->view.id),{
            {"roi.box.select.drag",vtkWidgetEvent::Select},{"roi.box.translate.drag",vtkWidgetEvent::Translate},
            {"roi.box.translate.shift",vtkWidgetEvent::Translate},{"roi.box.scale.drag",vtkWidgetEvent::Scale},
            {"roi.box.translate.ctrl",vtkWidgetEvent::Translate}});
    }
    bool FinishBoxGesture() {
        if(!m_reference||!m_reference->SetInteracting({"roi-editing","box"},false))return false;
        const bool applied=!m_isDragCancelled;
        m_nativeInputActive=false;m_state.isDragging=false;m_isDragCancelled=false;
        if(applied)SetHistory();
        SetWidgetBindings();return true;
    }
    InteractionResult OnBoxInput(const InteractionEvent& event) {
        const bool isPress=event.eventKind==InteractionEventKind::PrimaryPress||event.eventKind==InteractionEventKind::SecondaryPress||event.eventKind==InteractionEventKind::MiddlePress;
        if(isPress) {
            const auto matrix=m_reference?m_reference->GetModelToWorld():std::nullopt;
            if(!matrix||!GetMatrixValid(*matrix)||!m_reference->SetInteracting({"roi-editing","box"},true))return {true,true,false,InteractionFailureReason::StateRejected};
            m_nativeInputActive=true;m_state.isDragging=true;m_isDragCancelled=false;m_displayMatrix=*matrix;
            m_dragStart=m_tool.localToSource;m_dragDefinition=m_request.definition;return {true,false,true};
        }
        if(event.eventKind!=InteractionEventKind::Cancel)return {true,false,true};
        const bool active=m_nativeInputActive||m_state.isDragging;
        if(m_state.isDragging) {
            if(SetDefinition(m_dragDefinition)!=RoiError::None)return {true,true,false,InteractionFailureReason::CleanupRejected};
            m_tool.localToSource=m_dragStart;m_isDragCancelled=true;
        }
        if(active&&m_widget)static_cast<FeatureSupport::RoutedBoxWidget*>(m_widget.GetPointer())->StopGesture();
        if((m_state.isDragging||m_nativeInputActive)&&!FinishBoxGesture())return {true,true,false,InteractionFailureReason::CleanupRejected};
        SetWidgetBindings();return {true,true,true};
    }
    void OnWidget(unsigned long event)
    {
        if (GetAccessError()!=RoiError::None || !m_state.hasDraft || !m_reference) return;
        const auto matrix=m_reference->GetModelToWorld();
        if (!matrix || !GetMatrixValid(*matrix)) return;
        if (m_state.isDragging && *matrix!=m_displayMatrix) { (void)SetProjection(); }
        if (event==vtkCommand::StartInteractionEvent) {
            static_cast<FeatureSupport::RoutedBoxWidget*>(m_widget.GetPointer())->ReleaseInputFocus();
            m_nativeInputActive=true;
            if(!m_state.isDragging) {
                m_isDragCancelled=false;
                if (!m_reference->SetInteracting({"roi-editing","box"},true)) return;
                m_state.isDragging=true; m_displayMatrix=*matrix;m_dragStart=m_tool.localToSource;m_dragDefinition=m_request.definition;
            }
        }
        if (m_state.isDragging && !m_isDragCancelled) {
            vtkNew<vtkTransform> world; m_representation->GetTransform(world);
            vtkNew<vtkMatrix4x4> inverse; inverse->DeepCopy(m_displayMatrix.data()); inverse->Invert();
            vtkNew<vtkMatrix4x4> source; vtkMatrix4x4::Multiply4x4(inverse,world->GetMatrix(),source);
            std::array<double,16> next{}; std::copy_n(source->GetData(),16,next.begin());
            if (GetMatrixValid(next) && m_toolNodeIndex<m_dragDefinition.nodes.size()) {auto definition=m_dragDefinition;auto primitive=m_tool;primitive.localToSource=next;definition.nodes[m_toolNodeIndex].primitive=primitive;if(SetDefinition(std::move(definition))==RoiError::None)m_tool=primitive;}
        }
        if (event==vtkCommand::EndInteractionEvent) {
            if(!FinishBoxGesture())return;
        }
        (void)SetProjection();
    }
    static std::array<double,3> MapPoint(vtkMatrix4x4* matrix,const std::array<double,3>& p)
    {
        double input[4]{p[0],p[1],p[2],1},out[4]{};matrix->MultiplyPoint(input,out);
        return {out[0]/out[3],out[1]/out[3],out[2]/out[3]};
    }
    std::optional<std::array<double,3>> GetDrawingPoint(const InteractionEvent& event,const HostInputView& input)
    {
        vtkNew<vtkMatrix4x4> inverse;inverse->DeepCopy(m_drawDisplay.data());inverse->Invert();
        if(*m_drawing==RoiDrawingMode::VisibleBrush || (*m_drawing==RoiDrawingMode::Seed && !m_drawIsSlice) || (*m_drawing==RoiDrawingMode::EditControlPoint && m_drawPrimitive.shape==RoiShape::Brush && m_drawPrimitive.height==0)){
            vtkNew<vtkCellPicker> picker;picker->SetTolerance(.001);
            if(!picker->Pick(event.x,event.y,0,input.renderer))return {};
            const auto* position=picker->GetPickPosition();return MapPoint(inverse,{position[0],position[1],position[2]});
        }
        std::array<std::array<double,3>,2> ray{};
        for(int k=0;k<2;++k){input.renderer->SetDisplayPoint(event.x,event.y,k);input.renderer->DisplayToWorld();
            const auto* p=input.renderer->GetWorldPoint();if(!p || p[3]==0)return {};ray[k]=MapPoint(inverse,{p[0]/p[3],p[1]/p[3],p[2]/p[3]});}
        double dot=0,offset=0;for(int a=0;a<3;++a){dot+=m_drawPlane.normal[a]*(ray[1][a]-ray[0][a]);offset+=m_drawPlane.normal[a]*(m_drawPlane.origin[a]-ray[0][a]);}
        if(std::abs(dot)<1e-12)return {};
        std::array<double,3> p{};for(int a=0;a<3;++a)p[a]=ray[0][a]+offset/dot*(ray[1][a]-ray[0][a]);return p;
    }
    void CancelDrawing(bool restore)
    {
        if(!m_drawView.empty()){const auto port=m_views->GetFeaturePort(m_drawView);if(port)(void)port->SetInteracting({"roi-editing","drawing"},false);}
        const bool active=m_state.isDragging;m_state.isDragging=false;m_drawView.clear();m_drawPoints.clear();
        if(restore && active && m_state.hasDraft){m_tool=m_drawToolStart;m_toolNodeIndex=m_drawToolIndexStart;(void)SetDefinition(m_drawStart);}
    }
    InteractionResult OnInput(const InteractionEvent& event)
    {
        if(GetAccessError()!=RoiError::None || !m_state.hasDraft || !m_drawing)return {};
        if(event.eventKind==InteractionEventKind::Cancel){
            CancelDrawing(true);return {true,true,true};
        }
        if(event.phase!=InputPhase::Begin && event.phase!=InputPhase::Update && event.phase!=InputPhase::End)return {};
        const auto input=m_views->GetInputView({event.viewId});if(!input || !input->renderer)return {};
        const auto lease=input->lease.lock();if(!lease || !lease->GetIsActive() || !lease->GetIsOwnerThread())return {};
        const auto port=m_views->GetFeaturePort(event.viewId);const auto stamp=port?port->GetRenderInputStamp():std::nullopt;
        if(!stamp || stamp->dataRevision!=m_request.definition.source)return {};
        const bool slice=input->view.role==HostRenderViewRole::TopDownSlice || input->view.role==HostRenderViewRole::FrontBackSlice || input->view.role==HostRenderViewRole::LeftRightSlice;
        const bool planarShape=*m_drawing==RoiDrawingMode::SliceRectangle || *m_drawing==RoiDrawingMode::SliceEllipse || *m_drawing==RoiDrawingMode::SliceCircle;
        const bool edits=*m_drawing==RoiDrawingMode::EditControlPoint;
        const bool wantsSlice=edits?slice:planarShape || *m_drawing==RoiDrawingMode::SliceContour || *m_drawing==RoiDrawingMode::SliceBrush;
        if((*m_drawing!=RoiDrawingMode::Seed && !edits && slice!=wantsSlice) || m_state.isComputing)return {};
        if(event.phase==InputPhase::Begin){
            CancelDrawing(false);m_state.error=RoiError::None;m_drawStart=m_request.definition;m_drawToolStart=m_tool;m_drawToolIndexStart=m_toolNodeIndex;m_drawPoints.clear();m_drawView=event.viewId;m_drawIsSlice=slice;
            const auto matrix=port->GetModelToWorld();if(!matrix)return {};m_drawDisplay=*matrix;
            if(slice){const auto binding=std::find_if(m_bindings.begin(),m_bindings.end(),[&](const auto& b){return b.view==port;});if(binding==m_bindings.end())return {};m_drawPlane=binding->overlay->GetSlicePlane();}
            if(!port->SetInteracting({"roi-editing","drawing"},true))return {};m_state.isDragging=true;
            if(edits){
                m_drawPrimitive=m_tool;const auto controls=GetControlPoints(m_tool);double best=144.;m_controlIndex=SIZE_MAX;
                for(std::size_t i=0;i<controls.size();++i){vtkNew<vtkMatrix4x4> display;display->DeepCopy(matrix->data());const auto p=MapPoint(display,controls[i]);input->renderer->SetWorldPoint(p[0],p[1],p[2],1);input->renderer->WorldToDisplay();const auto* q=input->renderer->GetDisplayPoint();const double distance=(q[0]-event.x)*(q[0]-event.x)+(q[1]-event.y)*(q[1]-event.y);if(distance<best){best=distance;m_controlIndex=i;}}
                if(m_controlIndex==SIZE_MAX){CancelDrawing(false);return {};}
                if(m_tool.shape==RoiShape::ProjectedContour){vtkNew<vtkMatrix4x4> inverse;inverse->DeepCopy(m_tool.sourceToView.data());inverse->Invert();m_drawPlane.origin=MapPoint(inverse,{0,0,-(m_tool.depth[0]+m_tool.depth[1])*.5*std::hypot(m_tool.sourceToView[8],m_tool.sourceToView[9],m_tool.sourceToView[10])});m_drawPlane.normal={m_tool.sourceToView[8],m_tool.sourceToView[9],m_tool.sourceToView[10]};}
                else{m_drawPlane.origin={m_tool.localToSource[3],m_tool.localToSource[7],m_tool.localToSource[11]};m_drawPlane.normal={m_tool.localToSource[2],m_tool.localToSource[6],m_tool.localToSource[10]};}
                m_drawPlaneMatrix=m_drawPrimitive.localToSource;
            }else if(*m_drawing==RoiDrawingMode::ProjectedContour){
                vtkNew<vtkMatrix4x4> display;display->DeepCopy(matrix->data());vtkNew<vtkMatrix4x4> frozen;
                vtkMatrix4x4::Multiply4x4(input->renderer->GetActiveCamera()->GetModelViewTransformMatrix(),display,frozen);
                m_drawPrimitive={};m_drawPrimitive.shape=RoiShape::ProjectedContour;std::copy_n(frozen->GetData(),16,m_drawPrimitive.sourceToView.begin());
                std::copy_n(input->renderer->GetActiveCamera()->GetProjectionTransformMatrix(input->renderer->GetTiledAspectRatio(),-1,1)->GetData(),16,m_drawPrimitive.viewToClip.begin());
                const auto domain=m_data->GetRoiPreview(m_data->GetDataGraph(),{m_request.definition.source,{{RoiNodeKind::SourceDomain}}});if(!domain.roi){CancelDrawing(true);return {true,true,false,InteractionFailureReason::StateRejected};}const auto bounds=domain.roi->GetBounds();m_drawPrimitive.depth={INFINITY,0};
                for(int k=0;k<8;++k){const auto p=MapPoint(frozen,{bounds[(k&1)?1:0],bounds[(k&2)?3:2],bounds[(k&4)?5:4]});m_drawPrimitive.depth[0]=std::min(m_drawPrimitive.depth[0],-p[2]);m_drawPrimitive.depth[1]=std::max(m_drawPrimitive.depth[1],-p[2]);}
                const double depthScale=std::hypot(m_drawPrimitive.sourceToView[8],m_drawPrimitive.sourceToView[9],m_drawPrimitive.sourceToView[10]);m_drawPrimitive.depth[0]=std::max(0.,m_drawPrimitive.depth[0]/depthScale);m_drawPrimitive.depth[1]/=depthScale;
                input->renderer->GetTiledSizeAndOrigin(&m_drawViewport[0],&m_drawViewport[1],&m_drawViewport[2],&m_drawViewport[3]);
            }else if(*m_drawing!=RoiDrawingMode::Seed){
                m_drawPrimitive={};m_drawPrimitive.shape=planarShape?(*m_drawing==RoiDrawingMode::SliceRectangle?RoiShape::Box:RoiShape::EllipticCylinder):wantsSlice && *m_drawing==RoiDrawingMode::SliceContour?RoiShape::Contour:RoiShape::Brush;
                if(m_drawPrimitive.shape==RoiShape::Brush)m_drawPrimitive.radius=m_tool.radius>0?m_tool.radius:1.;
                if(slice){auto normal=m_drawPlane.normal;vtkMath::Normalize(normal.data());std::array<double,3> u{},v{};vtkMath::Perpendiculars(normal.data(),u.data(),v.data(),0);
                    for(int r=0;r<3;++r){m_drawPrimitive.localToSource[r*4]=u[r];m_drawPrimitive.localToSource[r*4+1]=v[r];m_drawPrimitive.localToSource[r*4+2]=normal[r];m_drawPrimitive.localToSource[r*4+3]=m_drawPlane.origin[r];}
                    m_drawPrimitive.depth=m_tool.depth;if(m_drawPrimitive.shape==RoiShape::Brush)m_drawPrimitive.height=1;
                }
                m_drawPlaneMatrix=m_drawPrimitive.localToSource;
            }
        }else if(!m_state.isDragging || event.viewId!=m_drawView)return {};
        const auto matrix=port->GetModelToWorld();if(!matrix || *matrix!=m_drawDisplay){CancelDrawing(true);return {true,true,false,InteractionFailureReason::StateRejected};}
        if(*m_drawing==RoiDrawingMode::ProjectedContour){
            if(m_drawViewport[0]<=0 || m_drawViewport[1]<=0){CancelDrawing(true);return {true,true,false,InteractionFailureReason::StateRejected};}
            const std::array<double,2> p{2.*(event.x-m_drawViewport[2])/m_drawViewport[0]-1,2.*(event.y-m_drawViewport[3])/m_drawViewport[1]-1};
            if(m_drawPrimitive.contours.empty())m_drawPrimitive.contours.emplace_back();auto& ring=m_drawPrimitive.contours.front();if(ring.empty() || ring.back()!=p)ring.push_back(p);
        }else{
            const auto point=GetDrawingPoint(event,*input);
            if(point){
                if(edits){
                    if(m_drawPrimitive.shape==RoiShape::Brush)m_drawPrimitive.path[m_controlIndex]=*point;
                    else{std::array<double,2> value{};if(m_drawPrimitive.shape==RoiShape::Contour){vtkNew<vtkMatrix4x4> inverse;inverse->DeepCopy(m_drawPrimitive.localToSource.data());inverse->Invert();const auto local=MapPoint(inverse,*point);value={local[0],local[1]};}
                        else{vtkNew<vtkMatrix4x4> view;view->DeepCopy(m_drawPrimitive.sourceToView.data());const auto p=MapPoint(view,*point);vtkNew<vtkMatrix4x4> clip;clip->DeepCopy(m_drawPrimitive.viewToClip.data());const auto q=MapPoint(clip,p);value={q[0],q[1]};}
                        auto index=m_controlIndex;for(auto& ring:m_drawPrimitive.contours){if(index<ring.size()){ring[index]=value;break;}index-=ring.size();}
                    }
                    auto definition=m_drawStart;definition.nodes[m_toolNodeIndex].primitive=m_drawPrimitive;m_state.error=SetDefinition(std::move(definition));if(m_state.error==RoiError::None)m_tool=m_drawPrimitive;
                }else if(*m_drawing==RoiDrawingMode::Seed){
                    if(event.phase==InputPhase::Begin && m_operation){auto request=*m_operation;auto* threshold=std::get_if<RoiThresholdParams>(&request.params);if(threshold){threshold->seeds.push_back(*point);CancelDrawing(false);m_state.error=StartOperation(request,false);return {true,true,true};}}
                }else if(planarShape){
                    vtkNew<vtkMatrix4x4> inverse;inverse->DeepCopy(m_drawPlaneMatrix.data());inverse->Invert();const auto local=MapPoint(inverse,*point);m_drawPoints.push_back(local);
                    const auto& first=m_drawPoints.front();double x=(first[0]+local[0])*.5,y=(first[1]+local[1])*.5,rx=std::abs(first[0]-local[0])*.5,ry=std::abs(first[1]-local[1])*.5;
                    if(*m_drawing==RoiDrawingMode::SliceCircle){x=first[0];y=first[1];rx=ry=std::hypot(local[0]-first[0],local[1]-first[1]);}
                    m_drawPrimitive.localToSource=m_drawPlaneMatrix;
                    const double z=m_drawPrimitive.shape==RoiShape::Box?(m_drawPrimitive.depth[0]+m_drawPrimitive.depth[1])*.5:0;
                    for(int r=0;r<3;++r){m_drawPrimitive.localToSource[r*4+3]+=m_drawPlaneMatrix[r*4]*x+m_drawPlaneMatrix[r*4+1]*y+m_drawPlaneMatrix[r*4+2]*z;m_drawPrimitive.localToSource[r*4]*=rx;m_drawPrimitive.localToSource[r*4+1]*=ry;if(m_drawPrimitive.shape==RoiShape::Box)m_drawPrimitive.localToSource[r*4+2]*=(m_drawPrimitive.depth[1]-m_drawPrimitive.depth[0])*.5;}
                }else if(m_drawPrimitive.shape==RoiShape::Contour){vtkNew<vtkMatrix4x4> inverse;inverse->DeepCopy(m_drawPrimitive.localToSource.data());inverse->Invert();const auto local=MapPoint(inverse,*point);if(m_drawPrimitive.contours.empty())m_drawPrimitive.contours.emplace_back();auto& ring=m_drawPrimitive.contours.front();const std::array<double,2> p{local[0],local[1]};if(ring.empty() || ring.back()!=p)ring.push_back(p);}
                else {if(m_drawPrimitive.path.empty() || m_drawPrimitive.path.back()!=*point)m_drawPrimitive.path.push_back(*point);}
            }
        }
        const bool valid=edits?m_controlIndex!=SIZE_MAX:planarShape?m_drawPoints.size()>1 && GetMatrixValid(m_drawPrimitive.localToSource):m_drawPrimitive.shape==RoiShape::Brush?!m_drawPrimitive.path.empty():!m_drawPrimitive.contours.empty() && m_drawPrimitive.contours.front().size()>=3;
        if(valid && !edits && *m_drawing!=RoiDrawingMode::Seed){auto primitive=m_drawPrimitive;if(primitive.shape==RoiShape::Box)primitive.depth={-.5,.5};m_state.error=SetSelection(primitive);}
        if(event.phase==InputPhase::End){const bool applied=valid && m_state.error==RoiError::None;CancelDrawing(!applied);if(applied)SetHistory();}
        return {true,true,m_state.error==RoiError::None,m_state.error==RoiError::None?InteractionFailureReason::None:InteractionFailureReason::StateRejected};
    }
    RoiError Begin(const RoiRequest& request)
    {
        if (m_state.hasDraft || !m_bindings.empty()) return RoiError::InvalidRequest;
        if (request.action!=RoiAction::Create && request.action!=RoiAction::SetGeometry) return RoiError::InvalidRequest;
        const auto graph=m_data->GetDataGraph();
        const auto source=m_data->GetData(graph,request.definition.source);
        if (!source) return RoiError::MissingInput;
        const auto preview=m_data->GetRoiPreview(graph,request.definition);
        if (preview.error!=RoiError::None || !preview.roi) return preview.error;
        const auto input=m_views->GetInputView(m_config.referenceView);
        if (!input || !input->renderer || !input->interactor) return RoiError::Unavailable;
        const bool reference3D=input->view.role==HostRenderViewRole::Primary3D || input->view.role==HostRenderViewRole::Composite3D;
        const bool referenceSlice=input->view.role==HostRenderViewRole::TopDownSlice || input->view.role==HostRenderViewRole::FrontBackSlice || input->view.role==HostRenderViewRole::LeftRightSlice;
        if(!reference3D && !referenceSlice)return RoiError::Unavailable;
        const auto lease=input->lease.lock();
        if (!lease || !lease->GetIsActive() || !lease->GetIsOwnerThread()) return RoiError::Unavailable;
        auto reference=m_views->GetFeaturePort(input->view.id);
        const auto stamp=reference ? reference->GetRenderInputStamp():std::nullopt;
        if (!stamp || stamp->dataRevision!=request.definition.source) return RoiError::SourceMismatch;
        if (request.expectedRoi) {
            const auto previous=m_data->GetRoi(graph,*request.expectedRoi,request.definition.source);
            if (previous.error!=RoiError::None) return previous.error;
        }
        auto refs=preview.roi->GetDependencies();if(request.expectedRoi)refs.push_back(*request.expectedRoi);auto draftReaders=FeatureInternal::StartInputReads(graph,refs,"roi-draft-reader");if(!draftReaders)return RoiError::Unavailable;
        const auto targets=m_views->GetViews(m_config.targetViews);
        if (targets.empty() || targets.size()>64) return RoiError::InvalidRequest;
        std::vector<Binding> prepared;
        std::set<std::string> ids;
        for (const auto& view:targets) {
            if (!ids.insert(view.id).second) continue;
            auto port=m_views->GetFeaturePort(view.id); auto overlays=m_views->GetOverlayPort(view.id);
            if (!port || !overlays) return RoiError::Unavailable;
            prepared.push_back({overlays,port,std::make_shared<RoiOverlay>(view.role),view.id});
        }
        if (!ids.count(input->view.id)) return RoiError::InvalidRequest;
        m_bindings.reserve(prepared.size());
        m_draftReaders=std::make_shared<const FeatureInternal::InputReads>(std::move(*draftReaders));m_request=request;
        m_request.expectedCatalogRevision.reset();m_updateTarget=request.expectedRoi;m_selection=RoiSelectionMode::Replace;m_applied=request.definition;m_operationStart=request.definition;
        m_tool=request.definition.nodes.front().primitive;m_toolNodeIndex=0;m_state.preview=preview.roi;m_geometryDirty=true;
        m_history.clear();m_redo.clear();m_geometryChanged=false;++m_state.previewRevision;
        if (!m_request.expectedSourceBinding) {
            const auto primary=m_data->GetDataBinding(graph,primaryVolumeBinding);
            if (primary && primary->target==request.definition.source) m_request.expectedSourceBinding=primary;
        }
        m_reference=std::move(reference); m_lease=input->lease;
        // 所有候选先准备好；部分挂载失败保留清理失败的资源，供 Detach 重试。
        for (auto& binding:prepared) {
            m_bindings.push_back(std::move(binding));
            const auto& attached=m_bindings.back();
            if (!attached.service->AttachOverlay(attached.overlay)) { (void)ClearDraft(); return RoiError::Unavailable; }
        }
        if (reference3D && request.definition.nodes.size()==1 && request.definition.nodes[0].kind==RoiNodeKind::Primitive && request.definition.nodes[0].primitive.shape==RoiShape::Box) {
        m_representation=vtkSmartPointer<vtkBoxRepresentation>::New();
        m_representation->SetPlaceFactor(1);
        double bounds[6]{-1,1,-1,1,-1,1}; m_representation->PlaceWidget(bounds);
        m_widget=vtkSmartPointer<FeatureSupport::RoutedBoxWidget>::New(); m_widget->SetRepresentation(m_representation);
        SetWidgetBindings();
        m_widget->SetInteractor(input->interactor); m_widget->SetCurrentRenderer(input->renderer); m_widget->SetDefaultRenderer(input->renderer);
        vtkNew<vtkCallbackCommand> callback; callback->SetClientData(this);
        callback->SetCallback([](vtkObject*,unsigned long event,void* client,void*) {
            try { static_cast<Impl*>(client)->OnWidget(event); } catch (...) {}
        });
        for (auto event:{vtkCommand::StartInteractionEvent,vtkCommand::InteractionEvent,vtkCommand::EndInteractionEvent})
            m_widget->AddObserver(event,callback);
        }
        m_state.hasDraft=true; m_state.draft=request.definition;
        m_history={GetEditState()};
        if(!SetProjection()){m_state.displayError=RoiError::Unavailable;m_state.displayStatus=RoiDisplayStatus::Failed;}
        return RoiError::None;
    }
    std::optional<RoiDrawingMode> m_drawing;std::string m_drawView;bool m_drawIsSlice=false;
    RoiDefinition m_drawStart;RoiPrimitive m_drawPrimitive,m_drawToolStart;std::size_t m_drawToolIndexStart=0;RoiPlane m_drawPlane;
    std::vector<std::array<double,3>> m_drawPoints;std::array<double,16> m_drawDisplay=roiIdentityMatrix,m_drawPlaneMatrix=roiIdentityMatrix;std::array<int,4> m_drawViewport{};
    RoiEditingConfig m_config;
    std::thread::id m_owner;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<FeatureViewService> m_reference;
    std::weak_ptr<const FeatureViewLease> m_lease;
    std::shared_ptr<const FeatureInternal::InputReads> m_draftReaders;
    std::vector<Binding> m_bindings;
    std::map<DataRevisionRef,Shown> m_shown;
    RoiPrimitive m_tool;
    std::size_t m_toolNodeIndex=0,m_controlIndex=SIZE_MAX;
    RoiDefinition m_applied,m_operationStart,m_dragDefinition;
    std::vector<EditState> m_history,m_redo;
    EditState m_operationOrigin;
    std::optional<DataRevisionRef> m_updateTarget;
    bool m_operationApplied=false;
    std::optional<std::uint64_t> m_saveSerial;
    RoiSelectionMode m_selection=RoiSelectionMode::Replace;
    RegionMesh m_draftMesh;
    bool m_geometryDirty=false,m_geometryChanged=false;
    std::future<DisplayResult> m_displayFuture;
    std::optional<DisplayWork> m_displayRunning;std::vector<DisplayWork> m_displayQueue;
    std::shared_ptr<std::atomic<bool>> m_displayCancel;
    std::shared_ptr<DisplayChannel> m_displayChannel;
    std::shared_ptr<RegionProduct> m_displayProduct;
    std::future<RoiOperationResult> m_future;
    std::thread m_worker;
    std::shared_ptr<FeatureHostControl> m_host;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::optional<RoiOperationRequest> m_pending,m_operation;
    std::uint64_t m_serial=0,m_runningSerial=0;
    vtkSmartPointer<vtkBoxWidget2> m_widget;
    vtkSmartPointer<vtkBoxRepresentation> m_representation;
    RoiRequest m_request;
    RoiEditingState m_state;
    std::array<double,16> m_displayMatrix=roiIdentityMatrix, m_dragStart=roiIdentityMatrix;
    bool m_isDragCancelled=false;
    bool m_nativeInputActive=false;
    bool m_isPublishing=false;
};
RoiEditingHostFeature::RoiEditingHostFeature(RoiEditingConfig config):m_impl(std::make_unique<Impl>(std::move(config))) {}
RoiEditingHostFeature::~RoiEditingHostFeature() noexcept = default;
std::string_view RoiEditingHostFeature::GetFeatureId() const noexcept {return "roi-editing";}
FeatureDataContract RoiEditingHostFeature::GetDataContract() const
{
    return {{{"source-data",DataFacets::scalarGrid3D,false},{"roi",DataFacets::roiGeometry,false}},
        {{"roi",DataTypes::roiGeometry,{DataFacets::roiGeometry}}}};
}
bool RoiEditingHostFeature::AttachHost(const HostFeatureContext& context)
{
    if (m_impl->m_state.isAttached || !context.data || !context.views) return false;
    m_impl->m_owner=std::this_thread::get_id(); m_impl->m_data=context.data; m_impl->m_views=context.views;m_impl->m_host=context.host;
    const auto weak=weak_from_this();
    if(context.host && !weak.expired()) {
        HostInputBinding binding;binding.featureId=std::string(GetFeatureId());binding.targetViews=m_impl->m_config.targetViews;
        InteractionActionInfo draw;draw.actionId="roi.draw";draw.ownerId=binding.featureId;
        draw.bindings.push_back({"roi.draw.drag",{InputTriggerKind::Drag,InputMouseButton::Primary},true,{},binding.featureId,{InputTriggerKind::Drag}});
        binding.actions.push_back(std::move(draw));
        InteractionActionInfo box;box.actionId="roi.box.edit";box.ownerId=binding.featureId;
        const auto addBox=[&](const char* key,InputMouseButton button,std::uint8_t required,std::uint8_t forbidden){
            BindingScope scope;scope.viewClass=InteractionViewClass::ThreeD;
            box.bindings.push_back({key,{InputTriggerKind::Drag,button,required,forbidden},true,scope,"widget:roi",{InputTriggerKind::Drag}});
        };
        addBox("roi.box.select.drag",InputMouseButton::Primary,0,3);
        addBox("roi.box.translate.drag",InputMouseButton::Middle,0,0);
        addBox("roi.box.translate.shift",InputMouseButton::Primary,1,2);
        addBox("roi.box.scale.drag",InputMouseButton::Secondary,0,0);
        addBox("roi.box.translate.ctrl",InputMouseButton::Primary,2,1);
        binding.actions.push_back(std::move(box));
        binding.getIsActionAvailable=[weak](std::string_view id,const InteractionEvent& e){
            const auto f=weak.lock();if(!f)return false;
            if(id=="roi.draw")return f->m_impl->m_drawing.has_value();
            if(id!="roi.box.edit")return true;
            const auto& p=*f->m_impl;const auto input=p.m_views->GetInputView({e.viewId});
            return !p.m_drawing&&p.m_widget&&p.m_widget->GetEnabled()&&input&&input->interactor==p.m_widget->GetInteractor()
                &&p.m_representation->ComputeInteractionState(e.x,e.y)!=vtkBoxRepresentation::Outside;
        };
        binding.getIsCaptureActive=[weak](std::string_view){const auto f=weak.lock();return f&&(f->m_impl->m_state.isDragging||f->m_impl->m_nativeInputActive);};
        binding.onBindingsChanged=[weak](const std::vector<EffectiveBinding>&){if(const auto f=weak.lock())f->m_impl->SetWidgetBindings();};
        for(const auto* id:{"interaction.confirm","interaction.cancel","interaction.undo","interaction.redo","roi.commit"}) {
            InteractionActionInfo info;info.actionId=id;info.ownerId=binding.featureId;info.isTriggerable=true;info.isCurrentToolCommand=info.actionId.substr(0,12)=="interaction.";binding.actions.push_back(std::move(info));
        }
        binding.onAction=[weak](std::string_view action,const InteractionEvent& event,const std::optional<HostSemanticTarget>&){
            const auto f=weak.lock();if(!f)return InteractionResult{};
            if(action=="roi.box.edit")return f->m_impl->OnBoxInput(event);
            if(action=="roi.draw"||event.eventKind==InteractionEventKind::Cancel) {
                auto routed=event;
                routed.phase=event.eventKind==InteractionEventKind::PointerMove?InputPhase::Update
                    :event.eventKind==InteractionEventKind::PrimaryRelease||event.eventKind==InteractionEventKind::SecondaryRelease||event.eventKind==InteractionEventKind::MiddleRelease?InputPhase::End:InputPhase::Begin;
                return f->m_impl->OnInput(routed);
            }
            RoiEditingRequest request;
            if(action=="interaction.confirm"||action=="roi.commit")request.action=RoiEditingAction::Commit;
            else if(action=="interaction.undo")request.action=RoiEditingAction::Undo;
            else if(action=="interaction.redo")request.action=RoiEditingAction::Redo;
            else if(action=="interaction.cancel") {
                if(f->m_impl->m_drawing){f->m_impl->CancelDrawing(true);f->m_impl->m_drawing.reset();return InteractionResult{true,true};}
                request.action=RoiEditingAction::Cancel;
            } else return InteractionResult{};
            const auto result=f->SendRequest(request);const bool ok=result.error==RoiError::None||result.error==RoiError::Pending;
            return InteractionResult{true,true,ok,ok?InteractionFailureReason::None:InteractionFailureReason::StateRejected};
        };
        binding.onTrigger=[weak](std::string_view id,const ActionContext& context){
            const auto f=weak.lock();if(!f)return ActionTriggerResult{};
            if(id=="interaction.confirm"||id=="roi.commit") {
                RoiEditingRequest request;request.action=RoiEditingAction::Commit;
                const auto result=f->SendRequest(request);
                return ActionTriggerResult{result.error==RoiError::Pending?ActionTriggerStatus::Accepted:result.error==RoiError::None?ActionTriggerStatus::Completed:ActionTriggerStatus::Failed,result.commitId};
            }
            InteractionEvent event;event.viewId=context.viewId;event.area=context.area;
            RoiEditingRequest request;
            if(id=="interaction.undo")request.action=RoiEditingAction::Undo;
            else if(id=="interaction.redo")request.action=RoiEditingAction::Redo;
            else if(id=="interaction.cancel") {
                if(f->m_impl->m_drawing){f->m_impl->CancelDrawing(true);f->m_impl->m_drawing.reset();return ActionTriggerResult{ActionTriggerStatus::Completed,0};}
                request.action=RoiEditingAction::Cancel;
            } else return ActionTriggerResult{};
            const auto result=f->SendRequest(request);return ActionTriggerResult{result.error==RoiError::None?ActionTriggerStatus::Completed:ActionTriggerStatus::Failed,result.commitId};
        };
        if(!context.host->AttachInput(std::move(binding))){m_impl->m_host.reset();m_impl->m_data.reset();m_impl->m_views.reset();return false;}
    }
    m_impl->m_state.isAttached=true; return true;
}
bool RoiEditingHostFeature::DetachHost()
{
    auto& state=*m_impl;
    if (!state.m_state.isAttached) return true;
    if (state.GetAccessError()!=RoiError::None || !state.ClearDraft()) return false;
    state.StopDisplay();
    if(state.m_host && !state.m_host->DetachInput(GetFeatureId()))return false;
    state.m_host.reset();
    state.ClearCommitted();
    state.m_data.reset(); state.m_views.reset(); state.m_state.isAttached=false; return true;
}
bool RoiEditingHostFeature::OnHostTick()
{
    auto& state=*m_impl;
    if (state.GetAccessError()!=RoiError::None) return false;
    if(!state.GetDraftCurrent()){state.m_state.error=RoiError::SourceMismatch;if(!state.ClearDraft())return false;}
    state.SetCommittedCurrent();state.PollDisplay();state.QueueSliceUpdates();state.StartDisplay();
    if (!state.m_state.hasDraft) return true;
    const auto stamp=state.m_reference->GetRenderInputStamp();
    const auto lease=state.m_lease.lock();
    bool isSourceCurrent=true;
    if (state.m_request.expectedSourceBinding) {
        const auto binding=state.m_data->GetDataBinding(state.m_data->GetDataGraph(),state.m_request.expectedSourceBinding->name);
        isSourceCurrent=binding && binding->revision==state.m_request.expectedSourceBinding->revision
            && binding->target==state.m_request.expectedSourceBinding->target;
    }
    if (!lease || !lease->GetIsActive() || !stamp || !isSourceCurrent || stamp->dataRevision!=state.m_request.definition.source) {
        state.m_state.error=RoiError::SourceMismatch; return state.ClearDraft();
    }
    const auto matrix=state.m_reference->GetModelToWorld();
    state.PollOperation();if (!state.m_state.hasDraft) return true;
    // 固定形状不每帧置脏；展示矩阵变化才重新投影。
    if (!matrix || *matrix!=state.m_displayMatrix || state.m_state.displayError==RoiError::Unavailable) {
        try{(void)state.SetProjection();}catch(...){state.m_state.displayError=RoiError::Unavailable;state.m_state.displayStatus=RoiDisplayStatus::Failed;}
    }
    return true;
}
RoiResult RoiEditingHostFeature::SendRequest(const RoiEditingRequest& request)
{
    auto& state=*m_impl; RoiResult result; result.error=state.GetAccessError();
    if (result.error!=RoiError::None) return result;
    if ((request.action==RoiEditingAction::Begin && !request.draft)
        || (request.draft && request.action!=RoiEditingAction::Begin && request.action!=RoiEditingAction::SetDraft && request.action!=RoiEditingAction::Commit && request.action!=RoiEditingAction::SetMetadata)
        || (request.action==RoiEditingAction::SetDraft && !request.primitive)
        || (request.primitive && request.action!=RoiEditingAction::SetDraft && request.action!=RoiEditingAction::Commit)
        || ((request.action==RoiEditingAction::BeginOperation || request.action==RoiEditingAction::SetOperation)!=request.operation.has_value())
        || (request.selection && request.action!=RoiEditingAction::SetDraft && request.action!=RoiEditingAction::Commit)
        || (request.action==RoiEditingAction::Commit && request.selection && !request.primitive)
        || (request.output && request.action!=RoiEditingAction::SetDraft && request.action!=RoiEditingAction::Commit && request.action!=RoiEditingAction::SetMetadata)
        || (request.action==RoiEditingAction::SetVisible && !request.isVisible)) {result.error=RoiError::InvalidRequest; return result;}
    try {
        switch (request.action) {
        case RoiEditingAction::Begin: result.error=state.Begin(*request.draft); break;
        case RoiEditingAction::BeginDrawing:
            if(!state.m_state.hasDraft || !request.drawing || (*request.drawing==RoiDrawingMode::EditControlPoint && (state.m_toolNodeIndex>=state.m_request.definition.nodes.size() || state.m_request.definition.nodes[state.m_toolNodeIndex].primitive.shape!=state.m_tool.shape || GetControlPoints(state.m_tool).empty())) || (*request.drawing==RoiDrawingMode::Seed && (!state.m_operation || !std::holds_alternative<RoiThresholdParams>(state.m_operation->params)))){result.error=RoiError::InvalidRequest;break;}state.CancelDrawing(true);state.m_drawing=request.drawing;result.error=RoiError::None;break;
        case RoiEditingAction::EndDrawing:state.CancelDrawing(true);state.m_drawing.reset();result.error=RoiError::None;break;
        case RoiEditingAction::SetDraft:
            if (!state.m_state.hasDraft || state.m_state.isDragging) {result.error=RoiError::InvalidRequest; break;}
            {
                // 有效几何和元信息成为草稿；显示状态独立报告。
                if(request.draft && (request.draft->definition.source!=state.m_request.definition.source
                    || request.draft->metadata.name.empty() || request.draft->metadata.name.size()>256
                    || request.draft->metadata.group.size()>256 || request.draft->metadata.description.size()>4096)) {result.error=RoiError::InvalidRequest;break;}
                if (!request.selection && (state.m_request.definition.nodes.size()!=1 || request.primitive->shape!=state.m_tool.shape)) {result.error=RoiError::InvalidRequest;break;}
                const auto previousSelection=state.m_selection;
                if(request.output){result.error=state.GetOutputError(*request.output);if(result.error!=RoiError::None)break;}
                if (request.selection) state.m_selection=*request.selection;
                result.error=state.SetSelection(*request.primitive);
                if(result.error!=RoiError::None)state.m_selection=previousSelection;
                else if(request.output)(void)state.SetOutput(*request.output);
                if (result.error==RoiError::None && request.draft) state.m_request.metadata=request.draft->metadata;
            }
            break;
        case RoiEditingAction::CancelSave:state.m_saveSerial.reset();state.m_state.isSaving=false;result.error=RoiError::None;break;
        case RoiEditingAction::Commit:
            if(request.output){result.error=state.GetOutputError(*request.output);if(result.error!=RoiError::None)break;}
            if(request.draft){const auto& metadata=request.draft->metadata;
                if(metadata.name.empty() || metadata.name.size()>256 || metadata.group.size()>256 || metadata.description.size()>4096){result.error=RoiError::InvalidRequest;break;}}
            if (request.primitive) {
                const auto previousSelection=state.m_selection;if (request.selection) state.m_selection=*request.selection;
                result.error=state.SetSelection(*request.primitive);if (result.error!=RoiError::None){state.m_selection=previousSelection;break;}
            }
            if(request.output)(void)state.SetOutput(*request.output);
            if(request.draft)state.SetMetadata(request.draft->metadata);
            result=state.CommitDraft();break;
        case RoiEditingAction::SetMetadata:
            if (!state.m_state.hasDraft || !request.draft || request.draft->metadata.name.empty() || request.draft->metadata.name.size()>256 || request.draft->metadata.group.size()>256 || request.draft->metadata.description.size()>4096) {result.error=RoiError::InvalidRequest;break;}
            if(request.output){result.error=state.SetOutput(*request.output);if(result.error!=RoiError::None)break;}
            state.SetMetadata(request.draft->metadata);result.error=RoiError::None;break;
        case RoiEditingAction::ShowRoi:
            result.error=request.roi ? state.ShowRoi(*request.roi,request.isVisible.value_or(true)):RoiError::InvalidRequest;break;
        case RoiEditingAction::ApplySelection:
            if (!state.m_state.hasDraft || state.m_state.isComputing) {result.error=RoiError::InvalidRequest;break;}
            state.ApplyCandidate();result.error=RoiError::None;break;
        case RoiEditingAction::BeginOperation:
        case RoiEditingAction::SetOperation:
            result.error=state.StartOperation(*request.operation,request.action==RoiEditingAction::BeginOperation);break;
        case RoiEditingAction::ApplyOperation:
            if (!state.m_state.hasDraft || state.m_state.isComputing || state.m_state.operation.error!=RoiError::None) {result.error=RoiError::InvalidRequest;break;}
            state.ApplyCandidate();result.error=RoiError::None;break;
        case RoiEditingAction::CancelOperation:
            if (!state.m_state.hasDraft || !state.m_operation || state.m_operationApplied) {result.error=RoiError::InvalidRequest;break;}
            ++state.m_serial;if (state.m_cancel) state.m_cancel->store(true);state.m_pending.reset();state.m_operation.reset();
            state.m_state.isComputing=false;state.m_state.isSaving=false;state.m_state.operation={};result.error=state.SetEditState(state.m_operationOrigin);break;
        case RoiEditingAction::Undo:
            if (!state.m_state.hasDraft || state.m_state.isComputing || state.m_history.size()<2) {result.error=RoiError::InvalidRequest;break;}
            state.m_redo.push_back(state.m_history.back());state.m_history.pop_back();result.error=state.SetEditState(state.m_history.back());
            state.m_state.undoCount=state.m_history.size()-1;state.m_state.redoCount=state.m_redo.size();break;
        case RoiEditingAction::Redo:
            if (!state.m_state.hasDraft || state.m_state.isComputing || state.m_redo.empty()) {result.error=RoiError::InvalidRequest;break;}
            result.error=state.SetEditState(state.m_redo.back());if(result.error==RoiError::None){state.m_history.push_back(state.m_redo.back());state.m_redo.pop_back();}
            state.m_state.undoCount=state.m_history.size()-1;state.m_state.redoCount=state.m_redo.size();break;
        case RoiEditingAction::Cancel: result.error=state.ClearDraft() ? RoiError::None:RoiError::Unavailable; break;
        case RoiEditingAction::SetVisible:
            {
                if (request.roi || (!state.m_state.hasDraft && state.m_state.committedRoi)) {
                    result.error=state.ShowRoi(request.roi ? *request.roi:*state.m_state.committedRoi,*request.isVisible);break;
                }
                const auto previous=state.m_state.isVisible;
                state.m_state.isVisible=*request.isVisible;
                if (state.m_state.hasDraft && !state.SetProjection()) {
                    state.m_state.isVisible=previous; result.error=RoiError::Unavailable;
                } else { result.error=RoiError::None; }
            }
            break;
        default: result.error=RoiError::InvalidRequest; break;
        }
    } catch (...) { if (request.action==RoiEditingAction::Begin) (void)state.ClearDraft(); result.error=RoiError::Unavailable;}
    state.m_state.error=result.error;
    if(result.error==RoiError::None)if(const auto host=state.m_host) {
        if(request.action==RoiEditingAction::Begin||request.action==RoiEditingAction::BeginDrawing)(void)host->SetInputOwner(GetFeatureId(),true);
        if(!state.m_state.hasDraft)(void)host->SetInputOwner(GetFeatureId(),false);
    }
    return result;
}
RoiEditingState RoiEditingHostFeature::GetState() const
{
    if (m_impl->m_owner!=std::thread::id{} && m_impl->m_owner!=std::this_thread::get_id()) {
        RoiEditingState state; state.error=RoiError::WrongThread; return state;
    }
    auto state=m_impl->m_state;state.selection=m_impl->m_selection;state.output=m_impl->m_operation?m_impl->m_operation->output:m_impl->m_request.action==RoiAction::Create?RoiOutputMode::CreateNew:RoiOutputMode::UpdateTarget;if(state.hasDraft && m_impl->m_toolNodeIndex<m_impl->m_request.definition.nodes.size() && m_impl->m_request.definition.nodes[m_impl->m_toolNodeIndex].kind==RoiNodeKind::Primitive && m_impl->m_request.definition.nodes[m_impl->m_toolNodeIndex].primitive.shape==m_impl->m_tool.shape)state.tool=m_impl->m_tool;state.displayFailures.clear();for(const auto& item:m_impl->m_shown)if(item.second.displayError!=RoiError::None)state.displayFailures.emplace_back(item.first,item.second.displayError);state.visibleRois.clear();for(const auto& item:m_impl->m_shown) if(item.second.isVisible && !item.second.bindings.empty())state.visibleRois.push_back(item.first);return state;
}
