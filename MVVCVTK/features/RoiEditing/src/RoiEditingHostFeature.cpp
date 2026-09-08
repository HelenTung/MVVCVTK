#include "Host/RoiEditingHostFeature.h"
#include "Data/DataPayloads.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Support/FeatureOverlayBase.h"

#include <vtkActor.h>
#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCubeSource.h>
#include <vtkCutter.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <thread>
#include <utility>

namespace {
bool GetMatrixValid(const std::array<double,16>& m)
{
    if (!std::all_of(m.begin(),m.end(),[](double x){return std::isfinite(x);})
        || m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1) return false;
    vtkNew<vtkMatrix4x4> matrix; matrix->DeepCopy(m.data());
    const double determinant=matrix->Determinant();
    return std::isfinite(determinant) && determinant!=0;
}

class RoiBoxOverlay final : public FeatureOverlayBase {
public:
    explicit RoiBoxOverlay(HostRenderViewRole role)
    {
        m_cube->SetBounds(-1,1,-1,1,-1,1);
        m_transformFilter->SetInputConnection(m_cube->GetOutputPort());
        m_transformFilter->SetTransform(m_transform);
        const int axis=role==HostRenderViewRole::TopDownSlice ? 2
            : role==HostRenderViewRole::FrontBackSlice ? 1 : role==HostRenderViewRole::LeftRightSlice ? 0:-1;
        if (axis>=0) {
            std::array<double,3> normal{}; normal[axis]=1; m_plane->SetNormal(normal.data());
            m_cutter->SetCutFunction(m_plane); m_cutter->SetInputConnection(m_transformFilter->GetOutputPort());
            m_mapper->SetInputConnection(m_cutter->GetOutputPort());
        } else m_mapper->SetInputConnection(m_transformFilter->GetOutputPort());
        m_mapper->ScalarVisibilityOff(); m_actor->SetMapper(m_mapper);
        m_actor->GetProperty()->SetRepresentationToWireframe(); m_actor->GetProperty()->SetColor(1,0.7,0.15);
        m_actor->GetProperty()->SetLighting(false); m_actor->GetProperty()->SetLineWidth(2);
        m_actor->PickableOff(); AttachProp(m_actor);
    }
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    void SetOverlayState(const FeatureOverlayState& state) override
    {
        m_plane->SetOrigin(state.cursor.data()); Set3DPropsTransform(state.modelToWorld);
    }
    void SetBox(const std::array<double,16>& box,bool isVisible)
    {
        vtkNew<vtkMatrix4x4> matrix; matrix->DeepCopy(box.data());
        m_transform->SetMatrix(matrix); m_actor->SetVisibility(isVisible);
    }
private:
    vtkSmartPointer<vtkCubeSource> m_cube=vtkSmartPointer<vtkCubeSource>::New();
    vtkSmartPointer<vtkTransform> m_transform=vtkSmartPointer<vtkTransform>::New();
    vtkSmartPointer<vtkTransformPolyDataFilter> m_transformFilter=vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    vtkSmartPointer<vtkPlane> m_plane=vtkSmartPointer<vtkPlane>::New();
    vtkSmartPointer<vtkCutter> m_cutter=vtkSmartPointer<vtkCutter>::New();
    vtkSmartPointer<vtkPolyDataMapper> m_mapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    vtkSmartPointer<vtkActor> m_actor=vtkSmartPointer<vtkActor>::New();
};
}

class RoiEditingHostFeature::Impl final {
public:
    explicit Impl(RoiEditingConfig config):m_config(std::move(config)) {}
    ~Impl() { (void)ClearDraft(); }
    struct Binding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureViewService> view;
        std::shared_ptr<RoiBoxOverlay> overlay;
    };
    RoiError GetAccessError() const noexcept
    {
        if (m_owner!=std::thread::id{} && m_owner!=std::this_thread::get_id()) return RoiError::WrongThread;
        return m_state.isAttached && !m_isPublishing ? RoiError::None:RoiError::Unavailable;
    }
    bool ClearDraft()
    {
        if (m_owner!=std::thread::id{} && m_owner!=std::this_thread::get_id()) return false;
        if (m_widget) {
            m_widget->RemoveAllObservers(); m_widget->Off(); m_widget->SetInteractor(nullptr);
            m_widget=nullptr; m_representation=nullptr;
        }
        try { if (m_reference) (void)m_reference->SetInteracting({"roi-editing","box"},false); } catch (...) {}
        m_state.isDragging=false; m_isDragCancelled=false;
        for (auto& binding:m_bindings) {
            binding.service->RemoveOverlay(binding.overlay);
            try { (void)binding.view->SetRenderNeeded(); } catch (...) {}
        }
        m_bindings.clear(); m_state.hasDraft=false; m_state.draft.reset(); m_request={}; m_reference.reset(); m_lease.reset();
        return true;
    }
    bool SetProjection()
    {
        if (!m_state.hasDraft || !m_reference || !m_widget) return false;
        const auto lease=m_lease.lock();
        if (!lease || !lease->GetIsActive() || !lease->GetIsOwnerThread()) return false;
        const auto matrix=m_reference->GetModelToWorld();
        if (!matrix || !GetMatrixValid(*matrix)) return false;
        if (m_state.isDragging && *matrix!=m_displayMatrix) {
            m_request.definition.nodes[0].primitive.localToSource=m_dragStart;
            m_state.draft=m_request.definition;
            m_state.isDragging=false; m_isDragCancelled=true;
            (void)m_reference->SetInteracting({"roi-editing","box"},false);
        }
        m_displayMatrix=*matrix;
        vtkNew<vtkMatrix4x4> display; display->DeepCopy(matrix->data());
        vtkNew<vtkMatrix4x4> box; box->DeepCopy(m_request.definition.nodes[0].primitive.localToSource.data());
        vtkNew<vtkMatrix4x4> world; vtkMatrix4x4::Multiply4x4(display,box,world);
        vtkNew<vtkTransform> transform; transform->SetMatrix(world);
        if (!m_state.isDragging) m_representation->SetTransform(transform);
        m_widget->SetEnabled(m_state.isVisible ? 1:0);
        for (auto& binding:m_bindings) {
            binding.overlay->SetBox(m_request.definition.nodes[0].primitive.localToSource,m_state.isVisible);
            (void)binding.view->SetRenderNeeded();
        }
        return true;
    }
    void OnWidget(unsigned long event)
    {
        if (GetAccessError()!=RoiError::None || !m_state.hasDraft || !m_reference) return;
        const auto matrix=m_reference->GetModelToWorld();
        if (!matrix || !GetMatrixValid(*matrix)) return;
        if (m_state.isDragging && *matrix!=m_displayMatrix) { (void)SetProjection(); }
        if (event==vtkCommand::StartInteractionEvent) {
            m_isDragCancelled=false;
            if (!m_reference->SetInteracting({"roi-editing","box"},true)) return;
            m_state.isDragging=true; m_displayMatrix=*matrix;
            m_dragStart=m_request.definition.nodes[0].primitive.localToSource;
        }
        if (m_state.isDragging && !m_isDragCancelled) {
            vtkNew<vtkTransform> world; m_representation->GetTransform(world);
            vtkNew<vtkMatrix4x4> inverse; inverse->DeepCopy(m_displayMatrix.data()); inverse->Invert();
            vtkNew<vtkMatrix4x4> source; vtkMatrix4x4::Multiply4x4(inverse,world->GetMatrix(),source);
            std::array<double,16> next{}; std::copy_n(source->GetData(),16,next.begin());
            if (GetMatrixValid(next)) { m_request.definition.nodes[0].primitive.localToSource=next; m_state.draft=m_request.definition; }
        }
        if (event==vtkCommand::EndInteractionEvent) {
            m_state.isDragging=false; m_isDragCancelled=false;
            (void)m_reference->SetInteracting({"roi-editing","box"},false);
        }
        (void)SetProjection();
    }
    RoiError Begin(const RoiRequest& request)
    {
        if (m_state.hasDraft || !m_bindings.empty()) return RoiError::InvalidRequest;
        if ((request.action!=RoiAction::Create && request.action!=RoiAction::SetGeometry)
            || request.definition.nodes.size()!=1 || request.definition.nodes[0].kind!=RoiNodeKind::Primitive
            || request.definition.nodes[0].primitive.shape!=RoiShape::Box
            || !GetMatrixValid(request.definition.nodes[0].primitive.localToSource)) return RoiError::UnsupportedRoi;
        const auto graph=m_data->GetDataGraph();
        const auto source=m_data->GetData(graph,request.definition.source);
        if (!source) return RoiError::MissingInput;
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
        const auto targets=m_views->GetViews(m_config.targetViews);
        if (targets.empty() || targets.size()>64) return RoiError::InvalidRequest;
        std::vector<Binding> prepared;
        std::set<std::string> ids;
        for (const auto& view:targets) {
            if (!ids.insert(view.id).second) continue;
            auto port=m_views->GetFeaturePort(view.id); auto overlays=m_views->GetOverlayPort(view.id);
            if (!port || !overlays) return RoiError::Unavailable;
            prepared.push_back({overlays,port,std::make_shared<RoiBoxOverlay>(view.role)});
        }
        if (!ids.count(input->view.id)) return RoiError::InvalidRequest;
        m_bindings.reserve(prepared.size());
        m_request=request;
        if (!m_request.expectedSourceBinding) {
            const auto primary=m_data->GetDataBinding(graph,primaryVolumeBinding);
            if (primary && primary->target==request.definition.source) m_request.expectedSourceBinding=primary;
        }
        m_reference=std::move(reference); m_lease=input->lease;
        // 所有候选先准备好；部分挂载失败保留清理失败的资源，供 Detach 重试。
        for (auto& binding:prepared) {
            if (!binding.service->AttachOverlay(binding.overlay)) { (void)ClearDraft(); return RoiError::Unavailable; }
            m_bindings.push_back(std::move(binding));
        }
        m_representation=vtkSmartPointer<vtkBoxRepresentation>::New();
        m_representation->SetPlaceFactor(1);
        double bounds[6]{-1,1,-1,1,-1,1}; m_representation->PlaceWidget(bounds);
        m_widget=vtkSmartPointer<vtkBoxWidget2>::New(); m_widget->SetRepresentation(m_representation);
        m_widget->SetInteractor(input->interactor); m_widget->SetCurrentRenderer(input->renderer); m_widget->SetDefaultRenderer(input->renderer);
        vtkNew<vtkCallbackCommand> callback; callback->SetClientData(this);
        callback->SetCallback([](vtkObject*,unsigned long event,void* client,void*) {
            try { static_cast<Impl*>(client)->OnWidget(event); } catch (...) {}
        });
        for (auto event:{vtkCommand::StartInteractionEvent,vtkCommand::InteractionEvent,vtkCommand::EndInteractionEvent})
            m_widget->AddObserver(event,callback);
        m_state.hasDraft=true; m_state.draft=request.definition;
        if (!SetProjection()) { (void)ClearDraft(); return RoiError::Unavailable; }
        return RoiError::None;
    }
    RoiEditingConfig m_config;
    std::thread::id m_owner;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<FeatureViewService> m_reference;
    std::weak_ptr<const FeatureViewLease> m_lease;
    std::vector<Binding> m_bindings;
    vtkSmartPointer<vtkBoxWidget2> m_widget;
    vtkSmartPointer<vtkBoxRepresentation> m_representation;
    RoiRequest m_request;
    RoiEditingState m_state;
    std::array<double,16> m_displayMatrix=roiIdentityMatrix, m_dragStart=roiIdentityMatrix;
    bool m_isDragCancelled=false;
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
    m_impl->m_owner=std::this_thread::get_id(); m_impl->m_data=context.data; m_impl->m_views=context.views;
    m_impl->m_state.isAttached=true; return true;
}
bool RoiEditingHostFeature::DetachHost()
{
    auto& state=*m_impl;
    if (!state.m_state.isAttached) return true;
    if (state.GetAccessError()!=RoiError::None || !state.ClearDraft()) return false;
    state.m_data.reset(); state.m_views.reset(); state.m_state.isAttached=false; return true;
}
bool RoiEditingHostFeature::OnHostTick()
{
    auto& state=*m_impl;
    if (state.GetAccessError()!=RoiError::None) return false;
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
    // 固定形状不每帧置脏；展示矩阵变化才重新投影。
    if (!matrix || *matrix!=state.m_displayMatrix) return state.SetProjection();
    return true;
}
RoiResult RoiEditingHostFeature::SendRequest(const RoiEditingRequest& request)
{
    auto& state=*m_impl; RoiResult result; result.error=state.GetAccessError();
    if (result.error!=RoiError::None) return result;
    if ((request.action==RoiEditingAction::Begin)!=request.draft.has_value()
        || (request.action==RoiEditingAction::SetDraft)!=request.boxToSource.has_value()
        || (request.action==RoiEditingAction::SetVisible)!=request.isVisible.has_value()) {result.error=RoiError::InvalidRequest; return result;}
    try {
        switch (request.action) {
        case RoiEditingAction::Begin: result.error=state.Begin(*request.draft); break;
        case RoiEditingAction::SetDraft:
            if (!state.m_state.hasDraft || state.m_state.isDragging || !GetMatrixValid(*request.boxToSource)) {result.error=RoiError::InvalidGeometry; break;}
            {
                const auto previous=state.m_request.definition.nodes[0].primitive.localToSource;
                state.m_request.definition.nodes[0].primitive.localToSource=*request.boxToSource;
                if (!state.SetProjection()) {state.m_request.definition.nodes[0].primitive.localToSource=previous; result.error=RoiError::Unavailable;}
                else {state.m_state.draft=state.m_request.definition; result.error=RoiError::None;}
            }
            break;
        case RoiEditingAction::Commit:
            if (!state.m_state.hasDraft || state.m_state.isDragging) {result.error=RoiError::InvalidRequest; break;}
            {
                const auto stamp=state.m_reference->GetRenderInputStamp();
                const auto lease=state.m_lease.lock();
                if (!lease || !lease->GetIsActive() || !stamp || stamp->dataRevision!=state.m_request.definition.source) {result.error=RoiError::SourceMismatch; break;}
            }
            state.m_isPublishing=true;
            try { result=state.m_data->SetRoi(state.m_request); } catch (...) {state.m_isPublishing=false; throw;}
            state.m_isPublishing=false;
            if (result.error==RoiError::None) {
                state.m_state.committedRoi=result.roi->revision;
                (void)state.ClearDraft();
            }
            break;
        case RoiEditingAction::Cancel: result.error=state.ClearDraft() ? RoiError::None:RoiError::Unavailable; break;
        case RoiEditingAction::SetVisible:
            {
                const auto previous=state.m_state.isVisible;
                state.m_state.isVisible=*request.isVisible;
                if (state.m_state.hasDraft && !state.SetProjection()) {
                    state.m_state.isVisible=previous; result.error=RoiError::Unavailable;
                } else result.error=RoiError::None;
            }
            break;
        default: result.error=RoiError::InvalidRequest; break;
        }
    } catch (...) { if (request.action==RoiEditingAction::Begin) (void)state.ClearDraft(); result.error=RoiError::Unavailable;}
    state.m_state.error=result.error;
    return result;
}
RoiEditingState RoiEditingHostFeature::GetState() const
{
    if (m_impl->m_owner!=std::thread::id{} && m_impl->m_owner!=std::this_thread::get_id()) {
        RoiEditingState state; state.error=RoiError::WrongThread; return state;
    }
    return m_impl->m_state;
}
