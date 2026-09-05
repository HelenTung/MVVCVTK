#include "QtHostMethodCases.h"
#include "RenderProductTestSupport.h"

#include "Render/Internal/RenderResourceCoordinator.h"
#include "Render/Internal/IsoSurfaceProductBuilder.h"
#include "Render/Internal/VolumeLodProductBuilder.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"
#include "Render/Strategies/VolumeStrategy.h"
#include "Render/Strategies/ColoredPlanesStrategy.h"
#include "Render/Strategies/CompositeStrategy.h"
#include "Render/Strategies/SliceStrategy.h"
#include "App/Services/AppServiceFactory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <vtkActor.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPlaneSource.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>

namespace {

int GetTransitionValueFailCount()
{
    int failureCount = 0;

    RenderTransitionState state;
    state.status = RenderProductStatus::Preparing;
    state.failureReason = RenderProductFailure::None;
    state.requestedQuality = VolumeQuality::Ultra;
    state.appliedQuality = VolumeQuality::High;
    state.stats.requestRevision = 7;
    state.stats.activeRevision = 6;
    state.stats.cpuPrepareUs = 101;
    state.stats.gpuReleaseUs = 11;
    state.stats.gpuUploadUs = 13;
    state.stats.firstRenderUs = 17;
    state.stats.candidateBytes = 4096;
    state.stats.activeBytes = 2048;
    state.stats.cacheBytes = 1024;
    state.stats.resolvedDimensions = { 30, 20, 10 };
    state.stats.partitions = { 2, 1, 1 };
    state.stats.isCacheHit = true;
    state.stats.isPreview = false;

    failureCount += GetCaseResult(
        state.requestedQuality == VolumeQuality::Ultra
            && state.appliedQuality == VolumeQuality::High
            && state.stats.requestRevision
                != state.stats.activeRevision,
        "Render transition keeps requested and applied quality distinct")
        ? 0 : 1;
    failureCount += GetCaseResult(
        state.stats.cpuPrepareUs == 101
            && state.stats.gpuReleaseUs == 11
            && state.stats.gpuUploadUs == 13
            && state.stats.firstRenderUs == 17
            && state.stats.candidateBytes == 4096
            && state.stats.activeBytes == 2048
            && state.stats.cacheBytes == 1024
            && state.stats.resolvedDimensions
                == std::array<int, 3>{ 30, 20, 10 }
            && state.stats.partitions
                == std::array<unsigned short, 3>{ 2, 1, 1 },
        "Render stats expose revision, dimensions, bytes and phase durations")
        ? 0 : 1;
    return failureCount;
}

int GetDefaultStrategyFailCount()
{
    int failureCount = 0;
    RenderProductStrategyProbe probe;
    VolumeStrategy volume;
    IsoSurfaceStrategy iso;

    const auto probeState = probe.GetTransitionState();
    const auto volumeState = volume.GetTransitionState();
    const auto isoState = iso.GetTransitionState();
    const bool isDefaultCommitted = probe.SetProductCommit();
    const auto committedState = probe.GetTransitionState();
    failureCount += GetCaseResult(
        isDefaultCommitted
            && probeState.status == RenderProductStatus::Idle
            && committedState.status == RenderProductStatus::Idle
            && volumeState.status == RenderProductStatus::Idle
            && isoState.status == RenderProductStatus::Idle,
        "Current synchronous Volume and Iso paths expose the same transition schema")
        ? 0 : 1;
    return failureCount;
}

int GetRenderLaneFailCount()
{
    int failureCount = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator resources(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    auto first = resources.CreateTaskChannel(
        RenderProductKind::VolumeLod);
    auto second = resources.CreateTaskChannel(
        RenderProductKind::IsoSurface);
    std::vector<std::uint64_t> calls;

    const auto getRequest = [&calls](const std::uint64_t revision) {
        RenderTaskRequest request;
        request.requestRevision = revision;
        request.estimatedBytes = revision * 100;
        request.work = [&calls, revision](RenderTaskToken token) {
            if (!token.GetIsStopped()) {
                calls.push_back(revision);
                (void)token.SetActualBytes(revision * 110);
            }
        };
        return request;
    };

    const auto firstAdmission = first->StartTask(getRequest(1));
    const auto secondAdmission = first->StartTask(getRequest(2));
    const auto thirdAdmission = first->StartTask(getRequest(3));
    failureCount += GetCaseResult(
        firstAdmission == RenderTaskAdmission::Accepted
            && secondAdmission == RenderTaskAdmission::Accepted
            && thirdAdmission == RenderTaskAdmission::Replaced
            && lane->GetPendingCount() == 1,
        "One channel keeps one running task and one latest pending request")
        ? 0 : 1;

    const bool isOldSent = lane->SendOne();
    const bool isLatestStarted = resources.SendTasks();
    const bool isLatestSent = lane->SendOne();
    const auto firstState = first->GetState();
    failureCount += GetCaseResult(
        isOldSent && isLatestStarted && isLatestSent
            && calls == std::vector<std::uint64_t>{ 3 }
            && firstState.status == RenderProductStatus::Ready
            && firstState.stats.requestRevision == 3,
        "Replacing pending work cancels the old running revision")
        ? 0 : 1;

    bool hasStateInsideWork = false;
    auto reentrant = getRequest(4);
    reentrant.work = [first, &hasStateInsideWork](RenderTaskToken token) {
        hasStateInsideWork = !token.GetIsStopped()
            && first->GetState().status
                == RenderProductStatus::Preparing;
    };
    const bool isReentrantAccepted =
        first->StartTask(std::move(reentrant))
            == RenderTaskAdmission::Accepted;
    const bool isSecondAccepted =
        second->StartTask(getRequest(1))
            == RenderTaskAdmission::Accepted;
    const bool hasOneGlobalWork = lane->GetPendingCount() == 1;
    const bool isFirstReentrantSent = lane->SendOne();
    const bool isSecondStarted = resources.SendTasks();
    const bool isSecondSent = lane->SendOne();
    failureCount += GetCaseResult(
        isReentrantAccepted && isSecondAccepted
            && hasOneGlobalWork && isFirstReentrantSent
            && isSecondStarted && isSecondSent
            && hasStateInsideWork
            && lane->GetStartCount() == 4,
        "Two channels share one global running slot without lock callbacks")
        ? 0 : 1;

    failureCount += GetCaseResult(
        first->SetActiveBytes(4, 440)
            && first->GetState().status
                == RenderProductStatus::Active
            && first->GetState().stats.activeBytes == 440,
        "Only a ready revision can become the active product")
        ? 0 : 1;

    const bool isRollbackAccepted =
        first->StartTask(getRequest(5))
            == RenderTaskAdmission::Accepted
        && lane->SendOne()
        && first->SetActiveBytes(5, 550)
        && first->RestoreActiveBytes(5);
    const auto rollbackState = first->GetState();
    failureCount += GetCaseResult(
        isRollbackAccepted
            && rollbackState.status == RenderProductStatus::Active
            && rollbackState.stats.activeRevision == 4
            && rollbackState.stats.activeBytes == 440
            && resources.GetResourceState().activeBytes == 440,
        "A failed GPU commit restores the previous active CPU lease")
        ? 0 : 1;
    return failureCount;
}

int GetRenderStopFailCount()
{
    int failureCount = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator resources(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    auto channel = resources.CreateTaskChannel(
        RenderProductKind::VolumeLod);
    RenderTaskRequest request;
    request.requestRevision = 1;
    request.estimatedBytes = 1;
    request.work = [](RenderTaskToken) {};
    const bool isAccepted = channel->StartTask(std::move(request))
        == RenderTaskAdmission::Accepted;
    const bool isStopStarted = resources.StartStop();
    const bool isRejected = channel->StartTask({})
        == RenderTaskAdmission::Stopping;
    const bool isDeadlineFailed = !resources.Stop(
        std::chrono::steady_clock::now());
    const bool isCancelledSent = lane->SendOne();
    const bool isRetryStopped = resources.Stop(
        std::chrono::steady_clock::now()
            + std::chrono::seconds(1));
    failureCount += GetCaseResult(
        isAccepted && isStopStarted && isRejected
            && isDeadlineFailed && isCancelledSent
            && isRetryStopped,
        "StartStop rejects new work and Stop remains retryable after a deadline")
        ? 0 : 1;
    return failureCount;
}

int GetExecutorLaneFailCount()
{
    std::atomic<int> workerCount{ 0 };
    auto executor = CreateAppTaskExecutor(
        [&workerCount](AppWorkerWork work) {
            ++workerCount;
            return std::thread(std::move(work));
        });
    executor.reset();
    return GetCaseResult(
        workerCount.load() == 4,
        "Render lane adds exactly one bounded worker")
        ? 0 : 1;
}

vtkSmartPointer<vtkImageData> BuildIsoImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    constexpr int side = 12;
    image->SetDimensions(side, side, side);
    image->AllocateScalars(VTK_FLOAT, 1);
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const double dx = static_cast<double>(x) - 5.5;
                const double dy = static_cast<double>(y) - 5.5;
                const double dz = static_cast<double>(z) - 5.5;
                image->SetScalarComponentFromFloat(
                    x, y, z, 0,
                    static_cast<float>(
                        dx * dx + dy * dy + dz * dz - 16.0));
            }
        }
    }
    image->GetPointData()->GetScalars()->Modified();
    image->Modified();
    return image;
}

IsoSurfaceBuildRequest GetIsoRequest(
    const vtkSmartPointer<vtkImageData>& image,
    const std::uint64_t revision,
    const VolumeQuality quality,
    const double isoValue)
{
    IsoSurfaceBuildRequest request;
    request.requestRevision = revision;
    request.requestedQuality = quality;
    request.input = image;
    request.key.inputStamp = { image.GetPointer(), 1 };
    request.key.inputMTime = image->GetMTime();
    request.key.inputScalarMTime =
        image->GetPointData()->GetScalars()->GetMTime();
    request.key.outputDimensions = { 12, 12, 12 };
    request.key.isoValue = isoValue;
    return request;
}

int GetIsoBuilderFailCount()
{
    int failureCount = 0;
    auto image = BuildIsoImage();
    auto lane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator resources(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    auto channel = resources.CreateTaskChannel(
        RenderProductKind::IsoSurface);
    IsoSurfaceBuildResult buildResult;
    auto request = GetIsoRequest(
        image, 1, VolumeQuality::Ultra, 0.0);
    RenderTaskRequest task;
    task.requestRevision = request.requestRevision;
    task.estimatedBytes = 4096;
    task.work = [&buildResult, request](RenderTaskToken token) {
        buildResult = IsoSurfaceProductBuilder().BuildProduct(
            request, token);
    };
    const bool isAccepted = channel->StartTask(std::move(task))
        == RenderTaskAdmission::Accepted;
    const bool isSent = lane->SendOne();
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    if (buildResult.product) {
        mapper->SetInputData(buildResult.product->surface);
    }
    failureCount += GetCaseResult(
        isAccepted && isSent
            && buildResult.failureReason == RenderProductFailure::None
            && buildResult.product
            && buildResult.product->surface
            && buildResult.product->surface->GetNumberOfPoints() > 0
            && buildResult.product->surface->GetNumberOfCells() > 0
            && buildResult.product->actualBytes > 0
            && vtkTrivialProducer::SafeDownCast(
                mapper->GetInputAlgorithm()),
        "Iso builder returns a materialized producer-free CPU product")
        ? 0 : 1;

    auto invalidMask = vtkSmartPointer<vtkImageData>::New();
    invalidMask->SetDimensions(3, 3, 3);
    invalidMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto invalidRequest = GetIsoRequest(
        image, 2, VolumeQuality::Ultra, 0.0);
    invalidRequest.mask = invalidMask;
    invalidRequest.key.maskIdentity = invalidMask.GetPointer();
    const auto invalidResult = IsoSurfaceProductBuilder().BuildProduct(
        invalidRequest, RenderTaskToken{});
    failureCount += GetCaseResult(
        invalidResult.failureReason
            == RenderProductFailure::InvalidInput
            && !invalidResult.product,
        "Iso builder rejects mismatched mask geometry without a partial product")
        ? 0 : 1;
    return failureCount;
}

int GetIsoStrategyTaskFailCount()
{
    int failureCount = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    auto resources = std::make_shared<RenderResourceCoordinator>(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    auto services = std::make_shared<RenderStrategyServices>();
    services->resources = resources;
    IsoSurfaceStrategy strategy(services);
    RenderParams initialParams;
    initialParams.volumeQuality = VolumeQuality::Auto;
    const bool isInitialQualitySet = strategy.SetVisualState(
        initialParams, UpdateFlags::Quality);
    auto image = BuildIsoImage();
    const bool isStampSet = strategy.SetRenderInputStamp({
        image.GetPointer(), 1 });
    const bool isInputSet = strategy.SetInputData(image, nullptr);
    const bool isInitialSent = lane->SendOne();
    const bool isInitialCommitted = strategy.SetProductCommit();
    auto* actor = vtkActor::SafeDownCast(strategy.GetMainProp());
    auto* mapper = actor
        ? vtkPolyDataMapper::SafeDownCast(actor->GetMapper())
        : nullptr;
    vtkPolyData* oldSurface = mapper ? mapper->GetInput() : nullptr;

    RenderParams params;
    params.isoValue = 0.0;
    params.volumeQuality = VolumeQuality::Low;
    const bool isLowAccepted = strategy.SetVisualState(
        params, UpdateFlags::Quality);
    const auto preparing = strategy.GetTransitionState();
    vtkPolyData* surfaceDuringPrepare =
        mapper ? mapper->GetInput() : nullptr;
    params.volumeQuality = VolumeQuality::Ultra;
    const bool isUltraAccepted = strategy.SetVisualState(
        params, UpdateFlags::Quality);
    params.volumeQuality = VolumeQuality::High;
    const bool isHighAccepted = strategy.SetVisualState(
        params, UpdateFlags::Quality);
    const bool isOldSent = lane->SendOne();
    const bool isLatestStarted = resources->SendTasks();
    const bool isLatestSent = lane->SendOne();
    const bool isCommitted = strategy.SetProductCommit();
    strategy.SetFirstRenderDuration(17);
    const auto active = strategy.GetTransitionState();
    vtkPolyData* newSurface = mapper ? mapper->GetInput() : nullptr;
    failureCount += GetCaseResult(
        isInitialQualitySet && isStampSet && isInputSet
            && isInitialSent && isInitialCommitted && oldSurface
            && isLowAccepted && isUltraAccepted && isHighAccepted
            && preparing.status == RenderProductStatus::Preparing
            && surfaceDuringPrepare == oldSurface
            && mapper && mapper->GetInput() == newSurface
            && isOldSent && isLatestStarted && isLatestSent
            && isCommitted && newSurface && newSurface != oldSurface
            && active.status == RenderProductStatus::Active
            && active.requestedQuality == VolumeQuality::High
            && active.appliedQuality == VolumeQuality::High
            && active.stats.activeRevision
                == active.stats.requestRevision
            && active.stats.gpuUploadUs > 0
            && active.stats.firstRenderUs == 17,
        "Iso Low to Ultra to High commits only the latest valid revision")
        ? 0 : 1;

    const std::size_t startsBeforeDrag = lane->GetStartCount();
    for (std::uint64_t index = 0; index < 100; ++index) {
        params.isoValue = static_cast<double>(index) * 0.01;
        (void)strategy.SetVisualState(params, UpdateFlags::IsoValue);
    }
    failureCount += GetCaseResult(
        lane->GetPendingCount() == 1
            && lane->GetStartCount() == startsBeforeDrag + 1,
        "One hundred IsoValue updates keep one running and one latest request")
        ? 0 : 1;
    (void)resources->StartStop();
    (void)lane->SendOne();
    (void)resources->Stop(
        std::chrono::steady_clock::now()
            + std::chrono::seconds(1));
    return failureCount;
}

VolumeLodBuildRequest GetVolumeRequest(
    const vtkSmartPointer<vtkImageData>& image,
    const vtkSmartPointer<vtkImageData>& mask,
    const std::uint64_t revision,
    const VolumeQuality quality,
    const std::array<int, 3>& dimensions,
    const bool isDenoiseOn)
{
    VolumeLodBuildRequest request;
    request.requestRevision = revision;
    request.requestedQuality = quality;
    request.input = image;
    request.mask = mask;
    request.key.inputStamp = { image.GetPointer(), 1 };
    request.key.maskIdentity = mask.GetPointer();
    request.key.inputMTime = image->GetMTime();
    request.key.inputScalarMTime =
        image->GetPointData()->GetScalars()->GetMTime();
    request.key.maskMTime = mask ? mask->GetMTime() : 0;
    request.key.maskScalarMTime = mask
        ? mask->GetPointData()->GetScalars()->GetMTime() : 0;
    request.key.outputDimensions = dimensions;
    request.key.denoiseThreshold = isDenoiseOn ? 0.1 : 0.0;
    request.key.isDenoiseOn = isDenoiseOn;
    return request;
}

int GetVolumeBuilderFailCount()
{
    int failureCount = 0;
    auto image = BuildIsoImage();
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(mask->GetScalarPointer()),
        mask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    mask->GetPointData()->GetScalars()->Modified();
    mask->Modified();

    const auto lowResult = VolumeLodProductBuilder().BuildProduct(
        GetVolumeRequest(
            image, mask, 1, VolumeQuality::Low,
            { 3, 3, 3 }, false),
        RenderTaskToken{});
    auto volumeMapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
    if (lowResult.product) {
        volumeMapper->SetInputData(lowResult.product->volume);
    }
    int lowDimensions[3] = {};
    if (lowResult.product && lowResult.product->volume) {
        lowResult.product->volume->GetDimensions(lowDimensions);
    }
    failureCount += GetCaseResult(
        lowResult.failureReason == RenderProductFailure::None
            && lowResult.product
            && lowResult.product->volume
            && lowResult.product->mask
            && std::equal(
                lowDimensions, lowDimensions + 3,
                std::array<int, 3>{ 3, 3, 3 }.begin())
            && lowResult.product->volume != image
            && lowResult.product->mask != mask
            && lowResult.product->actualBytes
                >= static_cast<std::uint64_t>(
                    lowResult.product->volume->GetActualMemorySize())
                    * 1024ULL
            && vtkTrivialProducer::SafeDownCast(
                volumeMapper->GetInputAlgorithm()),
        "Volume builder materializes disconnected scalar and mask products")
        ? 0 : 1;

    const auto nativeResult = VolumeLodProductBuilder().BuildProduct(
        GetVolumeRequest(
            image, nullptr, 2, VolumeQuality::Ultra,
            { 12, 12, 12 }, false),
        RenderTaskToken{});
    const auto denoiseResult = VolumeLodProductBuilder().BuildProduct(
        GetVolumeRequest(
            image, nullptr, 3, VolumeQuality::Ultra,
            { 12, 12, 12 }, true),
        RenderTaskToken{});
    failureCount += GetCaseResult(
        nativeResult.product
            && nativeResult.product->volume == image
            && denoiseResult.product
            && denoiseResult.product->volume
            && denoiseResult.product->volume != image
            && denoiseResult.product->volume->GetNumberOfPoints()
                == image->GetNumberOfPoints(),
        "Volume Ultra aliases native input only when denoise is disabled")
        ? 0 : 1;

    auto invalidMask = vtkSmartPointer<vtkImageData>::New();
    invalidMask->SetDimensions(2, 2, 2);
    invalidMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    const auto invalidResult = VolumeLodProductBuilder().BuildProduct(
        GetVolumeRequest(
            image, invalidMask, 4, VolumeQuality::High,
            { 6, 6, 6 }, false),
        RenderTaskToken{});
    failureCount += GetCaseResult(
        invalidResult.failureReason
            == RenderProductFailure::InvalidInput
            && !invalidResult.product,
        "Volume builder rejects invalid mask geometry without a partial product")
        ? 0 : 1;
    return failureCount;
}

int GetVolumeStrategyTaskFailCount()
{
    int failureCount = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    auto resources = std::make_shared<RenderResourceCoordinator>(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    auto services = std::make_shared<RenderStrategyServices>();
    services->resources = resources;
    VolumeStrategy strategy(services);
    RenderParams initialParams;
    initialParams.volumeQuality = VolumeQuality::Auto;
    const bool isInitialQualitySet = strategy.SetVisualState(
        initialParams, UpdateFlags::Quality);
    auto image = BuildIsoImage();
    const bool isStampSet = strategy.SetRenderInputStamp({
        image.GetPointer(), 1 });
    const bool isInputSet = strategy.SetInputData(image, nullptr);
    const bool isInitialSent = lane->SendOne();
    const bool isInitialCommitted = strategy.SetProductCommit();
    auto* volume = vtkVolume::SafeDownCast(strategy.GetMainProp());
    auto* mapper = volume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(volume->GetMapper())
        : nullptr;
    vtkImageData* oldInput = mapper
        ? vtkImageData::SafeDownCast(mapper->GetInput()) : nullptr;

    RenderParams params;
    params.volumeQuality = VolumeQuality::Low;
    const bool isLowAccepted = strategy.SetVisualState(
        params, UpdateFlags::Quality);
    const auto preparing = strategy.GetTransitionState();
    const bool isOldPreserved = mapper && mapper->GetInput() == oldInput;
    params.volumeQuality = VolumeQuality::Ultra;
    const bool isUltraAccepted = strategy.SetVisualState(
        params, UpdateFlags::Quality);
    params.volumeQuality = VolumeQuality::High;
    const bool isHighAccepted = strategy.SetVisualState(
        params, UpdateFlags::Quality);
    const bool isOldSent = lane->SendOne();
    const bool isLatestStarted = resources->SendTasks();
    const bool isLatestSent = lane->SendOne();
    const bool isCommitted = strategy.SetProductCommit();
    strategy.SetFirstRenderDuration(19);
    const auto active = strategy.GetTransitionState();
    int activeDimensions[3] = {};
    auto* activeInput = mapper
        ? vtkImageData::SafeDownCast(mapper->GetInput()) : nullptr;
    if (activeInput) {
        activeInput->GetDimensions(activeDimensions);
    }
    failureCount += GetCaseResult(
        isInitialQualitySet && isStampSet && isInputSet
            && isInitialSent && isInitialCommitted && oldInput
            && isLowAccepted && isUltraAccepted && isHighAccepted
            && preparing.status == RenderProductStatus::Preparing
            && isOldPreserved
            && isOldSent && isLatestStarted && isLatestSent
            && isCommitted && mapper && mapper->GetInput() != oldInput
            && std::equal(
                activeDimensions, activeDimensions + 3,
                std::array<int, 3>{ 6, 6, 6 }.begin())
            && active.status == RenderProductStatus::Active
            && active.requestedQuality == VolumeQuality::High
            && active.appliedQuality == VolumeQuality::High
            && active.stats.gpuReleaseUs > 0
            && active.stats.gpuUploadUs > 0
            && active.stats.firstRenderUs == 19,
        "Volume Low to Ultra to High commits only the latest CPU product")
        ? 0 : 1;

    const auto buildCount = strategy.GetResampleBuildCount();
    const auto updateCount = strategy.GetResampleUpdateCount();
    params.isInteracting = true;
    const bool isPreviewSet = strategy.SetVisualState(
        params, UpdateFlags::RenderRate);
    params.isInteracting = false;
    const bool isStillSet = strategy.SetVisualState(
        params, UpdateFlags::RenderRate);
    failureCount += GetCaseResult(
        isPreviewSet && isStillSet
            && strategy.GetResampleBuildCount() == buildCount
            && strategy.GetResampleUpdateCount() == updateCount
            && mapper->GetInput() != oldInput,
        "Explicit Volume interaction phase changes sampling without CPU rebuild")
        ? 0 : 1;
    (void)resources->StartStop();
    (void)resources->Stop(
        std::chrono::steady_clock::now()
            + std::chrono::seconds(1));
    return failureCount;
}

std::shared_ptr<VolumeLodProduct> BuildCachedVolumeProduct(
    const vtkSmartPointer<vtkImageData>& image,
    const std::uint64_t revision,
    const std::uint64_t bytes)
{
    auto product = std::make_shared<VolumeLodProduct>();
    product->requestRevision = revision;
    product->inputStamp = { image.GetPointer(), 1 };
    product->requestedQuality = VolumeQuality::High;
    product->outputDimensions = { 12, 12, 12 };
    product->volume = image;
    product->actualBytes = bytes;
    return product;
}

int GetResourceAccountingFailCount()
{
    int failureCount = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator resources(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    const bool isBudgetSet = resources.SetCpuBudgetBytes(1000);
    auto first = resources.CreateTaskChannel(
        RenderProductKind::VolumeLod);
    auto second = resources.CreateTaskChannel(
        RenderProductKind::IsoSurface);
    bool isActualAdmitted = true;
    RenderTaskRequest firstRequest;
    firstRequest.requestRevision = 1;
    firstRequest.estimatedBytes = 400;
    firstRequest.work = [&isActualAdmitted](RenderTaskToken token) {
        isActualAdmitted = token.SetActualBytes(800);
    };
    RenderTaskRequest secondRequest;
    secondRequest.requestRevision = 1;
    secondRequest.estimatedBytes = 300;
    secondRequest.work = [](RenderTaskToken token) {
        (void)token.SetActualBytes(300);
    };
    const bool isFirstAccepted = first->StartTask(
        std::move(firstRequest)) == RenderTaskAdmission::Accepted;
    const bool isSecondAccepted = second->StartTask(
        std::move(secondRequest)) == RenderTaskAdmission::Accepted;
    const auto preparingResources = resources.GetResourceState();
    const bool isFirstSent = lane->SendOne();
    const auto rejectedState = first->GetState();
    const bool isSecondStarted = resources.SendTasks();
    const bool isSecondSent = lane->SendOne();
    const auto readyResources = resources.GetResourceState();
    const bool isReadyReleased = second->SetReadyFailed(
        1, RenderProductFailure::BuildFailed,
        "The test builder rejected its ready product.");
    const auto failedResources = resources.GetResourceState();
    const auto failedState = second->GetState();
    failureCount += GetCaseResult(
        isBudgetSet && isFirstAccepted && isSecondAccepted
            && preparingResources.runningBytes == 400
            && preparingResources.pendingBytes == 300
            && preparingResources.runningBytes
                + preparingResources.pendingBytes
                <= preparingResources.cpuBudgetBytes
            && isFirstSent && !isActualAdmitted
            && rejectedState.status == RenderProductStatus::Failed
            && rejectedState.failureReason
                == RenderProductFailure::ResourceRejected
            && isSecondStarted && isSecondSent
            && readyResources.pendingBytes == 300
            && isReadyReleased
            && failedResources.pendingBytes == 0
            && failedState.status == RenderProductStatus::Failed
            && failedState.failureReason
                == RenderProductFailure::BuildFailed
            && readyResources.activeBytes
                + readyResources.runningBytes
                + readyResources.pendingBytes
                + readyResources.cacheBytes
                <= readyResources.cpuBudgetBytes,
        "Session CPU accounting re-admits actual bytes and releases rejection")
        ? 0 : 1;
    return failureCount;
}

int GetSharedCacheAndGpuFailCount()
{
    int failureCount = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    auto resources = std::make_shared<RenderResourceCoordinator>(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    const bool isBudgetSet = resources->SetCpuBudgetBytes(4096);
    auto image = BuildIsoImage();
    auto firstProduct = BuildCachedVolumeProduct(image, 1, 1024);
    auto firstRequest = GetVolumeRequest(
        image, nullptr, 1, VolumeQuality::High,
        { 12, 12, 12 }, false);
    const bool isFirstCached = resources->SetVolumeProduct(
        firstRequest.key, firstProduct);
    auto firstHit = resources->GetVolumeProduct(firstRequest.key);
    const bool isFirstPointerShared = firstHit == firstProduct;
    auto secondProduct = BuildCachedVolumeProduct(image, 2, 3500);
    auto secondKey = firstRequest.key;
    secondKey.outputDimensions = { 6, 6, 6 };
    const bool isProtected = !resources->SetVolumeProduct(
        secondKey, secondProduct);
    firstHit.reset();
    firstProduct.reset();
    const bool isEvictedForNew = resources->SetVolumeProduct(
        secondKey, secondProduct);
    const bool isOldEvicted =
        !resources->GetVolumeProduct(firstRequest.key);
    auto secondHit = resources->GetVolumeProduct(secondKey);

    int firstContext = 0;
    int secondContext = 0;
    int firstOwner = 0;
    int secondOwner = 0;
    const bool isFirstContextSet = resources->SetGpuContextBudget(
        &firstContext, 1000);
    const bool isFirstReservationSet = resources->SetGpuReservation(
        &firstContext, &firstOwner, 600);
    const bool isSameContextRejected = !resources->SetGpuReservation(
        &firstContext, &secondOwner, 500);
    const bool isSecondContextSet = resources->SetGpuContextBudget(
        &secondContext, 1000);
    const bool isSecondReservationSet = resources->SetGpuReservation(
        &secondContext, &secondOwner, 500);
    const auto firstGpu = resources->GetGpuResourceState(&firstContext);
    const auto secondGpu = resources->GetGpuResourceState(&secondContext);
    failureCount += GetCaseResult(
        isBudgetSet && isFirstCached && isFirstPointerShared
            && isProtected && isEvictedForNew && isOldEvicted
            && secondHit == secondProduct
            && resources->GetResourceState().cacheBytes == 3500
            && isFirstContextSet && isFirstReservationSet
            && isSameContextRejected
            && isSecondContextSet && isSecondReservationSet
            && firstGpu.reservedBytes == 600
            && firstGpu.reservationCount == 1
            && secondGpu.reservedBytes == 500
            && secondGpu.reservationCount == 1,
        "Typed LRU protects external owners and isolates GPU contexts")
        ? 0 : 1;

    auto evictionLane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator evictionResources(
        [evictionLane](RenderLaneWork work) {
            return evictionLane->Start(std::move(work));
        });
    const bool isEvictionBudgetSet =
        evictionResources.SetCpuBudgetBytes(1000);
    auto evictionProduct = BuildCachedVolumeProduct(image, 3, 600);
    auto evictionKey = firstRequest.key;
    evictionKey.outputDimensions = { 3, 3, 3 };
    const bool isEvictionCached = evictionResources.SetVolumeProduct(
        evictionKey, evictionProduct);
    evictionProduct.reset();
    auto evictionChannel = evictionResources.CreateTaskChannel(
        RenderProductKind::VolumeLod);
    RenderTaskRequest evictionRequest;
    evictionRequest.requestRevision = 1;
    evictionRequest.estimatedBytes = 500;
    evictionRequest.work = [](RenderTaskToken token) {
        (void)token.SetActualBytes(500);
    };
    const bool isAdmittedAfterEviction =
        evictionChannel->StartTask(std::move(evictionRequest))
            == RenderTaskAdmission::Accepted;
    const bool isEvictionSent = evictionLane->SendOne();
    failureCount += GetCaseResult(
        isEvictionBudgetSet && isEvictionCached
            && isAdmittedAfterEviction && isEvictionSent
            && !evictionResources.GetVolumeProduct(evictionKey),
        "Render admission evicts an unowned cache entry before rejection")
        ? 0 : 1;

    RenderResourceCoordinator sharedLeaseResources(
        [](RenderLaneWork) { return false; });
    const bool isSharedLeaseBudgetSet =
        sharedLeaseResources.SetCpuBudgetBytes(600);
    auto sharedLeaseProduct = BuildCachedVolumeProduct(
        image, 4, 600);
    auto sharedLeaseKey = firstRequest.key;
    sharedLeaseKey.outputDimensions = { 4, 4, 4 };
    const bool isSharedLeaseCached =
        sharedLeaseResources.SetVolumeProduct(
            sharedLeaseKey, sharedLeaseProduct);
    auto firstActive = sharedLeaseResources.CreateTaskChannel(
        RenderProductKind::VolumeLod);
    auto secondActive = sharedLeaseResources.CreateTaskChannel(
        RenderProductKind::VolumeLod);
    const bool isFirstActive = firstActive
        && firstActive->SetCachedActive(
            1, 600, sharedLeaseProduct.get())
        && firstActive->CompleteActiveBytes(1);
    const bool isSecondActive = secondActive
        && secondActive->SetCachedActive(
            1, 600, sharedLeaseProduct.get())
        && secondActive->CompleteActiveBytes(1);
    const auto sharedActiveState =
        sharedLeaseResources.GetResourceState();
    firstActive.reset();
    secondActive.reset();
    const auto sharedCachedState =
        sharedLeaseResources.GetResourceState();
    failureCount += GetCaseResult(
        isSharedLeaseBudgetSet && isSharedLeaseCached
            && isFirstActive && isSecondActive
            && sharedActiveState.activeBytes == 600
            && sharedActiveState.cacheBytes == 0
            && sharedCachedState.activeBytes == 0
            && sharedCachedState.cacheBytes == 600,
        "Shared active and cache leases count one physical CPU product")
        ? 0 : 1;

    auto sharedResources = std::make_shared<RenderResourceCoordinator>(
        [lane](RenderLaneWork work) {
            return lane->Start(std::move(work));
        });
    auto services = std::make_shared<RenderStrategyServices>();
    services->resources = sharedResources;
    VolumeStrategy firstView(services);
    VolumeStrategy secondView(services);
    RenderParams initialParams;
    initialParams.volumeQuality = VolumeQuality::Auto;
    const bool isInitialQualitySet = firstView.SetVisualState(
        initialParams, UpdateFlags::Quality)
        && secondView.SetVisualState(initialParams, UpdateFlags::Quality);
    const RenderInputStamp stamp{ image.GetPointer(), 9 };
    const bool isFirstStampSet = firstView.SetRenderInputStamp(stamp);
    const bool isSecondStampSet = secondView.SetRenderInputStamp(stamp);
    const bool isFirstInputSet = firstView.SetInputData(image, nullptr);
    const bool isFirstBuilt = lane->SendOne();
    const bool isFirstCommitted = firstView.SetProductCommit();
    const bool isSecondInputSet = secondView.SetInputData(image, nullptr);
    auto* firstVolume = vtkVolume::SafeDownCast(firstView.GetMainProp());
    auto* secondVolume = vtkVolume::SafeDownCast(secondView.GetMainProp());
    auto* firstMapper = firstVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(firstVolume->GetMapper())
        : nullptr;
    auto* secondMapper = secondVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(secondVolume->GetMapper())
        : nullptr;
    RenderParams ordinaryParams;
    ordinaryParams.volumeQuality = VolumeQuality::Auto;
    const auto planCount = firstView.GetLodPlanCount();
    const bool isOrdinaryStable = firstView.SetVisualState(
        ordinaryParams, UpdateFlags::Material)
        && firstView.GetLodPlanCount() == planCount;
    const bool isTopologyAdvanced =
        sharedResources->AdvanceTopologyRevision() != 0;
    const bool isTopologyRefreshed = firstView.SetVisualState(
        ordinaryParams, UpdateFlags::Material)
        && firstView.GetLodPlanCount() == planCount + 1;
    failureCount += GetCaseResult(
        isInitialQualitySet && isFirstStampSet && isSecondStampSet
            && isFirstInputSet && isFirstBuilt && isFirstCommitted
            && isSecondInputSet
            && firstMapper && secondMapper
            && firstMapper->GetInput() == secondMapper->GetInput()
            && secondView.GetTransitionState().stats.isCacheHit
            && isOrdinaryStable
            && isTopologyAdvanced
            && isTopologyRefreshed,
        "Volume products share across Views and Auto follows topology revisions")
        ? 0 : 1;
    return failureCount;
}

class CompositeMainProbe final : public BaseVisualStrategy {
public:
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override
    {
        input = std::move(data);
    }

    bool SetVisualState(
        const RenderParams&,
        UpdateFlags) override
    {
        return isAccepting;
    }

    bool SetProductCommit() override
    {
        if (activateRevision != 0) {
            transition.status = RenderProductStatus::Active;
            transition.stats.activeRevision = activateRevision;
            transition.stats.requestRevision = activateRevision;
            activateRevision = 0;
        }
        return isCommitValid;
    }

    RenderTransitionState GetTransitionState() const override
    {
        return transition;
    }

    vtkSmartPointer<vtkDataObject> input;
    RenderTransitionState transition;
    std::uint64_t activateRevision = 0;
    bool isAccepting = true;
    bool isCommitValid = true;
};

vtkPlaneSource* GetCompositePlaneSource(
    CompositeStrategy& strategy,
    vtkRenderer* renderer,
    const int axis)
{
    auto* props = renderer ? renderer->GetViewProps() : nullptr;
    if (!props) return nullptr;
    props->InitTraversal();
    while (auto* prop = props->GetNextProp()) {
        auto* actor = vtkActor::SafeDownCast(prop);
        if (!actor || strategy.GetPlaneAxis(actor) != axis) continue;
        auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        return mapper
            ? vtkPlaneSource::SafeDownCast(mapper->GetInputAlgorithm())
            : nullptr;
    }
    return nullptr;
}

int GetCompositeAtomicFailCount()
{
    int failureCount = 0;
    auto main = std::make_shared<CompositeMainProbe>();
    CompositeStrategy strategy(main);
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    strategy.SetInputData(image);
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    strategy.AttachRenderer(renderer);
    RenderParams params;
    params.cursor = { 1.0, 1.0, 1.0 };
    const auto geometryFlags =
        UpdateFlags::Cursor | UpdateFlags::Transform;
    const bool isInitialSet = strategy.SetVisualState(
        params, geometryFlags);
    auto* plane = GetCompositePlaneSource(strategy, renderer, 0);
    const double initialX = plane ? plane->GetOrigin()[0] : -1.0;

    main->isAccepting = false;
    params.cursor[0] = 2.0;
    const bool isRejected = !strategy.SetVisualState(
        params, UpdateFlags::Cursor);
    const bool isRejectedStable = plane
        && plane->GetOrigin()[0] == initialX;

    main->isAccepting = true;
    main->transition.status = RenderProductStatus::Preparing;
    main->transition.stats.requestRevision = 7;
    params.cursor[0] = 2.5;
    const bool isPreparingAccepted = strategy.SetVisualState(
        params, UpdateFlags::Cursor);
    const bool isPreparingStable = plane
        && plane->GetOrigin()[0] == initialX;
    main->activateRevision = 7;
    const bool isCommitted = strategy.SetProductCommit();
    const bool isMatchingCommitted = plane
        && plane->GetOrigin()[0] == 2.5;

    main->transition.status = RenderProductStatus::Preparing;
    main->transition.stats.requestRevision = 8;
    params.cursor[0] = 0.5;
    const bool isFailureStaged = strategy.SetVisualState(
        params, UpdateFlags::Cursor);
    main->transition.status = RenderProductStatus::Failed;
    const bool isFailureRejected = !strategy.SetProductCommit();
    const bool isFailureStable = plane
        && plane->GetOrigin()[0] == 2.5;

    main->transition.status = RenderProductStatus::Preparing;
    main->transition.stats.requestRevision = 9;
    params.cursor[0] = 0.25;
    const bool isOlderStaged = strategy.SetVisualState(
        params, UpdateFlags::Cursor);
    main->transition.stats.requestRevision = 10;
    params.cursor[0] = 2.75;
    const bool isNewerStaged = strategy.SetVisualState(
        params, UpdateFlags::Cursor);
    main->activateRevision = 10;
    const bool isNewerCommitted = strategy.SetProductCommit();
    failureCount += GetCaseResult(
        isInitialSet && plane && initialX == 1.0
            && isRejected && isRejectedStable
            && isPreparingAccepted && isPreparingStable
            && isCommitted && isMatchingCommitted
            && isFailureStaged && isFailureRejected
            && isFailureStable
            && isOlderStaged && isNewerStaged
            && isNewerCommitted
            && plane->GetOrigin()[0] == 2.75,
        "Composite commits reference planes only for the matching main revision")
        ? 0 : 1;
    strategy.DetachRenderer(renderer);
    return failureCount;
}

int GetSliceAndPlaneCacheFailCount()
{
    int failureCount = 0;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(8, 6, 4);
    image->SetSpacing(0.5, 1.0, 1.5);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    RenderParams params;
    params.cursor = { 1.0, 2.0, 3.0 };

    ColoredPlanesStrategy planes;
    planes.SetInputData(image);
    const bool isPlanesInitial = planes.SetVisualState(
        params, UpdateFlags::Transform | UpdateFlags::Cursor);
    const auto planeBuilds = planes.GetWorldBoundsBuildCount();
    for (int index = 0; index < 100; ++index) {
        params.cursor[0] = static_cast<double>(index) * 0.01;
        (void)planes.SetVisualState(params, UpdateFlags::Cursor);
    }
    const bool isPlaneCursorCached =
        planes.GetWorldBoundsBuildCount() == planeBuilds;
    params.modelMatrix[3] = 2.0;
    const bool isPlaneTransformSet = planes.SetVisualState(
        params, UpdateFlags::Transform);
    const bool isPlaneTransformOnce =
        planes.GetWorldBoundsBuildCount() == planeBuilds + 1;
    image->Modified();
    const bool isPlaneInputRefresh = planes.SetVisualState(
        params, UpdateFlags::Cursor);
    const bool isPlaneInputInvalidated =
        planes.GetWorldBoundsBuildCount() == planeBuilds + 2;
    const bool isPlaneInputCachedAgain = planes.SetVisualState(
        params, UpdateFlags::Cursor)
        && planes.GetWorldBoundsBuildCount() == planeBuilds + 2;

    SliceStrategy slice(Orientation::Top_down);
    slice.SetInputData(image);
    const bool isSliceInitial = slice.SetVisualState(
        params, UpdateFlags::Transform | UpdateFlags::Cursor);
    const auto sliceBounds = slice.GetWorldBoundsBuildCount();
    const auto sliceInverse = slice.GetInverseBuildCount();
    for (int index = 0; index < 100; ++index) {
        params.cursor[1] = static_cast<double>(index) * 0.01;
        (void)slice.SetVisualState(params, UpdateFlags::Cursor);
    }
    const bool isSliceCursorCached =
        slice.GetWorldBoundsBuildCount() == sliceBounds
        && slice.GetInverseBuildCount() == sliceInverse;
    params.modelMatrix[7] = -1.0;
    const bool isSliceTransformSet = slice.SetVisualState(
        params, UpdateFlags::Transform);
    const bool isSliceTransformOnce =
        slice.GetWorldBoundsBuildCount() == sliceBounds + 1
        && slice.GetInverseBuildCount() == sliceInverse + 1;
    image->Modified();
    const bool isSliceInputRefresh = slice.SetVisualState(
        params, UpdateFlags::Cursor);
    const bool isSliceInputInvalidated =
        slice.GetWorldBoundsBuildCount() == sliceBounds + 2
        && slice.GetInverseBuildCount() == sliceInverse + 2;
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    const int oldDepthPeeling = renderer->GetUseDepthPeeling();
    slice.AttachRenderer(renderer);
    const bool isEnvironmentUnchanged =
        renderer->GetUseDepthPeeling() == oldDepthPeeling;
    slice.DetachRenderer(renderer);

    failureCount += GetCaseResult(
        isPlanesInitial && planeBuilds == 1
            && isPlaneCursorCached
            && isPlaneTransformSet && isPlaneTransformOnce
            && isPlaneInputRefresh && isPlaneInputInvalidated
            && isPlaneInputCachedAgain
            && isSliceInitial && sliceBounds == 1 && sliceInverse == 1
            && isSliceCursorCached
            && isSliceTransformSet && isSliceTransformOnce
            && isSliceInputRefresh && isSliceInputInvalidated
            && isEnvironmentUnchanged,
        "Slice and colored planes reuse transform caches on cursor-only updates")
        ? 0 : 1;
    return failureCount;
}

} // namespace

int GetRenderProductFailCount()
{
    return GetTransitionValueFailCount()
        + GetDefaultStrategyFailCount()
        + GetRenderLaneFailCount()
        + GetRenderStopFailCount()
        + GetExecutorLaneFailCount()
        + GetIsoBuilderFailCount()
        + GetIsoStrategyTaskFailCount()
        + GetVolumeBuilderFailCount()
        + GetVolumeStrategyTaskFailCount()
        + GetResourceAccountingFailCount()
        + GetSharedCacheAndGpuFailCount()
        + GetCompositeAtomicFailCount()
        + GetSliceAndPlaneCacheFailCount();
}
