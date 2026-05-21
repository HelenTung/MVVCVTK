#include "VolumeStrategy.h"
#include <vtkSmartVolumeMapper.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkImageResample.h>
#include <vtkCamera.h>
#include <vtkMatrix4x4.h>
#include <cmath>

bool VolumeStrategy::GetOpacityChanged(double opacity) const
{
    return std::abs(m_opacity - opacity) > 1e-6;
}

void VolumeStrategy::SetCameraAligned(const std::array<double, 16>& modelMatrix)
{
    if (!m_renderer || !m_renderer->GetActiveCamera()) return;

    // 体对象经过模型矩阵变换后，保持相机到焦点的相对偏移不变，
    // 只把焦点整体搬到新的世界中心，避免用户视角在 Transform 后突然跳变。

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
    m_mapper = vtkSmartPointer<vtkSmartVolumeMapper>::New();
    m_volume->SetPickable(false); // 体渲染不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取
    m_mapper->SetRequestedRenderModeToGPU(); // 优先走 GPU 体渲染管线，避免大体数据退回 CPU 路径
    m_mapper->SetInteractiveUpdateRate(10.0);
    m_volume->SetMapper(m_mapper);

    auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
    volumeProperty->ShadeOn();
    volumeProperty->SetInterpolationTypeToLinear();
    m_volume->SetProperty(volumeProperty);

    SetManagedProp(m_volume);
    SetManagedProp(m_cubeAxes);
}

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    img->GetCenter(m_dataCenter);

    if (m_lastInput == data && m_mapper && m_mapper->GetInput() == img) {
        return;
    }
    m_lastInput = data;

    m_qualityResample = ImageProcessor::GetDownsampledImage(img, m_qualityTargetDim);
    m_interactionResample = ImageProcessor::GetDownsampledImage(img, m_interactionTargetDim);
    m_mapper->SetInputConnection(m_qualityResample->GetOutputPort());
    m_isInteracting = false;

	// 使用原始数据的边界来设置坐标轴范围，确保坐标轴反映真实空间位置
    m_cubeAxes->SetBounds(img->GetBounds());
}

void VolumeStrategy::SetRendererAttached(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::SetRendererAttached(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
}

void VolumeStrategy::SetCameraConfigured(vtkSmartPointer<vtkRenderer> ren) {
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void VolumeStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (!m_volume || !m_volume->GetProperty()) return;

    auto prop = m_volume->GetProperty();
    const bool isTfChanged = HasFlag(flags, UpdateFlags::TF);
    const bool isMaterialChanged = HasFlag(flags, UpdateFlags::Material);
    const bool isInteractionChanged = HasFlag(flags, UpdateFlags::Interaction);

    if ((isTfChanged || isInteractionChanged) && m_mapper) {
        const bool nextIsInteracting = isInteractionChanged
            ? params.isInteracting
            : m_isInteracting;
        const bool interactionStateChanged = (m_isInteracting != nextIsInteracting);

        m_isInteracting = nextIsInteracting;

        auto activeResample = m_isInteracting ? m_interactionResample : m_qualityResample;
        if (activeResample && (interactionStateChanged || isTfChanged)) {
            m_mapper->SetInputConnection(activeResample->GetOutputPort());
        }
    }

    // TF 与 Material 分开处理的原因是：
    // TF 变更通常意味着整条颜色/透明度曲线要重建；
    // 单纯材质变化则尽量只更新光照或全局 opacity，避免重复构造整套传输函数。
    if (HasFlag(flags, UpdateFlags::TF)) {
        // 遵循数据类与状态类分离、前后处理分离的思想，离线组装 VTK 函数，避免高频 Modified 触发重新渲染
        auto newCtf = vtkSmartPointer<vtkColorTransferFunction>::New();
        auto newOtf = vtkSmartPointer<vtkPiecewiseFunction>::New();

        double min = params.scalarRange[0];
        double max = params.scalarRange[1];
        double globalOpacity = params.material.opacity;

        for (const auto& node : params.tfNodes) {
            double val = min + node.position * (max - min);
            newCtf->AddRGBPoint(val, node.r, node.g, node.b);
            newOtf->AddPoint(val, node.opacity * globalOpacity);
        }

        // 单次应用到底层，避免多次触发重管线
        prop->SetColor(newCtf);
        prop->SetScalarOpacity(newOtf);
        m_opacity = params.material.opacity;
    }

    if (isMaterialChanged && !isTfChanged && GetOpacityChanged(params.material.opacity)) {
        auto otf = vtkSmartPointer<vtkPiecewiseFunction>::New();
        const double min = params.scalarRange[0];
        const double max = params.scalarRange[1];
        for (const auto& node : params.tfNodes) {
            const double val = min + node.position * (max - min);
            otf->AddPoint(val, node.opacity * params.material.opacity);
        }
        prop->SetScalarOpacity(otf);
        m_opacity = params.material.opacity;
    }

    if (isMaterialChanged) {
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
