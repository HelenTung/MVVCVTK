#pragma once

#include "OrthogonalCropTypes.h"
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

class vtkActor;
class vtkCallbackCommand;
class vtkCamera;
class vtkMatrix4x4;
class vtkPlane;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkSphereSource;

// 球显示的私有投影；模型空间的球心和半径始终由现有 operation 提供。
class CropSphereVisual final {
public:
    enum class State { Idle, Hover, Dragging, Passive };
    CropSphereVisual();
    ~CropSphereVisual();
    CropSphereVisual(const CropSphereVisual&) = delete;
    CropSphereVisual& operator=(const CropSphereVisual&) = delete;
    void SetGeometry(const CropVectorDouble3Array& center, double radius, vtkMatrix4x4* modelToWorld);
    void AttachRenderer(vtkRenderer* renderer);
    void DetachRenderer();
    void SetVisible(bool visible);
    void SetState(State state);
    void UpdateView();
    CropVectorDouble3Array GetRadiusHandle() const;
    bool GetNearOutline(int x, int y) const;
    static void BuildSection(vtkPolyData& output, const CropVectorDouble3Array& center,
        double radius, vtkPlane& plane);
private:
    void ApplyStyle();
    CropVectorDouble3Array m_center{}, m_radiusHandle{};
    double m_radius = 1;
    bool m_visible = false, m_viewDirty = true;
    State m_state = State::Idle;
    vtkWeakPointer<vtkRenderer> m_renderer;
    vtkWeakPointer<vtkCamera> m_camera;
    unsigned long m_renderTag = 0;
    unsigned long long m_cameraTime = 0;
    vtkSmartPointer<vtkCallbackCommand> m_renderObserver;
    vtkSmartPointer<vtkMatrix4x4> m_modelToWorld, m_worldToModel;
    vtkSmartPointer<vtkPolyData> m_rings, m_outline;
    vtkSmartPointer<vtkPolyDataMapper> m_ringMapper, m_outlineMapper, m_shellMapper;
    vtkSmartPointer<vtkActor> m_ringActor, m_outlineActor, m_shellActor;
    vtkSmartPointer<vtkSphereSource> m_shell;
};
