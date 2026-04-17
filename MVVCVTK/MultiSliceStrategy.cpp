#include "MultiSliceStrategy.h"
#include <vtkPlane.h>
#include <vtkImageProperty.h>

MultiSliceStrategy::MultiSliceStrategy() {
    for (int i = 0; i < 3; i++) {
        m_slices[i] = vtkSmartPointer<vtkImageSlice>::New();
        m_mappers[i] = vtkSmartPointer<vtkImageResliceMapper>::New();
        m_slices[i]->SetMapper(m_mappers[i]);
		RegisterProp(m_slices[i]);
    }
}

void MultiSliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    // 0: 
    // (X normal), 1: Front_back (Y normal), 2: Top_down (Z normal)
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
    int dims[3] = { 0, 0, 0 };
    if (auto input = m_mappers[0]->GetInput()) {
        input->GetDimensions(dims);
    }

    // 边界保护：确保索引在 [0, dims[i]-1] 之间
    m_indices[0] = std::max(0, std::min(x, dims[0] - 1));
    m_indices[1] = std::max(0, std::min(y, dims[1] - 1));
    m_indices[2] = std::max(0, std::min(z, dims[2] - 1));

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
    if (HasFlag(flags , UpdateFlags::Cursor)) return;
    UpdateAllPositions(params.cursor[0], params.cursor[1], params.cursor[2]);

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
        const int vis = (params.visibilityMask & VisFlags::ClipPlanes) ? 1 : 0;
        for (int i = 0; i < 3; i++) {
            if (m_slices[i]) m_slices[i]->SetVisibility(vis);
        }
    }
}

void MultiSliceStrategy::Attach(vtkSmartPointer<vtkRenderer> renderer) {
    BaseVisualStrategy::Attach(renderer);
    renderer->SetBackground(0.1, 0.1, 0.1);
}