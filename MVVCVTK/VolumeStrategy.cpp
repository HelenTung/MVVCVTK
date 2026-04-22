#include "VolumeStrategy.h"
#include <vtkSmartVolumeMapper.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkImageResample.h>
#include <vtkCamera.h>
#include <vtkMatrix4x4.h>

void VolumeStrategy::SetCameraAligned(const std::array<double, 16>& modelMatrix)
{
    if (!m_renderer || !m_renderer->GetActiveCamera()) return;

    auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
    mat->DeepCopy(modelMatrix.data());

    double modelCenter[4] = { m_dataCenter[0], m_dataCenter[1], m_dataCenter[2], 1.0 };
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

VolumeStrategy::VolumeStrategy() {
    m_volume = vtkSmartPointer<vtkVolume>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_volume->SetPickable(false); // 体渲染不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取

    SetManagedProp(m_volume);
    SetManagedProp(m_cubeAxes);
}

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    img->GetCenter(m_dataCenter);

    if (m_lastInput == data && m_volume->GetMapper()) {
        return;
    }
    m_lastInput = data;

    auto mapper = vtkSmartPointer<vtkSmartVolumeMapper>::New();
    // mapper->SetInputConnection(GetDownsampledOutputPort(img,766)); // 使用处理后(或原始)的数据
	mapper->SetInputData(img); // 使用原始数据
    mapper->SetAutoAdjustSampleDistances(1); // 自动调整采样距离
    mapper->SetInteractiveUpdateRate(10.0);

	// 使用原始数据的边界来设置坐标轴范围，确保坐标轴反映真实空间位置
    m_cubeAxes->SetBounds(img->GetBounds());
    m_volume->SetMapper(mapper);
    if (!m_volume->GetProperty()) {
        auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
        volumeProperty->ShadeOn();
        volumeProperty->SetInterpolationTypeToLinear();
        m_volume->SetProperty(volumeProperty);
    }
}

void VolumeStrategy::SetRendererAttached(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::SetRendererAttached(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
    ren->SetBackground(0.05, 0.05, 0.05); // 黑色背景
}

void VolumeStrategy::SetCameraConfigured(vtkSmartPointer<vtkRenderer> ren) {
    ren->GetActiveCamera()->ParallelProjectionOff();
}   

void VolumeStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{   
    if (!m_volume || !m_volume->GetProperty()) return;

    // 响应 UpdateFlags::TF
    auto prop = m_volume->GetProperty();
    if ((int)flags & (int)UpdateFlags::TF || ((int)flags & (int)UpdateFlags::Material)) {
        // 构建 VTK 函数
        auto ctf = prop->GetRGBTransferFunction();
        auto otf = prop->GetScalarOpacity();

        if (!ctf)
        {
            ctf = vtkSmartPointer<vtkColorTransferFunction>::New();
            prop->SetColor(ctf);
        }

        if (!otf)
        {
            otf = vtkSmartPointer<vtkPiecewiseFunction>::New();
            prop->SetScalarOpacity(otf);
        }
        ctf->RemoveAllPoints();
        otf->RemoveAllPoints();

        double min = params.scalarRange[0];
        double max = params.scalarRange[1];
        double globalOpacity = params.material.opacity;

        for (const auto& node : params.tfNodes) {
            double val = min + node.position * (max - min);
            ctf->AddRGBPoint(val, node.r, node.g, node.b);
            otf->AddPoint(val, node.opacity * globalOpacity);
        }
        // 应用到底层
        prop->SetColor(ctf);
        prop->SetScalarOpacity(otf);
        prop->Modified();
    }

    // 响应 UpdateFlags::Material
    if ((int)flags & (int)UpdateFlags::Material) {
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);

        if (params.material.shadeOn) prop->ShadeOn();
        else prop->ShadeOff();
    }

    // 响应变换矩阵
    if (HasFlag(flags,UpdateFlags::Transform)) {
        Set3DPropsTransform(params.modelMatrix);
        SetCameraAligned(params.modelMatrix);
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        if (m_cubeAxes)
            m_cubeAxes->SetVisibility(
                (params.visibilityMask & VisFlags::Ruler) ? 1 : 0);
    }
}

vtkProp3D* VolumeStrategy::GetMainProp()
{
    if (!m_volume) return nullptr;
    else return m_volume;
}