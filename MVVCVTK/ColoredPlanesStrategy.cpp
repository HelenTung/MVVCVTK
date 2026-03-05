#include "ColoredPlanesStrategy.h"
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkMatrix4x4.h>

ColoredPlanesStrategy::ColoredPlanesStrategy() {
    double colors[3][3] = {
        {1.0, 0.0, 0.0}, // Sagittal
        {0.0, 1.0, 0.0}, // Coronal
        {0.0, 0.0, 1.0}  // Axial
    };

    for (int i = 0; i < 3; i++) {
        m_planeSources[i] = vtkSmartPointer<vtkPlaneSource>::New();

        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputConnection(m_planeSources[i]->GetOutputPort());

        m_planeActors[i] = vtkSmartPointer<vtkActor>::New();
        m_planeActors[i]->SetMapper(mapper);
        m_planeActors[i]->GetProperty()->SetColor(colors[i]);
        m_planeActors[i]->GetProperty()->SetOpacity(0.2); // 设置半透明
        m_planeActors[i]->GetProperty()->SetLighting(false); // 关闭光照，显示纯色
    }

    // 设置每个平面的法线方向
    m_planeSources[0]->SetNormal(1.0, 0.0, 0.0); // X-axis normal (Sagittal)
    m_planeSources[1]->SetNormal(0.0, 1.0, 0.0); // Y-axis normal (Coronal)
    m_planeSources[2]->SetNormal(0.0, 0.0, 1.0); // Z-axis normal (Axial)
}

void ColoredPlanesStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;
    m_imageData = img;

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    m_imageData->GetBounds(bounds);
    
    // 取各轴的中心值作为初始平面位置
    double centerX = (bounds[0] + bounds[1]) * 0.5;
    double centerY = (bounds[2] + bounds[3]) * 0.5;
    double centerZ = (bounds[4] + bounds[5]) * 0.5;

    // 根据数据边界定义每个平面的大小
    // Sagittal Plane (YZ)，法线 X，初始位于 X 中心
    m_planeSources[0]->SetOrigin(centerX, bounds[2], bounds[4]);
    m_planeSources[0]->SetPoint1(centerX, bounds[3], bounds[4]);
    m_planeSources[0]->SetPoint2(centerX, bounds[2], bounds[5]);

    // Coronal Plane (XZ)，法线 Y，初始位于 Y 中心
    m_planeSources[1]->SetOrigin(bounds[0], centerY, bounds[4]);
    m_planeSources[1]->SetPoint1(bounds[1], centerY, bounds[4]);
    m_planeSources[1]->SetPoint2(bounds[0], centerY, bounds[5]);

    // Axial Plane (XY)，法线 Z，初始位于 Z 中心
    m_planeSources[2]->SetOrigin(bounds[0], bounds[2], centerZ);
    m_planeSources[2]->SetPoint1(bounds[1], bounds[2], centerZ);
    m_planeSources[2]->SetPoint2(bounds[0], bounds[3], centerZ);
}

void ColoredPlanesStrategy::UpdateAllPositions(int x, int y, int z) {
    if (!m_imageData) return;

    double bounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    double spacing[3] = { 1.0, 1.0, 1.0 };
    m_imageData->GetBounds(bounds);
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    double physX = origin[0] + x * spacing[0];
    double physY = origin[1] + y * spacing[1];
    double physZ = origin[2] + z * spacing[2];

    // Sagittal
    m_planeSources[0]->SetOrigin(physX, bounds[2], bounds[4]);
    m_planeSources[0]->SetPoint1(physX, bounds[3], bounds[4]);
    m_planeSources[0]->SetPoint2(physX, bounds[2], bounds[5]);

    // Coronal   
    m_planeSources[1]->SetOrigin(bounds[0], physY, bounds[4]);
    m_planeSources[1]->SetPoint1(bounds[1], physY, bounds[4]);
    m_planeSources[1]->SetPoint2(bounds[0], physY, bounds[5]);

    // Axial
    m_planeSources[2]->SetOrigin(bounds[0], bounds[2], physZ);
    m_planeSources[2]->SetPoint1(bounds[1], bounds[2], physZ);
    m_planeSources[2]->SetPoint2(bounds[0], bounds[3], physZ);

    for (int i = 0; i < 3; i++) {
        m_planeSources[i]->Modified();
    }
}

void ColoredPlanesStrategy::Attach(vtkSmartPointer<vtkRenderer> renderer) {
    for (int i = 0; i < 3; i++) renderer->AddActor(m_planeActors[i]);
}

void ColoredPlanesStrategy::Detach(vtkSmartPointer<vtkRenderer> renderer) {
    for (int i = 0; i < 3; i++) renderer->RemoveActor(m_planeActors[i]);
}

int ColoredPlanesStrategy::GetPlaneAxis(vtkActor* actor) {
    for (int i = 0; i < 3; ++i) {
        if (m_planeActors[i] == actor) {
            return i; // 0 for X, 1 for Y, 2 for Z
        }
    }
    return -1; // 未匹配
}

void ColoredPlanesStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (HasFlag(flags, UpdateFlags::Cursor)) {
        UpdateAllPositions(params.cursor[0], params.cursor[1], params.cursor[2]);
    }

    if (HasFlag(flags, UpdateFlags::Transform)) {
        auto vtkMat = vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMat->DeepCopy(params.modelMatrix.data());

        for (int i = 0; i < 3; i++) {
            if (m_planeActors[i]) {
                m_planeActors[i]->SetUserMatrix(vtkMat);
            }
        }
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        const int vis = (params.visibilityMask & VisFlags::ClipPlanes) ? 1 : 0;
        for (int i = 0; i < 3; i++) {
            if (m_planeActors[i]) m_planeActors[i]->SetVisibility(vis);
        }
    }
}