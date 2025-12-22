#include "VisualStrategies.h"
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCamera.h>
#include <vtkImageProperty.h>
#include <vtkPlane.h>

// ================= IsoSurfaceStrategy =================
IsoSurfaceStrategy::IsoSurfaceStrategy() {
    m_actor = vtkSmartPointer<vtkActor>::New();
}

void IsoSurfaceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto poly = vtkPolyData::SafeDownCast(data);
    if (!poly) return;

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(poly);
    mapper->ScalarVisibilityOff();
    m_actor->SetMapper(mapper);

    // VG Style
    auto prop = m_actor->GetProperty();
    prop->SetColor(0.75, 0.75, 0.75); // VG 灰
    prop->SetAmbient(0.2);
    prop->SetDiffuse(0.8);
    prop->SetSpecular(0.15);      // 稍微增加一点高光
    prop->SetSpecularPower(15.0);
    prop->SetInterpolationToGouraud();
}

void IsoSurfaceStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddActor(m_actor);
    ren->SetBackground(0.1, 0.15, 0.2); // 蓝色调背景
}

void IsoSurfaceStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveActor(m_actor);
}

void IsoSurfaceStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    // 3D 模式必须是透视投影
    ren->GetActiveCamera()->ParallelProjectionOff();
}

// ================= VolumeStrategy =================
VolumeStrategy::VolumeStrategy() {
    m_volume = vtkSmartPointer<vtkVolume>::New();
}

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    auto mapper = vtkSmartPointer<vtkSmartVolumeMapper>::New();
    mapper->SetInputData(img);

    double range[2];
    img->GetScalarRange(range);
    double minVal = range[0];
    double maxVal = range[1];
    double diff = maxVal - minVal;

    // 透明度函数 (Opacity)
    auto opacityTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
    opacityTF->AddPoint(minVal, 0.0);
    opacityTF->AddPoint(minVal + diff * 0.35, 0.0); // 过滤低密度噪点
    opacityTF->AddPoint(minVal + diff * 0.60, 0.6);
    opacityTF->AddPoint(maxVal, 1.0);

    // 颜色传递函数 (Color)
    auto colorTF = vtkSmartPointer<vtkColorTransferFunction>::New();
    colorTF->AddRGBPoint(minVal, 0.0, 0.0, 0.0);
    colorTF->AddRGBPoint(minVal + diff * 0.40, 0.75, 0.75, 0.75); // 灰色基调
    colorTF->AddRGBPoint(maxVal, 0.95, 0.95, 0.95); // 高亮部分偏白

    // 属性设置 (Property)
    auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
    volumeProperty->SetColor(colorTF);
    volumeProperty->SetScalarOpacity(opacityTF);
    volumeProperty->ShadeOn();
    volumeProperty->SetInterpolationTypeToLinear();
    volumeProperty->SetAmbient(0.2);
    volumeProperty->SetDiffuse(0.9);
    volumeProperty->SetSpecular(0.2);

    m_volume->SetMapper(mapper);
    m_volume->SetProperty(volumeProperty);
}

void VolumeStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddVolume(m_volume);
    ren->SetBackground(0.05, 0.05, 0.05); // 黑色背景
}

void VolumeStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveVolume(m_volume);
}

void VolumeStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    ren->GetActiveCamera()->ParallelProjectionOff();
}

// ================= SliceStrategy (2D) =================
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
    // 如果不设置，默认为 (0,0,0)，这可能会导致切片显示在数据范围之外（黑屏）
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

    // 自动对比度
    double range[2];
    img->GetScalarRange(range);
    double window = range[1] - range[0];
    double level = (range[1] + range[0]) / 2.0;

    if (window < 1.0) window = 1.0;

    m_slice->GetProperty()->SetColorWindow(window);
    m_slice->GetProperty()->SetColorLevel(level);
}

void SliceStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddViewProp(m_slice);
    ren->AddActor(m_vLineActor);
    ren->AddActor(m_hLineActor);
    ren->SetBackground(0, 0, 0);
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
    double distance = 1000.0; 
    
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
    // 更新内部记录
    m_currentIndex = index;

    // 安全检查
    if (m_currentIndex < 0) m_currentIndex = 0;
    if (m_currentIndex > m_maxIndex) m_currentIndex = m_maxIndex;

    // 执行 VTK 渲染更新
    UpdatePlanePosition();
}

void SliceStrategy::SetOrientation(Orientation orient)
{
    if (m_orientation == orient) return;
    m_orientation = orient;

    // 获取当前数据和 Mapper 中的平面
    if (!m_mapper) return;
    vtkImageData* input = m_mapper->GetInput();
    vtkPlane* plane = m_mapper->GetSlicePlane();
    if (!input || !plane) return;

    // 更新切片法线
    if (m_orientation == Orientation::AXIAL) {
        plane->SetNormal(0, 0, 1);
    }
    else if (m_orientation == Orientation::CORONAL) {
        plane->SetNormal(0, 1, 0);
    }
    else { // SAGITTAL
        plane->SetNormal(1, 0, 0);
    }

    // 更新最大索引 (因为不同轴向的维度不同)
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

    // 重置当前索引到中间，防止越界或黑屏
    m_currentIndex = m_maxIndex / 2;

    // 应用新的位置
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

    // 计算物理坐标
    double physX = origin[0] + x * spacing[0];
    double physY = origin[1] + y * spacing[1];
    double physZ = origin[2] + z * spacing[2];

    // 为了防止线穿插，稍微给一点偏移，或者利用 Layer
    double layerOffset = 0.1;

    if (m_orientation == Orientation::AXIAL) { // Z轴切片，看 XY 平面
        // 当前切片的 Z 高度
        double currentZ = origin[2] + m_currentIndex * spacing[2] + layerOffset;

        // 垂直线 (固定 X，画 Y 的范围)
        m_vLineSource->SetPoint1(physX, bounds[2], currentZ);
        m_vLineSource->SetPoint2(physX, bounds[3], currentZ);

        // 水平线 (固定 Y，画 X 的范围)
        m_hLineSource->SetPoint1(bounds[0], physY, currentZ);
        m_hLineSource->SetPoint2(bounds[1], physY, currentZ);
    }
    else if (m_orientation == Orientation::CORONAL) { // Y轴切片，看 XZ 平面
        double currentY = origin[1] + m_currentIndex * spacing[1] + layerOffset;

        // 垂直线 (固定 X，画 Z 的范围)
        m_vLineSource->SetPoint1(physX, currentY, bounds[4]);
        m_vLineSource->SetPoint2(physX, currentY, bounds[5]);

        // 水平线 (固定 Z，画 X 的范围)
        m_hLineSource->SetPoint1(bounds[0], currentY, physZ);
        m_hLineSource->SetPoint2(bounds[1], currentY, physZ);
    }
    else { // SAGITTAL, X轴切片，看 YZ 平面
        double currentX = origin[0] + m_currentIndex * spacing[0] + layerOffset;

        // 垂直线 (固定 Y，画 Z 的范围)
        m_vLineSource->SetPoint1(currentX, physY, bounds[4]);
        m_vLineSource->SetPoint2(currentX, physY, bounds[5]);

        // 水平线 (固定 Z，画 Y 的范围)
        m_hLineSource->SetPoint1(currentX, bounds[2], physZ);
        m_hLineSource->SetPoint2(currentX, bounds[3], physZ);
    }
}

void SliceStrategy::UpdatePlanePosition() {
    vtkImageData* input = m_mapper->GetInput();
    vtkPlane* plane = m_mapper->GetSlicePlane();

    double origin[3];  // 数据的世界坐标原点
    double spacing[3]; // 像素间距
    input->GetOrigin(origin);
    input->GetSpacing(spacing);

    double planeOrigin[3];
    plane->GetOrigin(planeOrigin); // 获取当前平面的其他轴坐标

    // 计算公式：物理位置 = 数据原点 + (层数 * 层厚)
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

// ================= MultiSliceStrategy (MPR) =================
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

    // 设置三个切片的方向
    // 0: Sagittal (X normal)
    // 1: Coronal (Y normal)
    // 2: Axial (Z normal)

    // 初始化 Plane 和 Mapper
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

    // 遍历三个 Mapper 更新位置
    for (int i = 0; i < 3; i++) {
        auto plane = m_mappers[i]->GetSlicePlane();
        auto input = m_mappers[i]->GetInput();
        if (!input) continue;

        double origin[3], spacing[3];
        input->GetOrigin(origin);
        input->GetSpacing(spacing);

        double planeOrigin[3];
        plane->GetOrigin(planeOrigin);

        // 计算物理坐标: Origin + Index * Spacing
        planeOrigin[i] = origin[i] + (m_indices[i] * spacing[i]);

        plane->SetOrigin(planeOrigin);
    }
}

void MultiSliceStrategy::Attach(vtkSmartPointer<vtkRenderer> renderer) {
    for (int i = 0; i < 3; i++) renderer->AddViewProp(m_slices[i]);
    renderer->SetBackground(0.1, 0.1, 0.1); // 深灰背景
}

void MultiSliceStrategy::Detach(vtkSmartPointer<vtkRenderer> renderer) {
    for (int i = 0; i < 3; i++) renderer->RemoveViewProp(m_slices[i]);
}


// ================= CompositeStrategy =================

CompositeStrategy::CompositeStrategy(VizMode mode) : m_mode(mode) {
    // 始终创建参考平面
    m_referencePlanes = std::make_shared<MultiSliceStrategy>();

    // 根据模式创建主策略
    if (m_mode == VizMode::CompositeVolume) {
        m_mainStrategy = std::make_shared<VolumeStrategy>();
    }
    else if (m_mode == VizMode::CompositeIsoSurface) {
        m_mainStrategy = std::make_shared<IsoSurfaceStrategy>();
    }
}

void CompositeStrategy::SetReferenceData(vtkSmartPointer<vtkImageData> img)
{
    if (m_referencePlanes && img) {
        m_referencePlanes->SetInputData(img);
    }
}

void CompositeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (m_mainStrategy) {
        // 如果是等值面模式，这里进来的就是 Service 转好的 PolyData
        // 如果是体渲染模式，这里进来的就是 ImageData
        m_mainStrategy->SetInputData(data);
    }
}

void CompositeStrategy::Attach(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->Attach(renderer);
    if (m_referencePlanes) m_referencePlanes->Attach(renderer);
    renderer->SetBackground(0.05, 0.05, 0.05);
}

void CompositeStrategy::Detach(vtkSmartPointer<vtkRenderer> renderer) {
    if (m_mainStrategy) m_mainStrategy->Detach(renderer);
    if (m_referencePlanes) m_referencePlanes->Detach(renderer);
}

void CompositeStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> renderer) {
    // 通常 3D 视图使用透视投影
    if (renderer && renderer->GetActiveCamera()) {
        renderer->GetActiveCamera()->ParallelProjectionOff();
    }
}

void CompositeStrategy::UpdateReferencePlanes(int x, int y, int z) {
    // 需要转型回 MultiSliceStrategy 才能调用特定接口
    auto multiSlice = std::dynamic_pointer_cast<MultiSliceStrategy>(m_referencePlanes);
    if (multiSlice) {
        multiSlice->UpdateAllPositions(x, y, z);
    }
}