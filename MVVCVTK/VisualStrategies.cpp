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
    ren->SetBackground(0, 0, 0);
}

void SliceStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveViewProp(m_slice);
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


void SliceStrategy::SetInteractionValue(int delta) {

    m_currentIndex += delta;

    if (m_currentIndex < 0) m_currentIndex = 0;
    if (m_currentIndex > m_maxIndex) m_currentIndex = m_maxIndex;

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