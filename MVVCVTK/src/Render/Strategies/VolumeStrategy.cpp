#include "VolumeStrategy.h"
#include "Render/Internal/VolumeLodController.h"
#include "Data/ImageProcessor.h"
#include <vtkAbstractMapper.h>
#include <vtkCommand.h>
#include <vtkInformation.h>
#include <vtkInformationObjectBaseVectorKey.h>
#include <vtkNew.h>
#include <vtkOpenGLGPUVolumeRayCastMapper.h>
#include <vtkOpenGLRenderPass.h>
#include <vtkObjectFactory.h>
#include <vtkShaderProgram.h>
#include <vtkVolumeProperty.h>
#include <vtkWeakPointer.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkImageData.h>
#include <vtkImageResample.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkMatrix4x4.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkRenderWindow.h>
#include <vtk_glad.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace {
constexpr std::size_t maxLodCacheEntries = 3;
constexpr long double cacheMemoryFraction = 0.25L;
constexpr long double cacheSourceFraction = 0.65L;
constexpr long double gpuBlockMemoryFraction = 0.5L;
constexpr std::array<unsigned short, 3> singlePartition{ 1, 1, 1 };
constexpr double ratioEpsilon = 1e-9;
// 静止态使用 0.001；所有交互质量档目标均不低于 8 FPS。
// 用速率区间识别交互，不再用固定 15 FPS 门槛排除 XHigh/Ultra。
constexpr double previewRateFloor = 1.0;
}
#include <windows.h>
#endif

class VolumeStrategy::Mapper final : public vtkOpenGLGPUVolumeRayCastMapper {
public:
    class EffectPass final : public vtkOpenGLRenderPass {
    public:
        static EffectPass* New()
        {
            return new EffectPass();
        }
        vtkTypeMacro(EffectPass, vtkOpenGLRenderPass);

        void Render(const vtkRenderState*) override {}

        bool SetShaderParameters(
            vtkShaderProgram* program,
            vtkAbstractMapper* mapper,
            vtkProp*,
            vtkOpenGLVertexArrayObject* = nullptr) override
        {
            if (!program || !mapper) {
                return false;
            }
            // VTK 的 Volume mapper 在 shader 重建分支不会发送
            // UpdateShaderEvent；这里拿到的已是 DoGPURender 本帧实际
            // program，沿用既有事件契约同步可选 effect 的 uniform。
            mapper->InvokeEvent(
                vtkCommand::UpdateShaderEvent, program);
            return true;
        }

    protected:
        EffectPass()
        {
            // 没有其他 render pass 时保持 Volume 默认的单颜色附件。
            SetActiveDrawBuffers(1);
        }
        ~EffectPass() override = default;
    };

    struct QualityState final {
        bool isAuto = false;
        double image = 1.0;
        double minImage = 1.0;
        double maxImage = 1.0;
        double ray = 1.0;
        double previewImage = 2.0;
        double previewRayMultiplier = 1.0;
        bool isJitter = true;
        bool isPreviewJitter = false;
    };

    static Mapper* New();
    vtkTypeMacro(Mapper, vtkOpenGLGPUVolumeRayCastMapper);

    bool SetEffectVolume(vtkVolume* volume)
    {
        if (m_effectVolume.GetPointer() == volume) {
            return true;
        }
        const bool isDetached = DetachEffectPass();
        m_effectVolume = volume;
        return isDetached && AttachEffectPass();
    }
    void SetEffectBinding(RenderEffectBinding* binding)
    {
        if (m_binding == binding) {
            return;
        }
        if (!binding) {
            (void)DetachEffectPass();
        }
        m_binding = binding;
        if (m_binding && !AttachEffectPass()) {
            std::cerr
                << "[VolumeEffect] actual shader program hook attach failed\n";
        }
    }
    bool SetStillQuality(const QualityState& state)
    {
        // Quality/LOD 提交本就受 Strategy owner thread 约束，继续同步更新
        // 静止基线以保留既有 getter/事务契约；只有高频 preview 切换留在
        // GPURender 的 OpenGL owner thread。
        m_stillQuality = state;
        return SetPreviewQuality(m_stillQuality, m_isPreviewActive);
    }

protected:
    Mapper()
        : m_effectPass(vtkSmartPointer<EffectPass>::New())
    {
    }

    ~Mapper() override
    {
        (void)DetachEffectPass();
    }

    void GPURender(vtkRenderer* renderer, vtkVolume* volume) override
    {
        auto* renderWindow = renderer
            ? renderer->GetRenderWindow() : nullptr;
        const bool isPreview = renderWindow
            && renderWindow->GetDesiredUpdateRate()
                >= previewRateFloor;
        if (isPreview != m_isPreviewActive) {
            if (SetPreviewQuality(m_stillQuality, isPreview)) {
                m_isPreviewActive = isPreview;
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
    bool AttachEffectPass()
    {
        if (!m_binding || m_isEffectPassAttached) {
            return true;
        }
        auto* volume = m_effectVolume.GetPointer();
        if (!volume || !m_effectPass) {
            return false;
        }

        vtkInformation* info = volume->GetPropertyKeys();
        if (!info) {
            vtkNew<vtkInformation> nextInfo;
            volume->SetPropertyKeys(nextInfo);
            info = volume->GetPropertyKeys();
            m_hasEffectKeys = true;
        }
        if (!info) {
            return false;
        }

        auto* key = vtkOpenGLRenderPass::RenderPasses();
        const int passCount = info->Length(key);
        for (int index = 0; index < passCount; ++index) {
            if (info->Get(key, index) == m_effectPass.GetPointer()) {
                m_isEffectPassAttached = true;
                return true;
            }
        }

        if (passCount == 0) {
            info->Append(key, m_effectPass);
        }
        else {
            // VTK 以最后一个 pass 的 ActiveDrawBuffers 决定 preview FBO
            // 附件数；只用引用计数成对的 Remove/Append 重建短列表，
            // 把 effect hook 插到末项之前，原有 pass 顺序与最终附件
            // 契约均保持不变。
            std::vector<vtkSmartPointer<vtkOpenGLRenderPass>> passes;
            passes.reserve(passCount);
            for (int index = 0; index < passCount; ++index) {
                passes.emplace_back(
                    static_cast<vtkOpenGLRenderPass*>(
                        info->Get(key, index)));
            }
            for (int index = passCount - 1; index >= 0; --index) {
                key->Remove(info, index);
            }
            for (int index = 0; index < passCount; ++index) {
                if (index == passCount - 1) {
                    info->Append(key, m_effectPass);
                }
                info->Append(key, passes[index]);
            }
        }
        m_isEffectPassAttached = true;
        return true;
    }

    bool DetachEffectPass()
    {
        auto* volume = m_effectVolume.GetPointer();
        auto* info = volume ? volume->GetPropertyKeys() : nullptr;
        auto* key = vtkOpenGLRenderPass::RenderPasses();
        if (m_isEffectPassAttached && info) {
            const int passCount = info->Length(key);
            for (int index = 0; index < passCount; ++index) {
                if (info->Get(key, index)
                    == m_effectPass.GetPointer()) {
                    key->Remove(info, index);
                    break;
                }
            }
        }
        m_isEffectPassAttached = false;
        if (info && info->Has(key)
            && info->Length(key) == 0) {
            key->Remove(info);
        }
        if (m_hasEffectKeys && volume && info
            && info->GetNumberOfKeys() == 0) {
            volume->SetPropertyKeys(nullptr);
            info = nullptr;
        }
        m_hasEffectKeys = false;
        if (!info) {
            return true;
        }
        const int passCount = info->Length(key);
        for (int index = 0; index < passCount; ++index) {
            if (info->Get(key, index) == m_effectPass.GetPointer()) {
                return false;
            }
        }
        return true;
    }

    bool SetPreviewQuality(
        const QualityState& quality,
        const bool isPreview)
    {
        if (isPreview) {
            SetAutoAdjustSampleDistances(false);
            SetImageSampleDistance(quality.previewImage);
            SetMinimumImageSampleDistance(
                quality.previewImage);
            SetMaximumImageSampleDistance(
                quality.previewImage);
            SetSampleDistance(
                quality.ray
                * std::max(
                    quality.previewRayMultiplier,
                    1.0));
            SetUseJittering(quality.isPreviewJitter);
            return true;
        }

        SetAutoAdjustSampleDistances(quality.isAuto);
        SetImageSampleDistance(quality.image);
        SetMinimumImageSampleDistance(quality.minImage);
        SetMaximumImageSampleDistance(quality.maxImage);
        SetSampleDistance(quality.ray);
        SetUseJittering(quality.isJitter);
        return true;
    }

    RenderEffectBinding* m_binding = nullptr;
    vtkWeakPointer<vtkVolume> m_effectVolume;
    vtkSmartPointer<EffectPass> m_effectPass;
    QualityState m_stillQuality;
    bool m_isPreviewActive = false;
    bool m_isEffectPassAttached = false;
    bool m_hasEffectKeys = false;
};

vtkStandardNewMacro(VolumeStrategy::Mapper);

struct VolumeStrategy::LodEntry final {
    vtkSmartPointer<vtkImageData> volume;
    vtkSmartPointer<vtkImageResample> volumeFilter;
    vtkSmartPointer<vtkImageData> mask;
    vtkSmartPointer<vtkImageResample> maskFilter;
    std::uint64_t dataVersion = 0;
    std::uint64_t maskVersion = 0;
    std::array<int, 3> outputDimensions{};
    std::array<double, 3> outputSpacing{};
    std::array<unsigned short, 3> partitions = singlePartition;
    double dimensionRatio = 1.0;
    std::uint64_t estimatedBytes = 0;
    std::uint64_t lastUse = 0;
    bool isDenoiseOn = false;
};

bool VolumeStrategy::GetOpacityChanged(double opacity) const
{
    return std::abs(m_opacity - opacity) > 1e-6;
}

VolumeStrategy::VolumeStrategy() {
    m_lodController = std::make_unique<VolumeLodController>();
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
    (void)m_mapper->SetEffectVolume(m_volume);

    auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
    volumeProperty->ShadeOff();
    volumeProperty->SetInterpolationTypeToLinear();
    volumeProperty->SetScalarOpacityUnitDistance(1.0);
    m_volume->SetProperty(volumeProperty);

    AttachProp(m_volume);
    AttachProp(m_cubeAxes);
}

VolumeStrategy::~VolumeStrategy() = default;

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data)
{
    (void)SetVolumeInput(std::move(data), nullptr);
}

bool VolumeStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    return SetVolumeInput(
        std::move(data), std::move(validityMask));
}

void VolumeStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    (void)SetVolumeInput(
        m_lastInput, std::move(validityMask));
}

bool VolumeStrategy::SetVolumeInput(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    auto* image = vtkImageData::SafeDownCast(data);
    if (!image || !m_lodController) return false;
    const bool hasInputChanged = m_lastInput != data
        || !GetInputKey(image);
    const bool hasMaskChanged = m_lastMask != validityMask
        || (validityMask && !GetMaskKey(validityMask));
    if (!hasInputChanged && !hasMaskChanged
        && GetProducersReady()) {
        return true;
    }

    const auto oldInput = m_lastInput;
    const auto oldMask = m_lastMask;
    const auto oldDenoise = m_denoiseFilter;
    const bool isProducerDenoiseOld = m_isProducerDenoiseOn;
    const vtkMTimeType oldInputMTime = m_inputMTime;
    const vtkMTimeType oldMaskMTime = m_maskMTime;
    const auto oldInputExtent = m_inputExtent;
    const auto oldMaskExtent = m_maskExtent;
    const auto oldInputSpacing = m_inputSpacing;
    const auto oldMaskSpacing = m_maskSpacing;
    const std::uint64_t oldInputEpoch = m_inputEpoch;
    const std::uint64_t oldMaskVersion = m_maskVersion;
    const std::uint64_t oldPlanCount = m_lodPlanCount;
    const VolumeLodController oldController = *m_lodController;

    m_lastInput = std::move(data);
    m_lastMask = std::move(validityMask);
    if (hasInputChanged) ++m_inputEpoch;
    if (hasMaskChanged) ++m_maskVersion;
    bool isPipelineSet = false;
    try {
        const bool isDenoiseSet = !hasInputChanged
            && m_isProducerDenoiseOn == m_isDenoiseOn
            ? true : BuildDenoise();
        const bool isPlanSet = isDenoiseSet
            && BuildLodPlan();
        const auto profile = m_lodController->GetProfile();
        isPipelineSet = isPlanSet
            && SetTargetLod(
                profile.outputDimensions,
                profile.dimensionRatio);
    }
    catch (const std::exception& error) {
        std::cerr
            << "[VolumeLod] input pipeline exception: "
            << error.what() << '\n';
    }
    catch (...) {
        std::cerr << "[VolumeLod] input pipeline unknown exception\n";
    }
    if (!isPipelineSet) {
        m_lastInput = oldInput;
        m_lastMask = oldMask;
        m_denoiseFilter = oldDenoise;
        m_isProducerDenoiseOn = isProducerDenoiseOld;
        m_inputMTime = oldInputMTime;
        m_maskMTime = oldMaskMTime;
        m_inputExtent = oldInputExtent;
        m_maskExtent = oldMaskExtent;
        m_inputSpacing = oldInputSpacing;
        m_maskSpacing = oldMaskSpacing;
        m_inputEpoch = oldInputEpoch;
        m_maskVersion = oldMaskVersion;
        m_lodPlanCount = oldPlanCount;
        *m_lodController = oldController;
        (void)ClearPendingLod();
        const bool isQualityRestored = !m_activeLod
            || SetMapperQuality(*m_activeLod);
        if (!isQualityRestored) {
            std::cerr
                << "[VolumeRollback] input mapper quality restore failed"
                << '\n';
        }
        return false;
    }
    (void)SetInputKey(image);
    (void)SetMaskKey(m_lastMask);
    image->GetCenter(m_dataCenter);
    // 坐标轴始终反映原始输入的物理空间，不跟随 LOD dimensions 缩放。
    m_cubeAxes->SetBounds(image->GetBounds());
    return true;
}

std::array<int, 3> VolumeStrategy::GetSourceDims() const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image) return {};
    const int* dimensions = image->GetDimensions();
    return dimensions
        ? std::array<int, 3>{
            dimensions[0], dimensions[1], dimensions[2] }
        : std::array<int, 3>{};
}

std::uint64_t VolumeStrategy::GetImageBytes(
    vtkImageData* image) const
{
    if (!image) return 0;
    const std::uint64_t kibibytes = static_cast<std::uint64_t>(
        image->GetActualMemorySize());
    constexpr std::uint64_t bytesPerKib = 1024ULL;
    return kibibytes
        <= std::numeric_limits<std::uint64_t>::max()
            / bytesPerKib
        ? kibibytes * bytesPerKib : 0ULL;
}

std::uint64_t VolumeStrategy::GetSourceBytes() const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    return GetImageBytes(image);
}

std::uint64_t VolumeStrategy::GetLodBytes(
    const LodEntry& lod) const
{
    // 原生 volume/mask 都只是输入别名，不计入缓存的增量内存。
    if (!lod.volumeFilter && !lod.maskFilter) return 0;
    return GetLodTextureBytes(lod);
}

std::uint64_t VolumeStrategy::GetLodTextureBytes(
    const LodEntry& lod) const
{
    const auto sourceDimensions = GetSourceDims();
    long double sourceVoxels = 1.0L;
    long double outputVoxels = 1.0L;
    for (std::size_t axis = 0;
        axis < sourceDimensions.size(); ++axis) {
        if (sourceDimensions[axis] <= 0
            || lod.outputDimensions[axis] <= 0) {
            return 0;
        }
        sourceVoxels *= static_cast<long double>(
            sourceDimensions[axis]);
        outputVoxels *= static_cast<long double>(
            lod.outputDimensions[axis]);
    }
    const std::uint64_t volumeBytes = GetSourceBytes();
    const std::uint64_t maskBytes = GetImageBytes(m_lastMask);
    if (volumeBytes == 0
        || maskBytes
            > std::numeric_limits<std::uint64_t>::max()
                - volumeBytes
        || sourceVoxels <= 0.0L) {
        return 0;
    }
    const long double estimatedBytes =
        static_cast<long double>(volumeBytes + maskBytes)
        * outputVoxels / sourceVoxels;
    const long double maximumBytes = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    if (!std::isfinite(estimatedBytes)
        || estimatedBytes <= 0.0L) {
        return 0;
    }
    return estimatedBytes >= maximumBytes
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(std::ceil(estimatedBytes));
}

std::uint64_t VolumeStrategy::GetLodBlockBytes(
    const LodEntry& lod,
    const std::array<unsigned short, 3>& partitions) const
{
    const std::uint64_t textureBytes = GetLodTextureBytes(lod);
    if (textureBytes == 0) return 0;

    long double totalVoxels = 1.0L;
    long double blockVoxels = 1.0L;
    for (std::size_t axis = 0;
        axis < lod.outputDimensions.size(); ++axis) {
        const int dimension = lod.outputDimensions[axis];
        const unsigned short partitionCount = partitions[axis];
        if (dimension <= 0 || partitionCount == 0) return 0;

        const std::uint64_t segmentCount = dimension > 1
            ? static_cast<std::uint64_t>(dimension - 1) : 0ULL;
        const std::uint64_t maxPartitionCount = segmentCount > 0
            ? std::min<std::uint64_t>(
                segmentCount,
                std::numeric_limits<unsigned short>::max())
            : 1ULL;
        if (partitionCount > maxPartitionCount) return 0;

        // VTK 的 block extent 端点均为闭区间；相邻块会共享一个边界体素。
        const std::uint64_t blockDimension = segmentCount > 0
            ? (segmentCount + partitionCount - 1ULL)
                / partitionCount + 1ULL
            : 1ULL;
        totalVoxels *= static_cast<long double>(dimension);
        blockVoxels *= static_cast<long double>(blockDimension);
    }

    const long double blockBytes =
        static_cast<long double>(textureBytes)
        * blockVoxels / totalVoxels;
    const long double maximumBytes = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    if (!std::isfinite(blockBytes) || blockBytes <= 0.0L) return 0;
    return blockBytes >= maximumBytes
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(std::ceil(blockBytes));
}

std::optional<std::array<unsigned short, 3>>
VolumeStrategy::GetLodPartitions(
    const LodEntry& lod,
    const std::uint64_t blockBudget) const
{
    if (blockBudget == 0) return std::nullopt;

    std::array<unsigned short, 3> partitions = singlePartition;
    std::uint64_t blockBytes = GetLodBlockBytes(lod, partitions);
    if (blockBytes == 0) return std::nullopt;

    // 每轮选择真正降低最大 block 的最优轴；跳过由于整数取整产生的
    // 无效 partition 数，最终得到近似均衡而且满足上限的三轴分块。
    while (blockBytes > blockBudget) {
        bool hasCandidate = false;
        auto nextPartitions = partitions;
        std::uint64_t nextBlockBytes = blockBytes;
        for (std::size_t axis = 0;
            axis < lod.outputDimensions.size(); ++axis) {
            const int dimension = lod.outputDimensions[axis];
            if (dimension <= 1) continue;
            const std::uint64_t maxPartitionCount =
                std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(dimension - 1),
                    std::numeric_limits<unsigned short>::max());
            for (std::uint64_t count =
                    static_cast<std::uint64_t>(partitions[axis]) + 1ULL;
                count <= maxPartitionCount; ++count) {
                auto candidate = partitions;
                candidate[axis] = static_cast<unsigned short>(count);
                const std::uint64_t candidateBytes =
                    GetLodBlockBytes(lod, candidate);
                if (candidateBytes == 0 || candidateBytes >= blockBytes) {
                    continue;
                }
                if (!hasCandidate || candidateBytes < nextBlockBytes) {
                    hasCandidate = true;
                    nextPartitions = candidate;
                    nextBlockBytes = candidateBytes;
                }
                break;
            }
        }
        if (!hasCandidate) return std::nullopt;
        partitions = nextPartitions;
        blockBytes = nextBlockBytes;
    }
    return partitions;
}

std::uint64_t VolumeStrategy::GetCacheBudget() const
{
    const std::uint64_t volumeBytes = GetSourceBytes();
    const std::uint64_t maskBytes = GetImageBytes(m_lastMask);
    if (volumeBytes == 0
        || maskBytes
            > std::numeric_limits<std::uint64_t>::max()
                - volumeBytes) {
        return 0;
    }
    const std::uint64_t sourceBytes = volumeBytes + maskBytes;
    const std::uint64_t systemBytes = GetSystemMemoryBytes();
    const long double fallbackBytes =
        static_cast<long double>(sourceBytes) * 2.0L;
    const long double availableBytes = systemBytes > 0
        ? static_cast<long double>(systemBytes) : fallbackBytes;
    // 缓存最多占可用物理内存的四分之一，同时不超过当前源数据的 65%。
    // 活动档即使超过预算也保留，避免为满足预算破坏当前 mapper 输入。
    const long double budgetBytes = std::min(
        availableBytes * cacheMemoryFraction,
        static_cast<long double>(sourceBytes)
            * cacheSourceFraction);
    const long double maximumBytes = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    if (!std::isfinite(budgetBytes) || budgetBytes <= 0.0L) {
        return 0;
    }
    return budgetBytes >= maximumBytes
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(budgetBytes);
}

std::uint64_t VolumeStrategy::GetSystemMemoryBytes() const
{
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return static_cast<std::uint64_t>(status.ullAvailPhys);
    }
#endif
    return 0;
}

std::uint64_t VolumeStrategy::GetGpuMemoryBytes() const
{
    if (!m_mapper || m_mapper->GetMaxMemoryInBytes() <= 0) return 0;
    const long double memoryBytes = static_cast<long double>(
        m_mapper->GetMaxMemoryInBytes())
        * static_cast<long double>(m_mapper->GetMaxMemoryFraction());
    if (memoryBytes <= 0.0L) return 0;
    const long double maximumBytes = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    return memoryBytes >= maximumBytes
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(memoryBytes);
}

std::uint64_t VolumeStrategy::GetGpuBlockBudget(
    const std::optional<std::uint64_t> freeBytes) const
{
    const std::uint64_t configuredBytes = GetGpuMemoryBytes();
    long double availableBytes = static_cast<long double>(configuredBytes);
    if (freeBytes.has_value()) {
        const long double mapperFraction = m_mapper
            ? static_cast<long double>(m_mapper->GetMaxMemoryFraction())
            : 0.0L;
        // MaxMemoryInBytes 在 VTK 无法探测显卡时会退成固定 128 MiB，
        // 不能反过来覆盖驱动刚报告的真实剩余容量。运行期只沿用其
        // fraction 作为保留比例；无厂商扩展时才使用配置值回退。
        availableBytes = static_cast<long double>(*freeBytes)
            * mapperFraction;
    }
    // 驱动值只是释放完成后的近似快照；mapper fraction 之外再保留一半
    // 等量空间应对帧缓冲、shader、驱动迁移和下一块重分配。没有绝对
    // block 常量，实际大小始终随本次剩余显存变化。
    const long double safeBytes =
        availableBytes * gpuBlockMemoryFraction;
    if (!std::isfinite(safeBytes) || safeBytes < 1.0L) return 0;
    return static_cast<std::uint64_t>(safeBytes);
}

std::optional<std::uint64_t> VolumeStrategy::GetGpuFreeBytes() const
{
    auto* renderer = m_renderer.GetPointer();
    auto* renderWindow = renderer
        ? vtkOpenGLRenderWindow::SafeDownCast(
            renderer->GetRenderWindow())
        : nullptr;
    if (!renderWindow || renderWindow->GetNeverRendered() != 0) {
        return std::nullopt;
    }
    renderWindow->MakeCurrent();

    // 清除早先命令留下的 error，随后只判断本次显存查询是否有效。
    for (int index = 0;
        index < 8 && glGetError() != GL_NO_ERROR; ++index) {
    }
    constexpr std::uint64_t bytesPerKib = 1024ULL;
    if (GLAD_GL_NVX_gpu_memory_info != 0) {
        GLint freeKib = 0;
        glGetIntegerv(
            GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX,
            &freeKib);
        if (glGetError() == GL_NO_ERROR && freeKib >= 0) {
            return static_cast<std::uint64_t>(freeKib) * bytesPerKib;
        }
    }

    for (int index = 0;
        index < 8 && glGetError() != GL_NO_ERROR; ++index) {
    }
    if (GLAD_GL_ATI_meminfo != 0) {
        GLint freeKib[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, freeKib);
        if (glGetError() == GL_NO_ERROR && freeKib[0] >= 0) {
            // 单个流式纹理受最大连续主内存块约束；驱动未提供该值时
            // 回退到 texture pool 的总空闲量。
            const GLint usableKib = freeKib[1] > 0
                ? std::min(freeKib[0], freeKib[1]) : freeKib[0];
            return static_cast<std::uint64_t>(usableKib) * bytesPerKib;
        }
    }
    return std::nullopt;
}

unsigned int VolumeStrategy::GetCpuThreadCount() const noexcept
{
    return std::max(1U, std::thread::hardware_concurrency());
}

std::array<int, 3> VolumeStrategy::GetLodDimensions(
    const VolumeQuality quality) const noexcept
{
    return m_lodController
        ? m_lodController->GetProfile(quality).outputDimensions
        : std::array<int, 3>{};
}

std::array<unsigned short, 3>
VolumeStrategy::GetGpuPartitions() const noexcept
{
    return m_activeLod ? m_activeLod->partitions : singlePartition;
}

bool VolumeStrategy::GetQualityValid(
    const VolumeQuality quality) const
{
    switch (quality) {
    case VolumeQuality::Auto:
    case VolumeQuality::Low:
    case VolumeQuality::High:
    case VolumeQuality::XHigh:
    case VolumeQuality::Ultra:
        return true;
    }
    return false;
}

bool VolumeStrategy::GetCpuBudgetValid(const LodEntry& lod) const
{
    const std::uint64_t textureBytes = GetLodTextureBytes(lod);
    if (textureBytes == 0) return false;

    // 原生挡位直接复用已加载输入，不需要为 resample 保留工作副本。
    if (!m_denoiseFilter && lod.outputDimensions == GetSourceDims()) {
        return true;
    }

    // vtkImageResample 至少需要目标输出和工作区；denoise 还会物化
    // 全尺寸输出与工作副本。按增量工作集准入，源输入本身已计入当前占用。
    const std::uint64_t systemBytes = GetSystemMemoryBytes();
    if (systemBytes == 0) return true;
    long double workingBytes = static_cast<long double>(textureBytes) * 2.0L;
    if (m_denoiseFilter) {
        workingBytes += static_cast<long double>(GetSourceBytes()) * 2.0L;
    }
    return std::isfinite(workingBytes)
        && workingBytes <= static_cast<long double>(systemBytes);
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
    const auto* lod = m_activeLod;
    const bool isMapperSet = lod && m_mapper
        && (lod->volumeFilter
            ? m_mapper->GetInputConnection(0, 0)
                == lod->volumeFilter->GetOutputPort()
            : m_mapper->GetInput() == lod->volume.GetPointer())
        && m_mapper->GetMaskInput() == lod->mask.GetPointer();
    return lod && lod->volume && isMapperSet
        && lod->dataVersion == m_inputEpoch
        && lod->maskVersion == m_maskVersion
        && m_lodController
        && lod->outputDimensions
            == m_lodController->GetProfile().outputDimensions
        && lod->isDenoiseOn == m_isDenoiseOn
        && GetInputKey(image)
        && (!m_lastMask
            || (lod->mask && GetMaskKey(m_lastMask)))
        && m_isProducerDenoiseOn == m_isDenoiseOn;
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
    const LodEntry& lod) const
{
    if (!lod.volume || !m_lodController) return 0.0;
    const double minSpacing = std::min(
        { lod.outputSpacing[0],
          lod.outputSpacing[1],
          lod.outputSpacing[2] });
    if (!std::isfinite(minSpacing) || minSpacing <= 0.0) {
        return 0.0;
    }
    return m_lodController->GetProfile().stillRayStepFactor
        * minSpacing;
}

bool VolumeStrategy::BuildLodPlan()
{
    if (!m_lodController) return false;
    VolumeLodController::Source source;
    source.dimensions = GetSourceDims();
    source.nativeBytes = GetSourceBytes();
    source.maskBytes = GetImageBytes(m_lastMask);
    source.systemMemoryBytes = GetSystemMemoryBytes();
    source.gpuMemoryBytes = GetGpuMemoryBytes();
    source.cpuThreadCount = GetCpuThreadCount();
    source.isNativeAliasAllowed = !m_isDenoiseOn;
    if (!m_lodController->SetSource(source)) return false;
    ++m_lodPlanCount;
    return true;
}

bool VolumeStrategy::BuildDenoise()
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image) return false;
    if (!m_isDenoiseOn) {
        m_denoiseFilter = nullptr;
        m_isProducerDenoiseOn = false;
        return true;
    }
    double range[2] = { 0.0, 0.0 };
    image->GetScalarRange(range);
    if (!std::isfinite(range[0]) || !std::isfinite(range[1])
        || range[1] < range[0]) {
        return false;
    }
    auto filter =
        vtkSmartPointer<vtkImageAnisotropicDiffusion3D>::New();
    filter->SetInputData(image);
    filter->SetNumberOfIterations(5);
    filter->SetDiffusionFactor(0.125);
    filter->SetDiffusionThreshold(
        0.02 * std::max(0.0, range[1] - range[0]));
    filter->FacesOn();
    filter->EdgesOff();
    filter->CornersOff();
    m_denoiseFilter = std::move(filter);
    m_isProducerDenoiseOn = m_isDenoiseOn;
    return true;
}

bool VolumeStrategy::BuildPendingLod(
    const std::array<int, 3>& outputDimensions,
    const double dimensionRatio)
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image
        || outputDimensions[0] <= 0
        || outputDimensions[1] <= 0
        || outputDimensions[2] <= 0
        || !std::isfinite(dimensionRatio)
        || dimensionRatio <= 0.0
        || dimensionRatio > 1.0) {
        return false;
    }
    LodEntry budgetLod;
    budgetLod.outputDimensions = outputDimensions;
    if (!GetCpuBudgetValid(budgetLod)) {
        std::cerr
            << "[VolumeLod] resource admission rejected"
            << " target_bytes=" << GetLodTextureBytes(budgetLod)
            << " system_available=" << GetSystemMemoryBytes()
            << '\n';
        return false;
    }
    m_pendingLod.reset();

    const bool hasNativeDimensions =
        outputDimensions == GetSourceDims();
    vtkSmartPointer<vtkImageData> volume;
    vtkSmartPointer<vtkImageResample> volumeFilter;
    if (hasNativeDimensions && !m_denoiseFilter) {
        // 原生挡位直接复用输入；Auto 命中 1.0 时不再重采样或 DeepCopy 整卷。
        volume = image;
    }
    else {
        vtkAlgorithmOutput* inputPort = m_denoiseFilter
            ? m_denoiseFilter->GetOutputPort() : nullptr;
        volumeFilter = ImageProcessor::CreateScaledImage(
            image, outputDimensions, inputPort);
        if (!volumeFilter) return false;
        ++m_resampleBuildCount;
        // 完整 scalar 数据由 mapper 的正常 Render 沿 connection 惰性请求；
        // 目标几何直接由加载期计划计算，不触发 filter Update/UpdateInformation。
        volume = volumeFilter->GetOutput();
    }
    if (!volume || (!volumeFilter
        && volume->GetNumberOfPoints() <= 0)) {
        return false;
    }

    const int* sourceDimensions = image->GetDimensions();
    const double* sourceSpacing = image->GetSpacing();
    std::array<double, 3> outputSpacing{};
    for (std::size_t axis = 0; axis < outputSpacing.size(); ++axis) {
        if (sourceDimensions[axis] <= 0
            || !std::isfinite(sourceSpacing[axis])
            || sourceSpacing[axis] <= 0.0) {
            return false;
        }
        const double axisRatio =
            static_cast<double>(outputDimensions[axis])
            / static_cast<double>(sourceDimensions[axis]);
        outputSpacing[axis] = sourceSpacing[axis] / axisRatio;
        if (!std::isfinite(outputSpacing[axis])
            || outputSpacing[axis] <= 0.0) {
            return false;
        }
    }

    vtkSmartPointer<vtkImageData> mask;
    vtkSmartPointer<vtkImageResample> maskFilter;
    if (m_lastMask) {
        if (hasNativeDimensions) {
            mask = m_lastMask;
        }
        else {
            maskFilter = ImageProcessor::CreateScaledMask(
                m_lastMask, outputDimensions);
            if (!maskFilter) return false;
            ++m_resampleBuildCount;
            maskFilter->Update();
            ++m_resampleUpdateCount;
            mask = maskFilter->GetOutput();
        }
    }
    if (mask) {
        constexpr double geometryEpsilon = 1e-9;
        const auto hasSameValues = [geometryEpsilon](
            const double* left, const double* right) {
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(left[axis] - right[axis])
                    > geometryEpsilon) {
                    return false;
                }
            }
            return true;
        };
        const int* maskDimensions = mask->GetDimensions();
        if (mask->GetNumberOfPoints() <= 0
            || !maskDimensions
            || !std::equal(
                outputDimensions.begin(),
                outputDimensions.end(),
                maskDimensions)
            || !hasSameValues(
                outputSpacing.data(), mask->GetSpacing())
            || !hasSameValues(
                image->GetOrigin(), mask->GetOrigin())) {
            return false;
        }
    }

    auto entry = std::make_unique<LodEntry>();
    entry->volume = std::move(volume);
    entry->volumeFilter = std::move(volumeFilter);
    entry->mask = std::move(mask);
    entry->maskFilter = std::move(maskFilter);
    entry->dataVersion = m_inputEpoch;
    entry->maskVersion = m_maskVersion;
    entry->outputDimensions = outputDimensions;
    entry->outputSpacing = outputSpacing;
    entry->dimensionRatio = dimensionRatio;
    entry->isDenoiseOn = m_isDenoiseOn;
    if (GetQualityStep(*entry) <= 0.0) return false;
    entry->estimatedBytes = GetLodBytes(*entry);
    m_pendingLod = std::move(entry);
    return true;
}

bool VolumeStrategy::ClearPendingLod()
{
    m_pendingLod.reset();
    return true;
}

bool VolumeStrategy::SetTargetLod(
    const std::array<int, 3>& outputDimensions,
    const double dimensionRatio)
{
    if (!m_lodController) return false;
    if (auto* cached = GetCachedLod(
        outputDimensions, dimensionRatio)) {
        (void)ClearPendingLod();
        return SwitchLod(*cached)
            && RemoveUnusedLods();
    }
    return BuildPendingLod(outputDimensions, dimensionRatio)
        && SwitchPendingLod();
}

VolumeStrategy::LodEntry* VolumeStrategy::GetCachedLod(
    const std::array<int, 3>& outputDimensions,
    const double dimensionRatio) const
{
    const auto iterator = std::find_if(
        m_lodCache.begin(),
        m_lodCache.end(),
        [&](const auto& cached) {
            return cached
                && cached->volume
                && cached->dataVersion == m_inputEpoch
                && cached->maskVersion == m_maskVersion
                && cached->outputDimensions == outputDimensions
                && std::abs(
                    cached->dimensionRatio - dimensionRatio)
                    <= ratioEpsilon
                && cached->isDenoiseOn == m_isDenoiseOn
                && (!m_lastMask || cached->mask);
        });
    return iterator != m_lodCache.end()
        ? iterator->get() : nullptr;
}

bool VolumeStrategy::SetMapperInput(const LodEntry& lod)
{
    if (!m_mapper || !lod.volume) return false;
    if (lod.volumeFilter) {
        m_mapper->SetInputConnection(
            lod.volumeFilter->GetOutputPort());
    }
    else {
        m_mapper->SetInputData(lod.volume);
    }
    vtkImageData* mask = lod.mask;
    if (mask) m_mapper->SetMaskTypeToBinary();
    m_mapper->SetMaskInput(mask);
    const bool isVolumeSet = lod.volumeFilter
        ? m_mapper->GetInputConnection(0, 0)
            == lod.volumeFilter->GetOutputPort()
        : m_mapper->GetInput() == lod.volume.GetPointer();
    return isVolumeSet && m_mapper->GetMaskInput() == mask;
}

bool VolumeStrategy::SetGpuPartitions(
    const std::array<unsigned short, 3>& partitions)
{
    if (!m_mapper
        || std::any_of(
            partitions.begin(), partitions.end(),
            [](const unsigned short count) { return count == 0; })) {
        return false;
    }
    m_mapper->SetPartitions(
        partitions[0], partitions[1], partitions[2]);
    return true;
}

bool VolumeStrategy::ClearGpuInput()
{
    auto* renderer = m_renderer.GetPointer();
    auto* renderWindow = renderer ? renderer->GetRenderWindow() : nullptr;
    if (!m_mapper) return false;
    if (!renderWindow || renderWindow->GetNeverRendered() != 0) return true;

    try {
        // 切档与 Render 共用 owner thread。等待旧 draw 完成后释放 mapper
        // 持有的 volume/mask texture，再等待删除提交完成，随后才能采样余量。
        renderWindow->MakeCurrent();
        renderWindow->WaitForCompletion();
        m_mapper->ReleaseGraphicsResources(renderWindow);
        renderWindow->WaitForCompletion();
        ++m_gpuReleaseCount;
        return true;
    }
    catch (const std::exception& error) {
        std::cerr
            << "[VolumeLod] GPU resource release exception: "
            << error.what() << '\n';
    }
    catch (...) {
        std::cerr
            << "[VolumeLod] GPU resource release unknown exception\n";
    }
    return false;
}

bool VolumeStrategy::BuildGpuInput(
    const std::array<unsigned short, 3>& partitions)
{
    auto* renderer = m_renderer.GetPointer();
    auto* renderWindow = renderer ? renderer->GetRenderWindow() : nullptr;
    if (!m_mapper || !m_volume) return false;
    if (partitions != singlePartition) {
        // 多块 PreLoadData 只创建 blocks，尚未执行 RenderSingleInput 的排序；
        // VTK 9.4 随后会访问空的 SortedVolumeBlocks。必须由正常 Render
        // 排序并逐块复用同一个 3D texture，不能走同步预加载入口。
        return true;
    }
    if (!renderWindow || renderWindow->GetNeverRendered() != 0) return true;

    // 单块候选已经满足释放后动态预算，可在发布 active 前同步验证。
    renderWindow->MakeCurrent();
    ++m_gpuPreloadCount;
    return m_mapper->PreLoadData(renderer, m_volume);
}

bool VolumeStrategy::SwitchLod(LodEntry& nextLod)
{
    if (!m_mapper || !nextLod.volume) {
        return false;
    }
    if (&nextLod == m_activeLod) {
        const bool isQualitySet = SetGpuPartitions(nextLod.partitions)
            && SetMapperQuality(nextLod);
        const bool isRatioSet = m_lodController
            && m_lodController->SetActiveRatio(
                nextLod.dimensionRatio);
        if (isQualitySet && isRatioSet) {
            nextLod.lastUse = ++m_lodUseStamp;
            return true;
        }
        return false;
    }

    if (!GetCpuBudgetValid(nextLod)
        || (m_lastMask && !nextLod.mask)) {
        std::cerr
            << "[VolumeLod] resource admission rejected"
            << " target_bytes=" << GetLodTextureBytes(nextLod)
            << " system_available=" << GetSystemMemoryBytes()
            << '\n';
        return false;
    }

    auto* oldLod = m_activeLod;
    const auto restore = [&]() {
        if (!oldLod || !oldLod->volume) {
            (void)SetGpuPartitions(singlePartition);
            m_mapper->SetInputData(
                static_cast<vtkImageData*>(nullptr));
            m_mapper->SetMaskInput(nullptr);
            return false;
        }
        return SetGpuPartitions(oldLod->partitions)
            && SetMapperInput(*oldLod)
            && SetMapperQuality(*oldLod);
    };

    // GPU 最终准入只能发生在旧纹理汰换之后；释放前的 free-memory
    // 快照包含旧档，不能用于决定新档 block 大小。
    if (!ClearGpuInput()) {
        return false;
    }
    auto* renderer = m_renderer.GetPointer();
    auto* renderWindow = renderer ? renderer->GetRenderWindow() : nullptr;
    const bool hasRenderedWindow = renderWindow
        && renderWindow->GetNeverRendered() == 0;
    std::optional<std::uint64_t> freeBytes;
    if (hasRenderedWindow) {
        ++m_gpuQueryCount;
        freeBytes = GetGpuFreeBytes();
        if (!freeBytes.has_value()) {
            std::cerr
                << "[VolumeLod] GPU free-memory extension unavailable; "
                << "using configured fallback\n";
        }
    }
    const std::uint64_t blockBudget = GetGpuBlockBudget(freeBytes);
    const auto nextPartitions = GetLodPartitions(nextLod, blockBudget);
    if (!nextPartitions.has_value()) {
        (void)restore();
        std::cerr
            << "[VolumeLod] GPU block admission rejected"
            << " target_bytes=" << GetLodTextureBytes(nextLod)
            << " free_bytes=" << freeBytes.value_or(0)
            << " block_budget=" << blockBudget
            << '\n';
        return false;
    }

    const std::uint64_t blockBytes =
        GetLodBlockBytes(nextLod, *nextPartitions);
    if (hasRenderedWindow || *nextPartitions != singlePartition) {
        std::cerr
            << "[VolumeLod] GPU block plan"
            << " target_bytes=" << GetLodTextureBytes(nextLod)
            << " free_bytes=" << freeBytes.value_or(0)
            << " block_budget=" << blockBudget
            << " block_bytes=" << blockBytes
            << " partitions="
            << (*nextPartitions)[0] << 'x'
            << (*nextPartitions)[1] << 'x'
            << (*nextPartitions)[2] << '\n';
    }

    if (!SetGpuPartitions(*nextPartitions)
        || !SetMapperQuality(nextLod)
        || !SetMapperInput(nextLod)) {
        (void)restore();
        return false;
    }
    bool isGpuBuilt = false;
    try {
        isGpuBuilt = BuildGpuInput(*nextPartitions);
    }
    catch (const std::exception& error) {
        std::cerr
            << "[VolumeLod] GPU preload exception: "
            << error.what() << '\n';
    }
    catch (...) {
        std::cerr << "[VolumeLod] GPU preload unknown exception\n";
    }
    if (!isGpuBuilt) {
        if (!restore() && oldLod) {
            std::cerr
                << "[VolumeRollback] active mapper restore failed"
                << '\n';
        }
        return false;
    }
    if (m_lodController
        && !m_lodController->SetActiveRatio(
            nextLod.dimensionRatio)) {
        if (!restore() && oldLod) {
            std::cerr
                << "[VolumeRollback] active mapper restore failed"
                << '\n';
        }
        return false;
    }
    nextLod.partitions = *nextPartitions;
    m_activeLod = &nextLod;
    nextLod.lastUse = ++m_lodUseStamp;
    ++m_mapperInputCount;
    return true;
}

bool VolumeStrategy::SwitchPendingLod()
{
    if (!m_pendingLod) {
        return false;
    }
    // 先把 pending 交给 cache 独占，再提交 active 观察指针；即使 vector
    // 分配失败，也不会出现 active 指向即将析构 pending 的窗口。
    m_lodCache.push_back(std::move(m_pendingLod));
    auto* nextLod = m_lodCache.back().get();
    if (!nextLod || !SwitchLod(*nextLod)) {
        m_pendingLod = std::move(m_lodCache.back());
        m_lodCache.pop_back();
        return false;
    }
    return RemoveUnusedLods();
}

bool VolumeStrategy::RemoveUnusedLods()
{
    // 新输入、mask 或 denoise 成功提交后再清理旧世代；失败回滚期间
    // 旧 active 仍由 cache 独占，mapper connection 不会悬空。
    m_lodCache.erase(
        std::remove_if(
            m_lodCache.begin(),
            m_lodCache.end(),
            [&](const auto& cached) {
                return !cached
                    || (cached.get() != m_activeLod
                        && (cached->dataVersion != m_inputEpoch
                            || cached->maskVersion != m_maskVersion
                            || cached->isDenoiseOn
                                != m_isDenoiseOn));
            }),
        m_lodCache.end());

    const std::uint64_t cacheBudget = GetCacheBudget();
    const auto getCacheBytes = [&]() {
        std::uint64_t cacheBytes = 0;
        for (const auto& cached : m_lodCache) {
            if (!cached
                || cached->estimatedBytes
                    > std::numeric_limits<std::uint64_t>::max()
                        - cacheBytes) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            cacheBytes += cached->estimatedBytes;
        }
        return cacheBytes;
    };

    std::uint64_t cacheBytes = getCacheBytes();
    while (m_lodCache.size() > maxLodCacheEntries
        || cacheBytes > cacheBudget) {
        const auto highDimensions = m_lodController
            ? m_lodController->GetProfile(
                VolumeQuality::High).outputDimensions
            : std::array<int, 3>{};
        auto victim = m_lodCache.end();
        for (auto iterator = m_lodCache.begin();
            iterator != m_lodCache.end(); ++iterator) {
            if (!*iterator || iterator->get() == m_activeLod) {
                continue;
            }
            if (victim == m_lodCache.end()) {
                victim = iterator;
                continue;
            }
            const bool isVictimHigh =
                (*victim)->outputDimensions == highDimensions;
            const bool isCandidateHigh =
                (*iterator)->outputDimensions == highDimensions;
            if ((isVictimHigh && !isCandidateHigh)
                || (isVictimHigh == isCandidateHigh
                    && (*iterator)->lastUse < (*victim)->lastUse)) {
                victim = iterator;
            }
        }
        if (victim == m_lodCache.end()) break;
        m_lodCache.erase(victim);
        cacheBytes = getCacheBytes();
    }
    return true;
}

bool VolumeStrategy::SetMapperQuality(const LodEntry& lod)
{
    if (!m_mapper || !m_lodController) return false;
    const double sampleDistance = GetQualityStep(lod);
    if (!std::isfinite(sampleDistance)
        || sampleDistance <= 0.0) return false;

    // 校验完成后一次更新完整静止基线；Mapper 会按当前 preview 状态
    // 原子式选择静止值或交互覆盖，失败路径不留下半套采样参数。
    const auto profile = m_lodController->GetProfile();
    Mapper::QualityState quality;
    quality.isAuto = profile.isAutoSampling;
    quality.image = 1.0;
    quality.minImage = 1.0;
    quality.maxImage = profile.maxImageDistance;
    quality.ray = sampleDistance;
    quality.previewImage = profile.previewImageDistance;
    quality.previewRayMultiplier = profile.previewRayFactor;
    quality.isJitter = profile.isJitterOn;
    quality.isPreviewJitter = profile.isPreviewJitterOn;
    return m_mapper->SetStillQuality(quality);
}

vtkSmartPointer<vtkColorTransferFunction>
VolumeStrategy::BuildColorTransfer(
    const RenderParams& params) const
{
    const auto& function = params.volumeTransferFunction;
    if (function.colorNodes.size() < 2) return nullptr;

    auto color = vtkSmartPointer<vtkColorTransferFunction>::New();
    color->SetColorSpaceToRGB();
    double previousScalar = function.colorNodes.front().scalar;
    for (std::size_t index = 0;
        index < function.colorNodes.size(); ++index) {
        const auto& node = function.colorNodes[index];
        const bool isUnit = std::isfinite(node.r)
            && node.r >= 0.0 && node.r <= 1.0
            && std::isfinite(node.g)
            && node.g >= 0.0 && node.g <= 1.0
            && std::isfinite(node.b)
            && node.b >= 0.0 && node.b <= 1.0;
        if (!std::isfinite(node.scalar)
            || !isUnit
            || (index > 0 && node.scalar <= previousScalar)) {
            return nullptr;
        }
        color->AddRGBPoint(
            node.scalar, node.r, node.g, node.b);
        previousScalar = node.scalar;
    }
    return color;
}

vtkSmartPointer<vtkPiecewiseFunction>
VolumeStrategy::BuildOpacityTransfer(
    const RenderParams& params) const
{
    const auto& function = params.volumeTransferFunction;
    if (function.opacityNodes.size() < 2
        || !std::isfinite(params.material.opacity)
        || params.material.opacity < 0.0
        || params.material.opacity > 1.0) {
        return nullptr;
    }

    auto opacity = vtkSmartPointer<vtkPiecewiseFunction>::New();
    double previousScalar = function.opacityNodes.front().scalar;
    for (std::size_t index = 0;
        index < function.opacityNodes.size(); ++index) {
        const auto& node = function.opacityNodes[index];
        if (!std::isfinite(node.scalar)
            || !std::isfinite(node.opacity)
            || node.opacity < 0.0
            || node.opacity > 1.0
            || (index > 0 && node.scalar <= previousScalar)) {
            return nullptr;
        }
        opacity->AddPoint(
            node.scalar,
            node.opacity * params.material.opacity);
        previousScalar = node.scalar;
    }
    return opacity;
}

void VolumeStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
}

bool VolumeStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    if (!m_volume || !m_volume->GetProperty()) return false;

    auto prop = m_volume->GetProperty();
    const bool hasVolumeTransferChanged =
        (flags & UpdateFlags::VolumeTransfer)
            != UpdateFlags::None;
    const bool hasMaterialChanged =
        (flags & UpdateFlags::Material) != UpdateFlags::None;
    const bool hasQualityChanged =
        (flags & UpdateFlags::Quality) != UpdateFlags::None
        && m_quality != params.volumeQuality;
    const bool hasRenderRateChanged =
        (flags & UpdateFlags::RenderRate) != UpdateFlags::None;
    const bool hasDenoiseChanged =
        (flags & UpdateFlags::Denoise) != UpdateFlags::None
        && m_isDenoiseOn != params.isDenoiseOn;
    const bool hasGradientChanged =
        (flags & UpdateFlags::GradientOpacity)
            != UpdateFlags::None;

    // 1. 在触碰 controller、mapper 和 VTK property 前构建全部候选值。
    // 2. 任一候选非法都整帧拒绝，不暴露新颜色+旧透明度。
    vtkSmartPointer<vtkColorTransferFunction> nextColor;
    if (hasVolumeTransferChanged) {
        nextColor = BuildColorTransfer(params);
        if (!nextColor) return false;
    }

    const bool hasOpacityChanged = hasVolumeTransferChanged
        || (hasMaterialChanged
            && GetOpacityChanged(params.material.opacity));
    vtkSmartPointer<vtkPiecewiseFunction> nextOpacity;
    if (hasOpacityChanged) {
        nextOpacity = BuildOpacityTransfer(params);
        if (!nextOpacity) return false;
    }

    const auto isUnit = [](const double value) {
        return std::isfinite(value)
            && value >= 0.0 && value <= 1.0;
    };
    if (hasMaterialChanged
        && (!isUnit(params.material.ambient)
            || !isUnit(params.material.diffuse)
            || !isUnit(params.material.specular)
            || !isUnit(params.material.opacity)
            || !std::isfinite(params.material.specularPower)
            || params.material.specularPower < 0.0)) {
        return false;
    }

    vtkSmartPointer<vtkPiecewiseFunction> nextGradient;
    if (hasGradientChanged && !params.gradientOpacity.empty()) {
        nextGradient = vtkSmartPointer<vtkPiecewiseFunction>::New();
        double previousGradient = params.gradientOpacity.front().gradient;
        for (std::size_t index = 0;
            index < params.gradientOpacity.size(); ++index) {
            const auto& node = params.gradientOpacity[index];
            if (!std::isfinite(node.gradient)
                || node.gradient < 0.0
                || !isUnit(node.opacity)
                || (index > 0
                    && node.gradient < previousGradient)) {
                return false;
            }
            nextGradient->AddPoint(
                node.gradient, node.opacity);
            previousGradient = node.gradient;
        }
    }

    const VolumeQuality oldQuality = m_quality;
    const bool isDenoiseOld = m_isDenoiseOn;
    const bool isInteractingOld = m_isInteracting;
    const std::uint64_t oldPlanCount = m_lodPlanCount;
    const VolumeLodController oldController = m_lodController
        ? *m_lodController : VolumeLodController{};
    if (hasQualityChanged
        && !GetQualityValid(params.volumeQuality)) {
        return false;
    }
    if (hasQualityChanged) {
        m_quality = params.volumeQuality;
        if (!m_lodController
            || !m_lodController->SetQuality(m_quality)) {
            m_quality = oldQuality;
            return false;
        }
    }
    if (hasDenoiseChanged) {
        m_isDenoiseOn = params.isDenoiseOn;
    }
    if (hasRenderRateChanged) {
        m_isInteracting = params.isInteracting;
    }

    auto nextProfile = m_lodController
        ? m_lodController->GetProfile()
        : VolumeLodController::Profile{};
    const bool hasLodChanged = m_activeLod
        && m_activeLod->outputDimensions
            != nextProfile.outputDimensions;
    const bool hasProducerConfigChanged =
        hasLodChanged || hasDenoiseChanged;
    // preview/still 由 GPURender 在 OpenGL owner thread 根据窗口速率选择；
    // RenderRate 只更新交互状态和材质，不触发 producer 或 mapper setter。
    bool isPipelineSet = true;
    if (isPipelineSet
        && hasProducerConfigChanged && m_lastInput) {
        const auto oldDenoise = m_denoiseFilter;
        const bool isProducerDenoiseOld =
            m_isProducerDenoiseOn;
        try {
            const bool isDenoiseSet = !hasDenoiseChanged
                || BuildDenoise();
            const bool isPlanSet = isDenoiseSet
                && (!hasDenoiseChanged || BuildLodPlan());
            if (isPlanSet && hasDenoiseChanged) {
                nextProfile = m_lodController->GetProfile();
            }
            isPipelineSet = isPlanSet
                && SetTargetLod(
                    nextProfile.outputDimensions,
                    nextProfile.dimensionRatio);
        }
        catch (const std::exception& error) {
            std::cerr
                << "[VolumeLod] pipeline exception: "
                << error.what() << '\n';
            isPipelineSet = false;
        }
        catch (...) {
            std::cerr << "[VolumeLod] pipeline unknown exception\n";
            isPipelineSet = false;
        }
        if (!isPipelineSet) {
            m_denoiseFilter = oldDenoise;
            m_isProducerDenoiseOn = isProducerDenoiseOld;
            (void)ClearPendingLod();
        }
    }
    else if (isPipelineSet
        && hasQualityChanged
        && m_activeLod) {
        isPipelineSet = SetMapperQuality(*m_activeLod);
    }
    if (!isPipelineSet) {
        // 配置与 pending LOD 必须一起提交；失败时 active 与 mapper 始终保持旧值。
        m_quality = oldQuality;
        m_isDenoiseOn = isDenoiseOld;
        m_isInteracting = isInteractingOld;
        m_lodPlanCount = oldPlanCount;
        if (m_lodController) {
            *m_lodController = oldController;
        }
        const bool isQualityRestored = !m_activeLod
            || SetMapperQuality(*m_activeLod);
        if (!isQualityRestored) {
            std::cerr
                << "[VolumeRollback] mapper quality restore failed"
                << '\n';
        }
        // 同一帧中的 TF/material/gradient 必须与 producer/quality 一起提交。
        // 质量事务失败后立即停止，避免形成“旧输入 + 新视觉函数”的半提交帧。
        return false;
    }

    // 候选值和 LOD 已全部成功，从这里开始只执行无失败返回的 VTK 写入。
    // 传输函数更新不触碰 mapper input。
    if (nextColor) prop->SetColor(nextColor);
    if (nextOpacity) {
        prop->SetScalarOpacity(nextOpacity);
        m_opacity = params.material.opacity;
    }

    if (hasMaterialChanged) {
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);
        m_isShadeOn = params.material.isShadeOn;
    }
    if (hasMaterialChanged || hasRenderRateChanged) {
        if (m_isShadeOn && !m_isInteracting) prop->ShadeOn();
        else prop->ShadeOff();
    }

    if (hasGradientChanged) {
        prop->SetGradientOpacity(nextGradient);
    }

    if (hasRenderRateChanged) {
        auto* renderWindow = m_renderer
            ? m_renderer->GetRenderWindow() : nullptr;
        if (renderWindow) {
            constexpr double staticRate = 0.001;
            const auto profile = m_lodController
                ? m_lodController->GetProfile()
                : VolumeLodController::Profile{};
            renderWindow->SetDesiredUpdateRate(
                m_isInteracting
                    ? profile.targetFps : staticRate);
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
    return true;
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
