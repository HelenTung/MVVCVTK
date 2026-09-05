#include "IsoSurfaceStrategy.h"
#include <vtkCubeAxesActor.h>
#include <vtkImageData.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkPolyData.h>
#include <vtkPointData.h>
#include <vtkProperty.h>
#include <vtkType.h>

#include <algorithm>
#include <array>
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

class IsoSurfaceStrategy::Mapper final : public vtkOpenGLPolyDataMapper {
public:
    static Mapper* New();
    vtkTypeMacro(Mapper, vtkOpenGLPolyDataMapper);

    void SetEffectBinding(RenderEffectBinding* binding) { m_binding = binding; }
    void RenderPiece(vtkRenderer* renderer, vtkActor* actor) override
    {
        if (m_binding) {
            (void)m_binding->OnRenderStart(renderer);
        }
        this->Superclass::RenderPiece(renderer, actor);
        if (m_binding) {
            (void)m_binding->OnRenderStop();
        }
    }

private:
    RenderEffectBinding* m_binding = nullptr;
};

vtkStandardNewMacro(IsoSurfaceStrategy::Mapper);

struct IsoSurfaceStrategy::AsyncState final {
    struct Completion final {
        std::uint64_t requestRevision = 0;
        IsoSurfaceKey key;
        IsoSurfaceBuildResult result;
        std::uint64_t cpuPrepareUs = 0;
    };

    std::mutex mutex;
    std::optional<Completion> completion;
};

IsoSurfaceStrategy::IsoSurfaceStrategy()
    : IsoSurfaceStrategy(nullptr)
{
}

IsoSurfaceStrategy::IsoSurfaceStrategy(
    std::shared_ptr<RenderStrategyServices> services)
    : m_lodController(std::make_unique<IsoLodController>())
    , m_asyncState(std::make_shared<AsyncState>())
{
    if (services && services->resources) {
        m_resources = services->resources;
        m_taskChannel = m_resources->CreateTaskChannel(
            RenderProductKind::IsoSurface);
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
            RenderProductKind::IsoSurface);
    }
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_mapper = vtkSmartPointer<Mapper>::New();

    // predicate 直接读取 vertexMC；禁用 VBO Shift/Scale 才能保持 input-model 坐标。
    m_mapper->SetVBOShiftScaleMethod(
        vtkOpenGLPolyDataMapper::DISABLE_SHIFT_SCALE);
    m_mapper->ScalarVisibilityOff();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetInterpolationToFlat();
    m_actor->SetPickable(false);
    m_cubeAxes->SetPickable(false);

    AttachProp(m_actor);
    AttachProp(m_cubeAxes);
}

IsoSurfaceStrategy::~IsoSurfaceStrategy()
{
    if (m_taskChannel) {
        (void)m_taskChannel->Stop();
    }
}

void IsoSurfaceStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    (void)SetIsoInput(std::move(data), nullptr);
}

bool IsoSurfaceStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    return SetIsoInput(
        std::move(data), std::move(validityMask));
}

void IsoSurfaceStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    (void)SetIsoInput(
        m_lastInput, std::move(validityMask));
}

bool IsoSurfaceStrategy::SetIsoInput(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    if (!data || !m_mapper || !m_lodController || !m_taskChannel) {
        return false;
    }
    if (GetInputCurrent(data, validityMask)) return true;
    if (vtkPolyData::SafeDownCast(data)) {
        return SetPolyInput(std::move(data));
    }

    auto* image = vtkImageData::SafeDownCast(data);
    const int* dimensions = image ? image->GetDimensions() : nullptr;
    if (!image || !dimensions
        || m_requestRevision
            == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }

    IsoLodController nextController = *m_lodController;
    IsoLodController::Source source;
    source.dimensions = {
        dimensions[0], dimensions[1], dimensions[2]
    };
    source.nativeBytes = GetImageBytes(image);
    source.maskBytes = GetImageBytes(validityMask);
    source.systemMemoryBytes = GetSystemMemoryBytes();
    if (m_resources) {
        const auto resources = m_resources->GetResourceState();
        std::uint64_t usedBytes = resources.activeBytes;
        for (const auto bytes : {
                resources.runningBytes,
                resources.pendingBytes,
                resources.cacheBytes }) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)()
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
    }
    source.cpuThreadCount = GetCpuThreadCount();
    if (!nextController.SetSource(source)) return false;
    const std::uint64_t topologyRevision = m_resources
        ? m_resources->GetTopologyRevision() : 0;

    IsoSurfaceBuildRequest request;
    request.requestRevision = m_requestRevision + 1;
    request.requestedQuality = nextController.GetQuality();
    request.input = image;
    request.mask = validityMask;
    request.key.inputStamp = m_renderInputStamp.identity == image
        ? m_renderInputStamp : RenderInputStamp{ image, 0 };
    request.key.maskIdentity = validityMask.GetPointer();
    request.key.inputMTime = image->GetMTime();
    request.key.inputScalarMTime = GetScalarTime(image);
    request.key.maskMTime = validityMask
        ? validityMask->GetMTime() : 0;
    request.key.maskScalarMTime = GetScalarTime(validityMask);
    request.key.outputDimensions =
        nextController.GetProfile().outputDimensions;
    request.key.isoValue = m_currentIsoValue == 0.0
        ? 0.0 : m_currentIsoValue;

    const auto oldController = *m_lodController;
    const auto oldInput = m_lastInput;
    const auto oldMask = m_lastMask;
    const auto oldTransition = m_transition;
    const auto oldRequestRevision = m_requestRevision;
    const auto oldInputMTime = m_inputMTime;
    const auto oldMaskMTime = m_maskMTime;
    const auto oldInputScalarMTime = m_inputScalarMTime;
    const auto oldMaskScalarMTime = m_maskScalarMTime;
    *m_lodController = std::move(nextController);
    m_requestRevision = request.requestRevision;
    m_lastInput = std::move(data);
    m_lastMask = std::move(validityMask);
    SetInputTimes(m_lastInput, m_lastMask);
    if (StartProduct(std::move(request))) {
        m_autoTopologyRevision = topologyRevision;
        m_cubeAxes->SetBounds(image->GetBounds());
        return true;
    }

    *m_lodController = oldController;
    m_lastInput = oldInput;
    m_lastMask = oldMask;
    m_transition = oldTransition;
    m_requestRevision = oldRequestRevision;
    m_inputMTime = oldInputMTime;
    m_maskMTime = oldMaskMTime;
    m_inputScalarMTime = oldInputScalarMTime;
    m_maskScalarMTime = oldMaskScalarMTime;
    return false;
}

bool IsoSurfaceStrategy::SetPolyInput(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* poly = vtkPolyData::SafeDownCast(data);
    if (!poly || !m_mapper || !m_lodController) return false;

    // 上游 mesh 没有可降采样的体数据输入；质量意图保留，但不改写几何。
    m_mapper->SetInputData(poly);
    m_mapper->ScalarVisibilityOff();
    m_activeProduct.reset();
    (void)m_lodController->Reset();
    m_lastInput = std::move(data);
    m_lastMask = nullptr;
    SetInputTimes(m_lastInput, nullptr);
    m_inputDimensions = {};
    m_maskDimensions = {};
    m_cubeAxes->SetBounds(poly->GetBounds());

    auto* property = m_actor->GetProperty();
    property->SetColor(0.75, 0.75, 0.75);
    property->SetAmbient(0.2);
    property->SetDiffuse(0.8);
    property->SetSpecular(0.15);
    property->SetSpecularPower(15.0);
    property->SetInterpolationToFlat();
    m_transition = {};
    return true;
}

std::optional<IsoSurfaceBuildRequest>
IsoSurfaceStrategy::BuildRequest(
    const std::uint64_t requestRevision,
    const VolumeQuality requestedQuality,
    const std::array<int, 3>& outputDimensions,
    const double isoValue,
    const bool isPreview) const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image || requestRevision == 0 || !std::isfinite(isoValue)
        || std::any_of(
            outputDimensions.begin(), outputDimensions.end(),
            [](const int value) { return value <= 0; })) {
        return std::nullopt;
    }

    IsoSurfaceBuildRequest request;
    request.requestRevision = requestRevision;
    request.requestedQuality = requestedQuality;
    request.input = image;
    request.mask = m_lastMask;
    request.isPreview = isPreview;
    request.key.inputStamp = m_renderInputStamp;
    request.key.maskIdentity = m_lastMask.GetPointer();
    request.key.inputMTime = image->GetMTime();
    request.key.inputScalarMTime = GetScalarTime(image);
    request.key.maskMTime = m_lastMask
        ? m_lastMask->GetMTime() : 0;
    request.key.maskScalarMTime = GetScalarTime(m_lastMask);
    request.key.outputDimensions = outputDimensions;
    request.key.isoValue = isoValue == 0.0 ? 0.0 : isoValue;
    return request;
}

bool IsoSurfaceStrategy::StartProduct(
    IsoSurfaceBuildRequest request)
{
    if (!m_asyncState || !request.input) return false;
    if (m_resources) {
        auto cached = m_resources->GetIsoSurfaceProduct(request.key);
        if (cached) {
            IsoSurfaceBuildResult result;
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
            m_transition.stats.isPreview = request.isPreview;
            return SetProduct(request.key, result, 0, false);
        }
    }
    if (!m_taskChannel) return false;

    const auto inputEstimate = GetEstimatedImageBytes(
        request.input, request.key.outputDimensions);
    const auto maskEstimate = GetEstimatedImageBytes(
        request.mask, request.key.outputDimensions);
    if (!inputEstimate || !maskEstimate) return false;
    std::uint64_t estimatedBytes = *inputEstimate;
    const std::uint64_t maskBytes = *maskEstimate;
    if (maskBytes
        > (std::numeric_limits<std::uint64_t>::max)()
            - estimatedBytes) {
        return false;
    }
    estimatedBytes += maskBytes;
    const auto asyncState = m_asyncState;
    RenderTaskRequest task;
    task.requestRevision = request.requestRevision;
    task.estimatedBytes = estimatedBytes;
    task.work = [asyncState, request](RenderTaskToken stopToken) {
        const auto prepareStart = std::chrono::steady_clock::now();
        auto result = IsoSurfaceProductBuilder().BuildProduct(
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
        m_transition.message = "The iso-surface task was not admitted.";
        return false;
    }

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
    m_transition.stats.isPreview = request.isPreview;
    m_transition.stats.isCacheHit = false;
    m_transition.message.clear();
    return m_taskChannel->GetState().status == RenderProductStatus::Ready
        ? SetProductCommit() : true;
}

bool IsoSurfaceStrategy::SetProduct(
    const IsoSurfaceKey& key,
    const IsoSurfaceBuildResult& result,
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
        || !result.product || !result.product->surface) {
        m_transition.status =
            result.failureReason == RenderProductFailure::Cancelled
            || result.failureReason == RenderProductFailure::Stopping
            ? RenderProductStatus::Cancelled
            : RenderProductStatus::Failed;
        m_transition.failureReason = result.failureReason ==
            RenderProductFailure::None
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
            "The iso-surface product no longer matches its input.";
        setReadyFailed(
            m_transition.failureReason, m_transition.message);
        return false;
    }
    if (m_taskChannel
        && !(isChannelReady
            ? m_taskChannel->SetActiveBytes(
                result.product->requestRevision,
                result.product->actualBytes,
                result.product.get())
            : m_taskChannel->SetCachedActive(
                activeRevision,
                result.product->actualBytes,
                result.product.get()))) {
        m_transition.status = RenderProductStatus::Failed;
        m_transition.failureReason =
            RenderProductFailure::ResourceRejected;
        m_transition.message =
            "The iso-surface active lease was rejected.";
        setReadyFailed(
            m_transition.failureReason, m_transition.message);
        return false;
    }

    const auto gpuCommitStart = std::chrono::steady_clock::now();
    m_mapper->SetInputData(result.product->surface);
    m_mapper->ScalarVisibilityOff();
    const auto gpuUploadUs = std::max(std::uint64_t{ 1 },
        static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - gpuCommitStart).count()));
    m_activeProduct = result.product;
    m_appliedIsoValue = result.product->isoValue;
    m_inputDimensions = result.product->inputDimensions;
    m_maskDimensions = m_lastMask
        ? result.product->inputDimensions
        : std::array<int, 3>{};
    const bool isPreview = m_transition.stats.isPreview;
    if (!isPreview) {
        m_appliedQuality = m_transition.requestedQuality;
        m_interactionPhase = RenderInteractionPhase::Still;
    }

    m_transition.status = RenderProductStatus::Active;
    m_transition.failureReason = RenderProductFailure::None;
    m_transition.inputStamp = key.inputStamp;
    m_transition.appliedQuality = m_appliedQuality;
    m_transition.stats.activeRevision =
        activeRevision;
    m_transition.stats.cpuPrepareUs = cpuPrepareUs;
    m_transition.stats.gpuReleaseUs = 0;
    m_transition.stats.gpuUploadUs = gpuUploadUs;
    m_transition.stats.candidateBytes = 0;
    m_transition.stats.activeBytes = result.product->actualBytes;
    m_transition.stats.resolvedDimensions =
        result.product->inputDimensions;
    m_transition.stats.isPreview = isPreview;
    m_transition.message.clear();
    if (m_taskChannel) {
        (void)m_taskChannel->CompleteActiveBytes(activeRevision);
    }
    if (m_resources) {
        // mapper/active lease 已提交；cache 插入仅影响后续复用，不得把
        // 可选缓存压力伪装成当前产品提交失败。
        (void)m_resources->SetIsoSurfaceProduct(
            key, result.product);
        m_transition.stats.cacheBytes =
            m_resources->GetResourceState().cacheBytes;
    }
    return true;
}

bool IsoSurfaceStrategy::GetKeyCurrent(
    const IsoSurfaceKey& key) const
{
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    const bool hasCurrentStamp = key.inputStamp == m_renderInputStamp;
    const bool hasInitialStamp = key.inputStamp.identity == image
        && key.inputStamp.version == 0;
    return image
        && (hasCurrentStamp || hasInitialStamp)
        && (!key.inputStamp.identity
            || key.inputStamp.identity == image)
        && key.maskIdentity == m_lastMask.GetPointer()
        && key.inputMTime == image->GetMTime()
        && key.inputScalarMTime == GetScalarTime(image)
        && key.maskMTime
            == (m_lastMask ? m_lastMask->GetMTime() : 0)
        && key.maskScalarMTime == GetScalarTime(m_lastMask);
}

bool IsoSurfaceStrategy::GetInputCurrent(
    vtkDataObject* data,
    vtkImageData* validityMask) const
{
    if (!data
        || m_lastInput != data
        || m_lastMask != validityMask
        || m_inputMTime != data->GetMTime()) {
        return false;
    }
    auto* image = vtkImageData::SafeDownCast(data);
    return (!image
            || m_inputScalarMTime == GetScalarTime(image))
        && (!validityMask
            || (m_maskMTime == validityMask->GetMTime()
                && m_maskScalarMTime
                    == GetScalarTime(validityMask)));
}

void IsoSurfaceStrategy::SetInputTimes(
    vtkDataObject* data,
    vtkImageData* validityMask)
{
    m_inputMTime = data ? data->GetMTime() : 0;
    m_maskMTime = validityMask ? validityMask->GetMTime() : 0;
    m_inputScalarMTime = GetScalarTime(
        vtkImageData::SafeDownCast(data));
    m_maskScalarMTime = GetScalarTime(validityMask);
}

vtkMTimeType IsoSurfaceStrategy::GetScalarTime(
    vtkImageData* image) const
{
    auto* pointData = image ? image->GetPointData() : nullptr;
    auto* scalars = pointData ? pointData->GetScalars() : nullptr;
    return scalars ? scalars->GetMTime() : 0;
}

std::uint64_t IsoSurfaceStrategy::GetImageBytes(
    vtkImageData* image) const
{
    if (!image) return 0;
    const std::uint64_t kibibytes = static_cast<std::uint64_t>(
        image->GetActualMemorySize());
    constexpr std::uint64_t bytesPerKib = 1024ULL;
    return kibibytes
        <= std::numeric_limits<std::uint64_t>::max() / bytesPerKib
        ? kibibytes * bytesPerKib : 0ULL;
}

std::uint64_t IsoSurfaceStrategy::GetSystemMemoryBytes() const
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

unsigned int IsoSurfaceStrategy::GetCpuThreadCount() const noexcept
{
    return std::max(1U, std::thread::hardware_concurrency());
}

void IsoSurfaceStrategy::AttachRenderer(
    vtkSmartPointer<vtkRenderer> renderer)
{
    BaseVisualStrategy::AttachRenderer(renderer);
    m_cubeAxes->SetCamera(renderer->GetActiveCamera());
}

bool IsoSurfaceStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    if (!m_actor || !m_lodController) return false;
    const bool hasQuality =
        (flags & UpdateFlags::Quality) != UpdateFlags::None;
    const bool hasIsoValue =
        (flags & UpdateFlags::IsoValue) != UpdateFlags::None;
    const bool hasRenderRate =
        (flags & UpdateFlags::RenderRate) != UpdateFlags::None;
    if (hasIsoValue && !std::isfinite(params.isoValue)) return false;

    if ((flags & UpdateFlags::Material) != UpdateFlags::None) {
        const auto isUnit = [](const double value) {
            return std::isfinite(value)
                && value >= 0.0 && value <= 1.0;
        };
        if (!isUnit(params.material.ambient)
            || !isUnit(params.material.diffuse)
            || !isUnit(params.material.specular)
            || !isUnit(params.material.opacity)
            || !std::isfinite(params.material.specularPower)
            || params.material.specularPower < 0.0) {
            return false;
        }
    }

    IsoLodController nextController = *m_lodController;
    if (hasQuality
        && !nextController.SetQuality(params.volumeQuality)) {
        return false;
    }
    const bool hasQualityChanged = hasQuality
        && (nextController.GetQuality()
                != m_lodController->GetQuality()
            || params.volumeQuality == VolumeQuality::Auto);
    const std::uint64_t topologyRevision = m_resources
        ? m_resources->GetTopologyRevision() : 0;
    const bool hasAutoRefresh =
        nextController.GetQuality() == VolumeQuality::Auto
        && ((hasQualityChanged
                && params.volumeQuality == VolumeQuality::Auto)
            || topologyRevision != m_autoTopologyRevision);
    if (hasAutoRefresh) {
        auto* currentImage = vtkImageData::SafeDownCast(m_lastInput);
        if (currentImage) {
            const int* dimensions = currentImage->GetDimensions();
            IsoLodController::Source source;
            source.dimensions = {
                dimensions[0], dimensions[1], dimensions[2]
            };
            source.nativeBytes = GetImageBytes(currentImage);
            source.maskBytes = GetImageBytes(m_lastMask);
            source.systemMemoryBytes = GetSystemMemoryBytes();
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
            }
            source.cpuThreadCount = GetCpuThreadCount();
            if (!nextController.SetSource(source)) return false;
        }
    }
    const double nextIsoValue = hasIsoValue
        ? (params.isoValue == 0.0 ? 0.0 : params.isoValue)
        : m_currentIsoValue;
    const bool hasIsoChanged = hasIsoValue
        && nextIsoValue != m_currentIsoValue;

    RenderInteractionPhase nextPhase = m_interactionPhase;
    if (hasRenderRate) {
        nextPhase = params.isInteracting
            ? RenderInteractionPhase::Interactive
            : (m_activeProduct && m_activeProduct->isPreview)
                || m_transition.stats.isPreview
                ? RenderInteractionPhase::Settling
                : RenderInteractionPhase::Still;
    }
    const bool needsFinalProduct = hasRenderRate
        && !params.isInteracting
        && nextPhase == RenderInteractionPhase::Settling;
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    const bool needsProduct = image
        && (hasQualityChanged || hasIsoChanged || needsFinalProduct
            || (hasAutoRefresh
                && nextController.GetProfile().outputDimensions
                    != m_lodController->GetProfile().outputDimensions));
    if (needsProduct) {
        if (m_requestRevision
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        const bool isPreview =
            nextPhase == RenderInteractionPhase::Interactive;
        auto outputDimensions =
            nextController.GetProfile().outputDimensions;
        if (isPreview) {
            const auto highDimensions = nextController.GetProfile(
                VolumeQuality::High).outputDimensions;
            for (std::size_t axis = 0;
                axis < outputDimensions.size(); ++axis) {
                outputDimensions[axis] = std::min(
                    outputDimensions[axis], highDimensions[axis]);
            }
        }
        const std::uint64_t nextRevision = m_requestRevision + 1;
        auto request = BuildRequest(
            nextRevision,
            nextController.GetQuality(),
            outputDimensions,
            nextIsoValue,
            isPreview);
        if (!request) return false;

        const auto oldController = *m_lodController;
        const double oldIsoValue = m_currentIsoValue;
        const auto oldPhase = m_interactionPhase;
        m_requestRevision = nextRevision;
        *m_lodController = nextController;
        m_currentIsoValue = nextIsoValue;
        m_interactionPhase = nextPhase;
        if (!StartProduct(std::move(*request))) {
            *m_lodController = oldController;
            m_currentIsoValue = oldIsoValue;
            m_interactionPhase = oldPhase;
            return false;
        }
        if (hasAutoRefresh) {
            m_autoTopologyRevision = topologyRevision;
        }
    }
    else {
        *m_lodController = std::move(nextController);
        m_currentIsoValue = nextIsoValue;
        m_interactionPhase = nextPhase;
        if (hasAutoRefresh) {
            m_autoTopologyRevision = topologyRevision;
        }
    }

    auto* property = m_actor->GetProperty();
    if ((flags & UpdateFlags::Material) != UpdateFlags::None) {
        property->SetAmbient(params.material.ambient);
        property->SetDiffuse(params.material.diffuse);
        property->SetSpecular(params.material.specular);
        property->SetSpecularPower(params.material.specularPower);
        property->SetOpacity(params.material.opacity);
        if (params.material.isShadeOn) {
            property->SetInterpolationToPhong();
        }
        else {
            property->SetInterpolationToFlat();
        }
    }

    if ((flags & UpdateFlags::Transform) != UpdateFlags::None) {
        Set3DPropsTransform(params.modelMatrix);
    }
    if ((flags & UpdateFlags::Visibility) != UpdateFlags::None) {
        m_cubeAxes->SetVisibility(
            (params.visibilityMask & VisFlags::Ruler) ? 1 : 0);
    }
    return true;
}

bool IsoSurfaceStrategy::SetProductCommit()
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
            completion->cpuPrepareUs,
            true);
}

RenderTransitionState
IsoSurfaceStrategy::GetTransitionState() const
{
    auto state = m_transition;
    if (m_taskChannel) {
        const auto channelState = m_taskChannel->GetState();
        if (channelState.stats.requestRevision
            == state.stats.requestRevision) {
            state.stats.candidateBytes =
                channelState.stats.candidateBytes;
            state.stats.cacheBytes =
                channelState.stats.cacheBytes;
        }
    }
    return state;
}

void IsoSurfaceStrategy::SetFirstRenderDuration(
    const std::uint64_t durationUs) noexcept
{
    if (m_transition.status == RenderProductStatus::Active
        && m_transition.stats.activeRevision != 0
        && m_transition.stats.firstRenderUs == 0) {
        m_transition.stats.firstRenderUs = std::max(
            durationUs, std::uint64_t{ 1 });
    }
}

vtkProp3D* IsoSurfaceStrategy::GetMainProp()
{
    return m_actor;
}

VolumeQuality IsoSurfaceStrategy::GetQuality() const noexcept
{
    return m_lodController
        ? m_lodController->GetQuality() : VolumeQuality::Low;
}

std::array<int, 3> IsoSurfaceStrategy::GetLodDimensions(
    const VolumeQuality quality) const noexcept
{
    return m_lodController
        ? m_lodController->GetProfile(quality).outputDimensions
        : std::array<int, 3>{};
}

std::array<int, 3>
IsoSurfaceStrategy::GetInputDimensions() const noexcept
{
    return m_inputDimensions;
}

std::array<int, 3>
IsoSurfaceStrategy::GetMaskDimensions() const noexcept
{
    return m_maskDimensions;
}

RenderEffectTarget IsoSurfaceStrategy::GetRenderEffectTarget() const
{
    RenderEffectTarget target;
    target.targetKind = RenderTargetKind::PolyData;
    target.mapper = m_mapper;
    target.shaderProperty = m_actor
        ? m_actor->GetShaderProperty() : nullptr;
    return target;
}

void IsoSurfaceStrategy::SetEffectBinding(
    RenderEffectBinding* binding)
{
    if (m_mapper) {
        m_mapper->SetEffectBinding(binding);
    }
}
