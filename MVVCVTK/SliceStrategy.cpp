#include "SliceStrategy.h"
#include <vtkPlane.h>
#include <vtkCamera.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkImageProperty.h>

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

    // 初始化颜色映射表
    m_lut = vtkSmartPointer<vtkLookupTable>::New();
    m_slice->GetProperty()->SetLookupTable(m_lut);
    m_slice->GetProperty()->SetUseLookupTableScalarRange(1);
}


void SliceStrategy::RebuildLUT(const RenderParams& params)
{
    const double minVal = params.scalarRange[0];
    const double maxVal = params.scalarRange[1];
    const double range = maxVal - minVal;
    if (range <= 0.0) return;

    const double globalOpacity = params.material.opacity;
    const int    nTable = 256;

    m_lut->SetNumberOfTableValues(nTable);
    m_lut->SetTableRange(minVal, maxVal);

    // 用 isoValue 做阈值：低于阈值 → 完全透明，高于阈值 → 按 tfNodes 着色
    // 归一化的阈值位置
    const double isoT = (range > 0.0)
        ? (params.isoValue - minVal) / range
        : 0.5;

    for (int i = 0; i < nTable; i++)
    {
        const double t = static_cast<double>(i) / static_cast<double>(nTable - 1);

        if (t < isoT)
        {
            // 背景区域：完全透明
            m_lut->SetTableValue(i, 0.0, 0.0, 0.0, 0.0);
        }
        else
        {
            // 模型区域：从 tfNodes 取颜色，但 alpha 强制不透明
            double r = 1.0, g = 1.0, b = 1.0; // 默认白色
            const auto& nodes = params.tfNodes;
            if (!nodes.empty())
            {
                if (t >= nodes.back().position)
                {
                    r = nodes.back().r;
                    g = nodes.back().g;
                    b = nodes.back().b;
                }
                else
                {
                    for (size_t k = 0; k + 1 < nodes.size(); ++k)
                    {
                        if (t >= nodes[k].position && t <= nodes[k + 1].position)
                        {
                            const double span = nodes[k + 1].position - nodes[k].position;
                            const double alpha = (span > 0.0)
                                ? (t - nodes[k].position) / span : 0.0;
                            r = nodes[k].r + alpha * (nodes[k + 1].r - nodes[k].r);
                            g = nodes[k].g + alpha * (nodes[k + 1].g - nodes[k].g);
                            b = nodes[k].b + alpha * (nodes[k + 1].b - nodes[k].b);
                            break;
                        }
                    }
                }
            }
            // alpha 不从 tfNodes 取，直接用 globalOpacity，保证实心填充
            m_lut->SetTableValue(i, r, g, b, 1.0);
        }
    }

    m_lut->Modified();

    auto imgProp = m_slice->GetProperty();
    imgProp->SetOpacity(globalOpacity);
    imgProp->SetAmbient(params.material.ambient);
    imgProp->SetDiffuse(params.material.diffuse);

}

void SliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    m_mapper->SetInputData(img);
    m_mapper->SliceFacesCameraOff();
    m_mapper->SliceAtFocalPointOff();

    // 创建 vtkPlane 对象
    auto plane = vtkSmartPointer<vtkPlane>::New();

    // 设置原点 (Origin)：让切片默认位于图像数据的几何中心
    double center[3];
    img->GetCenter(center);
    plane->SetOrigin(center);

    // 设置法线 (Normal)：决定切片的方向
    if (m_orientation == Orientation::AXIAL) {
        plane->SetNormal(0, 0, 1); // Z轴法线
    }
    else if (m_orientation == Orientation::CORONAL) {
        plane->SetNormal(0, 1, 0); // Y轴法线
    }
    else {
        plane->SetNormal(1, 0, 0); // X轴法线
    }

    // 将 Plane 对象传递给 Mapper
    m_mapper->SetSlicePlane(plane);
    m_slice->SetMapper(m_mapper);

    // 重建 mapper 后重新绑定 LUT 通道
    m_slice->GetProperty()->SetLookupTable(m_lut);
    m_slice->GetProperty()->SetUseLookupTableScalarRange(1);

    int dims[3];
    img->GetDimensions(dims);

    // 根据方向决定最大索引
    if (m_orientation == Orientation::AXIAL) {
        m_maxIndex = dims[2] - 1; // Z轴
    }
    else if (m_orientation == Orientation::CORONAL) {
        m_maxIndex = dims[1] - 1; // Y轴
    }
    else {
        m_maxIndex = dims[0] - 1; // X轴
    }

    // 重置当前索引为中间位置，保证一开始能看到图
    m_currentIndex = m_maxIndex / 2;

    // 强制更新一次位置，确保画面同步
    UpdatePlanePosition();
}

void SliceStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddViewProp(m_slice);
    ren->AddActor(m_vLineActor);
    ren->AddActor(m_hLineActor);
    ren->SetBackground(0, 0, 0);
    // 开启深度剥离，让 alpha<1 的像素正确透明（不影响不透明渲染）
    ren->SetUseDepthPeeling(1);
    ren->SetMaximumNumberOfPeels(4);
    ren->SetOcclusionRatio(0.0);
}

void SliceStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveViewProp(m_slice);
    ren->RemoveActor(m_vLineActor);
    ren->RemoveActor(m_hLineActor);
}

void SliceStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    if (!ren) return;
    vtkCamera* cam = ren->GetActiveCamera();
    cam->ParallelProjectionOn(); // 开启平行投影

    double imgCenter[3];
    if (m_mapper && m_mapper->GetInput()) {
        m_mapper->GetInput()->GetCenter(imgCenter);
    }

    // 初次设置
    cam->SetFocalPoint(imgCenter);
    double distance = 0.1;

    switch (m_orientation) {
    case Orientation::AXIAL:
        // AXIAL (轴状位): 从头顶往下看
        cam->SetPosition(imgCenter[0], imgCenter[1], imgCenter[2] + distance);
        cam->SetViewUp(0, 1, 0);
        break;

    case Orientation::CORONAL:
        // CORONAL (冠状位): 从前面往后看
        cam->SetPosition(imgCenter[0], imgCenter[1] + distance, imgCenter[2]);
        cam->SetViewUp(0, 0, 1); // Z轴是向上的
        break;

    case Orientation::SAGITTAL:
        // SAGITTAL (矢状位): 从侧面看
        cam->SetPosition(imgCenter[0] + distance, imgCenter[1], imgCenter[2]);
        cam->SetViewUp(0, 0, 1); // Z轴是向上的
        break;
    }

    ren->ResetCamera();
    ren->ResetCameraClippingRange();
}

void SliceStrategy::SetSliceIndex(int index) {
    m_currentIndex = index;
    if (m_currentIndex < 0) m_currentIndex = 0;
    if (m_currentIndex > m_maxIndex) m_currentIndex = m_maxIndex;
    UpdatePlanePosition();
}

void SliceStrategy::SetOrientation(Orientation orient)
{
    if (m_orientation == orient) return;
    m_orientation = orient;

    if (!m_mapper) return;
    vtkImageData* input = m_mapper->GetInput();
    vtkPlane* plane = m_mapper->GetSlicePlane();
    if (!input || !plane) return;

    if (m_orientation == Orientation::AXIAL) {
        plane->SetNormal(0, 0, 1);
    }
    else if (m_orientation == Orientation::CORONAL) {
        plane->SetNormal(0, 1, 0);
    }
    else { // SAGITTAL
        plane->SetNormal(1, 0, 0);
    }

    int dims[3];
    input->GetDimensions(dims);

    if (m_orientation == Orientation::AXIAL) {
        m_maxIndex = dims[2] - 1;
    }
    else if (m_orientation == Orientation::CORONAL) {
        m_maxIndex = dims[1] - 1;
    }
    else {
        m_maxIndex = dims[0] - 1;
    }

    m_currentIndex = m_maxIndex / 2;
    UpdatePlanePosition();
}

void SliceStrategy::UpdateCrosshair(int x, int y, int z) {
    if (!m_mapper->GetInput()) return;

    vtkImageData* img = m_mapper->GetInput();
    double origin[3], spacing[3];
    int dims[3];
    img->GetOrigin(origin);
    img->GetSpacing(spacing);
    img->GetDimensions(dims);
    double bounds[6];
    img->GetBounds(bounds);

    double physX = origin[0] + x * spacing[0];
    double physY = origin[1] + y * spacing[1];
    double physZ = origin[2] + z * spacing[2];

    double layerOffset = 0.05;

    if (m_orientation == Orientation::AXIAL) {
        double currentZ = origin[2] + m_currentIndex * spacing[2] + layerOffset;
        m_vLineSource->SetPoint1(physX, bounds[2], currentZ);
        m_vLineSource->SetPoint2(physX, bounds[3], currentZ);
        m_hLineSource->SetPoint1(bounds[0], physY, currentZ);
        m_hLineSource->SetPoint2(bounds[1], physY, currentZ);
    }
    else if (m_orientation == Orientation::CORONAL) {
        double currentY = origin[1] + m_currentIndex * spacing[1] + layerOffset;
        m_vLineSource->SetPoint1(physX, currentY, bounds[4]);
        m_vLineSource->SetPoint2(physX, currentY, bounds[5]);
        m_hLineSource->SetPoint1(bounds[0], currentY, physZ);
        m_hLineSource->SetPoint2(bounds[1], currentY, physZ);
    }
    else { // SAGITTAL
        double currentX = origin[0] + m_currentIndex * spacing[0] + layerOffset;
        m_vLineSource->SetPoint1(currentX, physY, bounds[4]);
        m_vLineSource->SetPoint2(currentX, physY, bounds[5]);
        m_hLineSource->SetPoint1(currentX, bounds[2], physZ);
        m_hLineSource->SetPoint2(currentX, bounds[3], physZ);
    }
}

void SliceStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (((int)flags & (int)UpdateFlags::Cursor))
    {
        int x = params.cursor[0];
        int y = params.cursor[1];
        int z = params.cursor[2];
        if (Orientation::AXIAL == m_orientation) {
            SetSliceIndex(z);
        }
        else if (Orientation::CORONAL == m_orientation) {
            SetSliceIndex(y);
        }
        else if (Orientation::SAGITTAL == m_orientation) {
            SetSliceIndex(x);
        }
        UpdateCrosshair(x, y, z);
    }

	// 颜色映射和材质参数改变都可能影响最终的视觉效果，所以放在一起处理
    if (HasFlag(flags, UpdateFlags::TF) || HasFlag(flags, UpdateFlags::Material))
    {
        RebuildLUT(params);  // 内部依赖 tfNodes + scalarRange + material.opacity

        // 同步 vtkImageProperty 的整体透明度（架构保持一致）
        if (m_slice && m_slice->GetProperty())
        {
            auto imgProp = m_slice->GetProperty();
            imgProp->SetOpacity(params.material.opacity);
            imgProp->SetAmbient(params.material.ambient);
            imgProp->SetDiffuse(params.material.diffuse);
        }
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        const int vis = (params.visibilityMask & VisFlags::Crosshair) ? 1 : 0;
        if (m_vLineActor) m_vLineActor->SetVisibility(vis);
        if (m_hLineActor) m_hLineActor->SetVisibility(vis);
    }
}

void SliceStrategy::UpdatePlanePosition() {
    vtkImageData* input = m_mapper->GetInput();
    vtkPlane* plane = m_mapper->GetSlicePlane();

    double origin[3], spacing[3];
    input->GetOrigin(origin);
    input->GetSpacing(spacing);

    double planeOrigin[3];
    plane->GetOrigin(planeOrigin);

    if (m_orientation == Orientation::AXIAL) {
        planeOrigin[2] = origin[2] + (m_currentIndex * spacing[2]);
    }
    else if (m_orientation == Orientation::CORONAL) {
        planeOrigin[1] = origin[1] + (m_currentIndex * spacing[1]);
    }
    else { // SAGITTAL
        planeOrigin[0] = origin[0] + (m_currentIndex * spacing[0]);
    }

    plane->SetOrigin(planeOrigin);
}