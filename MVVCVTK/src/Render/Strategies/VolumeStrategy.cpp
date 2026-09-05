#include "VolumeStrategy.h"
#include "Render/Internal/VolumeLodController.h"
#include <vtkAbstractMapper.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
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
#include <vtkMatrix4x4.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>
#include <vtk_glad.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace {
constexpr long double gpuBlockMemoryFraction = 0.5L;
constexpr std::array<unsigned short, 3> singlePartition{ 1, 1, 1 };
}
#include <windows.h>
#endif

namespace {

std::optional<std::uint64_t> GetEstimatedImageBytes(
    vtkImageData* image,
    const std::array<int, 3>& dimensions) noexcept
{
    if (!image) return std::uint64_t{ 0 };
    std::uint64_t voxelCount = 1;
    for (const int dimension : dimensions) {
        if (dimension <= 0
            || static_cast<std::uint64_t>(dimension)
                > (std::numeric_limits<std::uint64_t>::max)()
                    / voxelCount) {
            return std::nullopt;
        }
        voxelCount *= static_cast<std::uint64_t>(dimension);
    }
    const int componentCount = image->GetNumberOfScalarComponents();
    const int scalarSize = image->GetScalarSize();
    if (componentCount <= 0 || scalarSize <= 0) return std::nullopt;
    const auto bytesPerVoxel = static_cast<std::uint64_t>(componentCount)
        * static_cast<std::uint64_t>(scalarSize);
    if (bytesPerVoxel == 0
        || voxelCount
            > (std::numeric_limits<std::uint64_t>::max)()
                / bytesPerVoxel) {
        return std::nullopt;
    }
    return voxelCount * bytesPerVoxel;
}

} // namespace

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
    bool SetPreviewUse(const bool isPreview)
    {
        if (isPreview == m_isPreviewActive) return true;
        if (!SetPreviewQuality(m_stillQuality, isPreview)) return false;
        m_isPreviewActive = isPreview;
        return true;
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
    std::shared_ptr<const VolumeLodProduct> product;
    vtkSmartPointer<vtkImageData> volume;
    vtkSmartPointer<vtkImageData> mask;
    std::array<int, 3> outputDimensions{};
    std::array<double, 3> outputSpacing{};
    std::array<unsigned short, 3> partitions = singlePartition;
    double dimensionRatio = 1.0;
    std::uint64_t estimatedBytes = 0;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
};

struct VolumeStrategy::AsyncState final {
    struct Completion final {
        std::uint64_t requestRevision = 0;
        VolumeLodKey key;
        VolumeLodBuildResult result;
        double dimensionRatio = 1.0;
        std::uint64_t cpuPrepareUs = 0;
    };

    std::mutex mutex;
    std::optional<Completion> completion;
};

bool VolumeStrategy::GetOpacityChanged(double opacity) const
{
    return std::abs(m_opacity - opacity) > 1e-6;
}

VolumeStrategy::VolumeStrategy()
    : VolumeStrategy(nullptr)
{
}

VolumeStrategy::VolumeStrategy(
    std::shared_ptr<RenderStrategyServices> services)
    : m_asyncState(std::make_shared<AsyncState>())
{
    if (services && services->resources) {
        m_resources = services->resources;
        m_taskChannel = m_resources->CreateTaskChannel(
            RenderProductKind::VolumeLod);
    }
    else {
        m_resources = std::make_shared<RenderResourceCoordinator>(
            [](RenderLaneWork work) {
                if (!work.valid()) return false;
                TaskStopSource stopSource;
                work(stopSource.GetToken());
                return true;
            });
        m_taskChannel = m_resources->CreateTaskChannel(
            RenderProductKind::VolumeLod);
    }
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

VolumeStrategy::~VolumeStrategy()
{
    auto* renderer = m_renderer.GetPointer();
    auto* context = renderer ? renderer->GetRenderWindow() : nullptr;
    if (m_resources && context) {
        (void)m_resources->ClearGpuReservation(context, this);
    }
    if (m_taskChannel) {
        (void)m_taskChannel->Stop();
    }
}

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
    if (GetInputCurrent(data, validityMask)) return true;
    if (m_requestRevision
        == (std::numeric_limits<std::uint64_t>::max)()) return false;

    const auto oldInput = m_lastInput;
    const auto oldMask = m_lastMask;
    const vtkMTimeType oldInputMTime = m_inputMTime;
    const vtkMTimeType oldMaskMTime = m_maskMTime;
    const vtkMTimeType oldInputScalarMTime = m_inputScalarMTime;
    const vtkMTimeType oldMaskScalarMTime = m_maskScalarMTime;
    const auto oldInputExtent = m_inputExtent;
    const auto oldMaskExtent = m_maskExtent;
    const auto oldInputSpacing = m_inputSpacing;
    const auto oldMaskSpacing = m_maskSpacing;
    const std::uint64_t oldPlanCount = m_lodPlanCount;
    const std::uint64_t oldTopologyRevision =
        m_autoTopologyRevision;
    const std::uint64_t oldBuildCount = m_resampleBuildCount;
    const std::uint64_t oldUpdateCount = m_resampleUpdateCount;
    const std::uint64_t oldRequestRevision = m_requestRevision;
    const auto oldTransition = m_transition;
    const VolumeLodController oldController = *m_lodController;

    m_lastInput = std::move(data);
    m_lastMask = std::move(validityMask);
    SetInputTimes(m_lastInput, m_lastMask);
    bool isPipelineSet = false;
    try {
        const bool isPlanSet = BuildLodPlan();
        const auto profile = m_lodController->GetProfile();
        const std::uint64_t nextRevision = m_requestRevision + 1;
        auto request = isPlanSet
            ? BuildRequest(
                nextRevision,
                m_quality,
                profile.outputDimensions,
                m_isDenoiseOn)
            : std::nullopt;
        if (request) {
            m_requestRevision = nextRevision;
            m_transition.status = RenderProductStatus::Preparing;
            m_transition.failureReason = RenderProductFailure::None;
            m_transition.inputStamp = request->key.inputStamp;
            m_transition.requestedQuality = m_quality;
            m_transition.appliedQuality = m_appliedQuality;
            m_transition.stats.requestRevision = nextRevision;
            m_transition.stats.resolvedDimensions =
                profile.outputDimensions;
            isPipelineSet = StartProduct(
                std::move(*request), profile.dimensionRatio);
        }
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
        m_inputMTime = oldInputMTime;
        m_maskMTime = oldMaskMTime;
        m_inputScalarMTime = oldInputScalarMTime;
        m_maskScalarMTime = oldMaskScalarMTime;
        m_inputExtent = oldInputExtent;
        m_maskExtent = oldMaskExtent;
        m_inputSpacing = oldInputSpacing;
        m_maskSpacing = oldMaskSpacing;
        m_lodPlanCount = oldPlanCount;
        m_autoTopologyRevision = oldTopologyRevision;
        m_resampleBuildCount = oldBuildCount;
        m_resampleUpdateCount = oldUpdateCount;
        m_requestRevision = oldRequestRevision;
        m_transition = oldTransition;
        *m_lodController = oldController;
        const bool isQualityRestored = !m_activeLod
            || SetMapperQuality(*m_activeLod.get());
        if (!isQualityRestored) {
            std::cerr
                << "[VolumeRollback] input mapper quality restore failed"
                << '\n';
        }
        return false;
    }
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

bool VolumeStrategy::GetInputCurrent(
    vtkDataObject* data,
    vtkImageData* validityMask) const
{
    if (!data || m_lastInput != data || m_lastMask != validityMask
        || m_inputMTime != data->GetMTime()) {
        return false;
    }
    auto* image = vtkImageData::SafeDownCast(data);
    return image
        && m_inputScalarMTime == GetScalarTime(image)
        && (!validityMask
            || (m_maskMTime == validityMask->GetMTime()
                && m_maskScalarMTime
                    == GetScalarTime(validityMask)));
}

bool VolumeStrategy::GetKeyCurrent(const VolumeLodKey& key) const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    const bool hasCurrentStamp = key.inputStamp == m_renderInputStamp;
    return image
        && hasCurrentStamp && key.inputIdentity == image
        && key.maskIdentity == m_lastMask.GetPointer()
        && key.inputMTime == image->GetMTime()
        && key.inputScalarMTime == GetScalarTime(image)
        && key.maskMTime == (m_lastMask ? m_lastMask->GetMTime() : 0)
        && key.maskScalarMTime == GetScalarTime(m_lastMask);
}

void VolumeStrategy::SetInputTimes(
    vtkDataObject* data,
    vtkImageData* validityMask)
{
    auto* image = vtkImageData::SafeDownCast(data);
    m_inputMTime = data ? data->GetMTime() : 0;
    m_maskMTime = validityMask ? validityMask->GetMTime() : 0;
    m_inputScalarMTime = GetScalarTime(image);
    m_maskScalarMTime = GetScalarTime(validityMask);
    if (image) {
        std::copy_n(
            image->GetExtent(), m_inputExtent.size(),
            m_inputExtent.begin());
        std::copy_n(
            image->GetSpacing(), m_inputSpacing.size(),
            m_inputSpacing.begin());
    }
    else {
        m_inputExtent.fill(0);
        m_inputSpacing.fill(0.0);
    }
    if (validityMask) {
        std::copy_n(
            validityMask->GetExtent(), m_maskExtent.size(),
            m_maskExtent.begin());
        std::copy_n(
            validityMask->GetSpacing(), m_maskSpacing.size(),
            m_maskSpacing.begin());
    }
    else {
        m_maskExtent.fill(0);
        m_maskSpacing.fill(0.0);
    }
}

vtkMTimeType VolumeStrategy::GetScalarTime(vtkImageData* image) const
{
    auto* pointData = image ? image->GetPointData() : nullptr;
    auto* scalars = pointData ? pointData->GetScalars() : nullptr;
    return scalars ? scalars->GetMTime() : 0;
}

double VolumeStrategy::GetDenoiseThreshold(vtkImageData* image) const
{
    if (!image) return -1.0;
    double range[2] = { 0.0, 0.0 };
    image->GetScalarRange(range);
    if (!std::isfinite(range[0]) || !std::isfinite(range[1])
        || range[1] < range[0]) {
        return -1.0;
    }
    return 0.02 * std::max(0.0, range[1] - range[0]);
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
    return m_lodController->GetProfile(
        lod.requestedQuality).stillRayStepFactor
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
    if (m_resources) {
        const auto resources = m_resources->GetResourceState();
        std::uint64_t usedBytes = resources.activeBytes;
        const auto addUsed = [&usedBytes](const std::uint64_t bytes) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)()
                    - usedBytes) {
                usedBytes =
                    (std::numeric_limits<std::uint64_t>::max)();
            }
            else {
                usedBytes += bytes;
            }
        };
        addUsed(resources.runningBytes);
        addUsed(resources.pendingBytes);
        addUsed(resources.cacheBytes);
        if (resources.cpuBudgetBytes > usedBytes) {
            source.systemMemoryBytes =
                resources.cpuBudgetBytes - usedBytes;
        }
        auto* renderer = m_renderer.GetPointer();
        auto* context = renderer ? renderer->GetRenderWindow() : nullptr;
        const auto gpu = m_resources->GetGpuResourceState(context);
        if (gpu.budgetBytes > gpu.reservedBytes) {
            source.gpuMemoryBytes =
                gpu.budgetBytes - gpu.reservedBytes;
        }
    }
    source.cpuThreadCount = GetCpuThreadCount();
    source.isNativeAliasAllowed = !m_isDenoiseOn;
    if (!m_lodController->SetSource(source)) return false;
    m_autoTopologyRevision = m_resources
        ? m_resources->GetTopologyRevision() : 0;
    ++m_lodPlanCount;
    return true;
}

std::optional<VolumeLodBuildRequest>
VolumeStrategy::BuildRequest(
    const std::uint64_t requestRevision,
    const VolumeQuality requestedQuality,
    const std::array<int, 3>& outputDimensions,
    const bool isDenoiseOn) const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image || requestRevision == 0
        || std::any_of(
            outputDimensions.begin(), outputDimensions.end(),
            [](const int value) { return value <= 0; })) {
        return std::nullopt;
    }
    const double denoiseThreshold = isDenoiseOn
        ? GetDenoiseThreshold(image) : 0.0;
    if (denoiseThreshold < 0.0) return std::nullopt;

    VolumeLodBuildRequest request;
    request.requestRevision = requestRevision;
    request.requestedQuality = requestedQuality;
    request.input = image;
    request.mask = m_lastMask;
    request.key.inputStamp = m_renderInputStamp;
    request.key.inputIdentity = image;
    request.key.maskIdentity = m_lastMask.GetPointer();
    request.key.inputMTime = image->GetMTime();
    request.key.inputScalarMTime = GetScalarTime(image);
    request.key.maskMTime = m_lastMask
        ? m_lastMask->GetMTime() : 0;
    request.key.maskScalarMTime = GetScalarTime(m_lastMask);
    request.key.outputDimensions = outputDimensions;
    request.key.denoiseThreshold = denoiseThreshold;
    request.key.isDenoiseOn = isDenoiseOn;
    return request;
}

bool VolumeStrategy::StartProduct(
    VolumeLodBuildRequest request,
    const double dimensionRatio)
{
    if (!m_asyncState || !request.input
        || !std::isfinite(dimensionRatio)
        || dimensionRatio <= 0.0 || dimensionRatio > 1.0) {
        return false;
    }
    if (m_resources) {
        auto cached = m_resources->GetVolumeProduct(request.key);
        if (cached) {
            VolumeLodBuildResult result;
            result.product = std::move(cached);
            m_transition.status = RenderProductStatus::Ready;
            m_transition.failureReason = RenderProductFailure::None;
            m_transition.inputStamp = request.key.inputStamp;
            m_transition.requestedQuality = request.requestedQuality;
            m_transition.appliedQuality = m_appliedQuality;
            m_transition.stats.requestRevision = request.requestRevision;
            m_transition.stats.cpuPrepareUs = 0;
            m_transition.stats.gpuReleaseUs = 0;
            m_transition.stats.gpuUploadUs = 0;
            m_transition.stats.firstRenderUs = 0;
            m_transition.stats.resolvedDimensions =
                request.key.outputDimensions;
            m_transition.stats.isCacheHit = true;
            return SetProduct(
                request.key, result, dimensionRatio, 0, false);
        }
    }
    if (!m_taskChannel) return false;

    const auto volumeBytes = GetEstimatedImageBytes(
        request.input, request.key.outputDimensions);
    const auto maskEstimate = GetEstimatedImageBytes(
        request.mask, request.key.outputDimensions);
    if (!volumeBytes || !maskEstimate) return false;
    std::uint64_t estimatedBytes = *volumeBytes;
    const std::uint64_t maskBytes = *maskEstimate;
    if (maskBytes > (std::numeric_limits<std::uint64_t>::max)()
            - estimatedBytes) {
        return false;
    }
    estimatedBytes += maskBytes;
    const auto asyncState = m_asyncState;
    RenderTaskRequest task;
    task.requestRevision = request.requestRevision;
    task.estimatedBytes = estimatedBytes;
    task.work = [asyncState, request, dimensionRatio](
                    RenderTaskToken stopToken) {
        const auto prepareStart = std::chrono::steady_clock::now();
        auto result = VolumeLodProductBuilder().BuildProduct(
            request, stopToken);
        const auto prepareUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - prepareStart).count());
        std::lock_guard<std::mutex> lock(asyncState->mutex);
        const bool isNewer = !asyncState->completion
            || asyncState->completion->requestRevision
                <= request.requestRevision;
        if (isNewer) {
            asyncState->completion = AsyncState::Completion{
                request.requestRevision,
                request.key,
                std::move(result),
                dimensionRatio,
                prepareUs
            };
        }
    };
    const auto admission = m_taskChannel->StartTask(std::move(task));
    if (admission != RenderTaskAdmission::Accepted
        && admission != RenderTaskAdmission::Replaced) {
        m_transition.status = RenderProductStatus::Failed;
        m_transition.failureReason =
            admission == RenderTaskAdmission::ResourceRejected
            ? RenderProductFailure::ResourceRejected
            : admission == RenderTaskAdmission::Stopping
                ? RenderProductFailure::Stopping
                : RenderProductFailure::TaskRejected;
        m_transition.message = "The volume LOD task was not admitted.";
        return false;
    }

    const auto sourceDimensions = GetSourceDims();
    const bool hasNativeDimensions =
        request.key.outputDimensions == sourceDimensions;
    const std::uint64_t scalarPipelineCount =
        (!hasNativeDimensions || request.key.isDenoiseOn) ? 1ULL : 0ULL;
    const std::uint64_t maskPipelineCount =
        (request.mask && !hasNativeDimensions) ? 1ULL : 0ULL;
    m_resampleBuildCount += scalarPipelineCount + maskPipelineCount;
    m_resampleUpdateCount += scalarPipelineCount + maskPipelineCount;
    m_transition.status = RenderProductStatus::Preparing;
    m_transition.failureReason = RenderProductFailure::None;
    m_transition.inputStamp = request.key.inputStamp;
    m_transition.requestedQuality = request.requestedQuality;
    m_transition.appliedQuality = m_appliedQuality;
    m_transition.stats.requestRevision = request.requestRevision;
    m_transition.stats.cpuPrepareUs = 0;
    m_transition.stats.gpuReleaseUs = 0;
    m_transition.stats.gpuUploadUs = 0;
    m_transition.stats.firstRenderUs = 0;
    m_transition.stats.candidateBytes = estimatedBytes;
    m_transition.stats.resolvedDimensions =
        request.key.outputDimensions;
    m_transition.stats.isCacheHit = false;
    m_transition.message.clear();
    return m_taskChannel->GetState().status == RenderProductStatus::Ready
        ? SetProductCommit() : true;
}

std::unique_ptr<VolumeStrategy::LodEntry>
VolumeStrategy::BuildLodEntry(
    std::shared_ptr<const VolumeLodProduct> product,
    const double dimensionRatio,
    const VolumeQuality requestedQuality) const
{
    if (!product || !product->volume
        || !std::isfinite(dimensionRatio)
        || dimensionRatio <= 0.0 || dimensionRatio > 1.0) {
        return nullptr;
    }
    const double* spacing = product->volume->GetSpacing();
    if (!spacing) return nullptr;
    auto entry = std::make_unique<LodEntry>();
    entry->product = std::move(product);
    entry->volume = entry->product->volume;
    entry->mask = entry->product->mask;
    entry->outputDimensions = entry->product->outputDimensions;
    std::copy_n(spacing, entry->outputSpacing.size(),
        entry->outputSpacing.begin());
    entry->dimensionRatio = dimensionRatio;
    entry->estimatedBytes = entry->product->actualBytes;
    entry->requestedQuality = requestedQuality;
    return entry;
}

bool VolumeStrategy::SetProduct(
    const VolumeLodKey& key,
    const VolumeLodBuildResult& result,
    const double dimensionRatio,
    const std::uint64_t cpuPrepareUs,
    const bool isChannelReady)
{
    const std::uint64_t requestRevision =
        m_transition.stats.requestRevision;
    const auto setReadyFailed = [&](
        const RenderProductFailure failure,
        const std::string& message) {
        if (isChannelReady && m_taskChannel && requestRevision != 0) {
            (void)m_taskChannel->SetReadyFailed(
                requestRevision, failure, message);
        }
    };
    if (result.failureReason != RenderProductFailure::None
        || !result.product || !result.product->volume) {
        m_transition.status =
            result.failureReason == RenderProductFailure::Cancelled
            || result.failureReason == RenderProductFailure::Stopping
            ? RenderProductStatus::Cancelled
            : RenderProductStatus::Failed;
        m_transition.failureReason = result.failureReason
            == RenderProductFailure::None
            ? RenderProductFailure::BuildFailed
            : result.failureReason;
        m_transition.message = result.message;
        setReadyFailed(
            m_transition.failureReason, m_transition.message);
        return false;
    }
    const std::uint64_t activeRevision = requestRevision;
    if (!GetKeyCurrent(key) || activeRevision == 0) {
        m_transition.status = RenderProductStatus::Failed;
        m_transition.failureReason = RenderProductFailure::StaleInput;
        m_transition.message =
            "The volume LOD product no longer matches its input.";
        setReadyFailed(
            m_transition.failureReason, m_transition.message);
        return false;
    }
    auto entry = BuildLodEntry(
        result.product,
        dimensionRatio,
        m_transition.requestedQuality);
    if (!entry) {
        m_transition.status = RenderProductStatus::Failed;
        m_transition.failureReason = RenderProductFailure::BuildFailed;
        m_transition.message = "The volume LOD product is invalid.";
        setReadyFailed(
            m_transition.failureReason, m_transition.message);
        return false;
    }
    const std::uint64_t leaseRevision = isChannelReady
        ? result.product->requestRevision : activeRevision;
    const bool isLeaseCommitted = !m_taskChannel
        || (isChannelReady
            ? m_taskChannel->SetActiveBytes(
                leaseRevision,
                result.product->actualBytes,
                result.product.get())
            : m_taskChannel->SetCachedActive(
                leaseRevision,
                result.product->actualBytes,
                result.product.get()));
    if (!isLeaseCommitted) {
        m_transition.status = RenderProductStatus::Failed;
        m_transition.failureReason =
            RenderProductFailure::ResourceRejected;
        m_transition.message =
            "The volume LOD active lease was rejected.";
        setReadyFailed(
            m_transition.failureReason, m_transition.message);
        return false;
    }
    std::uint64_t gpuReleaseUs = 0;
    std::uint64_t gpuUploadUs = 0;
    if (!SwitchLod(
            std::move(entry), gpuReleaseUs, gpuUploadUs)) {
        if (m_taskChannel) {
            (void)m_taskChannel->RestoreActiveBytes(
                leaseRevision);
        }
        m_transition.status = RenderProductStatus::Failed;
        m_transition.failureReason = RenderProductFailure::CommitFailed;
        m_transition.message = "The volume LOD GPU commit failed.";
        m_transition.stats.gpuReleaseUs = gpuReleaseUs;
        m_transition.stats.gpuUploadUs = gpuUploadUs;
        return false;
    }
    if (m_taskChannel) {
        (void)m_taskChannel->CompleteActiveBytes(leaseRevision);
    }
    if (m_resources) {
        // active lease 已完整核算；cache 是可选复用层，插入失败不回滚
        // 已成功的 owner/GPU 提交。
        (void)m_resources->SetVolumeProduct(
            key, result.product);
    }
    m_appliedQuality = m_transition.requestedQuality;
    m_transition.status = RenderProductStatus::Active;
    m_transition.failureReason = RenderProductFailure::None;
    m_transition.inputStamp = key.inputStamp;
    m_transition.appliedQuality = m_appliedQuality;
    m_transition.stats.activeRevision =
        activeRevision;
    m_transition.stats.cpuPrepareUs = cpuPrepareUs;
    m_transition.stats.gpuReleaseUs = gpuReleaseUs;
    m_transition.stats.gpuUploadUs = gpuUploadUs;
    m_transition.stats.candidateBytes = 0;
    m_transition.stats.activeBytes = result.product->actualBytes;
    m_transition.stats.resolvedDimensions =
        result.product->outputDimensions;
    m_transition.stats.partitions = m_activeLod
        ? m_activeLod->partitions : singlePartition;
    if (m_resources) {
        m_transition.stats.cacheBytes =
            m_resources->GetResourceState().cacheBytes;
    }
    m_transition.message.clear();
    return true;
}

bool VolumeStrategy::SetMapperInput(const LodEntry& lod)
{
    if (!m_mapper || !lod.volume) return false;
    m_mapper->SetInputData(lod.volume);
    vtkImageData* mask = lod.mask;
    if (mask) m_mapper->SetMaskTypeToBinary();
    m_mapper->SetMaskInput(mask);
    return m_mapper->GetInput() == lod.volume.GetPointer()
        && m_mapper->GetMaskInput() == mask;
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

bool VolumeStrategy::SwitchLod(
    std::unique_ptr<LodEntry> next,
    std::uint64_t& gpuReleaseUs,
    std::uint64_t& gpuUploadUs)
{
    gpuReleaseUs = 0;
    gpuUploadUs = 0;
    if (!next || !m_mapper || !next->volume
        || (m_lastMask && !next->mask)) {
        return false;
    }
    auto& nextLod = *next;

    auto* oldLod = m_activeLod.get();
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
    const auto releaseStart = std::chrono::steady_clock::now();
    if (!ClearGpuInput()) {
        gpuReleaseUs = std::max(std::uint64_t{ 1 },
            static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - releaseStart).count()));
        return false;
    }
    gpuReleaseUs = std::max(std::uint64_t{ 1 },
        static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - releaseStart).count()));
    const auto uploadStart = std::chrono::steady_clock::now();
    const auto setUploadDuration = [&]() {
        gpuUploadUs = std::max(std::uint64_t{ 1 },
            static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - uploadStart).count()));
    };
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
        setUploadDuration();
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

    const void* contextIdentity = renderWindow;
    RenderGpuResourceState oldGpuState;
    const std::uint64_t oldGpuBytes = oldLod
        ? GetLodBlockBytes(*oldLod, oldLod->partitions) : 0;
    bool hasGpuLease = false;
    const auto restoreGpuLease = [&]() {
        if (!m_resources || !contextIdentity) return true;
        (void)m_resources->ClearGpuReservation(
            contextIdentity, this);
        if (oldGpuState.budgetBytes > 0) {
            (void)m_resources->SetGpuContextBudget(
                contextIdentity, oldGpuState.budgetBytes);
        }
        return oldGpuBytes == 0
            || m_resources->SetGpuReservation(
                contextIdentity, this, oldGpuBytes);
    };
    if (m_resources && contextIdentity) {
        oldGpuState = m_resources->GetGpuResourceState(
            contextIdentity);
        (void)m_resources->ClearGpuReservation(
            contextIdentity, this);
        const std::uint64_t contextBudget = blockBudget;
        hasGpuLease = contextBudget > 0
            && m_resources->SetGpuContextBudget(
                contextIdentity, contextBudget)
            && m_resources->SetGpuReservation(
                contextIdentity, this, blockBytes);
        if (!hasGpuLease) {
            (void)restoreGpuLease();
            (void)restore();
            setUploadDuration();
            return false;
        }
    }

    if (!SetGpuPartitions(*nextPartitions)
        || !SetMapperQuality(nextLod)
        || !SetMapperInput(nextLod)) {
        if (hasGpuLease) (void)restoreGpuLease();
        (void)restore();
        setUploadDuration();
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
        if (hasGpuLease) (void)restoreGpuLease();
        if (!restore() && oldLod) {
            std::cerr
                << "[VolumeRollback] active mapper restore failed"
                << '\n';
        }
        setUploadDuration();
        return false;
    }
    if (m_lodController
        && !m_lodController->SetActiveRatio(
            nextLod.dimensionRatio)) {
        if (hasGpuLease) (void)restoreGpuLease();
        if (!restore() && oldLod) {
            std::cerr
                << "[VolumeRollback] active mapper restore failed"
                << '\n';
        }
        setUploadDuration();
        return false;
    }
    nextLod.partitions = *nextPartitions;
    m_activeLod = std::move(next);
    ++m_mapperInputCount;
    setUploadDuration();
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
    const auto profile = m_lodController->GetProfile(
        lod.requestedQuality);
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
    auto* oldRenderer = m_renderer.GetPointer();
    auto* oldContext = oldRenderer
        ? oldRenderer->GetRenderWindow() : nullptr;
    auto* nextContext = ren ? ren->GetRenderWindow() : nullptr;
    if (m_resources && oldContext && oldContext != nextContext) {
        (void)m_resources->ClearGpuReservation(oldContext, this);
    }
    BaseVisualStrategy::AttachRenderer(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren ? ren->GetActiveCamera() : nullptr);
}

void VolumeStrategy::DetachRenderer(
    vtkSmartPointer<vtkRenderer> renderer)
{
    auto* current = m_renderer.GetPointer();
    auto* context = current ? current->GetRenderWindow() : nullptr;
    if (m_resources && context) {
        (void)m_resources->ClearGpuReservation(context, this);
    }
    BaseVisualStrategy::DetachRenderer(renderer);
    if (current == renderer.GetPointer()) {
        m_renderer = nullptr;
    }
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
    const bool hasQualityRequest =
        (flags & UpdateFlags::Quality) != UpdateFlags::None;
    const bool hasQualityChanged = hasQualityRequest
        && (m_quality != params.volumeQuality
            || params.volumeQuality == VolumeQuality::Auto);
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
    const VolumeQuality oldAppliedQuality = m_appliedQuality;
    const bool isDenoiseOld = m_isDenoiseOn;
    const bool isInteractingOld = m_isInteracting;
    const auto oldPhase = m_interactionPhase;
    const std::uint64_t oldPlanCount = m_lodPlanCount;
    const std::uint64_t oldRequestRevision = m_requestRevision;
    const VolumeQuality oldActiveQuality = m_activeLod
        ? m_activeLod->requestedQuality : m_appliedQuality;
    const VolumeLodController oldController = m_lodController
        ? *m_lodController : VolumeLodController{};
    const std::uint64_t topologyRevision = m_resources
        ? m_resources->GetTopologyRevision() : 0;
    if (hasQualityChanged
        && !GetQualityValid(params.volumeQuality)) {
        return false;
    }
    if (!m_lodController) return false;
    VolumeLodController nextController = *m_lodController;
    const VolumeQuality nextQuality = hasQualityChanged
        ? params.volumeQuality : m_quality;
    const bool nextDenoise = hasDenoiseChanged
        ? params.isDenoiseOn : m_isDenoiseOn;
    const bool nextInteracting = hasRenderRateChanged
        ? params.isInteracting : m_isInteracting;
    if (hasQualityChanged
        && !nextController.SetQuality(nextQuality)) {
        return false;
    }
    const bool hasAutoRefresh = nextQuality == VolumeQuality::Auto
        && ((hasQualityRequest
                && params.volumeQuality == VolumeQuality::Auto)
            || topologyRevision != m_autoTopologyRevision);
    if ((hasDenoiseChanged || hasAutoRefresh) && m_lastInput) {
        VolumeLodController::Source source;
        source.dimensions = GetSourceDims();
        source.nativeBytes = GetSourceBytes();
        source.maskBytes = GetImageBytes(m_lastMask);
        source.systemMemoryBytes = GetSystemMemoryBytes();
        source.gpuMemoryBytes = GetGpuMemoryBytes();
        if (m_resources) {
            const auto resources = m_resources->GetResourceState();
            std::uint64_t usedBytes = resources.activeBytes;
            for (const auto bytes : {
                    resources.runningBytes,
                    resources.pendingBytes,
                    resources.cacheBytes }) {
                if (bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - usedBytes) {
                    usedBytes =
                        (std::numeric_limits<std::uint64_t>::max)();
                    break;
                }
                usedBytes += bytes;
            }
            if (resources.cpuBudgetBytes > usedBytes) {
                source.systemMemoryBytes =
                    resources.cpuBudgetBytes - usedBytes;
            }
            auto* renderer = m_renderer.GetPointer();
            auto* context = renderer
                ? renderer->GetRenderWindow() : nullptr;
            const auto gpu = m_resources->GetGpuResourceState(context);
            if (gpu.budgetBytes > gpu.reservedBytes) {
                source.gpuMemoryBytes =
                    gpu.budgetBytes - gpu.reservedBytes;
            }
        }
        source.cpuThreadCount = GetCpuThreadCount();
        source.isNativeAliasAllowed = !nextDenoise;
        if (!nextController.SetSource(source)) return false;
    }

    const auto nextProfile = nextController.GetProfile();
    const bool hasLodChanged = m_activeLod
        && m_activeLod->outputDimensions
            != nextProfile.outputDimensions;
    const bool hasProducerConfigChanged =
        hasLodChanged || hasDenoiseChanged;
    bool isPipelineSet = true;
    m_quality = nextQuality;
    m_isDenoiseOn = nextDenoise;
    m_isInteracting = nextInteracting;
    m_interactionPhase = nextInteracting
        ? RenderInteractionPhase::Interactive
        : RenderInteractionPhase::Still;
    *m_lodController = nextController;
    if ((hasDenoiseChanged || hasAutoRefresh) && m_lastInput) {
        ++m_lodPlanCount;
    }

    if (hasProducerConfigChanged && m_lastInput) {
        try {
            if (m_requestRevision
                == (std::numeric_limits<std::uint64_t>::max)()) {
                isPipelineSet = false;
            }
            else {
                const std::uint64_t nextRevision =
                    m_requestRevision + 1;
                auto request = BuildRequest(
                    nextRevision,
                    nextQuality,
                    nextProfile.outputDimensions,
                    nextDenoise);
                if (!request) {
                    isPipelineSet = false;
                }
                else {
                    m_requestRevision = nextRevision;
                    isPipelineSet = StartProduct(
                        std::move(*request),
                        nextProfile.dimensionRatio);
                }
            }
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
    }
    else if (hasQualityChanged
        && m_activeLod) {
        m_activeLod->requestedQuality = nextQuality;
        isPipelineSet = SetMapperQuality(*m_activeLod);
        if (isPipelineSet) {
            m_appliedQuality = nextQuality;
            m_transition.appliedQuality = nextQuality;
        }
    }
    if (isPipelineSet && hasRenderRateChanged) {
        isPipelineSet = m_mapper
            && m_mapper->SetPreviewUse(nextInteracting);
        m_transition.stats.isPreview = nextInteracting;
    }
    if (!isPipelineSet) {
        m_quality = oldQuality;
        m_appliedQuality = oldAppliedQuality;
        m_isDenoiseOn = isDenoiseOld;
        m_isInteracting = isInteractingOld;
        m_interactionPhase = oldPhase;
        m_lodPlanCount = oldPlanCount;
        m_requestRevision = oldRequestRevision;
        if (m_lodController) {
            *m_lodController = oldController;
        }
        if (m_activeLod) {
            m_activeLod->requestedQuality = oldActiveQuality;
        }
        const bool isQualityRestored = !m_activeLod
            || SetMapperQuality(*m_activeLod);
        if (m_mapper) {
            (void)m_mapper->SetPreviewUse(isInteractingOld);
        }
        if (!isQualityRestored) {
            std::cerr
                << "[VolumeRollback] mapper quality restore failed"
                << '\n';
        }
        return false;
    }
    if (hasAutoRefresh) {
        m_autoTopologyRevision = topologyRevision;
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

bool VolumeStrategy::SetProductCommit()
{
    if (!m_taskChannel || !m_asyncState) return true;
    const auto channelState = m_taskChannel->GetState();
    if (channelState.stats.requestRevision
            == m_transition.stats.requestRevision
        && (channelState.status == RenderProductStatus::Failed
            || channelState.status == RenderProductStatus::Cancelled)) {
        m_transition.status = channelState.status;
        m_transition.failureReason = channelState.failureReason;
        m_transition.message = channelState.message;
        return channelState.status != RenderProductStatus::Failed;
    }
    if (channelState.status != RenderProductStatus::Ready
        || channelState.stats.requestRevision
            != m_transition.stats.requestRevision) {
        return true;
    }

    std::optional<AsyncState::Completion> completion;
    {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        if (m_asyncState->completion
            && m_asyncState->completion->requestRevision
                == m_transition.stats.requestRevision) {
            completion = std::move(m_asyncState->completion);
            m_asyncState->completion.reset();
        }
        else if (m_asyncState->completion
            && m_asyncState->completion->requestRevision
                < m_transition.stats.requestRevision) {
            m_asyncState->completion.reset();
        }
    }
    return !completion
        || SetProduct(
            completion->key,
            completion->result,
            completion->dimensionRatio,
            completion->cpuPrepareUs,
            true);
}

RenderTransitionState VolumeStrategy::GetTransitionState() const
{
    auto state = m_transition;
    if (m_taskChannel) {
        const auto channelState = m_taskChannel->GetState();
        if (channelState.stats.requestRevision
            == state.stats.requestRevision) {
            state.stats.candidateBytes =
                channelState.stats.candidateBytes;
            state.stats.cacheBytes = channelState.stats.cacheBytes;
        }
    }
    return state;
}

void VolumeStrategy::SetFirstRenderDuration(
    const std::uint64_t durationUs) noexcept
{
    if (m_transition.status == RenderProductStatus::Active
        && m_transition.stats.activeRevision != 0
        && m_transition.stats.firstRenderUs == 0) {
        m_transition.stats.firstRenderUs = std::max(
            durationUs, std::uint64_t{ 1 });
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
