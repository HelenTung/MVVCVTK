#pragma once
#include "OrthogonalCropTypes.h"
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>
class vtkActor;class vtkPolyData;class vtkMatrix4x4;class vtkRenderer;class vtkCamera;class vtkCallbackCommand;class vtkPlane;
// 圆柱显示只读取参数；轮廓随视角更新，绝不反写裁切体。
class CropCylinderVisual final {
public:
    CropCylinderVisual();~CropCylinderVisual();
    void AttachRenderer(vtkRenderer*);void DetachRenderer();
    void SetGeometry(const CropOpItem&,vtkMatrix4x4*);
    void SetVisible(bool);void SetRadiusActive(bool);void UpdateView();
    CropVectorDouble3Array GetRadiusHandleDirection(const CropVectorDouble3Array&) const;
    static void BuildSection(vtkPolyData&,const CropOpItem&,vtkPlane&,vtkRenderer*,vtkMatrix4x4*);
private:
    CropOpItem m_op;bool m_visible=false,m_radiusActive=false,m_dirty=true;
    vtkSmartPointer<vtkPolyData> m_outline,m_auxiliary;
    vtkSmartPointer<vtkActor> m_actor,m_auxActor;
    vtkSmartPointer<vtkMatrix4x4> m_matrix,m_inverse;
    vtkWeakPointer<vtkRenderer> m_renderer;vtkWeakPointer<vtkCamera> m_camera;
    vtkSmartPointer<vtkCallbackCommand> m_observer;unsigned long m_tag=0;
    unsigned long long m_cameraTime=0;std::array<int,2> m_size{};std::array<double,4> m_viewport{};
};
