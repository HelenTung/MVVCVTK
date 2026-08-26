#include "VolumeStrategy.h"
#include <vtkOpenGLGPUVolumeRayCastMapper.h>
#include <vtkObjectFactory.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkImageResample.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkMatrix4x4.h>
#include <vtkRenderWindow.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <thread>

class VolumeStrategy::Mapper final : public vtkOpenGLGPUVolumeRayCastMapper {
public:
    struct QualityState final {
        bool isAuto = false;
        double image = 1.0;
        double minImage = 1.0;
        double maxImage = 1.0;
        double ray = 1.0;
        bool isJitter = true;
    };

    static Mapper* New();
    vtkTypeMacro(Mapper, vtkOpenGLGPUVolumeRayCastMapper);

    void SetEffectBinding(RenderEffectBinding* binding) { m_binding = binding; }
    bool SetStillQuality(const QualityState& state)
    {
        m_qualityThread = std::this_thread::get_id();
        m_stillQuality = state;
        return SetPreviewQuality(m_isPreviewActive);
    }

protected:
    void GPURender(vtkRenderer* renderer, vtkVolume* volume) override
    {
        auto* renderWindow = renderer ? renderer->GetRenderWindow() : nullptr;
        const bool isPreview =
            renderWindow
            && renderWindow->GetDesiredUpdateRate() >= GetRenderRate(true);
        if (isPreview != m_isPreviewActive) {
            m_isPreviewActive = isPreview;
            (void)SetPreviewQuality(isPreview);
            const std::thread::id renderThread =
                std::this_thread::get_id();
            const bool isThreadValid =
                m_qualityThread == renderThread
                && m_previewThread == renderThread;
            if (!isThreadValid) {
                std::cerr
                    << "[VolumeQualityThread] owner mismatch"
                    << " quality="
                    << std::hash<std::thread::id>{}(
                        m_qualityThread)
                    << " preview="
                    << std::hash<std::thread::id>{}(
                        m_previewThread)
                    << " render="
                    << std::hash<std::thread::id>{}(
                        renderThread)
                    << '\n';
            }
            if (!isThreadValid && isPreview) {
                // VTK mapper setter 不允许跨 owner thread 使用；门失败时立即
                // 恢复静止基线，不让 preview 覆盖继续产品化。
                m_isPreviewActive = false;
                (void)SetPreviewQuality(false);
            }
        }
        if (m_binding) {
            (void)m_binding->OnRenderStart(renderer);
        }
        this->Superclass::GPURender(renderer, volume);
        if (m_binding) {
            (void)m_binding->OnRenderStop();
        }
    }

private:
    bool SetPreviewQuality(bool isPreview)
    {
        m_previewThread = std::this_thread::get_id();
        if (isPreview) {
            SetAutoAdjustSampleDistances(false);
            SetImageSampleDistance(2.0);
            SetMinimumImageSampleDistance(m_stillQuality.minImage);
            SetMaximumImageSampleDistance(m_stillQuality.maxImage);
            SetSampleDistance(std::max(
                m_stillQuality.ray, 2.0 * m_stillQuality.ray));
            SetUseJittering(m_stillQuality.isJitter);
            return true;
        }

        SetAutoAdjustSampleDistances(m_stillQuality.isAuto);
        SetImageSampleDistance(m_stillQuality.image);
        SetMinimumImageSampleDistance(m_stillQuality.minImage);
        SetMaximumImageSampleDistance(m_stillQuality.maxImage);
        SetSampleDistance(m_stillQuality.ray);
        SetUseJittering(m_stillQuality.isJitter);
        return true;
    }

    RenderEffectBinding* m_binding = nullptr;
    QualityState m_stillQuality;
    bool m_isPreviewActive = false;
    std::thread::id m_qualityThread;
    std::thread::id m_previewThread;
};

vtkStandardNewMacro(VolumeStrategy::Mapper);

bool VolumeStrategy::GetOpacityChanged(double opacity) const
{
    return std::abs(m_opacity - opacity) > 1e-6;
}

VolumeStrategy::VolumeStrategy() {
    m_volume = vtkSmartPointer<vtkVolume>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_mapper = vtkSmartPointer<Mapper>::New();
    // 体渲染的材质、gradient opacity 与前向透明度曲线都以 Composite 合成为契约；
    // 显式固定默认值，避免 VTK 默认策略变化时静默切换显示语义。
    m_mapper->SetBlendModeToComposite();
    m_volume->SetPickable(false); // 体渲染不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取
    m_mapper->SetAutoAdjustSampleDistances(false);
    m_mapper->SetImageSampleDistance(1.0);
    m_mapper->SetMinimumImageSampleDistance(1.0);
    m_mapper->SetMaximumImageSampleDistance(1.0);
    m_mapper->SetSampleDistance(1.0);
    m_mapper->SetUseJittering(true);
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
    const vtkMTimeType oldMaskMTime = m_maskMTime;
    const auto oldMaskExtent = m_maskExtent;
    const auto oldMaskSpacing = m_maskSpacing;
    m_lastMask = validityMask;
    if (!BuildMasks(m_customTargetDim)
        || !SetMapperInput()) {
        m_lastMask = oldMask;
        m_maskInput = oldMaskInput;
        m_qualityMask = oldQualityMask;
        m_customMask = oldCustomMask;
        m_qualityMaskDim = oldQualityMaskDim;
        m_customMaskDim = oldCustomMaskDim;
        m_maskMTime = oldMaskMTime;
        m_maskExtent = oldMaskExtent;
        m_maskSpacing = oldMaskSpacing;
        (void)SetMapperInput();
        return;
    }
}

int VolumeStrategy::GetCustomDim() const
{
    return m_quality.quality == VolumeQuality::Custom
        ? m_quality.maxDimension : 766;
}

bool VolumeStrategy::GetInputKey(vtkImageData* image) const
{
    return image
        && m_inputMTime == image->GetMTime()
        && std::equal(
            m_inputExtent.begin(),
            m_inputExtent.end(),
            image->GetExtent())
        && std::equal(
            m_inputSpacing.begin(),
            m_inputSpacing.end(),
            image->GetSpacing());
}

bool VolumeStrategy::GetMaskKey(vtkImageData* image) const
{
    return image
        && m_maskMTime == image->GetMTime()
        && std::equal(
            m_maskExtent.begin(),
            m_maskExtent.end(),
            image->GetExtent())
        && std::equal(
            m_maskSpacing.begin(),
            m_maskSpacing.end(),
            image->GetSpacing());
}

bool VolumeStrategy::GetProducersReady() const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    return m_qualityResample
        && m_customResample
        && m_producerInput == m_lastInput
        && GetInputKey(image)
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
        && GetMaskKey(m_lastMask)
        && m_qualityMaskDim == 766
        && m_customMaskDim == customTargetDim;
}

bool VolumeStrategy::SetInputKey(vtkImageData* image)
{
    if (!image) {
        m_inputMTime = 0;
        m_inputExtent.fill(0);
        m_inputSpacing.fill(0.0);
        return true;
    }
    m_inputMTime = image->GetMTime();
    std::copy_n(
        image->GetExtent(),
        m_inputExtent.size(),
        m_inputExtent.begin());
    std::copy_n(
        image->GetSpacing(),
        m_inputSpacing.size(),
        m_inputSpacing.begin());
    return true;
}

bool VolumeStrategy::SetMaskKey(vtkImageData* image)
{
    if (!image) {
        m_maskMTime = 0;
        m_maskExtent.fill(0);
        m_maskSpacing.fill(0.0);
        return true;
    }
    m_maskMTime = image->GetMTime();
    std::copy_n(
        image->GetExtent(),
        m_maskExtent.size(),
        m_maskExtent.begin());
    std::copy_n(
        image->GetSpacing(),
        m_maskSpacing.size(),
        m_maskSpacing.begin());
    return true;
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
        (void)SetMaskKey(nullptr);
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
    (void)SetMaskKey(m_lastMask);
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
    const vtkMTimeType oldInputMTime = m_inputMTime;
    const vtkMTimeType oldMaskMTime = m_maskMTime;
    const auto oldInputExtent = m_inputExtent;
    const auto oldMaskExtent = m_maskExtent;
    const auto oldInputSpacing = m_inputSpacing;
    const auto oldMaskSpacing = m_maskSpacing;

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
    (void)SetInputKey(image);
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
    m_inputMTime = oldInputMTime;
    m_maskMTime = oldMaskMTime;
    m_inputExtent = oldInputExtent;
    m_maskExtent = oldMaskExtent;
    m_inputSpacing = oldInputSpacing;
    m_maskSpacing = oldMaskSpacing;
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
    // BuildMasks 只准备缓存；所有可能失败的质量校验也必须先完成，
    // 再统一提交不会返回失败的 mapper input/mask setter。
    if (!SetMapperQuality()) return false;
    m_mapper->SetInputConnection(activeResample->GetOutputPort());
    if (activeMask) {
        m_mapper->SetMaskTypeToBinary();
    }
    m_mapper->SetMaskInput(activeMask);
    return true;
}

bool VolumeStrategy::SetMapperQuality()
{
    if (!m_mapper) return false;

    double sampleDistance = 0.0;
    bool isJitterOn = false;
    switch (GetVolumeQuality(m_quality, m_isFeatureActive)) {
    case VolumeQuality::Quality:
        sampleDistance = GetQualityStep(m_qualityResample);
        if (sampleDistance <= 0.0) return false;
        isJitterOn = true;
        break;
    case VolumeQuality::Custom:
        if (m_quality.maxDimension < 1
            || !std::isfinite(m_quality.sampleDistance)
            || m_quality.sampleDistance <= 0.0) {
            return false;
        }
        sampleDistance = m_quality.sampleDistance;
        isJitterOn = m_quality.isJitterOn;
        break;
    default:
        return false;
    }

    // 校验完成后一次更新完整静止基线；Mapper 会按当前 preview 状态
    // 原子式选择静止值或交互覆盖，失败路径不留下半套采样参数。
    return m_mapper->SetStillQuality({
        false,
        1.0,
        1.0,
        1.0,
        sampleDistance,
        isJitterOn
    });
}

void VolumeStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
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
    if (hasQualityChanged && !isQualityValid) {
        // 非法质量请求在触碰任何同帧状态前整体拒绝；TF/material/gradient
        // 也不能绕过质量事务单独提交。
        return;
    }
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
    bool isPipelineSet =
        (!hasQualityChanged || isQualityValid)
        && (!hasBuildStarted || hasProducerRebuilt);
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
        const bool isMapperRestored = SetMapperInput();
        if (!isMapperRestored) {
            std::cerr
                << "[VolumeRollback] mapper input restore failed"
                << '\n';
        }
        // 同一帧中的 TF/material/gradient 必须与 producer/quality 一起提交。
        // 质量事务失败后立即停止，避免形成“旧输入 + 新视觉函数”的半提交帧。
        return;
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
