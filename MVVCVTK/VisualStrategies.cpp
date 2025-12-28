#include "VisualStrategies.h"
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCamera.h>
#include <vtkImageProperty.h>


// ================= IsoSurfaceStrategy =================
IsoSurfaceStrategy::IsoSurfaceStrategy() {
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
}

void IsoSurfaceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto poly = vtkPolyData::SafeDownCast(data);
    if (!poly) return;

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(poly);
    mapper->ScalarVisibilityOff();
    m_actor->SetMapper(mapper);

    m_cubeAxes->SetBounds(poly->GetBounds());
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
    ren->AddActor(m_cubeAxes);
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
    ren->SetBackground(0.1, 0.15, 0.2); // 蓝色调背景
}

void IsoSurfaceStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveActor(m_actor);
	ren->RemoveActor(m_cubeAxes);
}

void IsoSurfaceStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    // 3D 模式必须是透视投影
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void IsoSurfaceStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
	return; // 等值面不需要更新
}

// ================= VolumeStrategy =================
VolumeStrategy::VolumeStrategy() {
    m_volume = vtkSmartPointer<vtkVolume>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
}

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    auto mapper = vtkSmartPointer<vtkSmartVolumeMapper>::New();
    mapper->SetInputData(img);
	mapper->SetAutoAdjustSampleDistances(1); // 自动调整采样距离 ?
	m_cubeAxes->SetBounds(img->GetBounds()); // 更新坐标轴范围

    m_volume->SetMapper(mapper);
    if (!m_volume->GetProperty()) {
        auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
        volumeProperty->ShadeOn();
        volumeProperty->SetInterpolationTypeToLinear();
        m_volume->SetProperty(volumeProperty);
    }
}

void VolumeStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddVolume(m_volume);
	ren->AddActor(m_cubeAxes);
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
    ren->SetBackground(0.05, 0.05, 0.05); // 黑色背景
}

void VolumeStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveVolume(m_volume);
	ren->RemoveActor(m_cubeAxes);
}

void VolumeStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void VolumeStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (!((int)flags & (int)UpdateFlags::TF)) return;
    if (!m_volume || !m_volume->GetProperty()) return;

	// 获取当前传输函数
	auto prop = m_volume->GetProperty();
	auto opacityFunc = prop->GetScalarOpacity();
	auto colorFunc = prop->GetRGBTransferFunction();

	if (!opacityFunc || !colorFunc) return;

	opacityFunc->RemoveAllPoints();
	colorFunc->RemoveAllPoints();

	double min = params.scalarRange[0];
	double max = params.scalarRange[1];

    for(const auto& node : params.tfNodes) {
        double realPos = min + node.position * (max - min);
        colorFunc->AddRGBPoint(realPos, node.r, node.g, node.b);
        opacityFunc->AddPoint(realPos, node.opacity);
	}

	ApplyTransferParams(colorFunc, opacityFunc);
}

void VolumeStrategy::ApplyTransferParams(vtkSmartPointer<vtkColorTransferFunction> ctf, vtkSmartPointer<vtkPiecewiseFunction> otf)
{
    if (!m_volume->GetProperty()) return;
    m_volume->GetProperty()->SetColor(ctf);
	m_volume->GetProperty()->SetScalarOpacity(otf);
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

	// 初始化颜色映射表
    m_lut = vtkSmartPointer<vtkColorTransferFunction>::New();
    m_slice->GetProperty()->SetLookupTable(m_lut);
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
    double layerOffset = 0.05;

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

void SliceStrategy::ApplyColorMap(vtkSmartPointer<vtkColorTransferFunction> ctf)
{
    if (!m_slice || !ctf) return;
    // 将 2D 切片的属性设置为使用 LookupTable
    m_slice->GetProperty()->SetLookupTable(ctf);
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

	// 更新颜色映射表
    if (((int)flags & (int)UpdateFlags::TF))
    {
        if (!params.tfNodes.empty()) {
            m_lut->RemoveAllPoints();
            double min = params.scalarRange[0];
            double diff = params.scalarRange[1] - min;

            for (const auto& node : params.tfNodes) {
                double scalarVal = min + diff * node.position;
                m_lut->AddRGBPoint(scalarVal, node.r, node.g, node.b);
            }
        }
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

void MultiSliceStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
	if (!((int)flags & (int)UpdateFlags::Cursor)) return;
    UpdateAllPositions(params.cursor[0], params.cursor[1], params.cursor[2]);
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
    m_referencePlanes = std::make_shared<ColoredPlanesStrategy>();

    // 根据模式创建主策略
    if (m_mode == VizMode::CompositeVolume) {
        m_mainStrategy = std::make_shared<VolumeStrategy>();
    }
    else if (m_mode == VizMode::CompositeIsoSurface) {
        m_mainStrategy = std::make_shared<IsoSurfaceStrategy>();
    }
}

void CompositeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (m_mainStrategy) {
        // 如果是等值面模式，这里进来的就是 Service 转好的 PolyData
        // 如果是体渲染模式，这里进来的就是 ImageData
        m_mainStrategy->SetInputData(data);
    }

    if (m_referencePlanes) {
        m_referencePlanes->SetInputData(data);
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

int CompositeStrategy::GetPlaneAxis(vtkActor* actor) {
    return m_referencePlanes->GetPlaneAxis(actor);
}

void CompositeStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (m_referencePlanes) {
        m_referencePlanes->UpdateVisuals(params,flags);
    }

    // 2. 更新主视图 (体渲染或等值面)
    if (m_mainStrategy) {
        // 多态调用！如果是 VolumeStrategy，它会自动更新 TF；如果是 IsoSurface，则什么都不做
        m_mainStrategy->UpdateVisuals(params,flags);
    }
}

// ================= ColoredPlanesStrategy =================
ColoredPlanesStrategy::ColoredPlanesStrategy() {
    double colors[3][3] = {
        {1.0, 0.0, 0.0}, // 红色: 矢状面 (Sagittal)
        {0.0, 1.0, 0.0}, // 绿色: 冠状面 (Coronal)
        {0.0, 0.0, 1.0}  // 蓝色: 轴状面 (Axial)
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
    m_imageData = vtkImageData::SafeDownCast(data);
    if (!m_imageData) return;

    double bounds[6];
    m_imageData->GetBounds(bounds);

    // 根据数据边界定义每个平面的大小
    // Sagittal Plane (YZ)
    m_planeSources[0]->SetOrigin(0, bounds[2], bounds[4]);
    m_planeSources[0]->SetPoint1(0, bounds[3], bounds[4]);
    m_planeSources[0]->SetPoint2(0, bounds[2], bounds[5]);

    // Coronal Plane (XZ)
    m_planeSources[1]->SetOrigin(bounds[0], 0, bounds[4]);
    m_planeSources[1]->SetPoint1(bounds[1], 0, bounds[4]);
    m_planeSources[1]->SetPoint2(bounds[0], 0, bounds[5]);

    // Axial Plane (XY)
    m_planeSources[2]->SetOrigin(bounds[0], bounds[2], 0);
    m_planeSources[2]->SetPoint1(bounds[1], bounds[2], 0);
    m_planeSources[2]->SetPoint2(bounds[0], bounds[3], 0);
}

void ColoredPlanesStrategy::UpdateAllPositions(int x, int y, int z) {
    if (!m_imageData) return;

    double origin[3], spacing[3];
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    // 将索引坐标转换为物理世界坐标
    double physPos[3];
    physPos[0] = origin[0] + x * spacing[0];
    physPos[1] = origin[1] + y * spacing[1];
    physPos[2] = origin[2] + z * spacing[2];

    // 移动每个平面的中心点到新的物理坐标
    m_planeSources[0]->SetCenter(physPos[0], m_planeSources[0]->GetCenter()[1], m_planeSources[0]->GetCenter()[2]);
    m_planeSources[1]->SetCenter(m_planeSources[1]->GetCenter()[0], physPos[1], m_planeSources[1]->GetCenter()[2]);
    m_planeSources[2]->SetCenter(m_planeSources[2]->GetCenter()[0], m_planeSources[2]->GetCenter()[1], physPos[2]);
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
	if (!((int)flags & (int)UpdateFlags::Cursor)) return;
    UpdateAllPositions(params.cursor[0], params.cursor[1], params.cursor[2]);
}
