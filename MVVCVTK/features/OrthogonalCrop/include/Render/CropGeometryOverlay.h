#pragma once

#include "OrthogonalCropTypes.h"
#include "Host/Types/HostViewTypes.h"
#include "Render/Support/FeatureOverlayBase.h"
#include "Render/Support/RenderTextStyle.h"
#include "Render/Contracts/SlicePlaneState.h"
#include "Render/Support/SliceContourPlane.h"
#include "Render/CropSphereVisual.h"
#include "Render/CropCylinderVisual.h"
#include <vtkActor2D.h>
#include <vtkCoordinate.h>
#include <vtkCubeSource.h>
#include <vtkCutter.h>
#include <vtkCamera.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkFeatureEdges.h>
#include <vtkPlaneSource.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty2D.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkNew.h>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>

// 私有几何投影不参与裁切 predicate、物化或历史计算。
class CropGeometryOverlay final : public FeatureOverlayBase {
public:
    explicit CropGeometryOverlay(HostRenderViewRole role)
    {
        m_sectionObserver=vtkSmartPointer<vtkCallbackCommand>::New();m_sectionObserver->SetClientData(this);
        m_sectionObserver->SetCallback([](vtkObject*,unsigned long,void* self,void*){if(self)static_cast<CropGeometryOverlay*>(self)->UpdateCylinderSection();});
        m_axis = role == HostRenderViewRole::TopDownSlice ? 2
            : role == HostRenderViewRole::FrontBackSlice ? 1
            : role == HostRenderViewRole::LeftRightSlice ? 0 : -1;
        if(m_axis<0){m_sphereVisual=std::make_unique<CropSphereVisual>();m_cylinderVisual=std::make_unique<CropCylinderVisual>();}
        m_projection->SetCoordinateSystemToWorld();
        m_mapper->SetTransformCoordinate(m_projection);m_mapper->TransformCoordinateUseDoubleOn();
        m_mapper->SetInputConnection(m_displayGeometry->GetOutputPort());
        m_displayTransform->SetMatrix(m_modelToWorld);m_displayGeometry->SetTransform(m_displayTransform);
        m_planeEdges->SetInputData(m_mesh);m_planeEdges->BoundaryEdgesOn();
        m_planeEdges->FeatureEdgesOff();m_planeEdges->NonManifoldEdgesOff();m_planeEdges->ManifoldEdgesOff();
        m_mapper->ScalarVisibilityOff(); m_actor->SetMapper(m_mapper);
        m_actor->GetProperty()->SetColor(0.95, 0.2, 0.2);
        m_actor->GetProperty()->SetLineWidth(1.5);
        m_actor->PickableOff();m_actor->UseBoundsOff();
        if (m_axis >= 0) {
            // 切片辅助线通过本 prop 的前景投影绘制，不修改 VTK 全局重合几何模式。
            m_actor->GetProperty()->SetDisplayLocationToForeground();
            std::array<double,3> normal{}; normal[m_axis] = 1;
            m_sliceNormal=normal;
            m_plane->SetNormal(normal.data()); m_cutter->SetCutFunction(m_plane);
            m_cutter->GenerateTrianglesOff();
            m_displayGeometry->SetInputConnection(m_cutter->GetOutputPort());
            AttachProp(m_actor);
        }
        m_text->GetTextProperty()->SetFontSize(12);
        RenderTextStyle::SetFont(*m_text->GetTextProperty());
        m_text->GetTextProperty()->SetColor(1, 1, 1);
        m_text->GetTextProperty()->ShadowOn();
        m_text->SetPosition(12, 12);
        m_text->PickableOff();m_text->UseBoundsOff(); AttachProp(m_text);
    }
    ~CropGeometryOverlay() override {
        if(m_renderer)DetachRenderer(m_renderer.GetPointer());
        m_sectionObserver->SetClientData(nullptr);
    }
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        if(!renderer)return;
        if(m_renderer.GetPointer()!=renderer.GetPointer())StopSectionObserver();
        FeatureOverlayBase::AttachRenderer(renderer);
        m_renderer=renderer;m_sectionDirty=true;UpdateCurveRenderer();UpdateCylinderSection();
    }
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        StopSectionObserver();
        if(m_sphereVisual)m_sphereVisual->DetachRenderer();
        if(m_cylinderVisual)m_cylinderVisual->DetachRenderer();
        m_renderer=nullptr;
        FeatureOverlayBase::DetachRenderer(renderer);
    }
    void SetCurveEditing(bool editing)
    {
        m_curveEditing=editing;
        if(m_sphereVisual)m_sphereVisual->SetVisible(m_shape==CropShape::Sphere&&!editing);
        if(m_cylinderVisual)m_cylinderVisual->SetVisible(m_shape==CropShape::Cylinder&&!editing);
    }
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    void SetOverlayState(const FeatureOverlayState& state) override
    {
        (void)SliceContourPlane::SetPlane(*m_plane,state.cursor,m_sliceNormal,state.modelToWorld);
        m_modelToWorld->DeepCopy(state.modelToWorld.data());
        m_displayTransform->SetMatrix(m_modelToWorld);
        if(m_shape==CropShape::Sphere) {
            UpdateSphere();Set3DPropsTransform(state.modelToWorld);return;
        }
        if(m_shape==CropShape::Cylinder&&m_cylinderVisual)m_cylinderVisual->SetGeometry(m_operation,m_modelToWorld);
        if(m_shape==CropShape::Cylinder) {UpdateCylinderSection();Set3DPropsTransform(state.modelToWorld);return;}
        double n[3];m_plane->GetNormal(n);
        const double dot=n[0]*m_normal[0]+n[1]*m_normal[1]+n[2]*m_normal[2];
        const double length=std::hypot(n[0],n[1],n[2])*std::hypot(m_normal[0],m_normal[1],m_normal[2]);
        const bool isCoplanar=m_shape==CropShape::Plane && length>0 && std::abs(dot)/length>1-1e-8
            && std::abs(m_plane->EvaluateFunction(m_center.data()))<1e-6;
        if(isCoplanar)m_displayGeometry->SetInputConnection(m_planeEdges->GetOutputPort());
        else m_displayGeometry->SetInputConnection(m_cutter->GetOutputPort());
        Set3DPropsTransform(state.modelToWorld);
    }
    void SetOperation(const CropOpItem& op, const CropBoundsDouble6Array& bounds)
    {
        m_shape=op.geometryType;m_operation=op;UpdateCurveRenderer();
        if(m_cylinderVisual){m_cylinderVisual->SetGeometry(op,m_modelToWorld);m_cylinderVisual->SetVisible(m_shape==CropShape::Cylinder&&!m_curveEditing);}
        if(m_sphereVisual)m_sphereVisual->SetVisible(false);m_text->SetVisibility(true);
        m_center=op.planeCenterInInputModel;
        m_normal=op.planeNormalInInputModel;
        if(op.geometryType==CropShape::Cylinder) {
            m_sectionDirty=true;UpdateCylinderSection();
            std::ostringstream text;text<<std::fixed<<std::setprecision(2)<<u8"半径："<<op.radius<<u8" mm  长度："<<op.height<<" mm";
            m_text->SetInput(text.str().c_str());m_text->SetVisibility(m_axis>=0);return;
        }
        auto mesh = vtkSmartPointer<vtkPolyData>::New();
        std::ostringstream text; text << std::fixed << std::setprecision(2);
        vtkNew<vtkMatrix4x4> matrix; matrix->Identity();
        if (op.geometryType == CropShape::Box) {
            vtkNew<vtkCubeSource> source; source->SetBounds(-1,1,-1,1,-1,1); source->Update();
            mesh->ShallowCopy(source->GetOutput()); matrix->DeepCopy(op.boxToInputModelMatrix.data());
            text << u8"尺寸：";
            for (int axis=0;axis<3;++axis) {
                double squared=0;for(int row=0;row<3;++row) squared+=std::pow(matrix->GetElement(row,axis),2);
                if(axis)text<<u8" × ";text<<2*std::sqrt(squared);
            }
            text << " mm";
        } else if (op.geometryType == CropShape::Sphere) {
            m_center=op.centerInInputModel;m_radius=op.radius;UpdateSphere();
            text << u8"半径：" << op.radius << " mm";m_text->SetInput(text.str().c_str());
            m_text->SetVisibility(m_axis>=0);return;
        } else {
            const auto& n=op.planeNormalInInputModel;
            const double radius=std::hypot(bounds[1]-bounds[0],bounds[3]-bounds[2],bounds[5]-bounds[4])*0.5;
            std::array<double,3> a{n[1],-n[0],0};
            if(std::hypot(a[0],a[1])<1e-12)a={1,0,0};
            const double length=std::hypot(a[0],a[1],a[2]);for(auto& value:a)value*=radius/length;
            const std::array<double,3> c{n[1]*a[2]-n[2]*a[1],n[2]*a[0]-n[0]*a[2],n[0]*a[1]-n[1]*a[0]};
            std::array<double,3> p{},q{},r{};
            for(int i=0;i<3;++i){p[i]=op.planeCenterInInputModel[i]-a[i]-c[i];q[i]=p[i]+2*a[i];r[i]=p[i]+2*c[i];}
            vtkNew<vtkPlaneSource> source;source->SetOrigin(p.data());source->SetPoint1(q.data());source->SetPoint2(r.data());
            source->Update();mesh->ShallowCopy(source->GetOutput());
            text<<u8"法线：("<<n[0]<<", "<<n[1]<<", "<<n[2]<<")";
        }
        vtkNew<vtkTransform> transform;transform->SetMatrix(matrix);
        vtkNew<vtkTransformPolyDataFilter> filter;filter->SetTransform(transform);filter->SetInputData(mesh);
        filter->Update();m_mesh->ShallowCopy(filter->GetOutput());
        m_cutter->SetInputData(m_mesh);m_displayGeometry->SetInputConnection(m_cutter->GetOutputPort());
        m_text->SetInput(text.str().c_str());
        if(m_shape==CropShape::Cylinder)m_text->SetVisibility(m_axis>=0);
    }
private:
    void StopSectionObserver(){if(m_renderer&&m_sectionTag)m_renderer->RemoveObserver(m_sectionTag);m_sectionTag=0;}
    void UpdateCurveRenderer(){
        if(!m_renderer)return;
        if(m_axis>=0){
            if(m_shape==CropShape::Cylinder){if(!m_sectionTag)m_sectionTag=m_renderer->AddObserver(vtkCommand::StartEvent,m_sectionObserver,1.f);}
            else StopSectionObserver();
            return;
        }
        if(m_shape==CropShape::Sphere)m_sphereVisual->AttachRenderer(m_renderer);else m_sphereVisual->DetachRenderer();
        if(m_shape==CropShape::Cylinder)m_cylinderVisual->AttachRenderer(m_renderer);else m_cylinderVisual->DetachRenderer();
    }
    void UpdateCylinderSection(){
        if(m_axis<0||m_shape!=CropShape::Cylinder||!m_renderer||!m_actor->GetVisibility())return;
        auto* camera=m_renderer->GetActiveCamera();const auto* size=m_renderer->GetSize();const auto* viewport=m_renderer->GetViewport();
        if(size[0]<=0||size[1]<=0)return;
        const std::array<int,2> dimensions{size[0],size[1]};const std::array<double,4> area{viewport[0],viewport[1],viewport[2],viewport[3]};
        if(!m_sectionDirty&&m_sectionCamera==camera&&m_sectionCameraTime==camera->GetMTime()
            &&m_sectionPlaneTime==m_plane->GetMTime()&&m_sectionTransformTime==m_modelToWorld->GetMTime()
            &&m_sectionSize==dimensions&&m_sectionViewport==area)return;
        CropCylinderVisual::BuildSection(*m_mesh,m_operation,*m_plane,m_renderer,m_modelToWorld);
        m_displayGeometry->SetInputData(m_mesh);m_actor->GetProperty()->SetPointSize(4);
        m_sectionDirty=false;m_sectionCamera=camera;m_sectionCameraTime=camera->GetMTime();
        m_sectionPlaneTime=m_plane->GetMTime();m_sectionTransformTime=m_modelToWorld->GetMTime();m_sectionSize=dimensions;m_sectionViewport=area;
    }
    void UpdateSphere()
    {
        if(m_axis>=0) {
            CropSphereVisual::BuildSection(*m_mesh,m_center,m_radius,*m_plane);
            m_displayGeometry->SetInputData(m_mesh);m_actor->GetProperty()->SetPointSize(4);
        } else {
            m_sphereVisual->SetGeometry(m_center,m_radius,m_modelToWorld);
            m_sphereVisual->SetState(CropSphereVisual::State::Passive);
            m_sphereVisual->SetVisible(!m_curveEditing);
        }
    }
    CropShape m_shape=CropShape::Box;
    std::unique_ptr<CropSphereVisual> m_sphereVisual;
    std::unique_ptr<CropCylinderVisual> m_cylinderVisual;
    vtkWeakPointer<vtkRenderer> m_renderer;CropOpItem m_operation;
    vtkSmartPointer<vtkCallbackCommand> m_sectionObserver;vtkWeakPointer<vtkCamera> m_sectionCamera;unsigned long m_sectionTag=0;
    bool m_sectionDirty=true;unsigned long long m_sectionCameraTime=0,m_sectionPlaneTime=0,m_sectionTransformTime=0;
    std::array<int,2> m_sectionSize{};std::array<double,4> m_sectionViewport{};
    bool m_curveEditing=false;
    double m_radius=1;
    vtkSmartPointer<vtkMatrix4x4> m_modelToWorld=vtkSmartPointer<vtkMatrix4x4>::New();
    std::array<double,3> m_center{},m_normal{};
    std::array<double,3> m_sliceNormal{0,0,1};
    int m_axis=-1;
    vtkSmartPointer<vtkPolyData> m_mesh=vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkPlane> m_plane=vtkSmartPointer<vtkPlane>::New();
    vtkSmartPointer<vtkCutter> m_cutter=vtkSmartPointer<vtkCutter>::New();
    vtkSmartPointer<vtkFeatureEdges> m_planeEdges=vtkSmartPointer<vtkFeatureEdges>::New();
    vtkSmartPointer<vtkTransform> m_displayTransform=vtkSmartPointer<vtkTransform>::New();
    vtkSmartPointer<vtkTransformPolyDataFilter> m_displayGeometry=vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    vtkSmartPointer<vtkCoordinate> m_projection=vtkSmartPointer<vtkCoordinate>::New();
    vtkSmartPointer<vtkPolyDataMapper2D> m_mapper=vtkSmartPointer<vtkPolyDataMapper2D>::New();
    vtkSmartPointer<vtkActor2D> m_actor=vtkSmartPointer<vtkActor2D>::New();
    vtkSmartPointer<vtkTextActor> m_text=vtkSmartPointer<vtkTextActor>::New();
};
