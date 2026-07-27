#include "VolumeStrategy.h"
#include <vtkOpenGLGPUVolumeRayCastMapper.h>
#include <vtkObjectFactory.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkImageResample.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkCamera.h>
#include <vtkMatrix4x4.h>
#include <algorithm>
#include <cmath>

class VolumeStrategy::Mapper final : public vtkOpenGLGPUVolumeRayCastMapper {
public:
    static Mapper* New();
    vtkTypeMacro(Mapper, vtkOpenGLGPUVolumeRayCastMapper);

    void SetEffectBinding(RenderEffectBinding* binding) { m_binding = binding; }

protected:
    void GPURender(vtkRenderer* renderer, vtkVolume* volume) override
    {
        if (m_binding) {
            (void)m_binding->OnRenderStart(renderer);
        }
        this->Superclass::GPURender(renderer, volume);
        if (m_binding) {
            (void)m_binding->OnRenderStop();
        }
    }

private:
    RenderEffectBinding* m_binding = nullptr;
};

vtkStandardNewMacro(VolumeStrategy::Mapper);

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
    m_mapper = vtkSmartPointer<Mapper>::New();
    m_volume->SetPickable(false); // 体渲染不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取
    m_mapper->SetAutoAdjustSampleDistances(false);
    m_mapper->SetImageSampleDistance(1.0);
    m_mapper->SetMinimumImageSampleDistance(1.0);
    m_mapper->SetMaximumImageSampleDistance(1.0);
    m_volume->SetMapper(m_mapper);

    auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
    volumeProperty->ShadeOff();
    volumeProperty->SetInterpolationTypeToLinear();
    m_volume->SetProperty(volumeProperty);

    AttachProp(m_volume);
    AttachProp(m_cubeAxes);
}

VolumeStrategy::~VolumeStrategy() = default;

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    if (m_lastInput == data && GetProducersReady()) {
        return;
    }
    const auto oldInput = m_lastInput;
    const auto oldMask = m_lastMask;
    m_lastInput = data;
    m_lastMask = nullptr;
    if (!BuildProducers()) {
        // producer 未提交时恢复期望键；同一输入修正后再次下发仍会重新尝试构建。
        m_lastInput = oldInput;
        m_lastMask = oldMask;
        (void)SetMapperInput();
        return;
    }

    img->GetCenter(m_dataCenter);
	// 使用原始数据的边界来设置坐标轴范围，确保坐标轴反映真实空间位置
    m_cubeAxes->SetBounds(img->GetBounds());
}

void VolumeStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    if (m_lastMask == validityMask
        && GetMasksReady(m_customTargetDim)) {
        return;
    }
    const auto oldMask = m_lastMask;
    const auto oldMaskInput = m_maskInput;
    const auto oldQualityMask = m_qualityMask;
    const auto oldCustomMask = m_customMask;
    const int oldQualityMaskDim = m_qualityMaskDim;
    const int oldCustomMaskDim = m_customMaskDim;
    m_lastMask = validityMask;
    if (!BuildMasks(m_customTargetDim)
        || !SetMapperInput()) {
        m_lastMask = oldMask;
        m_maskInput = oldMaskInput;
        m_qualityMask = oldQualityMask;
        m_customMask = oldCustomMask;
        m_qualityMaskDim = oldQualityMaskDim;
        m_customMaskDim = oldCustomMaskDim;
        (void)SetMapperInput();
        return;
    }
}

int VolumeStrategy::GetCustomDim() const
{
    return m_quality.quality == VolumeQuality::Custom
        ? m_quality.maxDimension : 766;
}

bool VolumeStrategy::GetProducersReady() const
{
    return m_qualityResample
        && m_customResample
        && m_producerInput == m_lastInput
        && m_qualityTargetDim == 766
        && m_customTargetDim == GetCustomDim()
        && m_isProducerDenoiseOn == m_isDenoiseOn;
}

bool VolumeStrategy::GetMasksReady(const int customTargetDim) const
{
    if (!m_lastMask) {
        return !m_maskInput
            && !m_qualityMask
            && !m_customMask;
    }
    return customTargetDim > 0
        && m_qualityMask
        && m_customMask
        && m_maskInput == m_lastMask
        && m_qualityMaskDim == 766
        && m_customMaskDim == customTargetDim;
}

double VolumeStrategy::GetQualityStep(
    vtkImageResample* qualityResample) const
{
    if (!qualityResample) return 0.0;
    qualityResample->UpdateInformation();
    const double* spacing = qualityResample->GetOutput()->GetSpacing();
    const double minSpacing = std::min(
        { spacing[0], spacing[1], spacing[2] });
    return std::isfinite(minSpacing) && minSpacing > 0.0
        ? 0.5 * minSpacing : 0.0;
}

bool VolumeStrategy::BuildMasks(const int customTargetDim)
{
    if (!m_mapper) return false;
    if (!m_lastMask) {
        m_qualityMask = nullptr;
        m_customMask = nullptr;
        m_maskInput = nullptr;
        m_qualityMaskDim = 0;
        m_customMaskDim = 0;
        m_mapper->SetMaskInput(nullptr);
        return true;
    }
    if (GetMasksReady(customTargetDim)) {
        return true;
    }
    if (customTargetDim <= 0) return false;

    auto qualityMask = ImageProcessor::GetDownsampledMask(
        m_lastMask, 766);
    if (!qualityMask) return false;
    vtkSmartPointer<vtkImageResample> customMask;
    if (customTargetDim == 766) {
        customMask = qualityMask;
    }
    else {
        customMask = ImageProcessor::GetDownsampledMask(
            m_lastMask, customTargetDim);
        if (!customMask) return false;
    }
    qualityMask->Update();
    if (customMask != qualityMask) {
        customMask->Update();
    }
    m_qualityMask = std::move(qualityMask);
    m_customMask = std::move(customMask);
    m_maskInput = m_lastMask;
    m_qualityMaskDim = 766;
    m_customMaskDim = customTargetDim;
    m_mapper->SetMaskTypeToBinary();
    return true;
}

bool VolumeStrategy::BuildProducers()
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image || !m_mapper) return false;

    constexpr int qualityTargetDim = 766;
    const int customTargetDim = GetCustomDim();
    if (customTargetDim <= 0) return false;
    if (GetProducersReady()) {
        return SetMapperInput();
    }

    vtkAlgorithmOutput* inputPort = nullptr;
    vtkSmartPointer<vtkImageAnisotropicDiffusion3D> denoiseFilter;
    if (m_isDenoiseOn) {
        double range[2] = { 0.0, 0.0 };
        image->GetScalarRange(range);
        if (!std::isfinite(range[0]) || !std::isfinite(range[1])
            || range[1] < range[0]) {
            return false;
        }
        denoiseFilter =
            vtkSmartPointer<vtkImageAnisotropicDiffusion3D>::New();
        denoiseFilter->SetInputData(image);
        denoiseFilter->SetNumberOfIterations(5);
        denoiseFilter->SetDiffusionFactor(0.125);
        denoiseFilter->SetDiffusionThreshold(
            0.02 * std::max(0.0, range[1] - range[0]));
        denoiseFilter->FacesOn();
        denoiseFilter->EdgesOff();
        denoiseFilter->CornersOff();
        inputPort = denoiseFilter->GetOutputPort();
    }

    auto qualityResample = ImageProcessor::GetDownsampledImage(
        image, qualityTargetDim, inputPort);
    if (!qualityResample) return false;
    vtkSmartPointer<vtkImageResample> customResample;
    if (customTargetDim == qualityTargetDim) {
        customResample = qualityResample;
    }
    else {
        customResample = ImageProcessor::GetDownsampledImage(
            image, customTargetDim, inputPort);
        if (!customResample) return false;
    }
    if (GetQualityStep(qualityResample) <= 0.0) return false;

    const auto oldDenoiseFilter = m_denoiseFilter;
    const auto oldQualityResample = m_qualityResample;
    const auto oldCustomResample = m_customResample;
    const auto oldProducerInput = m_producerInput;
    const int oldQualityTargetDim = m_qualityTargetDim;
    const int oldCustomTargetDim = m_customTargetDim;
    const bool wasProducerDenoiseOn = m_isProducerDenoiseOn;
    const auto oldMaskInput = m_maskInput;
    const auto oldQualityMask = m_qualityMask;
    const auto oldCustomMask = m_customMask;
    const int oldQualityMaskDim = m_qualityMaskDim;
    const int oldCustomMaskDim = m_customMaskDim;

    // mask 的结构键不含 denoise；仅 mask 输入或目标尺寸变化时同步重建。
    if (!GetMasksReady(customTargetDim)
        && !BuildMasks(customTargetDim)) {
        return false;
    }

    m_denoiseFilter = std::move(denoiseFilter);
    m_qualityResample = std::move(qualityResample);
    m_customResample = std::move(customResample);
    m_producerInput = m_lastInput;
    m_qualityTargetDim = qualityTargetDim;
    m_customTargetDim = customTargetDim;
    m_isProducerDenoiseOn = m_isDenoiseOn;
    if (SetMapperInput()) {
        return true;
    }

    m_denoiseFilter = oldDenoiseFilter;
    m_qualityResample = oldQualityResample;
    m_customResample = oldCustomResample;
    m_producerInput = oldProducerInput;
    m_qualityTargetDim = oldQualityTargetDim;
    m_customTargetDim = oldCustomTargetDim;
    m_isProducerDenoiseOn = wasProducerDenoiseOn;
    m_maskInput = oldMaskInput;
    m_qualityMask = oldQualityMask;
    m_customMask = oldCustomMask;
    m_qualityMaskDim = oldQualityMaskDim;
    m_customMaskDim = oldCustomMaskDim;
    return false;
}

bool VolumeStrategy::SetMapperInput()
{
    if (!m_mapper || !GetProducersReady()) return false;
    const VolumeQuality activeQuality = GetVolumeQuality(
        m_quality, m_isFeatureActive);
    const auto activeResample = activeQuality == VolumeQuality::Custom
        ? m_customResample : m_qualityResample;
    if (!activeResample) return false;
    vtkImageData* activeMask = nullptr;
    if (m_lastMask) {
        if (!GetMasksReady(m_customTargetDim)) {
            return false;
        }
        const auto maskResample = activeQuality == VolumeQuality::Custom
            ? m_customMask : m_qualityMask;
        if (!maskResample) return false;
        activeMask = maskResample->GetOutput();
    }
    m_mapper->SetInputConnection(activeResample->GetOutputPort());
    m_mapper->SetMaskInput(activeMask);
    return SetMapperQuality();
}

bool VolumeStrategy::SetMapperQuality()
{
    if (!m_mapper) return false;

    m_mapper->SetAutoAdjustSampleDistances(false);
    m_mapper->SetImageSampleDistance(1.0);
    m_mapper->SetMinimumImageSampleDistance(1.0);
    m_mapper->SetMaximumImageSampleDistance(1.0);
    switch (GetVolumeQuality(m_quality, m_isFeatureActive)) {
    case VolumeQuality::Quality: {
        const double sampleDistance = GetQualityStep(m_qualityResample);
        if (sampleDistance <= 0.0) return false;
        m_mapper->SetSampleDistance(sampleDistance);
        m_mapper->SetUseJittering(true);
        return true;
    }
    case VolumeQuality::Custom:
        if (m_quality.maxDimension < 1
            || !std::isfinite(m_quality.sampleDistance)
            || m_quality.sampleDistance <= 0.0) {
            return false;
        }
        m_mapper->SetSampleDistance(m_quality.sampleDistance);
        m_mapper->SetUseJittering(m_quality.isJitterOn);
        return true;
    }
    return false;
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
    const bool hasTfChanged = ((flags & UpdateFlags::TF) != UpdateFlags::None);
    const bool hasMaterialChanged = ((flags & UpdateFlags::Material) != UpdateFlags::None);
    const bool hasQualityChanged =
        ((flags & UpdateFlags::Quality) != UpdateFlags::None);
    const bool hasDenoiseChanged =
        ((flags & UpdateFlags::Denoise) != UpdateFlags::None);

    const VolumeQualityParams oldQuality = m_quality;
    const bool wasFeatureActive = m_isFeatureActive;
    const bool wasDenoiseOn = m_isDenoiseOn;
    const VolumeQuality oldActiveQuality = GetVolumeQuality(
        m_quality, m_isFeatureActive);
    const bool isQualityModeValid =
        params.volumeQuality.quality == VolumeQuality::Quality
        || params.volumeQuality.quality == VolumeQuality::Custom;
    const bool isQualityValid = isQualityModeValid
        && (params.volumeQuality.quality != VolumeQuality::Custom
        || (params.volumeQuality.maxDimension >= 1
            && params.volumeQuality.maxDimension <= 16384
            && std::isfinite(params.volumeQuality.sampleDistance)
            && params.volumeQuality.sampleDistance > 0.0));
    if (hasQualityChanged && isQualityValid) {
        m_quality = params.volumeQuality;
        m_isFeatureActive = params.isFeatureActive;
    }
    if (hasDenoiseChanged) {
        m_isDenoiseOn = params.isDenoiseOn;
    }

    // producer 的结构键只含输入、目标尺寸与 denoise 配置。Feature 进入/退出只在
    // 已缓存的 Quality/Custom 连接间切换，通用交互状态不触碰体渲染管线。
    const bool hasProducerConfigChanged =
        (hasQualityChanged && isQualityValid)
        || hasDenoiseChanged;
    bool hasBuildStarted = false;
    bool hasProducerRebuilt = false;
    if (hasProducerConfigChanged
        && m_lastInput
        && !GetProducersReady()) {
        hasBuildStarted = true;
        hasProducerRebuilt = BuildProducers();
    }
    bool isPipelineSet = !hasBuildStarted || hasProducerRebuilt;
    if (!hasBuildStarted && GetProducersReady() && m_mapper) {
        const VolumeQuality activeQuality = GetVolumeQuality(
            m_quality, m_isFeatureActive);
        const bool hasActiveQualityChanged =
            oldActiveQuality != activeQuality;
        if (hasActiveQualityChanged || hasTfChanged) {
            isPipelineSet = SetMapperInput();
        }
        else if (hasQualityChanged && isQualityValid) {
            isPipelineSet = SetMapperQuality();
        }
    }
    if (!isPipelineSet) {
        // 配置与已构建缓存必须一起提交；失败时恢复旧真值和旧 mapper 连接。
        m_quality = oldQuality;
        m_isFeatureActive = wasFeatureActive;
        m_isDenoiseOn = wasDenoiseOn;
        (void)SetMapperInput();
    }

    // TF 与 Material 分开处理的原因是：
    // TF 变更通常意味着整条颜色/透明度曲线要重建；
    // 单纯材质变化则尽量只更新光照或全局 opacity，避免重复构造整套传输函数。
    if (((flags & UpdateFlags::TF) != UpdateFlags::None)) {
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

    if ((flags & UpdateFlags::GradientOpacity) != UpdateFlags::None) {
        if (params.gradientOpacity.empty()) {
            prop->SetGradientOpacity(
                static_cast<vtkPiecewiseFunction*>(nullptr));
        }
        else {
            auto gradient =
                vtkSmartPointer<vtkPiecewiseFunction>::New();
            for (const auto& node : params.gradientOpacity) {
                gradient->AddPoint(node.gradient, node.opacity);
            }
            prop->SetGradientOpacity(gradient);
        }
    }

    // 响应变换矩阵
    if (((flags & UpdateFlags::Transform) != UpdateFlags::None)) {
        Set3DPropsTransform(params.modelMatrix);
        AlignCamera(params.modelMatrix);
    }

    if (((flags & UpdateFlags::Visibility) != UpdateFlags::None)) {
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

RenderEffectTarget VolumeStrategy::GetRenderEffectTarget() const
{
    RenderEffectTarget target;
    target.targetKind = RenderTargetKind::Volume;
    target.mapper = m_mapper;
    target.shaderProperty = m_volume
        ? m_volume->GetShaderProperty() : nullptr;
    return target;
}

void VolumeStrategy::SetEffectBinding(RenderEffectBinding* binding)
{
    if (m_mapper) {
        m_mapper->SetEffectBinding(binding);
    }
}
