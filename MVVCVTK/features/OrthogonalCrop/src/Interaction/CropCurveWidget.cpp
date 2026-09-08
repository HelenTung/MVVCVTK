#include "Interaction/CropCurveWidget.h"
#include "Algorithms/CropGeometry.h"
#include <vtkActor.h>
#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkCylinderSource.h>
#include <vtkHandleWidget.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPointHandleRepresentation3D.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkWeakPointer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
using P=CropVectorDouble3Array;
P Add(const P& a,const P& b,double scale=1) {return {a[0]+scale*b[0],a[1]+scale*b[1],a[2]+scale*b[2]};}
double Dot(const P& a,const P& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
P Cross(const P& a,const P& b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
P Unit(const P& value) {const double length=std::hypot(value[0],value[1],value[2]);return {value[0]/length,value[1]/length,value[2]/length};}
P Radial(const P& axis) {
    P reference{};int index=0;for(int i=1;i<3;++i)if(std::abs(axis[i])<std::abs(axis[index]))index=i;
    reference[index]=1;return Unit(Cross(axis,reference));
}
P Transform(vtkMatrix4x4* matrix,const P& point) {
    const double input[4]={point[0],point[1],point[2],1};double output[4];matrix->MultiplyPoint(input,output);
    return {output[0],output[1],output[2]};
}
}

class CropCurveWidget::Impl final {
public:
    struct Handle final {
        Impl* owner=nullptr;std::size_t index=0;
        vtkSmartPointer<vtkHandleWidget> widget;
        vtkSmartPointer<vtkPointHandleRepresentation3D> representation;
        vtkSmartPointer<vtkCallbackCommand> observer;
        std::array<unsigned long,3> tags{};
    };
    Impl() {
        modelToWorld=vtkSmartPointer<vtkMatrix4x4>::New();worldToModel=vtkSmartPointer<vtkMatrix4x4>::New();
        mapper=vtkSmartPointer<vtkPolyDataMapper>::New();actor=vtkSmartPointer<vtkActor>::New();actor->SetMapper(mapper);
        actor->SetPickable(false);actor->GetProperty()->SetRepresentationToWireframe();
        actor->GetProperty()->SetColor(0.74,0.80,0.86);actor->GetProperty()->SetLineWidth(1.5);
        sphere=vtkSmartPointer<vtkSphereSource>::New();sphere->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);sphere->SetThetaResolution(40);sphere->SetPhiResolution(24);
        cylinder=vtkSmartPointer<vtkCylinderSource>::New();cylinder->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);cylinder->SetResolution(48);cylinder->CappingOn();
        for(std::size_t i=0;i<handles.size();++i) {
            auto& handle=handles[i];handle.owner=this;handle.index=i;
            handle.widget=vtkSmartPointer<vtkHandleWidget>::New();
            handle.representation=vtkSmartPointer<vtkPointHandleRepresentation3D>::New();
            handle.representation->AllOff();handle.representation->TranslationModeOn();
            handle.representation->GetProperty()->SetColor(i==1?0.2:0.9,i==1?0.8:0.6,0.2);
            handle.representation->GetSelectedProperty()->SetColor(1,0.68,0.16);
            handle.representation->SetHandleSize(8);handle.widget->AllowHandleResizeOff();
            handle.widget->EnableAxisConstraintOff();handle.widget->SetRepresentation(handle.representation);
            handle.observer=vtkSmartPointer<vtkCallbackCommand>::New();handle.observer->SetClientData(&handle);
            handle.observer->SetCallback([](vtkObject*,unsigned long event,void* data,void*) {
                if(!data)return;
                auto& handle=*static_cast<Handle*>(data);
                if(handle.owner)handle.owner->OnEvent(handle.index,event);
            });
            const std::array<unsigned long,3> events{vtkCommand::StartInteractionEvent,vtkCommand::InteractionEvent,vtkCommand::EndInteractionEvent};
            for(int event=0;event<3;++event)handle.tags[event]=handle.widget->AddObserver(events[event],handle.observer);
        }
    }
    ~Impl() {
        SetEnabled(false);
        for(auto& handle:handles) {
            handle.owner=nullptr;for(auto tag:handle.tags)handle.widget->RemoveObserver(tag);
            handle.observer->SetClientData(nullptr);handle.widget->SetInteractor(nullptr);
            handle.widget->SetDefaultRenderer(nullptr);handle.widget->SetCurrentRenderer(nullptr);
        }
    }
    void SetContext(vtkRenderWindowInteractor* value,vtkRenderer* target) {
        if(interactor==value&&renderer==target)return;
        SetEnabled(false);interactor=value;renderer=target;
        for(auto& handle:handles) {
            handle.widget->SetInteractor(value);handle.widget->SetDefaultRenderer(target);handle.widget->SetCurrentRenderer(target);
        }
    }
    bool SetGeometry(CropOpItem value,const CropMatrixDouble16Array& transform) {
        if(value.geometryType!=CropShape::Sphere&&value.geometryType!=CropShape::Cylinder)return false;
        const auto valid=CropGeometry::Build(value);
        if(!valid||!std::all_of(transform.begin(),transform.end(),[](double x){return std::isfinite(x);})
            ||transform[12]!=0||transform[13]!=0||transform[14]!=0||transform[15]!=1)return false;
        vtkNew<vtkMatrix4x4> candidate;candidate->DeepCopy(transform.data());
        const auto determinant=candidate->Determinant();if(!std::isfinite(determinant)||determinant==0)return false;
        vtkNew<vtkMatrix4x4> inverse;vtkMatrix4x4::Invert(candidate,inverse);
        for(int row=0;row<4;++row)for(int col=0;col<4;++col)if(!std::isfinite(inverse->GetElement(row,col)))return false;
        operation=valid->GetOperation();modelToWorld->DeepCopy(candidate);worldToModel->DeepCopy(inverse);ready=true;
        Sync();return true;
    }
    bool SetEnabled(bool value) {
        if(value&&(!ready||!interactor||!renderer))return false;
        enabled=false;active=-1;
        for(auto& handle:handles)handle.widget->Off();
        if(renderer)renderer->RemoveActor(actor);
        if(!value)return true;
        Sync();renderer->AddActor(actor);enabled=true;
        const auto count=operation.geometryType==CropShape::Sphere?2:4;
        for(int i=0;i<count;++i)handles[i].widget->On();
        return true;
    }
    void Sync(int skip=-1) {
        if(!ready)return;
        const bool isSphere=operation.geometryType==CropShape::Sphere;
        const auto& center=operation.centerInInputModel;const auto& axis=operation.axisInInputModel;
        const P radial=isSphere?P{1,0,0}:Radial(axis);
        const std::array<P,4> positions{center,Add(center,radial,operation.radius),
            Add(center,axis,-operation.height/2),Add(center,axis,operation.height/2)};
        for(int i=0;i<4;++i)if(i!=skip) {
            auto world=Transform(modelToWorld,positions[i]);handles[i].representation->SetWorldPosition(world.data());
        }
        if(isSphere) {
            sphere->SetCenter(center.data());sphere->SetRadius(operation.radius);mapper->SetInputConnection(sphere->GetOutputPort());
            actor->SetUserMatrix(modelToWorld);
        } else {
            cylinder->SetRadius(operation.radius);cylinder->SetHeight(operation.height);
            mapper->SetInputConnection(cylinder->GetOutputPort());
            const auto z=Cross(radial,axis);vtkNew<vtkMatrix4x4> local;local->Identity();
            for(int row=0;row<3;++row) {local->SetElement(row,0,radial[row]);local->SetElement(row,1,axis[row]);local->SetElement(row,2,z[row]);local->SetElement(row,3,center[row]);}
            vtkNew<vtkMatrix4x4> world;vtkMatrix4x4::Multiply4x4(modelToWorld,local,world);actor->SetUserMatrix(world);
        }
    }
    void OnEvent(std::size_t index,unsigned long event) {
        if(!enabled||index>=(operation.geometryType==CropShape::Sphere?2U:4U))return;
        try {if(gate&&!gate()){active=-1;Sync();return;}}catch(...){active=-1;Sync();return;}
        if(event==vtkCommand::StartInteractionEvent) {active=static_cast<int>(index);if(callback)callback(CropInteractionPhase::Hover);return;}
        if(active!=static_cast<int>(index)){Sync();return;}
        if(event==vtkCommand::InteractionEvent) {
            P position;handles[index].representation->GetWorldPosition(position.data());position=Transform(worldToModel,position);
            auto next=operation;
            if(index==0)next.centerInInputModel=position;
            else if(index==1) {
                auto delta=Add(position,operation.centerInInputModel,-1);
                if(operation.geometryType==CropShape::Cylinder)delta=Add(delta,operation.axisInInputModel,-Dot(delta,operation.axisInInputModel));
                next.radius=std::hypot(delta[0],delta[1],delta[2]);
            } else {
                const auto opposite=Add(operation.centerInInputModel,operation.axisInInputModel,(index==2?1:-1)*operation.height/2);
                auto axis=index==2?Add(opposite,position,-1):Add(position,opposite,-1);
                next.height=std::hypot(axis[0],axis[1],axis[2]);
                if(next.height>0)next.axisInInputModel=Unit(axis);
                for(int i=0;i<3;++i)next.centerInInputModel[i]=position[i]*0.5+opposite[i]*0.5;
            }
            const auto valid=CropGeometry::Build(next);
            if(!valid){Sync();return;}
            operation=valid->GetOperation();Sync(static_cast<int>(index));
            if(callback)callback(CropInteractionPhase::Dragging);
        } else if(event==vtkCommand::EndInteractionEvent) {
            active=-1;Sync();if(callback)callback(CropInteractionPhase::Released);
        }
    }
    CropOpItem operation;bool ready=false,enabled=false;int active=-1;
    vtkRenderWindowInteractor* interactor=nullptr;vtkWeakPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkMatrix4x4> modelToWorld,worldToModel;
    vtkSmartPointer<vtkPolyDataMapper> mapper;vtkSmartPointer<vtkActor> actor;
    vtkSmartPointer<vtkSphereSource> sphere;vtkSmartPointer<vtkCylinderSource> cylinder;
    std::array<Handle,4> handles;
    std::function<void(CropInteractionPhase)> callback;
    std::function<bool()> gate;
};

CropCurveWidget::CropCurveWidget():m_impl(std::make_unique<Impl>()){}
CropCurveWidget::~CropCurveWidget()=default;
void CropCurveWidget::SetContext(vtkRenderWindowInteractor* interactor,vtkRenderer* renderer){m_impl->SetContext(interactor,renderer);}
bool CropCurveWidget::SetGeometry(CropOpItem operation,const CropMatrixDouble16Array& matrix){return m_impl->SetGeometry(std::move(operation),matrix);}
CropOpItem CropCurveWidget::GetGeometry() const{return m_impl->operation;}
void CropCurveWidget::SetCallback(std::function<void(CropInteractionPhase)> callback){m_impl->callback=std::move(callback);}
bool CropCurveWidget::SetEnabled(bool enabled){return m_impl->SetEnabled(enabled);}
bool CropCurveWidget::GetEnabled() const{return m_impl->enabled;}
vtkHandleWidget* CropCurveWidget::GetHandle(std::size_t index) const{return index<m_impl->handles.size()?m_impl->handles[index].widget.GetPointer():nullptr;}

bool CropCurveWidget::GetTransformSame(const CropMatrixDouble16Array& matrix) const {
    if(!m_impl->ready)return false;
    for(int row=0;row<4;++row)for(int column=0;column<4;++column)
        if(matrix[row*4+column]!=m_impl->modelToWorld->GetElement(row,column))return false;
    return true;
}
void CropCurveWidget::SetContextGate(std::function<bool()> gate){m_impl->gate=std::move(gate);}
