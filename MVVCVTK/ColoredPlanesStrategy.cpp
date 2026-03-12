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

    // 获取物理边界
    double b[6];
    m_imageData->GetBounds(b);

    // ── 关键点 1：将几何定义在相对 bounds 的基准位置 ──
    // Sagittal (YZ): 定义在 X 轴最小边界处
    m_planeSources[0]->SetOrigin(b[0], b[2], b[4]);
    m_planeSources[0]->SetPoint1(b[0], b[3], b[4]);
    m_planeSources[0]->SetPoint2(b[0], b[2], b[5]);

    // Coronal (XZ): 定义在 Y 轴最小边界处
    m_planeSources[1]->SetOrigin(b[0], b[2], b[4]);
    m_planeSources[1]->SetPoint1(b[1], b[2], b[4]);
    m_planeSources[1]->SetPoint2(b[0], b[2], b[5]);

    // Axial (XY): 定义在 Z 轴最小边界处
    m_planeSources[2]->SetOrigin(b[0], b[2], b[4]);
    m_planeSources[2]->SetPoint1(b[1], b[2], b[4]);
    m_planeSources[2]->SetPoint2(b[0], b[3], b[4]);

    // 缓存最大索引用于 UpdateAllPositions 内部校验
    int dims[3];
    m_imageData->GetDimensions(dims);
    for (int i = 0; i < 3; ++i) m_maxIndices[i] = dims[i] - 1;
}

void ColoredPlanesStrategy::UpdateAllPositions(int x, int y, int z) {
    if (!m_imageData) return;

    // 边界裁剪
    int cx = std::max(0, std::min(x, m_maxIndices[0]));
    int cy = std::max(0, std::min(y, m_maxIndices[1]));
    int cz = std::max(0, std::min(z, m_maxIndices[2]));

    double spacing[3];
    m_imageData->GetSpacing(spacing);

    double shiftX = cx * spacing[0];
    double shiftY = cy * spacing[1];
    double shiftZ = cz * spacing[2];

    m_planeActors[0]->SetPosition(shiftX, 0, 0);
    m_planeActors[1]->SetPosition(0, shiftY, 0);
    m_planeActors[2]->SetPosition(0, 0, shiftZ);
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