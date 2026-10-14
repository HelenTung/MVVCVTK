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

namespace {
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
        m_handleMapper->SetInputData(m_handles);m_handleActor->SetMapper(m_handleMapper);m_handleActor->GetProperty()->SetColor(1,.9,.3);m_handleActor->GetProperty()->SetPointSize(7);m_handleActor->GetProperty()->SetLighting(false);m_handleActor->PickableOff();AttachProp(m_handleActor);
    }
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    void SetOverlayState(const FeatureOverlayState& state) override
    {
        (void)SliceContourPlane::SetPlane(*m_plane,state.cursor,m_normal,state.modelToWorld);
        Set3DPropsTransform(state.modelToWorld);
    }
    RoiPlane GetSlicePlane() const {RoiPlane p;std::copy_n(m_plane->GetOrigin(),3,p.origin.begin());std::copy_n(m_plane->GetNormal(),3,p.normal.begin());return p;}
    void SetPrimitive(const RoiPrimitive& primitive,bool isVisible,bool isDraft=true)
    {
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
    void SetColour(const std::array<double,3>& colour){m_actor->GetProperty()->SetColor(colour[0],colour[1],colour[2]);}
    void SetRegion(vtkPolyData* region,bool isVisible,bool isDraft)
    {
        m_transform->Identity();m_transformFilter->SetInputData(region);m_actor->SetVisibility(isVisible);
        m_actor->GetProperty()->SetColor(isDraft ? 1.0:0.1,isDraft ? 0.7:0.95,isDraft ? 0.15:1.0);
    }
private:
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
    };
    struct Shown final {DataRevisionRef source;RoiReadSnapshot read;std::vector<Binding> bindings;vtkSmartPointer<vtkPolyData> mesh;bool isVisible=true;};
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
            if (!simple && !shown.mesh) {QueueDisplay(shown.read,ref);return RoiError::None;}
            for (const auto& view:m_views->GetViews(m_config.targetViews)) {
                auto port=m_views->GetFeaturePort(view.id);auto overlays=m_views->GetOverlayPort(view.id);
                const auto stamp=port ? port->GetRenderInputStamp():std::nullopt;if (!overlays || !stamp || stamp->dataRevision!=shown.read->GetSource()) continue;
                if(std::any_of(shown.bindings.begin(),shown.bindings.end(),[&](const auto& b){return b.view==port;}))continue;
                auto overlay=std::make_shared<RoiOverlay>(view.role);
                if (simple) overlay->SetPrimitive(p,true,false);
                else if (shown.mesh) overlay->SetRegion(shown.mesh,true,false);
                else {m_state.displayError=RoiError::TooLarge;continue;}
                std::uint64_t hash=1469598103934665603ULL;for(auto byte:ref.entityId.bytes){hash^=byte;hash*=1099511628211ULL;}const double angle=double(hash%1048576)/1048576*6.283185307179586;overlay->SetColour({.55+.4*std::cos(angle),.55+.4*std::cos(angle+2.094395102393195),.55+.4*std::cos(angle+4.188790204786391)});
                shown.bindings.push_back({overlays,port,overlay});if (!overlays->AttachOverlay(overlay)) {overlays->RemoveOverlay(overlay);shown.bindings.pop_back();m_state.displayError=RoiError::Unavailable;continue;}
                (void)port->SetRenderNeeded();
            }
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
    RoiError GetAccessError() const noexcept
    {
        if (m_owner!=std::thread::id{} && m_owner!=std::this_thread::get_id()) return RoiError::WrongThread;
        return m_state.isAttached && !m_isPublishing ? RoiError::None:RoiError::Unavailable;
    }
    static vtkSmartPointer<vtkPolyData> BuildRegion(const RoiReadSnapshot& read,const DataSnapshot& source,const std::shared_ptr<std::atomic<bool>>& cancel)
    {
        if (!read) return {};
        const auto* image=source ? dynamic_cast<const ImageGrid3DPayload*>(source->payload.get()):nullptr;
        if (!image) return {};
        const auto& grid=image->GetGeometry();
        const auto point=[&](const std::array<double,3>& index) {auto p=grid.origin;for (int r=0;r<3;++r) for (int a=0;a<3;++a) p[r]+=grid.direction[r*3+a]*grid.spacing[a]*index[a];return p;};
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> faces;
        const auto bounds=read->GetBounds();auto extent=grid.extent;
        vtkNew<vtkMatrix4x4> inverse;for(int r=0;r<3;++r){for(int a=0;a<3;++a)inverse->SetElement(r,a,grid.direction[r*3+a]*grid.spacing[a]);inverse->SetElement(r,3,grid.origin[r]);}inverse->Invert();
        std::array<double,6> indexed{INFINITY,-INFINITY,INFINITY,-INFINITY,INFINITY,-INFINITY};
        for(int a=0;a<3;++a)if(bounds[a*2]>bounds[a*2+1])return vtkSmartPointer<vtkPolyData>::New();
        for(unsigned k=0;k<8;++k){const auto p=MapPoint(inverse,{bounds[(k&1)?1:0],bounds[(k&2)?3:2],bounds[(k&4)?5:4]});for(int a=0;a<3;++a){indexed[a*2]=std::min(indexed[a*2],p[a]);indexed[a*2+1]=std::max(indexed[a*2+1],p[a]);}}
        for(int a=0;a<3;++a){const auto low=std::max(double(extent[a*2]),std::ceil(indexed[a*2])),high=std::min(double(extent[a*2+1]),std::floor(indexed[a*2+1]));if(low>high)return vtkSmartPointer<vtkPolyData>::New();extent[a*2]=int(low);extent[a*2+1]=int(high);}
        for (std::int64_t z=extent[4];z<=extent[5];++z) for (std::int64_t y=extent[2];y<=extent[3];++y) for (std::int64_t x=extent[0];x<=extent[1];++x) {
            if(cancel->load())return {};
            const std::array<double,3> index{static_cast<double>(x),static_cast<double>(y),static_cast<double>(z)};
            const auto p=point(index);bool candidate=true;for (int a=0;a<3;++a) if (p[a]<bounds[a*2] || p[a]>bounds[a*2+1]) candidate=false;
            if (!candidate || !read->GetContains(p)) continue;
            for (int axis=0;axis<3;++axis) for (int side=0;side<2;++side) {
                auto neighbour=index;neighbour[axis]+=side ? 1:-1;if (read->GetContains(point(neighbour))) continue;
                if (static_cast<std::size_t>(points->GetNumberOfPoints())>=roiCopyLimit/(sizeof(double)*3)) return {};
                vtkIdType ids[4];const int a=(axis+1)%3,b=(axis+2)%3;
                for (int corner=0;corner<4;++corner) {auto vertex=index;vertex[axis]+=side ? .5:-.5;vertex[a]+=(corner==1 || corner==2) ? .5:-.5;vertex[b]+=corner>=2 ? .5:-.5;
                    for(int a=0;a<3;++a)vertex[a]=std::clamp(vertex[a],double(grid.extent[a*2]),double(grid.extent[a*2+1]));
                    const auto world=point(vertex);ids[corner]=points->InsertNextPoint(world.data());}
                faces->InsertNextCell(4,ids);
            }
        }
        auto mesh=vtkSmartPointer<vtkPolyData>::New();mesh->SetPoints(points);mesh->SetPolys(faces);return mesh;
    }
    RoiError SetDefinition(RoiDefinition definition)
    {
        const auto preview=m_data->GetRoiPreview(m_data->GetDataGraph(),definition);
        if (preview.error!=RoiError::None || !preview.roi) return preview.error;
        m_request.definition=std::move(definition);m_state.draft=m_request.definition;m_state.preview=preview.roi;
        m_geometryChanged=m_history.empty() || !SameDefinition(m_request.definition,m_history.front());
        ++m_state.previewRevision;m_geometryDirty=true;
        return SetProjection() ? RoiError::None:RoiError::Unavailable;
    }
    RoiError SetSelection(const RoiPrimitive& primitive)
    {
        RoiDefinition selection{m_request.definition.source,{{RoiNodeKind::Primitive,primitive}}};
        auto definition=selection;
        if (m_selection==RoiSelectionMode::Add || m_selection==RoiSelectionMode::Subtract || m_selection==RoiSelectionMode::Intersect) {
            definition=m_applied;const auto left=static_cast<std::uint32_t>(definition.nodes.size()-1),right=static_cast<std::uint32_t>(definition.nodes.size());
            definition.nodes.push_back(selection.nodes.front());definition.nodes.push_back({m_selection==RoiSelectionMode::Add ? RoiNodeKind::Union:m_selection==RoiSelectionMode::Subtract ? RoiNodeKind::Difference:RoiNodeKind::Intersection,{},left,right});
        } else if (m_selection!=RoiSelectionMode::Replace && m_selection!=RoiSelectionMode::NewRoi) return RoiError::InvalidRequest;
        const auto previous=m_tool;m_tool=primitive;m_toolNodeIndex=definition.nodes.size()==1?0:definition.nodes.size()-2;const auto error=SetDefinition(std::move(definition));
        if(error==RoiError::None && m_operation){++m_serial;if(m_cancel)m_cancel->store(true);m_pending.reset();m_operation.reset();m_state.operation={};}
        if (error!=RoiError::None) m_tool=previous;
        else if (m_selection==RoiSelectionMode::NewRoi) {m_request.action=RoiAction::Create;m_request.expectedRoi.reset();}
        return error;
    }
    void SetHistory()
    {
        m_applied=m_request.definition;m_history.push_back(m_applied);m_redo.clear();
        m_state.undoCount=m_history.size()-1;m_state.redoCount=0;
    }
    RoiError StartOperation(const RoiOperationRequest& request,bool begins)
    {
        if (!m_state.hasDraft || m_state.isDragging) return RoiError::InvalidRequest;
        if (begins || !m_operation) m_operationStart=m_request.definition;
        auto operation=request;operation.source=m_request.definition.source;operation.input=m_operationStart;
        operation.target=m_request.expectedRoi;operation.metadata=m_request.metadata;operation.expectedSourceBinding=m_request.expectedSourceBinding;
        if (!operation.target) operation.output=RoiOutputMode::CreateNew;
        ++m_serial;if (m_cancel) m_cancel->store(true);m_pending=operation;m_operation=std::move(operation);
        m_state.isComputing=true;m_state.isSaving=false;m_state.operation={};m_state.operation.error=RoiError::Pending;
        if(m_displayFuture.valid() && m_displayCancel)m_displayCancel->store(true);
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
        m_future=task.get_future();const auto host=m_host;
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
                if (m_state.isSaving) {m_state.isSaving=false;if(m_state.error==RoiError::None) {const auto saved=CommitDraft();m_state.error=saved.error;}}
            }
            StartPendingOperation();StartDisplay();
        }
    }
    struct DisplayWork {RoiReadSnapshot read;DataSnapshot source;std::shared_ptr<const FeatureInternal::InputReads> readers;std::optional<DataRevisionRef> ref;std::uint64_t previewRevision=0;};
    void QueueDisplay(const RoiReadSnapshot& read,std::optional<DataRevisionRef> ref={})
    {
        if(!read)return;
        if(!ref){m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[](const auto& w){return !w.ref;}),m_displayQueue.end());if(m_displayRunning && !m_displayRunning->ref && m_displayCancel)m_displayCancel->store(true);}
        else if((m_displayRunning && m_displayRunning->ref==ref) || std::any_of(m_displayQueue.begin(),m_displayQueue.end(),[&](const auto& w){return w.ref==ref;}))return;
        const auto graph=m_data->GetDataGraph();auto refs=read->GetDependencies();refs.push_back(read->GetSource());
        auto readers=FeatureInternal::StartInputReads(graph,refs,"roi-display-reader");if(!readers){m_state.displayError=RoiError::Unavailable;return;}
        m_displayQueue.push_back({read,m_data->GetData(graph,read->GetSource()),std::make_shared<const FeatureInternal::InputReads>(std::move(*readers)),ref,m_state.previewRevision});StartDisplay();
    }
    void StartDisplay()
    {
        if(m_future.valid() || m_pending || m_displayFuture.valid() || m_displayQueue.empty())return;
        m_displayRunning=std::move(m_displayQueue.front());m_displayQueue.erase(m_displayQueue.begin());
        m_displayCancel=std::make_shared<std::atomic<bool>>(false);const auto cancel=m_displayCancel;const auto host=m_host;
        const auto work=*m_displayRunning;
        std::packaged_task<vtkSmartPointer<vtkPolyData>()> task([work,cancel]{return BuildRegion(work.read,work.source,cancel);});
        m_displayFuture=task.get_future();
        try{m_worker=std::thread([task=std::move(task),host]()mutable{task();if(const auto owner=host.lock())(void)owner->SendWorkAvailable();});}
        catch(...){m_displayFuture={};m_displayRunning.reset();m_state.displayError=RoiError::TooLarge;}
    }
    void PollDisplay()
    {
        if(m_displayFuture.valid() && m_displayFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            vtkSmartPointer<vtkPolyData> mesh;try{mesh=m_displayFuture.get();}catch(...){}
            if(m_worker.joinable())m_worker.join();const auto work=*m_displayRunning;m_displayRunning.reset();
            if(!m_displayCancel->load()){
                if(work.ref){const auto found=m_shown.find(*work.ref);if(found!=m_shown.end()){found->second.mesh=mesh?mesh:vtkSmartPointer<vtkPolyData>::New();(void)ShowRoi(*work.ref,found->second.isVisible);}}
                else if(m_state.hasDraft && work.previewRevision==m_state.previewRevision){m_draftMesh=mesh?mesh:vtkSmartPointer<vtkPolyData>::New();(void)SetProjection();}
                if(!mesh)m_state.displayError=RoiError::TooLarge;
            }
            StartPendingOperation();StartDisplay();
        }
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
        if (m_state.isComputing) {m_state.isSaving=true;result.error=RoiError::Pending;return result;}
        const auto stamp=m_reference->GetRenderInputStamp();const auto lease=m_lease.lock();
        if (!lease || !lease->GetIsActive() || !stamp || stamp->dataRevision!=m_request.definition.source) {result.error=RoiError::SourceMismatch;return result;}
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
                else{auto request=m_request;request.action=RoiAction::SetMetadata;request.definition={};request.expectedSourceBinding.reset();
                    if(request.metadata.name.empty())request.metadata=descriptor->metadata;result=m_data->SetRoi(request);}
            }else result=m_data->SetRoi(m_request);
        } catch (...) {m_isPublishing=false;throw;}
        m_isPublishing=false;
        if (result.error==RoiError::None) {
            if (result.roi) m_state.committedRoi=result.roi->revision;
            if (!m_operation && result.roi) outputs.push_back(*result.roi);
            (void)ClearDraft();
            for (const auto& roi:outputs) (void)ShowRoi(roi.revision,true);
        }return result;
    }
    void StopDisplay()
    {
        if(m_displayCancel)m_displayCancel->store(true);
        if(m_displayFuture.valid()){m_displayFuture.wait();try{(void)m_displayFuture.get();}catch(...){}}
        if(m_worker.joinable())m_worker.join();m_displayRunning.reset();
    }
    bool ClearDraft()
    {
        if (m_owner!=std::thread::id{} && m_owner!=std::this_thread::get_id()) return false;
        CancelDrawing(false);m_drawing.reset();
        ++m_serial;if (m_cancel) m_cancel->store(true);
        if (m_future.valid()) {m_future.wait();try {(void)m_future.get();} catch (...) {}}
        if(m_displayCancel)m_displayCancel->store(true);
        if(!m_displayFuture.valid() && m_worker.joinable())m_worker.join();
        m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[](const auto& w){return !w.ref;}),m_displayQueue.end());
        m_pending.reset();m_operation.reset();m_state.operation={};m_state.isComputing=false;m_state.isSaving=false;
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
        m_state.preview.reset();m_draftReaders.reset();m_history.clear();m_redo.clear();m_state.undoCount=m_state.redoCount=0;
        if(const auto host=m_host.lock())(void)host->SetInputOwner("roi-editing",false);
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
            if(item.isVisible && sourceVisible)for(const auto& view:m_views->GetViews(m_config.targetViews)){const auto port=m_views->GetFeaturePort(view.id);const auto stamp=port?port->GetRenderInputStamp():std::nullopt;if(stamp && stamp->dataRevision==item.source && std::none_of(item.bindings.begin(),item.bindings.end(),[&](const auto& b){return b.view==port;})){restore.push_back(it->first);break;}}
            if(!sourceVisible)item.read.reset();
            ++it;
        }
        m_displayQueue.erase(std::remove_if(m_displayQueue.begin(),m_displayQueue.end(),[&](const auto& work){return work.ref && !GetSourceVisible(work.read->GetSource());}),m_displayQueue.end());
        if(m_displayRunning && m_displayRunning->ref && !GetSourceVisible(m_displayRunning->read->GetSource()) && m_displayCancel)m_displayCancel->store(true);
        for(const auto& ref:restore)(void)ShowRoi(ref,true);
    }
    bool SetProjection()
    {
        if (!m_state.hasDraft || !m_reference) return false;
        const auto lease=m_lease.lock();
        if (!lease || !lease->GetIsActive() || !lease->GetIsOwnerThread()) return false;
        const auto matrix=m_reference->GetModelToWorld();
        if (!matrix || !GetMatrixValid(*matrix)) return false;
        if (m_state.isDragging && *matrix!=m_displayMatrix) {
            m_state.isDragging=false; m_isDragCancelled=true;
            m_tool.localToSource=m_dragStart;(void)SetDefinition(m_dragDefinition);
            (void)m_reference->SetInteracting({"roi-editing","box"},false);
        }
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
            if (!(m_request.definition.nodes.size()==1 && m_request.definition.nodes[0].kind==RoiNodeKind::Primitive
                && (m_tool.shape==RoiShape::Box || m_tool.shape==RoiShape::Sphere || m_tool.shape==RoiShape::Cylinder || m_tool.shape==RoiShape::Ellipsoid))) {
                m_draftMesh=vtkSmartPointer<vtkPolyData>::New();QueueDisplay(m_state.preview);
            }m_geometryDirty=false;
        }
        for (auto& binding:m_bindings) {
            binding.overlay->SetHandles(m_tool,m_state.isVisible);
            if (m_draftMesh) binding.overlay->SetRegion(m_draftMesh,m_state.isVisible,true);
            else binding.overlay->SetPrimitive(m_request.definition.nodes.front().primitive,m_state.isVisible);
            (void)binding.view->SetRenderNeeded();
        }
        return true;
    }
    void SetWidgetBindings() {
        if(!m_widget||m_state.isDragging||m_nativeInputActive||!m_views)return;
        const auto input=m_views->GetInputView(m_config.referenceView);
        const auto host=m_host.lock();if(!input||!host)return;
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
        if (!input || !input->renderer || !input->interactor
            || (input->view.role!=HostRenderViewRole::Primary3D && input->view.role!=HostRenderViewRole::Composite3D)) return RoiError::Unavailable;
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
            prepared.push_back({overlays,port,std::make_shared<RoiOverlay>(view.role)});
        }
        if (!ids.count(input->view.id)) return RoiError::InvalidRequest;
        m_bindings.reserve(prepared.size());
        m_draftReaders=std::make_shared<const FeatureInternal::InputReads>(std::move(*draftReaders));m_request=request;
        m_request.expectedCatalogRevision.reset();m_applied=request.definition;m_operationStart=request.definition;
        m_tool=request.definition.nodes.front().primitive;m_toolNodeIndex=0;m_state.preview=preview.roi;m_geometryDirty=true;
        m_history={request.definition};m_redo.clear();m_geometryChanged=false;++m_state.previewRevision;
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
        if (request.definition.nodes.size()==1 && request.definition.nodes[0].kind==RoiNodeKind::Primitive && request.definition.nodes[0].primitive.shape==RoiShape::Box) {
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
        if (!SetProjection()) { (void)ClearDraft(); return RoiError::Unavailable; }
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
    std::vector<RoiDefinition> m_history,m_redo;
    RoiSelectionMode m_selection=RoiSelectionMode::Replace;
    vtkSmartPointer<vtkPolyData> m_draftMesh;
    bool m_geometryDirty=false,m_geometryChanged=false;
    std::future<vtkSmartPointer<vtkPolyData>> m_displayFuture;
    std::optional<DisplayWork> m_displayRunning;std::vector<DisplayWork> m_displayQueue;
    std::shared_ptr<std::atomic<bool>> m_displayCancel;
    std::future<RoiOperationResult> m_future;
    std::thread m_worker;
    std::weak_ptr<FeatureHostControl> m_host;
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
        if(!context.host->AttachInput(std::move(binding))){m_impl->m_data.reset();m_impl->m_views.reset();return false;}
    }
    m_impl->m_state.isAttached=true; return true;
}
bool RoiEditingHostFeature::DetachHost()
{
    auto& state=*m_impl;
    if (!state.m_state.isAttached) return true;
    if (state.GetAccessError()!=RoiError::None || !state.ClearDraft()) return false;
    state.StopDisplay();
    if(const auto host=state.m_host.lock())if(!host->DetachInput(GetFeatureId()))return false;
    state.ClearCommitted();
    state.m_data.reset(); state.m_views.reset(); state.m_state.isAttached=false; return true;
}
bool RoiEditingHostFeature::OnHostTick()
{
    auto& state=*m_impl;
    if (state.GetAccessError()!=RoiError::None) return false;
    state.PollDisplay();state.StartDisplay();state.SetCommittedCurrent();
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
    if (!matrix || *matrix!=state.m_displayMatrix) return state.SetProjection();
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
        || (request.action==RoiEditingAction::SetVisible && !request.isVisible)) {result.error=RoiError::InvalidRequest; return result;}
    try {
        switch (request.action) {
        case RoiEditingAction::Begin: result.error=state.Begin(*request.draft); break;
        case RoiEditingAction::BeginDrawing:
            if(!state.m_state.hasDraft || !request.drawing || (*request.drawing==RoiDrawingMode::EditControlPoint && (state.m_toolNodeIndex>=state.m_request.definition.nodes.size() || state.m_request.definition.nodes[state.m_toolNodeIndex].primitive.shape!=state.m_tool.shape || GetControlPoints(state.m_tool).empty())) || (*request.drawing==RoiDrawingMode::Seed && (!state.m_operation || !std::holds_alternative<RoiThresholdParams>(state.m_operation->params)))){result.error=RoiError::InvalidRequest;break;}state.CancelDrawing(true);state.m_drawing=request.drawing;result.error=RoiError::None;break;
        case RoiEditingAction::EndDrawing:state.CancelDrawing(true);state.m_drawing.reset();result.error=RoiError::None;break;
        case RoiEditingAction::SetDraft:
            if (!state.m_state.hasDraft || state.m_state.isDragging || state.m_state.isComputing) {result.error=RoiError::InvalidRequest; break;}
            {
                // 几何和同源元信息在成功投影后一起成为当前草稿。
                if(request.draft && (request.draft->definition.source!=state.m_request.definition.source
                    || request.draft->metadata.name.empty() || request.draft->metadata.name.size()>256
                    || request.draft->metadata.group.size()>256 || request.draft->metadata.description.size()>4096)) {result.error=RoiError::InvalidRequest;break;}
                if (!request.selection && (state.m_request.definition.nodes.size()!=1 || request.primitive->shape!=state.m_tool.shape)) {result.error=RoiError::InvalidRequest;break;}
                if (request.selection) state.m_selection=*request.selection;
                result.error=state.SetSelection(*request.primitive);
                if (result.error==RoiError::None && request.draft) state.m_request.metadata=request.draft->metadata;
            }
            break;
        case RoiEditingAction::Commit:
            if(request.selection==RoiSelectionMode::NewRoi && !state.m_operation){state.m_request.action=RoiAction::Create;state.m_request.expectedRoi.reset();}
            if (request.primitive) {
                if (request.selection) state.m_selection=*request.selection;
                result.error=state.SetSelection(*request.primitive);if (result.error!=RoiError::None) break;
            }
            if (request.draft) state.SetMetadata(request.draft->metadata);
            result=state.CommitDraft();
            break;
        case RoiEditingAction::SetMetadata:
            if (!state.m_state.hasDraft || !request.draft || request.draft->metadata.name.empty()) {result.error=RoiError::InvalidRequest;break;}
            state.SetMetadata(request.draft->metadata);result.error=RoiError::None;break;
        case RoiEditingAction::ShowRoi:
            result.error=request.roi ? state.ShowRoi(*request.roi,request.isVisible.value_or(true)):RoiError::InvalidRequest;break;
        case RoiEditingAction::ApplySelection:
            if (!state.m_state.hasDraft || state.m_state.isComputing) {result.error=RoiError::InvalidRequest;break;}
            state.SetHistory();if(!state.m_operation || !std::get_if<RoiComponentParams>(&state.m_operation->params) || std::get<RoiComponentParams>(state.m_operation->params).operation!=RoiComponentOperation::Split){state.m_operation.reset();state.m_state.operation={};}result.error=RoiError::None;break;
        case RoiEditingAction::BeginOperation:
        case RoiEditingAction::SetOperation:
            result.error=state.StartOperation(*request.operation,request.action==RoiEditingAction::BeginOperation);break;
        case RoiEditingAction::ApplyOperation:
            if (!state.m_state.hasDraft || state.m_state.isComputing || state.m_state.operation.error!=RoiError::None || state.m_state.operation.rois.size()!=1) {result.error=RoiError::InvalidRequest;break;}
            state.SetHistory();if(!state.m_operation || !std::get_if<RoiComponentParams>(&state.m_operation->params) || std::get<RoiComponentParams>(state.m_operation->params).operation!=RoiComponentOperation::Split){state.m_operation.reset();state.m_state.operation={};}result.error=RoiError::None;break;
        case RoiEditingAction::CancelOperation:
            if (!state.m_state.hasDraft) {result.error=RoiError::InvalidRequest;break;}
            ++state.m_serial;if (state.m_cancel) state.m_cancel->store(true);state.m_pending.reset();state.m_operation.reset();
            state.m_state.isComputing=false;state.m_state.isSaving=false;state.m_state.operation={};result.error=state.SetDefinition(state.m_operationStart);break;
        case RoiEditingAction::Undo:
            if (!state.m_state.hasDraft || state.m_state.isComputing || state.m_history.size()<2) {result.error=RoiError::InvalidRequest;break;}
            state.m_operation.reset();state.m_state.operation={};
            state.m_redo.push_back(state.m_history.back());state.m_history.pop_back();state.m_applied=state.m_history.back();for(std::size_t i=0;i<state.m_applied.nodes.size();++i)if(state.m_applied.nodes[i].kind==RoiNodeKind::Primitive){state.m_toolNodeIndex=i;state.m_tool=state.m_applied.nodes[i].primitive;}
            state.m_state.undoCount=state.m_history.size()-1;state.m_state.redoCount=state.m_redo.size();result.error=state.SetDefinition(state.m_applied);break;
        case RoiEditingAction::Redo:
            if (!state.m_state.hasDraft || state.m_state.isComputing || state.m_redo.empty()) {result.error=RoiError::InvalidRequest;break;}
            state.m_operation.reset();state.m_state.operation={};
            state.m_applied=state.m_redo.back();state.m_redo.pop_back();state.m_history.push_back(state.m_applied);for(std::size_t i=0;i<state.m_applied.nodes.size();++i)if(state.m_applied.nodes[i].kind==RoiNodeKind::Primitive){state.m_toolNodeIndex=i;state.m_tool=state.m_applied.nodes[i].primitive;}
            state.m_state.undoCount=state.m_history.size()-1;state.m_state.redoCount=state.m_redo.size();result.error=state.SetDefinition(state.m_applied);break;
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
    if(result.error==RoiError::None)if(const auto host=state.m_host.lock()) {
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
    auto state=m_impl->m_state;if(state.hasDraft && m_impl->m_toolNodeIndex<m_impl->m_request.definition.nodes.size() && m_impl->m_request.definition.nodes[m_impl->m_toolNodeIndex].kind==RoiNodeKind::Primitive && m_impl->m_request.definition.nodes[m_impl->m_toolNodeIndex].primitive.shape==m_impl->m_tool.shape)state.tool=m_impl->m_tool;state.visibleRois.clear();for(const auto& item:m_impl->m_shown) if(item.second.isVisible && !item.second.bindings.empty())state.visibleRois.push_back(item.first);return state;
}
