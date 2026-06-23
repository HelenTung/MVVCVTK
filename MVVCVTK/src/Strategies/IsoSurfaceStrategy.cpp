#include "IsoSurfaceStrategy.h"
#include <vtkProperty.h>
#include <vtkCamera.h>
#include <vtkMatrix4x4.h>

namespace {

constexpr int kInteractionIsoTargetDim = 256;
constexpr int kQualityIsoTargetDim = 766;

}

void IsoSurfaceStrategy::AlignCamera(const std::array<double, 16>& modelMatrix)
{
    if (!m_renderer || !m_renderer->GetActiveCamera()) return;

    // 与 VolumeStrategy 保持一致：模型变换后相机只跟着焦点中心平移，
    // 这样 3D 视角感知稳定，不会因为对象移动而瞬间“跳镜头”。

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

IsoSurfaceStrategy::IsoSurfaceStrategy() {
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_qualityIsoFilter = vtkSmartPointer<vtkFlyingEdges3D>::New();
    m_interactionIsoFilter = vtkSmartPointer<vtkFlyingEdges3D>::New();
    m_mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_mapper->SetVBOShiftScaleMethod(vtkPolyDataMapper::DISABLE_SHIFT_SCALE);
    // 初始绑定
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetInterpolationToPhong();

    m_actor->SetPickable(false); // 等值面不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取

    // 静态数据
    m_actor->GetProperty()->SetInterpolationToPhong();
    m_qualityIsoFilter->ComputeNormalsOff();
    m_qualityIsoFilter->ComputeGradientsOff();
    m_interactionIsoFilter->ComputeNormalsOff();
    m_interactionIsoFilter->ComputeGradientsOff();

    AddManagedProp(m_actor);
    AddManagedProp(m_cubeAxes);
}

void IsoSurfaceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (m_lastInput == data) {
        return;
    }

    auto poly = vtkPolyData::SafeDownCast(data);
    if (poly) {
        // 如果上游已经给的是 mesh，则直接走 PolyData 路径，不再重复提等值面。
        m_lastInput = data;
        poly->GetCenter(m_dataCenter);
        m_mapper->SetInputData(poly);
        m_mapper->ScalarVisibilityOff();
        m_actor->SetMapper(m_mapper);
        m_cubeAxes->SetBounds(poly->GetBounds());

        // VG Style
        auto prop = m_actor->GetProperty();
        prop->SetColor(0.75, 0.75, 0.75); // VG 灰
        prop->SetAmbient(0.2);
        prop->SetDiffuse(0.8);
        prop->SetSpecular(0.15);      // 稍微增加一点高光
        prop->SetSpecularPower(15.0);
        prop->SetInterpolationToGouraud();
        return;
    }

    // 作为 ImageData (需要��时计算)
    auto img = vtkImageData::SafeDownCast(data);
    if (img) {
        m_lastInput = data;
        img->GetCenter(m_dataCenter);

        m_qualityResample = ImageProcessor::GetDownsampledImage(img, kQualityIsoTargetDim);
        m_interactionResample = ImageProcessor::GetDownsampledImage(img, kInteractionIsoTargetDim);
        if (m_qualityResample) {
            m_qualityIsoFilter->SetInputConnection(m_qualityResample->GetOutputPort());
        }
        if (m_interactionResample) {
            m_interactionIsoFilter->SetInputConnection(m_interactionResample->GetOutputPort());
        }

        m_currentIsoValue = 0.0;
        m_qualityIsoFilter->SetValue(0, m_currentIsoValue);
        m_interactionIsoFilter->SetValue(0, m_currentIsoValue);
        m_isInteracting = false;
        m_mapper->SetInputConnection(m_qualityIsoFilter->GetOutputPort());
        m_mapper->ScalarVisibilityOff();
        m_cubeAxes->SetBounds(img->GetBounds());
    }
}

void IsoSurfaceStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
}

void IsoSurfaceStrategy::ConfigureCamera(vtkSmartPointer<vtkRenderer> ren) {
    // 3D 模式必须是透视投影
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void IsoSurfaceStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (!m_actor) return;
    auto prop = m_actor->GetProperty();

    // 等值面策略主要消费三类状态：
    // 1. Material 控制表面光照和透明度
    // 2. IsoValue 控制几何提取阈值
    // 3. Transform / Visibility 控制空间摆放与辅助元素显隐

    // 响应 UpdateFlags::Material
    if (HasFlag(flags, UpdateFlags::Material)) {

        // 设置光照参数
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);

        // 设置几何体透明度
        prop->SetOpacity(params.material.opacity);
        // 设置着色方式,开启光照
        if (params.material.shadeOn) prop->SetInterpolationToPhong();
        else prop->SetInterpolationToFlat();
    }

    // 等值面这里也采用与体渲染一致的状态收敛思路：
    // 1. 先计算这一帧最终应落到的交互态 nextIsInteracting
    // 2. 再计算这一帧最终应使用的阈值 nextIsoValue
    // 3. 根据 nextIsInteracting 选择当前活动过滤器 activeIsoFilter
    // 4. 只在交互态切换或阈值确实变化时，才把最终值下发到活动过滤器
    // 这样可以保证：
    // - 拖动期间只更新 256 预览等值面
    // - 松手后切回 766 质量等值面
    // - 同一帧同时带有 Interaction 和 IsoValue 时，统一按最终状态收口
    if (HasFlag(flags, UpdateFlags::Interaction) || HasFlag(flags, UpdateFlags::IsoValue)) {
        const bool nextIsInteracting = HasFlag(flags, UpdateFlags::Interaction)
            ? params.isInteracting
            : m_isInteracting;
        const double nextIsoValue = HasFlag(flags, UpdateFlags::IsoValue)
            ? params.isoValue
            : m_currentIsoValue;
        const bool interactionChanged = (m_isInteracting != nextIsInteracting);
        const bool isoValueChanged = (m_currentIsoValue != nextIsoValue);

        m_isInteracting = nextIsInteracting;
        m_currentIsoValue = nextIsoValue;

        auto activeIsoFilter = m_isInteracting ? m_interactionIsoFilter : m_qualityIsoFilter;
        if (activeIsoFilter && activeIsoFilter->GetInput()) {
            if ((interactionChanged || isoValueChanged)
                && activeIsoFilter->GetValue(0) != m_currentIsoValue) {
                // 活动过滤器只接收这一帧最终阈值，避免交互过程中两条等值面管线同时重算。
                activeIsoFilter->SetValue(0, m_currentIsoValue);
            }

            if (interactionChanged) {
                // 只有交互态真的切换时才改 mapper 输入，
                // 避免单纯阈值变化时重复做输入重绑。
                m_mapper->SetInputConnection(activeIsoFilter->GetOutputPort());
            }
        }
    }

    // 响应 UpdateFlags::Transform
    if (HasFlag(flags, UpdateFlags::Transform)) {
        Set3DPropsTransform(params.modelMatrix);
        AlignCamera(params.modelMatrix);
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        if (m_cubeAxes)
            m_cubeAxes->SetVisibility(
                (params.visibilityMask & VisFlags::Ruler) ? 1 : 0);
    }
}

vtkProp3D* IsoSurfaceStrategy::GetMainProp()
{
    return m_actor;
}
