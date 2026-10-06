#pragma once

#include "OrthogonalCropTypes.h"
#include "Host/Types/HostViewTypes.h"
#include "Render/Support/FeatureOverlayBase.h"
#include "Render/Contracts/SlicePlaneState.h"
#include "Render/Support/SliceContourPlane.h"
#include <vtkActor.h>
#include <vtkCubeSource.h>
#include <vtkCutter.h>
#include <vtkCylinderSource.h>
#include <vtkPlaneSource.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkSphereSource.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkNew.h>
#include <cmath>
#include <iomanip>
#include <sstream>

// 私有几何投影不参与裁切 predicate、物化或历史计算。
class CropGeometryOverlay final : public FeatureOverlayBase {
public:
    explicit CropGeometryOverlay(HostRenderViewRole role)
    {
        m_axis = role == HostRenderViewRole::TopDownSlice ? 2
            : role == HostRenderViewRole::FrontBackSlice ? 1
            : role == HostRenderViewRole::LeftRightSlice ? 0 : -1;
        m_mapper->ScalarVisibilityOff(); m_actor->SetMapper(m_mapper);
        m_actor->GetProperty()->SetColor(0.95, 0.2, 0.2);
        m_actor->GetProperty()->SetRepresentationToWireframe();
        m_actor->GetProperty()->LightingOff(); m_actor->GetProperty()->SetLineWidth(1.5);
        m_actor->PickableOff();
        if (m_axis >= 0) {
            std::array<double,3> normal{}; normal[m_axis] = 1;
            m_sliceNormal=normal;
            m_plane->SetNormal(normal.data()); m_cutter->SetCutFunction(m_plane);
            m_cutter->GenerateTrianglesOff();
            m_mapper->SetInputConnection(m_cutter->GetOutputPort());
            AttachProp(m_actor);
        }
        m_text->GetTextProperty()->SetFontSize(12);
        m_text->GetTextProperty()->SetColor(1, 1, 1);
        m_text->GetTextProperty()->ShadowOn();
        m_text->SetPosition(12, 12);
        m_text->PickableOff(); AttachProp(m_text);
    }
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    void SetOverlayState(const FeatureOverlayState& state) override
    {
        (void)SliceContourPlane::SetPlane(*m_plane,state.cursor,m_sliceNormal,state.modelToWorld);
        double n[3];m_plane->GetNormal(n);
        const double dot=n[0]*m_normal[0]+n[1]*m_normal[1]+n[2]*m_normal[2];
        const double length=std::hypot(n[0],n[1],n[2])*std::hypot(m_normal[0],m_normal[1],m_normal[2]);
        const bool coplanar=m_shape==CropShape::Plane && length>0 && std::abs(dot)/length>1-1e-8
            && std::abs(m_plane->EvaluateFunction(m_center.data()))<1e-6;
        if(coplanar)m_mapper->SetInputData(m_mesh);
        else m_mapper->SetInputConnection(m_cutter->GetOutputPort());
        Set3DPropsTransform(state.modelToWorld);
    }
    void SetOperation(const CropOpItem& op, const CropBoundsDouble6Array& bounds)
    {
        m_shape=op.geometryType;
        m_center=op.planeCenterInInputModel;
        m_normal=op.planeNormalInInputModel;
        auto mesh = vtkSmartPointer<vtkPolyData>::New();
        std::ostringstream text; text << std::fixed << std::setprecision(2);
        vtkNew<vtkMatrix4x4> matrix; matrix->Identity();
        if (op.geometryType == CropShape::Box) {
            vtkNew<vtkCubeSource> source; source->SetBounds(-1,1,-1,1,-1,1); source->Update();
            mesh->ShallowCopy(source->GetOutput()); matrix->DeepCopy(op.boxToInputModelMatrix.data());
            text << "Box ";
            for (int axis=0;axis<3;++axis) {
                double squared=0;for(int row=0;row<3;++row) squared+=std::pow(matrix->GetElement(row,axis),2);
                if(axis)text<<" x ";text<<2*std::sqrt(squared);
            }
            text << " mm";
        } else if (op.geometryType == CropShape::Sphere) {
            vtkNew<vtkSphereSource> source; source->SetCenter(op.centerInInputModel.data());
            source->SetRadius(op.radius);source->SetThetaResolution(64);source->SetPhiResolution(48);source->Update();
            mesh->ShallowCopy(source->GetOutput()); text << "Sphere R=" << op.radius << " mm";
        } else if (op.geometryType == CropShape::Cylinder) {
            vtkNew<vtkCylinderSource> source;source->SetRadius(op.radius);source->SetHeight(op.height);
            source->SetResolution(64);source->CappingOn();source->Update();mesh->ShallowCopy(source->GetOutput());
            const auto& axis=op.axisInInputModel;
            std::array<double,3> radial{axis[1],-axis[0],0};
            if(std::hypot(radial[0],radial[1])<1e-12)radial={1,0,0};
            const double length=std::hypot(radial[0],radial[1],radial[2]);
            for(auto& value:radial)value/=length;
            const std::array<double,3> third{radial[1]*axis[2]-radial[2]*axis[1],
                radial[2]*axis[0]-radial[0]*axis[2],radial[0]*axis[1]-radial[1]*axis[0]};
            for(int row=0;row<3;++row) {
                matrix->SetElement(row,0,radial[row]);matrix->SetElement(row,1,axis[row]);
                matrix->SetElement(row,2,third[row]);matrix->SetElement(row,3,op.centerInInputModel[row]);
            }
            text<<"Cylinder R="<<op.radius<<", L="<<op.height<<" mm";
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
            text<<"Plane N=("<<n[0]<<", "<<n[1]<<", "<<n[2]<<")";
        }
        vtkNew<vtkTransform> transform;transform->SetMatrix(matrix);
        vtkNew<vtkTransformPolyDataFilter> filter;filter->SetTransform(transform);filter->SetInputData(mesh);
        filter->Update();m_mesh->ShallowCopy(filter->GetOutput());
        m_cutter->SetInputData(m_mesh);m_text->SetInput(text.str().c_str());
    }
private:
    CropShape m_shape=CropShape::Box;
    std::array<double,3> m_center{},m_normal{};
    std::array<double,3> m_sliceNormal{0,0,1};
    int m_axis=-1;
    vtkSmartPointer<vtkPolyData> m_mesh=vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkPlane> m_plane=vtkSmartPointer<vtkPlane>::New();
    vtkSmartPointer<vtkCutter> m_cutter=vtkSmartPointer<vtkCutter>::New();
    vtkSmartPointer<vtkPolyDataMapper> m_mapper=vtkSmartPointer<vtkPolyDataMapper>::New();
    vtkSmartPointer<vtkActor> m_actor=vtkSmartPointer<vtkActor>::New();
    vtkSmartPointer<vtkTextActor> m_text=vtkSmartPointer<vtkTextActor>::New();
};
