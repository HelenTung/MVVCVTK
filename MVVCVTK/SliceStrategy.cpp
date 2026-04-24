#include "SliceStrategy.h"
#include <vtkPlane.h>
#include <vtkCamera.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkImageProperty.h>
#include <algorithm>
#include <vtkTransform.h>
#include <vtkImageResliceMapper.h>
#include <limits>

void SliceStrategy::SetWorldBounds(const double bounds[6],
    const std::array<double, 16>& modelMatrix,
    double worldBounds[6]) const
{
    auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
    mat->DeepCopy(modelMatrix.data());

    worldBounds[0] = worldBounds[2] = worldBounds[4] = std::numeric_limits<double>::max();
    worldBounds[1] = worldBounds[3] = worldBounds[5] = std::numeric_limits<double>::lowest();

    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                double localPoint[4] = {
                    ix == 0 ? bounds[0] : bounds[1],
                    iy == 0 ? bounds[2] : bounds[3],
                    iz == 0 ? bounds[4] : bounds[5],
                    1.0
                };
                double worldPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
                mat->MultiplyPoint(localPoint, worldPoint);

                const double invW = std::abs(worldPoint[3]) > 1e-12 ? 1.0 / worldPoint[3] : 1.0;
                const double x = worldPoint[0] * invW;
                const double y = worldPoint[1] * invW;
                const double z = worldPoint[2] * invW;

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

void SliceStrategy::SetCameraAligned(const std::array<double, 16>& modelMatrix,
    const double bounds[6])
{
    if (!m_renderer || !m_renderer->GetActiveCamera()) return;

    auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
    mat->DeepCopy(modelMatrix.data());

    double modelCenter[4] = {
        (bounds[0] + bounds[1]) * 0.5,
        (bounds[2] + bounds[3]) * 0.5,
        (bounds[4] + bounds[5]) * 0.5,
        1.0
    };
    double worldCenter4[4] = { 0.0, 0.0, 0.0, 1.0 };
    mat->MultiplyPoint(modelCenter, worldCenter4);

    const double invW = std::abs(worldCenter4[3]) > 1e-12 ? 1.0 / worldCenter4[3] : 1.0;
    double worldCenter[3] = {
        worldCenter4[0] * invW,
        worldCenter4[1] * invW,
        worldCenter4[2] * invW
    };

    vtkCamera* cam = m_renderer->GetActiveCamera();
    double oldFocal[3] = { 0.0, 0.0, 0.0 };
    double oldPosition[3] = { 0.0, 0.0, 0.0 };
    cam->GetFocalPoint(oldFocal);
    cam->GetPosition(oldPosition);

    double offset[3] = {
        oldPosition[0] - oldFocal[0],
        oldPosition[1] - oldFocal[1],
        oldPosition[2] - oldFocal[2]
    };

    cam->SetFocalPoint(worldCenter);
    cam->SetPosition(worldCenter[0] + offset[0], worldCenter[1] + offset[1], worldCenter[2] + offset[2]);
    m_renderer->ResetCameraClippingRange();
}

SliceStrategy::SliceStrategy(Orientation orient) : m_orientation(orient) {
    m_slice = vtkSmartPointer<vtkImageSlice>::New();
    m_mapper = vtkSmartPointer<vtkImageResliceMapper>::New();
    m_slicePlane = vtkSmartPointer<vtkPlane>::New();
    m_mapper->SetSlicePlane(m_slicePlane);

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

    m_vLineActor->GetProperty()->SetLineWidth(1.5);
    // 为了防止遮挡，可以关闭深度测试或者稍微抬高一点 Z 值，但 VTK RendererLayer 更好
    m_vLineActor->GetProperty()->SetLighting(false); // 关闭光照，纯色显示

    m_hLineActor->GetProperty()->SetLineWidth(1.5);
    m_hLineActor->GetProperty()->SetLighting(false);

    // 禁用 LUT 映射，交由 UpdateVisuals 动态更新原生 WindowLevel
    m_slice->GetProperty()->SetUseLookupTableScalarRange(0);
    m_mapper->SliceFacesCameraOff(); // 截面永远绝对平行于相机屏幕
    m_mapper->SliceAtFocalPointOff(); // 截面不强制穿过相机焦点，由状态统一驱动

    if (m_orientation == Orientation::Top_down) {
        m_slicePlane->SetNormal(0, 0, 1); // Z轴法线
        m_vLineActor->GetProperty()->SetColor(1, 0, 0);
        m_hLineActor->GetProperty()->SetColor(0, 1, 0);
        m_hLineActor->GetProperty()->SetOpacity(0.4);
        m_vLineActor->GetProperty()->SetOpacity(0.4);
    }
    else if (m_orientation == Orientation::Front_back) {
        m_slicePlane->SetNormal(0, 1, 0); // Y轴法线
        m_vLineActor->GetProperty()->SetColor(1, 0, 0);
        m_hLineActor->GetProperty()->SetColor(0, 0, 1);
        m_hLineActor->GetProperty()->SetOpacity(0.4);
        m_vLineActor->GetProperty()->SetOpacity(0.4);
    }
    else {
        m_slicePlane->SetNormal(1, 0, 0); // X轴法线
        m_vLineActor->GetProperty()->SetColor(0, 1, 0);
        m_hLineActor->GetProperty()->SetColor(0, 0, 1);
        m_vLineActor->GetProperty()->SetOpacity(0.4);
        m_hLineActor->GetProperty()->SetOpacity(0.4);
    }

    SetManagedProp(m_slice);
    SetManagedProp(m_vLineActor);
    SetManagedProp(m_hLineActor);
}

void SliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    if (m_lastInput == data && m_mapper->GetInput()) {
        return;
    }
    m_lastInput = data;

    m_mapper->SetInputData(img);

    // 设置原点 (Origin)：让切片默认位于图像数据的几何中心
    double center[3];
    img->GetCenter(center);
    m_slicePlane->SetOrigin(center);
    m_slice->SetMapper(m_mapper);
}

void SliceStrategy::SetRendererAttached(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::SetRendererAttached(ren); // 
    m_renderer = ren;
    ren->SetBackground(0, 0, 0);
    // 开启深度剥离，让 alpha<1 的像素正确透明（不影响不透明渲染）
    ren->SetUseDepthPeeling(1);
    ren->SetMaximumNumberOfPeels(4);
    ren->SetOcclusionRatio(0.0);
}

void SliceStrategy::SetCameraConfigured(vtkSmartPointer<vtkRenderer> ren) {
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

void SliceStrategy::SetCrosshair(const double focusWorld[3],
    const double worldBounds[6],
    double safeOffset) 
{
    if (!m_hLineSource || !m_vLineSource) return;
    const double physX = focusWorld[0];
    const double physY = focusWorld[1];
    const double physZ = focusWorld[2];
    if (m_orientation == Orientation::Top_down) {
        const double z = physZ + safeOffset;
        m_vLineSource->SetPoint1(physX, worldBounds[2], z);
        m_vLineSource->SetPoint2(physX, worldBounds[3], z);
        m_hLineSource->SetPoint1(worldBounds[0], physY, z);
        m_hLineSource->SetPoint2(worldBounds[1], physY, z);
    }
    else if (m_orientation == Orientation::Front_back) {
        const double y = physY + safeOffset;
        m_vLineSource->SetPoint1(physX, y, worldBounds[4]);
        m_vLineSource->SetPoint2(physX, y, worldBounds[5]);
        m_hLineSource->SetPoint1(worldBounds[0], y, physZ);
        m_hLineSource->SetPoint2(worldBounds[1], y, physZ);
    }
    else {
        const double x = physX + safeOffset;
        m_vLineSource->SetPoint1(x, physY, worldBounds[4]);
        m_vLineSource->SetPoint2(x, physY, worldBounds[5]);
        m_hLineSource->SetPoint1(x, worldBounds[2], physZ);
        m_hLineSource->SetPoint2(x, worldBounds[3], physZ);
    }

    m_vLineSource->Modified();
    m_hLineSource->Modified();
}

void SliceStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    // ── 窗宽/窗位或材质改变 → 重建灰阶 LUT（切片专用）─────────
    if (HasFlag(flags, UpdateFlags::WindowLevel) || HasFlag(flags, UpdateFlags::Material))
    {
        if (m_slice && m_slice->GetProperty())
        {
            auto imgProp = m_slice->GetProperty();
            imgProp->SetOpacity(1.0);
            // imgProp->SetOpacity(params.material.opacity);
            imgProp->SetColorWindow(params.windowLevel.windowWidth);
            imgProp->SetColorLevel(params.windowLevel.windowCenter);
            // 切片无光照，不设 ambient/diffuse（与 vtkImageProperty 语义一致）
        }
    }

	if (HasFlag(flags, UpdateFlags::Transform) || HasFlag(flags, UpdateFlags::Cursor))
    {
        auto resliceMapper = vtkImageResliceMapper::SafeDownCast(m_mapper);
        if (!resliceMapper || !resliceMapper->GetInput()) return;

        double spacing[3], bounds[6];
        resliceMapper->GetInput()->GetSpacing(spacing);
        resliceMapper->GetInput()->GetBounds(bounds);
        double worldBounds[6] = { 0.0 };
        SetWorldBounds(bounds, params.modelMatrix, worldBounds);

        if (HasFlag(flags, UpdateFlags::Transform)) {
            auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
            mat->DeepCopy(params.modelMatrix.data());
            if (m_slice)      m_slice->SetUserMatrix(mat);
            if (m_vLineActor) m_vLineActor->SetUserMatrix(nullptr);
            if (m_hLineActor) m_hLineActor->SetUserMatrix(nullptr);
        }

        double worldNormal[3] = { 0.0, 0.0, 0.0 };
        if (m_orientation == Orientation::Top_down) worldNormal[2] = 1.0;
        else if (m_orientation == Orientation::Front_back) worldNormal[1] = 1.0;
        else worldNormal[0] = 1.0;

        auto slicePlane = resliceMapper->GetSlicePlane();
        if (!slicePlane) {
            slicePlane = vtkSmartPointer<vtkPlane>::New();
            resliceMapper->SetSlicePlane(slicePlane);
        }
        slicePlane->SetOrigin(params.cursor[0], params.cursor[1], params.cursor[2]);
        slicePlane->SetNormal(worldNormal[0], worldNormal[1], worldNormal[2]);

        const double safeOffset =
            std::min({ spacing[0], spacing[1], spacing[2] });
        SetCrosshair(params.cursor.data(), worldBounds, safeOffset);

        if (HasFlag(flags, UpdateFlags::Transform)) {
            SetCameraAligned(params.modelMatrix, bounds);
        }
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        const int vis = (params.visibilityMask & VisFlags::Crosshair) ? 1 : 0;
        if (m_vLineActor) m_vLineActor->SetVisibility(vis);
        if (m_hLineActor) m_hLineActor->SetVisibility(vis);
    }
}
