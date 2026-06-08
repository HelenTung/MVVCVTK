#include "ColoredPlanesStrategy.h"
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkMatrix4x4.h>
#include <vtkTransform.h>
#include <limits>

void ColoredPlanesStrategy::SetWorldBounds(const std::array<double, 16>& modelMatrix, double worldBounds[6]) const {
    // 与 SliceStrategy 一样，把局部包围盒角点映射到世界空间，
    // 这样参考平面在模型旋转后依旧沿当前世界包围盒正确铺开。
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorldMatrix->DeepCopy(modelMatrix.data());

    worldBounds[0] = worldBounds[2] = worldBounds[4] = std::numeric_limits<double>::max();
    worldBounds[1] = worldBounds[3] = worldBounds[5] = std::numeric_limits<double>::lowest();

    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                double modelToWorldInputPoint[4] = {
                    ix == 0 ? m_bounds[0] : m_bounds[1],
                    iy == 0 ? m_bounds[2] : m_bounds[3],
                    iz == 0 ? m_bounds[4] : m_bounds[5],
                    1.0
                };
                double modelToWorldOutputPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
                modelToWorldMatrix->MultiplyPoint(modelToWorldInputPoint, modelToWorldOutputPoint);

                const double invW = std::abs(modelToWorldOutputPoint[3]) > 1e-12 ? 1.0 / modelToWorldOutputPoint[3] : 1.0;
                const double x = modelToWorldOutputPoint[0] * invW;
                const double y = modelToWorldOutputPoint[1] * invW;
                const double z = modelToWorldOutputPoint[2] * invW;

                worldBounds[0] = std::min(worldBounds[0], x);
                worldBounds[1] = std::max(worldBounds[1], x);
                worldBounds[2] = std::min(worldBounds[2], y);
                worldBounds[3] = std::max(worldBounds[3], y);
                worldBounds[4] = std::min(worldBounds[4], z);
                worldBounds[5] = std::max(worldBounds[5], z);
            }
        }
    }
}

ColoredPlanesStrategy::ColoredPlanesStrategy() {
    double colors[3][3] = {
        {1.0, 0.0, 0.0}, // Left_right
        {0.0, 1.0, 0.0}, // Front_back
        {0.0, 0.0, 1.0}  // Top_down
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
    m_planeSources[0]->SetNormal(1.0, 0.0, 0.0); // X-axis normal (Left_right)
    m_planeSources[1]->SetNormal(0.0, 1.0, 0.0); // Y-axis normal (Front_back)
    m_planeSources[2]->SetNormal(0.0, 0.0, 1.0); // Z-axis normal (Top_down)

    for (int i = 0; i < 3; i++) {
        SetManagedProp(m_planeActors[i]); 
    }
}

void ColoredPlanesStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    if (m_lastInput == data) {
        return;
    }
    m_lastInput = data;

    // 获取物理边界
    img->GetBounds(m_bounds);

    // 缓存最大索引用于 UpdateAllPositions 内部校验
    int dims[3];
    img->GetDimensions(dims);
    for (int i = 0; i < 3; ++i) m_maxIndices[i] = dims[i] - 1;

	// 保存 origin/spacing 后，三张参考平面的初始几何就可以完全由数据空间定义。
    img->GetOrigin(m_origin);
    img->GetSpacing(m_spacing);

	// Left_right (YZ面): 平面固定在 X = origin[0]，YZ 跨度为完整边界
    m_planeSources[0]->SetOrigin(m_origin[0], m_bounds[2], m_bounds[4]);
    m_planeSources[0]->SetPoint1(m_origin[0], m_bounds[3], m_bounds[4]);
    m_planeSources[0]->SetPoint2(m_origin[0], m_bounds[2], m_bounds[5]);

    // Front_back (XZ面): 平面固定在 Y = origin[1]，XZ 跨度为完整边界
    m_planeSources[1]->SetOrigin(m_bounds[0], m_origin[1], m_bounds[4]);
    m_planeSources[1]->SetPoint1(m_bounds[1], m_origin[1], m_bounds[4]);
    m_planeSources[1]->SetPoint2(m_bounds[0], m_origin[1], m_bounds[5]);

    // Top_down (XY面): 平面固定在 Z = origin[2]，XY 跨度为完整边界
    m_planeSources[2]->SetOrigin(m_bounds[0], m_bounds[2], m_origin[2]);
    m_planeSources[2]->SetPoint1(m_bounds[1], m_bounds[2], m_origin[2]);
    m_planeSources[2]->SetPoint2(m_bounds[0], m_bounds[3], m_origin[2]);

    for (int i = 0; i < 3; i++) {
        // 确保 Actor 无额外位置偏移，平面位置完全由 PlaneSource 几何决定
        m_planeActors[i]->SetPosition(0.0, 0.0, 0.0);
        m_planeSources[i]->Update();
    }
}

void ColoredPlanesStrategy::SetAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix) {
    if (!m_lastInput) return;

    double worldBounds[6] = { 0.0 };
    SetWorldBounds(modelMatrix, worldBounds);

    // 参考平面在世界空间里沿三个主轴穿过当前 cursor，
    // 本质上是在 3D 视图中把三个切片位置可视化出来。
    const double physX = std::max(worldBounds[0], std::min(cursorWorld[0], worldBounds[1]));
    const double physY = std::max(worldBounds[2], std::min(cursorWorld[1], worldBounds[3]));
    const double physZ = std::max(worldBounds[4], std::min(cursorWorld[2], worldBounds[5]));

    // Left_right: 移动 X 轴
    m_planeSources[0]->SetOrigin(physX, worldBounds[2], worldBounds[4]);
    m_planeSources[0]->SetPoint1(physX, worldBounds[3], worldBounds[4]);
    m_planeSources[0]->SetPoint2(physX, worldBounds[2], worldBounds[5]);
    m_planeSources[0]->Update();

    // Front_back: 移动 Y 轴
    m_planeSources[1]->SetOrigin(worldBounds[0], physY, worldBounds[4]);
    m_planeSources[1]->SetPoint1(worldBounds[1], physY, worldBounds[4]);
    m_planeSources[1]->SetPoint2(worldBounds[0], physY, worldBounds[5]);
    m_planeSources[1]->Update();

    // Top_down: 移动 Z 轴
    m_planeSources[2]->SetOrigin(worldBounds[0], worldBounds[2], physZ);
    m_planeSources[2]->SetPoint1(worldBounds[1], worldBounds[2], physZ);
    m_planeSources[2]->SetPoint2(worldBounds[0], worldBounds[3], physZ);
    m_planeSources[2]->Update();

    for (int i = 0; i < 3; ++i) {
        if (m_planeActors[i]) {
            m_planeActors[i]->SetUserMatrix(nullptr);
        }
    }
}

int ColoredPlanesStrategy::GetPlaneAxis(vtkActor* actor) {
    for (int i = 0; i < 3; ++i) {
        if (m_planeActors[i] == actor) {
            return i; // 0 for X, 1 for Y, 2 for Z
        }
    }
    return -1; // 未匹配
}

void ColoredPlanesStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (HasFlag(flags, UpdateFlags::Cursor) || HasFlag(flags,UpdateFlags::Transform)) {
        // Cursor 与 Transform 任一变化，都需要重新把三张平面放到正确世界位置。
        SetAllPositions(params.cursor.data(), params.modelMatrix);
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        const int vis = (params.visibilityMask & VisFlags::Planes3D) ? 1 : 0;
        for (int i = 0; i < 3; i++) {
            if (m_planeActors[i]) m_planeActors[i]->SetVisibility(vis);
        }
    }
}
