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
    if (m_orientation == AXIAL) {
        plane->SetNormal(0, 0, 1); // Z轴法线
    }
    else if (m_orientation == CORONAL) {
        plane->SetNormal(0, 1, 0); // Y轴法线
    }
    else {
        plane->SetNormal(1, 0, 0); // X轴法线
    }

    // 将 Plane 对象传递给 Mapper
    m_mapper->SetSlicePlane(plane);
    m_slice->SetMapper(m_mapper);

    // 自动对比度
    double range[2];
    img->GetScalarRange(range);
    m_slice->GetProperty()->SetColorWindow(range[1] - range[0]);
    m_slice->GetProperty()->SetColorLevel((range[1] + range[0]) / 2.0);
}

void SliceStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddViewProp(m_slice);
    ren->SetBackground(0, 0, 0);
}

void SliceStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveViewProp(m_slice);
}

void SliceStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    vtkCamera* cam = ren->GetActiveCamera();
    cam->ParallelProjectionOn(); // 开启平行投影

    // 强制相机视角
    if (m_orientation == AXIAL) {
        cam->SetViewUp(0, 1, 0);
        cam->SetPosition(0, 0, 1000); // 放在Z轴远处
        cam->SetFocalPoint(0, 0, 0);
    }
    else if (m_orientation == CORONAL) {
        cam->SetViewUp(0, 0, 1);
        cam->SetPosition(0, 1000, 0); // 放在Y轴远处
        cam->SetFocalPoint(0, 0, 0);
    }
    else { // SAGITTAL
        cam->SetViewUp(0, 0, 1);
        cam->SetPosition(1000, 0, 0);
    } // 放在X轴远处
	ren->ResetCamera(); 
}

void SliceStrategy::SetInteractionValue(int value) {
    vtkPlane* plane = m_mapper->GetSlicePlane();
    if (!plane) return;

    vtkImageData* input = m_mapper->GetInput();
    if (!input) return;

    double spacing[3]; // 层厚 (比如 0.5mm)
    double origin[3];  // 数据原本的起始坐标 (比如 -100.0mm)
    input->GetSpacing(spacing);
    input->GetOrigin(origin);

    // 获取当前平面的原点，准备修改它
    double planeOrigin[3];
    plane->GetOrigin(planeOrigin);

    // 计算物理坐标： 物理位置 = 起始点 + (层数 * 层厚)
    if (m_orientation == AXIAL) {
        // 修改 Z 轴高度
        planeOrigin[2] = origin[2] + (value * spacing[2]);
    }
    else if (m_orientation == CORONAL) {
        // 修改 Y 轴深度
        planeOrigin[1] = origin[1] + (value * spacing[1]);
    }
    else { // SAGITTAL
        // 修改 X 轴水平位置
        planeOrigin[0] = origin[0] + (value * spacing[0]);
    }

    // 应用新的位置
    plane->SetOrigin(planeOrigin);
}
