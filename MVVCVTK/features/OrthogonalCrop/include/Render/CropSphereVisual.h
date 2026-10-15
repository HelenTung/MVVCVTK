#pragma once

#include "OrthogonalCropTypes.h"
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>
#include <array>

class vtkActor;
class vtkCallbackCommand;
class vtkCamera;
class vtkMatrix4x4;
class vtkPlane;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkUnsignedCharArray;

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
    void SetRadiusHandleDirection(const CropVectorDouble3Array& direction);
    void UpdateView();
    CropVectorDouble3Array GetRadiusHandle() const;
    bool GetNearOutline(int x, int y) const;
    static void BuildSection(vtkPolyData& output, const CropVectorDouble3Array& center,
        double radius, vtkPlane& plane);
private:
    void ApplyStyle();
    void AppendCircle(vtkPolyData& output, const CropVectorDouble3Array& center,
        double radius, const CropVectorDouble3Array& a, const CropVectorDouble3Array& b);
    CropVectorDouble3Array Project(const CropVectorDouble3Array& point) const;
    bool GetHandleUsable(const CropVectorDouble3Array& point) const;
    void UpdateHandle();
    CropVectorDouble3Array m_center{}, m_radiusHandle{};
    CropVectorDouble3Array m_handleDirection{}, m_viewNormal{};
    double m_radius = 1;
    double m_tangentOffset = 0;
    bool m_visible = false, m_viewDirty = true, m_hasHandleDirection = false, m_handleDirty = true;
    std::array<int, 4> m_viewportPixels{};
    State m_state = State::Idle;
    vtkWeakPointer<vtkRenderer> m_renderer;
    vtkWeakPointer<vtkCamera> m_camera;
    unsigned long m_renderTag = 0;
    unsigned long long m_cameraTime = 0;
    vtkSmartPointer<vtkCallbackCommand> m_renderObserver;
    vtkSmartPointer<vtkMatrix4x4> m_modelToWorld, m_worldToModel;
    vtkSmartPointer<vtkPolyData> m_rings, m_outline;
    vtkSmartPointer<vtkPolyDataMapper> m_ringMapper, m_outlineMapper;
    vtkSmartPointer<vtkActor> m_ringActor, m_outlineActor;
    vtkSmartPointer<vtkUnsignedCharArray> m_ringColors;
};
