#include "MultiSliceStrategy.h"
#include <vtkPlane.h>
#include <vtkImageProperty.h>
#include <vtkTransform.h>
MultiSliceStrategy::MultiSliceStrategy() {
    for (int i = 0; i < 3; i++) {
        m_slices[i] = vtkSmartPointer<vtkImageSlice>::New();
        m_mappers[i] = vtkSmartPointer<vtkImageResliceMapper>::New();
        m_slicePlanes[i] = vtkSmartPointer<vtkPlane>::New();
        m_mappers[i]->SetSlicePlane(m_slicePlanes[i]);
        m_slices[i]->SetMapper(m_mappers[i]);
        m_mappers[i]->SliceFacesCameraOff();
        m_mappers[i]->SliceAtFocalPointOff();
      SetManagedProp(m_slices[i]);
    }

    m_slicePlanes[0]->SetNormal(1, 0, 0);
    m_slicePlanes[1]->SetNormal(0, 1, 0);
    m_slicePlanes[2]->SetNormal(0, 0, 1);
}

void MultiSliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    if (m_lastInput == data && m_mappers[0] && m_mappers[0]->GetInput()) {
        return;
    }
    m_lastInput = data;

    // 0:
    // (X normal), 1: Front_back (Y normal), 2: Top_down (Z normal)
    for (int i = 0; i < 3; i++) {
        m_mappers[i]->SetInputData(img);

        double center[3];
        img->GetCenter(center);
        m_slicePlanes[i]->SetOrigin(center);

        // 调整对比度
        double range[2];
        img->GetScalarRange(range);
        m_slices[i]->GetProperty()->SetColorWindow(range[1] - range[0]);
        m_slices[i]->GetProperty()->SetColorLevel((range[1] + range[0]) * 0.5);
    }
}

void MultiSliceStrategy::SetAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix) {
    auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
    mat->DeepCopy(modelMatrix.data());

    auto inv = vtkSmartPointer<vtkTransform>::New();
    inv->SetMatrix(mat);
    inv->Inverse();

    double cursorWorld4[4] = { cursorWorld[0], cursorWorld[1], cursorWorld[2], 1.0 };
    double cursorModel4[4] = { 0.0, 0.0, 0.0, 1.0 };
    inv->MultiplyPoint(cursorWorld4, cursorModel4);

    for (int i = 0; i < 3; i++) {
        auto plane = m_mappers[i]->GetSlicePlane();
        auto input = m_mappers[i]->GetInput();
        if (!plane || !input) continue;

        double planeOrigin[3];
        plane->GetOrigin(planeOrigin);

        double bounds[6];
        input->GetBounds(bounds);

        planeOrigin[0] = std::max(bounds[0], std::min(cursorModel4[0], bounds[1]));
        planeOrigin[1] = std::max(bounds[2], std::min(cursorModel4[1], bounds[3]));
        planeOrigin[2] = std::max(bounds[4], std::min(cursorModel4[2], bounds[5]));
        plane->SetOrigin(planeOrigin);
    }
}

void MultiSliceStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (HasFlag(flags , UpdateFlags::Cursor) || HasFlag(flags, UpdateFlags::Transform)) {
        SetAllPositions(params.cursor.data(), params.modelMatrix);
    }

    if (HasFlag(flags, UpdateFlags::WindowLevel) || HasFlag(flags, UpdateFlags::Material)) {
        for (int i = 0; i < 3; i++) {
            if (m_slices[i] && m_slices[i]->GetProperty()) {
                m_slices[i]->GetProperty()->SetColorWindow(params.windowLevel.windowWidth);
                m_slices[i]->GetProperty()->SetColorLevel(params.windowLevel.windowCenter);
                m_slices[i]->GetProperty()->SetOpacity(params.material.opacity);
            }
        }
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        const int vis = (params.visibilityMask & VisFlags::Planes3D) ? 1 : 0;
        for (int i = 0; i < 3; i++) {
            if (m_slices[i]) m_slices[i]->SetVisibility(vis);
        }
    }
}

void MultiSliceStrategy::SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer) {
    BaseVisualStrategy::SetRendererAttached(renderer);
}
