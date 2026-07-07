#include "VolumeStrategy.h"
#include <vtkGPUVolumeRayCastMapper.h>
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

void VolumeStrategy::AlignCamera(const std::array<double, 16>& modelMatrix)
{
    if (!m_renderer || !m_renderer->GetActiveCamera()) return;

    // 体对象经过模型矩阵变换后，保持相机到焦点的相对偏移不变，
    // 只把焦点整体搬到新的世界中心，避免用户视角在 Transform 后突然跳变。

    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorldMatrix->DeepCopy(modelMatrix.data());

    double modelToWorldInputCenter[4] = { m_dataCenter[0], m_dataCenter[1], m_dataCenter[2], 1.0 };
    double modelToWorldOutputCenter[4] = { 0.0, 0.0, 0.0, 1.0 };
    modelToWorldMatrix->MultiplyPoint(modelToWorldInputCenter, modelToWorldOutputCenter);

    const double invW = std::abs(modelToWorldOutputCenter[3]) > 1e-12 ? 1.0 / modelToWorldOutputCenter[3] : 1.0;
    double worldCenter[3] = {
        modelToWorldOutputCenter[0] * invW,
        modelToWorldOutputCenter[1] * invW,
        modelToWorldOutputCenter[2] * invW
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
    // 固定 GPU mapper：volume RemoveInside 预览需要 shader discard 后端表达。
    m_mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
    m_volume->SetPickable(false); // 体渲染不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取
    m_mapper->SetAutoAdjustSampleDistances(true);
    m_volume->SetMapper(m_mapper);

    auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
    volumeProperty->ShadeOn();
    volumeProperty->SetInterpolationTypeToLinear();
    m_volume->SetProperty(volumeProperty);

    AddManagedProp(m_volume);
    AddManagedProp(m_cubeAxes);
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

void VolumeStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
}

void VolumeStrategy::SetCamera(vtkSmartPointer<vtkRenderer> ren) {
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void VolumeStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (!m_volume || !m_volume->GetProperty()) return;

    auto prop = m_volume->GetProperty();
    const bool hasTfChanged = GetFlagOn(flags, UpdateFlags::TF);
    const bool hasMaterialChanged = GetFlagOn(flags, UpdateFlags::Material);

    // 体渲染这里把 Interaction 与 TF 放在同一个状态收敛块里处理：
    // 1. 先推导“这一帧结束后”应该处于的交互态 nextIsInteracting
    // 2. 再根据 nextIsInteracting 选择当前活动输入 activeResample
    // 3. 只有在交互态真的切换，或 TF 刷新要求重新绑定当前输入时，才改 mapper 输入
    // 这样可以保证：
    // - 交互过程中 TF 高频变化仍然稳定落在 256 预览输入
    // - 静止期 TF 更新会继续落在 766 质量输入
    // - 同一帧里若同时出现 TF 与 Interaction，也只按最终状态收口一次
    if ((hasTfChanged || GetFlagOn(flags, UpdateFlags::Interaction)) && m_mapper) {
        const bool nextIsInteracting = GetFlagOn(flags, UpdateFlags::Interaction)
            ? params.isInteracting
            : m_isInteracting;
        const bool hasInteractionChanged = (m_isInteracting != nextIsInteracting);

        m_isInteracting = nextIsInteracting;

        auto activeResample = m_isInteracting ? m_interactionResample : m_qualityResample;
        if (activeResample && (hasInteractionChanged || hasTfChanged)) {
            // TF 改变后需要确保 mapper 仍绑在“当前应该显示”的那一路输入上；
            // 这里不区分是交互切换导致，还是 TF 更新导致，统一按活动输入重绑即可。
            m_mapper->SetInputConnection(activeResample->GetOutputPort());
        }
    }

    // TF 与 Material 分开处理的原因是：
    // TF 变更通常意味着整条颜色/透明度曲线要重建；
    // 单纯材质变化则尽量只更新光照或全局 opacity，避免重复构造整套传输函数。
    if (GetFlagOn(flags, UpdateFlags::TF)) {
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

    if (hasMaterialChanged && !hasTfChanged && GetOpacityChanged(params.material.opacity)) {
        // 当 TF 没变、只有全局 opacity 变动时，不必重建颜色函数；
        // 这里只重建 OTF，把当前 opacity 重新折算进已有 TF 节点即可。
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

    if (hasMaterialChanged) {
        // 光照相关参数和 TF/OTF 解耦处理，避免纯材质调整时不必要地重建体数据映射函数。
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);

        if (params.material.isShadeOn) prop->ShadeOn();
        else prop->ShadeOff();
    }

    // 响应变换矩阵
    if (GetFlagOn(flags,UpdateFlags::Transform)) {
        Set3DPropsTransform(params.modelMatrix);
        AlignCamera(params.modelMatrix);
    }

    if (GetFlagOn(flags, UpdateFlags::Visibility)) {
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
