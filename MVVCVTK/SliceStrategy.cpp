#include "SliceStrategy.h"
#include <vtkPlane.h>
#include <vtkCamera.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkImageProperty.h>
#include <algorithm>
#include <vtkTransform.h>
#include <vtkImageResliceMapper.h>

SliceStrategy::SliceStrategy(Orientation orient) : m_orientation(orient) {
    m_slice = vtkSmartPointer<vtkImageSlice>::New();
    m_mapper = vtkSmartPointer<vtkImageResliceMapper>::New();

    // --- 初始化十字线资源 ---
    m_vLineSource = vtkSmartPointer<vtkLineSource>::New();
    m_hLineSource = vtkSmartPointer<vtkLineSource>::New();

    m_vLineActor = vtkSmartPointer<vtkActor>::New();
    m_hLineActor = vtkSmartPointer<vtkActor>::New();

    // 设置 Mapper
    auto vMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    vMapper->SetInputConnection(m_vLineSource->GetOutputPort());
    m_vLineActor->SetMapper(vMapper);

    auto hMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    hMapper->SetInputConnection(m_hLineSource->GetOutputPort());
    m_hLineActor->SetMapper(hMapper);

    // 设置颜色 (例如黄色) 和线宽
    m_vLineActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
    m_vLineActor->GetProperty()->SetLineWidth(1.5);
    // 为了防止遮挡，可以关闭深度测试或者稍微抬高一点 Z 值，但 VTK RendererLayer 更好
    m_vLineActor->GetProperty()->SetLighting(false); // 关闭光照，纯色显示

    m_hLineActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
    m_hLineActor->GetProperty()->SetLineWidth(1.5);
    m_hLineActor->GetProperty()->SetLighting(false);

    // 禁用 LUT 映射，交由 UpdateVisuals 动态更新原生 WindowLevel
    m_slice->GetProperty()->SetUseLookupTableScalarRange(0);

    RegisterProp(m_slice);
    RegisterProp(m_vLineActor);
    RegisterProp(m_hLineActor);
}

void SliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    m_mapper->SetInputData(img);
    m_mapper->SliceFacesCameraOff(); // 截面永远绝对平行于相机屏幕
    m_mapper->SliceAtFocalPointOff(); // // 截面永远穿过相机焦点

    // 创建 vtkPlane 对象
    auto plane = vtkSmartPointer<vtkPlane>::New();
    // 设置原点 (Origin)：让切片默认位于图像数据的几何中心
    double center[3];
    img->GetCenter(center);
    plane->SetOrigin(center);

    // 设置法线 (Normal)：决定切片的方向
    if (m_orientation == Orientation::Top_down) {
        plane->SetNormal(0, 0, 1); // Z轴法线
    }
    else if (m_orientation == Orientation::Front_back) {
        plane->SetNormal(0, 1, 0); // Y轴法线
    }
    else {
        plane->SetNormal(1, 0, 0); // X轴法线
    }
    // 将 Plane 对象传递给 Mapper
    m_mapper->SetSlicePlane(plane);
    m_slice->SetMapper(m_mapper);
}

void SliceStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::Attach(ren); // 
    m_renderer = ren;
    ren->SetBackground(0, 0, 0);
    // 开启深度剥离，让 alpha<1 的像素正确透明（不影响不透明渲染）
    ren->SetUseDepthPeeling(1);
    ren->SetMaximumNumberOfPeels(4);
    ren->SetOcclusionRatio(0.0);
}

void SliceStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    if (!ren) return;
    vtkCamera* cam = ren->GetActiveCamera();
    cam->ParallelProjectionOn(); // 开启平行投影
    double distance = cam->GetDistance();

    double imgCenter[3] = { 0.0, 0.0, 0.0 };
    if (m_mapper && m_mapper->GetInput()) {
        m_mapper->GetInput()->GetCenter(imgCenter);
    }

    // 初次设置
    cam->SetFocalPoint(imgCenter);

    // 同一个物理坐标系下
    switch (m_orientation) {
    case Orientation::Top_down:
        // Top_down: 从 Z+ 往 -Z 看，屏幕水平=X，屏幕垂直=Y
        cam->SetPosition(imgCenter[0], imgCenter[1], imgCenter[2] + distance);
        cam->SetViewUp(0, 1, 0);
        //cam->Roll(180.0);
        break;

    case Orientation::Front_back:
        // Front_back: 从 Y+ 往 -Y 看，屏幕水平=X，屏幕垂直=Z
        cam->SetPosition(imgCenter[0], imgCenter[1] + distance, imgCenter[2]);
        cam->SetViewUp(0, 0, 1);
        break;

    case Orientation::Left_right:
        // Left_right: 从 X+ 往 -X 看，屏幕水平=Y，屏幕垂直=Z
        cam->SetPosition(imgCenter[0] + distance, imgCenter[1], imgCenter[2]);
        cam->SetViewUp(0, 0, 1);
        //cam->Roll(180.0);
        break;
    }

    ren->ResetCamera();
    ren->ResetCameraClippingRange();
}

void SliceStrategy::UpdateCrosshair(int cx, int cy, int cz,
    const double bounds[6],
    const double origin[3],
    const double spacing[3],
    double safeOffset)
{
    if (!m_hLineSource || !m_vLineSource) return;

    const double physX = origin[0] + cx * spacing[0];
    const double physY = origin[1] + cy * spacing[1];
    const double physZ = origin[2] + cz * spacing[2];

    if (m_orientation == Orientation::Top_down) {
        // 切片平面 Z = physZ；safeOffset 沿局部 Z 轴偏移防穿模
        const double z = physZ + safeOffset;
        m_vLineSource->SetPoint1(physX, bounds[2], z);
        m_vLineSource->SetPoint2(physX, bounds[3], z);
        m_hLineSource->SetPoint1(bounds[0], physY, z);
        m_hLineSource->SetPoint2(bounds[1], physY, z);
    }
    else if (m_orientation == Orientation::Front_back) {
        // 切片平面 Y = physY；safeOffset 沿局部 Y 轴偏移
        const double y = physY + safeOffset;
        m_vLineSource->SetPoint1(physX, y, bounds[4]);
        m_vLineSource->SetPoint2(physX, y, bounds[5]);
        m_hLineSource->SetPoint1(bounds[0], y, physZ);
        m_hLineSource->SetPoint2(bounds[1], y, physZ);
    }
    else {  // Left_right
        // 切片平面 X = physX；safeOffset 沿局部 X 轴偏移
        const double x = physX + safeOffset;
        m_vLineSource->SetPoint1(x, physY, bounds[4]);
        m_vLineSource->SetPoint2(x, physY, bounds[5]);
        m_hLineSource->SetPoint1(x, bounds[2], physZ);
        m_hLineSource->SetPoint2(x, bounds[3], physZ);
    }

    m_vLineSource->Modified();
    m_hLineSource->Modified();
}

void SliceStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    // ── 窗宽/窗位或材质改变 → 重建灰阶 LUT（切片专用）─────────
    if (HasFlag(flags, UpdateFlags::WindowLevel) || HasFlag(flags, UpdateFlags::Material))
    {
        if (m_slice && m_slice->GetProperty())
        {
            auto imgProp = m_slice->GetProperty();
            imgProp->SetOpacity(params.material.opacity);
            imgProp->SetColorWindow(params.windowLevel.windowWidth);
            imgProp->SetColorLevel(params.windowLevel.windowCenter);
            // 切片无光照，不设 ambient/diffuse（与 vtkImageProperty 语义一致）
        }
    }

	if (HasFlag(flags, UpdateFlags::Transform) || HasFlag(flags, UpdateFlags::Cursor))
    {
        // 调用更新cursor
        // 切面逆向，Actor 正向
        // Pw =  M X Pm
        auto resliceMapper = vtkImageResliceMapper::SafeDownCast(m_mapper);
        if (!resliceMapper || !resliceMapper->GetInput()) return;

        double origin[3], spacing[3], bounds[6];
        resliceMapper->GetInput()->GetOrigin(origin);
        resliceMapper->GetInput()->GetSpacing(spacing);
        resliceMapper->GetInput()->GetBounds(bounds);

        auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
        mat->DeepCopy(params.modelMatrix.data());

        // ── 三个 Actor 共用同一个 UserMatrix = M ──────────
        //
        // 线 Actor 与切片 Actor 共用 M，所有坐标在局部空间计算，
        // VTK 统一将局部坐标变换到世界空间。
        // 绝对不能给切片 Actor 挂载 UserMatrix，否则会引发二次变换和裁剪面错位畸变。
        if (m_slice)      m_slice->SetUserMatrix(nullptr); 
        if (m_vLineActor) m_vLineActor->SetUserMatrix(nullptr);  // 与切片共用 M
        if (m_hLineActor) m_hLineActor->SetUserMatrix(nullptr);  // 与切片共用 M

        // ── 局部空间固定轴法线（数据空间，与模型旋转无关）────────────
        double localNormal4[4] = { 0, 0, 0, 0 };
        if (m_orientation == Orientation::Top_down)   localNormal4[2] = 1.0;
        else if (m_orientation == Orientation::Front_back) localNormal4[1] = 1.0;
        else if  (m_orientation == Orientation::Left_right) localNormal4[0] = 1.0;

        // ── 世界法线 = M × localNormal（用于 SlicePlane）─────────────
        //
        // VTK 接受世界坐标的 SlicePlane，内部乘 M⁻¹ 换回局部坐标：
        //   localNormal = M⁻¹ × (M × localNormal) = localNormal ✓
        //
        // 原代码直接用固定世界轴 (0,0,1)，VTK 换回局部后得到斜切法线，
        // 导致切面倾斜变形为三角形。
        double worldNormal4[4];
        mat->MultiplyPoint(localNormal4, worldNormal4);

        // ── 游标局部坐标 → 世界坐标（SlicePlane 原点）───────────────
        double localFocus4[4] = {
            origin[0] + params.cursor[0] * spacing[0],
            origin[1] + params.cursor[1] * spacing[1],
            origin[2] + params.cursor[2] * spacing[2],
            1.0
        };
        double worldFocus4[4];
        mat->MultiplyPoint(localFocus4, worldFocus4);

        //// ── 切割平面（世界坐标，VTK 自动转换到数据空间）─────────────
        auto slicePlane = resliceMapper->GetSlicePlane();
        if (!slicePlane) {
            slicePlane = vtkSmartPointer<vtkPlane>::New();
            resliceMapper->SetSlicePlane(slicePlane);
        }
        slicePlane->SetOrigin(worldFocus4[0], worldFocus4[1], worldFocus4[2]);
        slicePlane->SetNormal(worldNormal4[0], worldNormal4[1], worldNormal4[2]);

        // ── 十字线
        const double safeOffset =
            std::min({ spacing[0], spacing[1], spacing[2] });
        UpdateCrosshair(
            params.cursor[0], params.cursor[1], params.cursor[2],
            bounds, origin, spacing, safeOffset);

        // 法线和相机向量
        // 想让相机能“正面”看到一个物体，这个物体的法线（正脸）必须迎着相机的视线，两者是方向相反的。
        // 切点确定，法线确定，确定平面位置，提交给切割器重采样切片
        // 这里设置了model矩阵为nullptr，所以切割器会直接把这个点当成世界坐标来用（不经过变换），
        // 所以我们在这里就要把模型坐标系下的切点转换成世界坐标系下的切点。

        // 对于 vtkProp3D 来说，传给它的 UserMatrix，其实就是直接喂给显卡 Shader 的 ModelMatrix（模型矩阵）。
        // 显卡天生就是吃 4*4 矩阵的，
        // 不需要 vtkTransform 这种高级 C++ 封装，它只需要那 16 个双精度浮点数（double[16]）
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        const int vis = (params.visibilityMask & VisFlags::Crosshair) ? 1 : 0;
        if (m_vLineActor) m_vLineActor->SetVisibility(vis);
        if (m_hLineActor) m_hLineActor->SetVisibility(vis);
    }
}
