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

	// 初始时模型矩阵为单位矩阵，避免空指针
	m_cachedModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
}

void ColoredPlanesStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;
    m_imageData = img;

    // 获取物理边界
    double b[6];
    m_imageData->GetBounds(b);

    // 缓存最大索引用于 UpdateAllPositions 内部校验
    int dims[3];
    m_imageData->GetDimensions(dims);
    for (int i = 0; i < 3; ++i) m_maxIndices[i] = dims[i] - 1;

	// 获取原点和间距
    m_imageData->GetOrigin(m_origin);
    m_imageData->GetSpacing(m_spacing);

	// Sagittal (YZ面): 平面固定在 X = origin[0]，YZ 跨度为完整边界
    m_planeSources[0]->SetOrigin(m_origin[0], b[2], b[4]);
    m_planeSources[0]->SetPoint1(m_origin[0], b[3], b[4]);
    m_planeSources[0]->SetPoint2(m_origin[0], b[2], b[5]);

    // Coronal (XZ面): 平面固定在 Y = origin[1]，XZ 跨度为完整边界
    m_planeSources[1]->SetOrigin(b[0], m_origin[1], b[4]);
    m_planeSources[1]->SetPoint1(b[1], m_origin[1], b[4]);
    m_planeSources[1]->SetPoint2(b[0], m_origin[1], b[5]);

    // Axial (XY面): 平面固定在 Z = origin[2]，XY 跨度为完整边界
    m_planeSources[2]->SetOrigin(b[0], b[2], m_origin[2]);
    m_planeSources[2]->SetPoint1(b[1], b[2], m_origin[2]);
    m_planeSources[2]->SetPoint2(b[0], b[3], m_origin[2]);

    for (int i = 0; i < 3; i++) {
        // 确保 Actor 无额外位置偏移，平面位置完全由 PlaneSource 几何决定
        m_planeActors[i]->SetPosition(0.0, 0.0, 0.0);
        m_planeSources[i]->Update();
    }
}

void ColoredPlanesStrategy::UpdateAllPositions(int x, int y, int z) {
    if (!m_imageData) return;

    // 边界裁剪
    int cx = std::max(0, std::min(x, m_maxIndices[0]));
    int cy = std::max(0, std::min(y, m_maxIndices[1]));
    int cz = std::max(0, std::min(z, m_maxIndices[2]));

    double spacing[3];
    m_imageData->GetSpacing(spacing);

    double physX = cx * spacing[0] + m_origin[0];
    double physY = cy * spacing[1] + m_origin[1];
    double physZ = cz * spacing[2] + m_origin[2];

    double b[6];
    m_imageData->GetBounds(b);

    // Sagittal: 移动 X 轴
    m_planeSources[0]->SetOrigin(physX, b[2], b[4]);
    m_planeSources[0]->SetPoint1(physX, b[3], b[4]);
    m_planeSources[0]->SetPoint2(physX, b[2], b[5]);
    m_planeSources[0]->Update();

    // Coronal: 移动 Y 轴
    m_planeSources[1]->SetOrigin(b[0], physY, b[4]);
    m_planeSources[1]->SetPoint1(b[1], physY, b[4]);
    m_planeSources[1]->SetPoint2(b[0], physY, b[5]);
    m_planeSources[1]->Update();

    // Axial: 移动 Z 轴
    m_planeSources[2]->SetOrigin(b[0], b[2], physZ);
    m_planeSources[2]->SetPoint1(b[1], b[2], physZ);
    m_planeSources[2]->SetPoint2(b[0], b[3], physZ);
    m_planeSources[2]->Update();
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
        m_cachedModelMatrix->DeepCopy(params.modelMatrix.data());
        for (int i = 0; i < 3; i++) {
            if (m_planeActors[i]) {
                m_planeActors[i]->SetUserMatrix(m_cachedModelMatrix);
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