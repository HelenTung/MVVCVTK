#include "MultiSliceStrategy.h"
#include <vtkPlane.h>
#include <vtkImageProperty.h>

MultiSliceStrategy::MultiSliceStrategy() {
    for (int i = 0; i < 3; i++) {
        m_slices[i] = vtkSmartPointer<vtkImageSlice>::New();
        m_mappers[i] = vtkSmartPointer<vtkImageResliceMapper>::New();
        m_slices[i]->SetMapper(m_mappers[i]);
    }
}

void MultiSliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    // 0: Sagittal (X normal), 1: Coronal (Y normal), 2: Axial (Z normal)
    for (int i = 0; i < 3; i++) {
        m_mappers[i]->SetInputData(img);
        auto plane = vtkSmartPointer<vtkPlane>::New();

        double center[3];
        img->GetCenter(center);
        plane->SetOrigin(center);

        if (i == 0) plane->SetNormal(1, 0, 0); // X
        else if (i == 1) plane->SetNormal(0, 1, 0); // Y
        else plane->SetNormal(0, 0, 1); // Z

        m_mappers[i]->SetSlicePlane(plane);

        // 调整对比度
        double range[2];
        img->GetScalarRange(range);
        m_slices[i]->GetProperty()->SetColorWindow(range[1] - range[0]);
        m_slices[i]->GetProperty()->SetColorLevel((range[1] + range[0]) * 0.5);
    }
}

void MultiSliceStrategy::UpdateAllPositions(int x, int y, int z) {
    m_indices[0] = x;
    m_indices[1] = y;
    m_indices[2] = z;

    for (int i = 0; i < 3; i++) {
        auto plane = m_mappers[i]->GetSlicePlane();
        auto input = m_mappers[i]->GetInput();
        if (!input) continue;

        double origin[3], spacing[3];
        input->GetOrigin(origin);
        input->GetSpacing(spacing);

        double planeOrigin[3];
        plane->GetOrigin(planeOrigin);

        planeOrigin[i] = origin[i] + (m_indices[i] * spacing[i]);
        plane->SetOrigin(planeOrigin);
    }
}

void MultiSliceStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (!((int)flags & (int)UpdateFlags::Cursor)) return;
    UpdateAllPositions(params.cursor[0], params.cursor[1], params.cursor[2]);
}

void MultiSliceStrategy::Attach(vtkSmartPointer<vtkRenderer> renderer) {
    for (int i = 0; i < 3; i++) renderer->AddViewProp(m_slices[i]);
    renderer->SetBackground(0.1, 0.1, 0.1);
}

void MultiSliceStrategy::Detach(vtkSmartPointer<vtkRenderer> renderer) {
    for (int i = 0; i < 3; i++) renderer->RemoveViewProp(m_slices[i]);
}