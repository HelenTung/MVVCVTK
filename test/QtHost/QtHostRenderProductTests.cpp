// 测试用途：验证渲染产品构建、缓存、策略切换、任务停止和资源核算。
#include "../TestDataPort.h"
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
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "Data/DataManager.h"
#include "Interaction/ViewContextFactory.h"
#include "Render/Internal/RenderWorkBudget.h"
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkCommand.h>
#include <vtkCallbackCommand.h>
#include <vtkRenderWindow.h>
#include <stdexcept>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <limits>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkFloatArray.h>
#include <vtkInformation.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkOpenGLIndexBufferObject.h>
#include <vtkPlaneSource.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkTrivialProducer.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkWindowToImageFilter.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

namespace {

class TransitionData final : public RawVolumeDataManager {
public:
    using BaseDataManager::SetOwnedImage;
};

class TransitionProbe final : public BaseVisualStrategy {
public:
    bool isDelayed = false;
    int attachCount = 0;
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    bool SetProductCommit() override { return true; }
    RenderTransitionState GetTransitionState() const override
    {
        RenderTransitionState state;
        state.status = isDelayed ? RenderProductStatus::Preparing : RenderProductStatus::Idle;
        return state;
    }
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        ++attachCount;
        BaseVisualStrategy::AttachRenderer(std::move(renderer));
    }
};

class RenderWindowProbe final : public vtkGenericOpenGLRenderWindow {
public:
    static RenderWindowProbe* New() { return new RenderWindowProbe; }
    vtkTypeMacro(RenderWindowProbe, vtkGenericOpenGLRenderWindow);
    bool isSkipped = false;
    bool hasError = false;
    bool isThrowing = false;
    int renderCount = 0;
    void Render() override
    {
        ++renderCount;
        if (isThrowing) throw std::runtime_error("injected Render failure");
        if (hasError) InvokeEvent(vtkCommand::ErrorEvent);
        if (!isSkipped) InvokeEvent(vtkCommand::EndEvent);
    }
};

class GpuHeadroomProbe final : public VolumeStrategy {
public:
    using VolumeStrategy::VolumeStrategy;
    std::optional<std::uint64_t> freeBytes;
private:
    std::optional<std::uint64_t> GetGpuFreeBytes() const override{return freeBytes;}
};

class RangeReadProbe final : public vtkFloatArray {
public:
    static RangeReadProbe* New() {return new RangeReadProbe;}
    vtkTypeMacro(RangeReadProbe, vtkFloatArray);
    inline static bool shouldThrow = false;
    void* GetVoidPointer(vtkIdType index) override
    {
        if (shouldThrow) {
            shouldThrow = false;
            throw std::runtime_error("injected optional range preparation failure");
        }
        return this->Superclass::GetVoidPointer(index);
    }
};

int GetGpuHeadroomFailCount()
{
    int failures=0;
    constexpr std::uint64_t MiB=1024ULL*1024;
    for(const auto free: {std::optional<std::uint64_t>(128*MiB),std::optional<std::uint64_t>{},std::optional<std::uint64_t>(0)}) {
        auto lane=std::make_shared<ManualRenderLane>();
        auto resources=std::make_shared<RenderResourceCoordinator>([lane](RenderLaneWork work){return lane->Start(std::move(work));});
        auto services=std::make_shared<RenderStrategyServices>();services->resources=resources;
        auto renderer=vtkSmartPointer<vtkRenderer>::New();
        auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);window->SetSize(32,32);window->AddRenderer(renderer);window->Render();
        GpuHeadroomProbe candidate(services);candidate.freeBytes=free;
        auto image=vtkSmartPointer<vtkImageData>::New();image->SetDimensions(32,32,32);image->AllocateScalars(VTK_FLOAT,1);image->GetPointData()->GetScalars()->FillComponent(0,128);
        RenderParams params;params.volumeQuality=VolumeQuality::Ultra;
        params.volumeTransferFunction.colorNodes={{0,0,0,0},{255,1,1,1}};
        params.volumeTransferFunction.opacityNodes={{0,0},{255,1}};
        bool valid=candidate.SetVisualState(params,UpdateFlags::Quality|UpdateFlags::VolumeTransfer)
            &&candidate.SetRenderInputStamp(RenderInputStamp{GetTestDataRef(1011)})
            &&candidate.SetInputData(image,nullptr)&&lane->SendOne()&&candidate.SetProductCommit();
        // Another strategy owns the displayed result in this same context.
        // Its reservation is not removed by clearing the candidate's own lease.
        int displayedOwner=0;
        valid=resources->SetGpuContextBudget(window,128*MiB)
            &&resources->SetGpuReservation(window,&displayedOwner,96*MiB)&&valid;
        int errors=0;auto observer=vtkSmartPointer<vtkCallbackCommand>::New();observer->SetClientData(&errors);
        observer->SetCallback([](vtkObject*,unsigned long,void* data,void*){++*static_cast<int*>(data);});
        const auto tag=window->AddObserver(vtkCommand::ErrorEvent,observer);
        candidate.AttachRenderer(renderer);renderer->ResetCamera();window->Render();window->WaitForCompletion();
        const auto state=resources->GetGpuResourceState(window);
        auto* volume=vtkVolume::SafeDownCast(candidate.GetMainProp());
        auto* mapper=volume?vtkGPUVolumeRayCastMapper::SafeDownCast(volume->GetMapper()):nullptr;
        auto* input=mapper?vtkImageData::SafeDownCast(mapper->GetInput()):nullptr;
        const bool hasHeadroom=free&&*free!=0;
        valid=valid&&candidate.GetGpuQueryCount()>0&&input&&input->GetDimensions()[0]==32
            &&(hasHeadroom?(errors==0&&state.reservationCount==2&&state.reservedBytes==96*MiB+32*32*32*sizeof(float))
                :(errors>0&&state.reservationCount==1&&state.reservedBytes==96*MiB));
        failures+=GetCaseResult(valid,hasHeadroom?
            "GPU driver headroom admits a Root candidate alongside another strategy's resident result":
            "Unknown or zero GPU headroom keeps conservative admission and the displayed reservation")?0:1;
        candidate.DetachRenderer(renderer);window->RemoveObserver(tag);
        (void)resources->ClearGpuReservation(window,&displayedOwner);
    }
    return failures;
}

int GetLatestTransitionFailCount()
{
    int failureCount = 0;
    for (const auto finalMode : {VizMode::Volume, VizMode::SliceTop_down}) {
        auto data = std::make_shared<TransitionData>();
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(3, 3, 3);
        image->AllocateScalars(VTK_FLOAT, 1);
        image->GetPointData()->GetScalars()->FillComponent(0, 1);
        const bool isSeeded = data->SetOwnedImage(image);
        auto events = std::make_shared<SharedStateBroadcaster>();
        auto sharedState = std::make_shared<SharedInteractionState>(events);
        std::shared_ptr<TransitionProbe> delayed;
        int failureAttempts = 0;
        AppServiceArgs args;
        args.dataManager = data;
        args.interactionState = sharedState;
        args.eventSource = events;
        args.strategyCreate = [&](VizMode mode) -> std::shared_ptr<AbstractVisualStrategy> {
            if (mode == VizMode::CompositeIsoSurface) { ++failureAttempts; return {}; }
            auto probe = std::make_shared<TransitionProbe>();
            if (mode == VizMode::IsoSurface) { probe->isDelayed = true; delayed = probe; }
            return probe;
        };
        auto ports = CreateAppPorts(std::move(args));
        auto window = vtkSmartPointer<vtkRenderWindow>::New();
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        const bool isBound = ports.renderBind->SetRenderTarget(window, renderer);
        AppViewUpdate initial;
        initial.mode = VizMode::SliceTop_down;
        const bool isInitial = ports.app.view->SendViewUpdate(initial)
            && ports.interaction.update->SendUpdates()
            && ports.app.view->GetViewState().mode == *initial.mode;
        AppViewUpdate next;
        next.mode = VizMode::IsoSurface;
        const bool isPreparing = ports.app.view->SendViewUpdate(next)
            && ports.interaction.update->SendUpdates() && delayed
            && delayed->attachCount == 0;
        auto stale = delayed;
        next.mode = finalMode;
        const bool isLatest = ports.app.view->SendViewUpdate(next);
        if (stale) stale->isDelayed = false;
        for (int tick = 0; tick < 4; ++tick) (void)ports.interaction.update->SendUpdates();
        failureCount += GetCaseResult(isSeeded && isBound && isInitial && isPreparing
            && isLatest && stale && stale->attachCount == 0
            && ports.app.view->GetViewState().mode == finalMode,
            "A-B-C and A-B-A discard stale candidates before renderer attachment") ? 0 : 1;
        next.mode = VizMode::CompositeIsoSurface;
        (void)ports.app.view->SendViewUpdate(next);
        for (int tick = 0; tick < 8; ++tick) (void)ports.interaction.update->SendUpdates();
        const bool isTerminal = failureAttempts == 1;
        (void)ports.app.view->SendViewUpdate(next);
        (void)ports.interaction.update->SendUpdates();
        failureCount += GetCaseResult(isTerminal && failureAttempts == 2
            && ports.app.view->GetViewState().mode == finalMode,
            "Terminal mode failure stops polling builds and explicit same-mode request retries") ? 0 : 1;

        // 正式 load 的候选仍用相同事务号；普通 refresh 无权代为提交。
        next.mode = VizMode::IsoSurface;
        (void)ports.app.view->SendViewUpdate(next);
        constexpr std::uint64_t transaction = 73;
        const auto snapshot = data->GetPrimaryImage();
        const auto started = ports.dataStage->StartDataStage(snapshot, transaction);
        next.mode = VizMode::SliceLeft_right;
        (void)ports.app.view->SendViewUpdate(next);
        (void)ports.interaction.update->SendPendingUpdates();
        const bool isOwned = ports.dataStage->GetDataStageStatus(transaction) == DataStageStatus::Preparing;
        (void)ports.dataStage->SetDataStageReady(snapshot, transaction);
        const auto ready = ports.dataStage->SetDataStageReady(snapshot, transaction);
        const bool isCommitted = ready == DataStageStatus::Ready
            && ports.dataStage->SetViewStage(snapshot, transaction);
        const bool isNewMode = ports.app.view->GetViewState().mode == VizMode::SliceLeft_right;
        bool isFinalizeReady = false;
        if (isCommitted) {
            next.mode = VizMode::SliceFront_back;
            (void)ports.app.view->SendViewUpdate(next);
            isFinalizeReady = ports.dataStage->SetDataStageReady(snapshot, transaction) == DataStageStatus::Ready;
            ports.dataStage->SetDataStageComplete(transaction);
            (void)ports.interaction.update->SendUpdates();
        }
        else (void)ports.dataStage->ClearDataStage(transaction);
        failureCount += GetCaseResult(started == DataStageStatus::Preparing && isOwned && isCommitted
            && isNewMode && isFinalizeReady
            && ports.app.view->GetViewState().mode == VizMode::SliceFront_back,
            "Display supersession preserves load revision and lets already committed stages finalize") ? 0 : 1;

        const auto context = CreateViewContext(ports.interaction, true);
        auto probeWindow = vtkSmartPointer<RenderWindowProbe>::New();
        probeWindow->SetReadyForRendering(false);
        const bool isWindowSet = context && context->SetRenderWindow(probeWindow);
        const bool isDeferred = isWindowSet && !context->SendRender() && probeWindow->renderCount == 0;
        probeWindow->SetReadyForRendering(true);
        probeWindow->isSkipped = true;
        const bool isSkipped = !context->SendRender();
        probeWindow->isSkipped = false;
        probeWindow->hasError = true;
        const bool isError = !context->SendRender();
        probeWindow->hasError = false;
        probeWindow->isThrowing = true;
        bool isCaught = false;
        try { (void)context->SendRender(); } catch (...) { isCaught = true; }
        probeWindow->isThrowing = false;
        failureCount += GetCaseResult(isDeferred && isSkipped && isError && isCaught && context->SendRender(),
            "Render completion requires readiness and EndEvent; error/exception observers are cleaned") ? 0 : 1;
    }
    return failureCount;
}

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

int GetOptionalCpuBudgetFailCount()
{
    int failureCount = 0;
    for (const auto budget : { std::uint64_t{ 256 }, std::uint64_t{ 64 } }) {
        auto lane = std::make_shared<ManualRenderLane>();
        RenderResourceCoordinator resources(
            [lane](RenderLaneWork work) {
                return lane->Start(std::move(work));
            });
        const bool isBudgetSet = resources.SetCpuBudgetBytes(budget);
        auto channel = resources.CreateTaskChannel(
            RenderProductKind::IsoSurface);
        bool isInitialSet = false;
        bool isOptionalSet = false;
        bool isNotStopped = false;
        bool hasNoFailure = false;
        bool hasExpectedRunningBytes = false;
        bool isFinalSet = false;
        RenderTaskRequest request;
        request.requestRevision = 1;
        request.estimatedBytes = 16;
        request.work = [&](RenderTaskToken token) {
            isInitialSet = token.SetActualBytes(32);
            isOptionalSet = token.TrySetActualBytes(128);
            const auto afterOptional = resources.GetResourceState();
            isNotStopped = !token.GetIsStopped();
            hasNoFailure = channel->GetState().failureReason
                == RenderProductFailure::None;
            hasExpectedRunningBytes = afterOptional.runningBytes
                == (budget == 256 ? 128 : 32);
            isFinalSet = token.SetActualBytes(32);
        };
        const bool isAccepted = channel->StartTask(std::move(request))
            == RenderTaskAdmission::Accepted;
        const bool isSent = lane->SendOne();
        const auto ready = channel->GetState();
        const auto readyResources = resources.GetResourceState();
        const bool isActive = channel->SetActiveBytes(1, 32)
            && channel->CompleteActiveBytes(1);
        const auto active = channel->GetState();
        const auto activeResources = resources.GetResourceState();
        const bool isStopped = channel->Stop();
        channel.reset();
        const auto stopped = resources.GetResourceState();
        const bool isEmpty = stopped.activeBytes == 0
            && stopped.pendingBytes == 0
            && stopped.runningBytes == 0
            && stopped.cacheBytes == 0;
        failureCount += GetCaseResult(
            isBudgetSet && isAccepted && isSent && isInitialSet
                && isOptionalSet == (budget == 256)
                && isNotStopped && hasNoFailure && hasExpectedRunningBytes
                && isFinalSet && ready.status == RenderProductStatus::Ready
                && readyResources.pendingBytes == 32
                && isActive && active.stats.activeBytes == 32
                && activeResources.activeBytes == 32
                && activeResources.pendingBytes == 0
                && activeResources.runningBytes == 0
                && isStopped && isEmpty,
            budget == 256
                ? "Optional Iso reservation succeeds and returns to the mesh byte count"
                : "Optional Iso reservation can fail without cancelling or rejecting the mesh")
            ? 0 : 1;
    }

    {
        auto lane = std::make_shared<ManualRenderLane>();
        RenderResourceCoordinator resources(
            [lane](RenderLaneWork work) {
                return lane->Start(std::move(work));
            });
        const bool isBudgetSet = resources.SetCpuBudgetBytes(64);
        auto channel = resources.CreateTaskChannel(
            RenderProductKind::IsoSurface);
        bool isHardReject = false;
        bool isStopped = false;
        RenderTaskRequest request;
        request.requestRevision = 1;
        request.estimatedBytes = 16;
        request.work = [&](RenderTaskToken token) {
            isHardReject = !token.SetActualBytes(128);
            isStopped = token.GetIsStopped();
        };
        const bool isAccepted = channel->StartTask(std::move(request))
            == RenderTaskAdmission::Accepted;
        const bool isSent = lane->SendOne();
        const auto state = channel->GetState();
        (void)resources.StartStop();
        const bool isStopCompleted = resources.Stop(
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        const auto stopped = resources.GetResourceState();
        failureCount += GetCaseResult(
            isBudgetSet && isAccepted && isSent && isHardReject && isStopped
                && state.failureReason == RenderProductFailure::ResourceRejected
                && isStopCompleted && stopped.activeBytes == 0
                && stopped.pendingBytes == 0 && stopped.runningBytes == 0
                && stopped.cacheBytes == 0,
            "SetActualBytes keeps its hard ResourceRejected behavior")
            ? 0 : 1;
    }

    {
        auto lane = std::make_shared<ManualRenderLane>();
        RenderResourceCoordinator resources(
            [lane](RenderLaneWork work) {
                return lane->Start(std::move(work));
            });
        auto channel = resources.CreateTaskChannel(
            RenderProductKind::IsoSurface);
        std::promise<void> readyPromise;
        auto readyFuture = readyPromise.get_future();
        std::promise<void> goPromise;
        auto goFuture = goPromise.get_future();
        std::promise<void> budgetDonePromise;
        auto budgetDoneFuture = budgetDonePromise.get_future();
        bool isOptionalSet = false;
        bool isFinalSet = false;
        bool isNotStopped = false;
        RenderTaskRequest request;
        request.requestRevision = 1;
        request.estimatedBytes = 16;
        request.work = [&](RenderTaskToken token) {
            readyPromise.set_value();
            goFuture.wait();
            isOptionalSet = token.TrySetActualBytes(128);
            budgetDoneFuture.wait();
            isFinalSet = token.SetActualBytes(16);
            isNotStopped = !token.GetIsStopped();
        };
        const bool isAccepted = channel->StartTask(std::move(request))
            == RenderTaskAdmission::Accepted;
        bool isSent = false;
        std::thread worker([&] {
            isSent = lane->SendOne();
            if (!isSent) readyPromise.set_value();
        });
        readyFuture.wait();
        goPromise.set_value();
        const bool budgetChanged = resources.SetCpuBudgetBytes(64);
        budgetDonePromise.set_value();
        worker.join();
        const auto state = channel->GetState();
        const auto beforeStop = resources.GetResourceState();
        (void)resources.StartStop();
        const bool isStopCompleted = resources.Stop(
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        const auto stopped = resources.GetResourceState();
        failureCount += GetCaseResult(
            isAccepted && isSent && (isOptionalSet != budgetChanged) && isFinalSet
                && isNotStopped && state.status == RenderProductStatus::Ready
                && state.failureReason == RenderProductFailure::None
                && state.stats.candidateBytes == 16
                && beforeStop.pendingBytes == 16
                && beforeStop.runningBytes == 0
                && isStopCompleted && stopped.activeBytes == 0
                && stopped.pendingBytes == 0 && stopped.runningBytes == 0
                && stopped.cacheBytes == 0,
            "Optional Iso reservation and concurrent budget change are mutually exclusive")
            ? 0 : 1;
    }

    {
        auto lane = std::make_shared<ManualRenderLane>();
        RenderResourceCoordinator resources(
            [lane](RenderLaneWork work) {
                return lane->Start(std::move(work));
            });
        auto channel = resources.CreateTaskChannel(
            RenderProductKind::IsoSurface);
        bool isOldOptionalSet = false;
        bool isReplacementAccepted = false;
        bool isOldCancelled = false;
        bool isLateOldOptionalRejected = false;
        bool isReplacementSet = false;
        RenderTaskRequest first;
        first.requestRevision = 1;
        first.estimatedBytes = 16;
        first.work = [&](RenderTaskToken token) {
            isOldOptionalSet = token.TrySetActualBytes(128);
            RenderTaskRequest replacement;
            replacement.requestRevision = 2;
            replacement.estimatedBytes = 16;
            replacement.work = [&](RenderTaskToken nextToken) {
                isReplacementSet = nextToken.SetActualBytes(32);
            };
            isReplacementAccepted = channel->StartTask(
                std::move(replacement)) == RenderTaskAdmission::Accepted;
            isLateOldOptionalRejected = !token.TrySetActualBytes(256);
            isOldCancelled = token.GetIsStopped();
        };
        const bool isAccepted = channel->StartTask(std::move(first))
            == RenderTaskAdmission::Accepted;
        const bool isOldSent = lane->SendOne();
        const bool isReplacementStarted = resources.SendTasks();
        const bool isReplacementSent = lane->SendOne();
        const auto ready = channel->GetState();
        const auto beforeStop = resources.GetResourceState();
        (void)resources.StartStop();
        const bool isStopCompleted = resources.Stop(
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        const auto stopped = resources.GetResourceState();
        failureCount += GetCaseResult(
            isAccepted && isOldSent && isOldOptionalSet
                && isReplacementAccepted && isLateOldOptionalRejected
                && isOldCancelled && isReplacementStarted
                && isReplacementSent && isReplacementSet
                && ready.status == RenderProductStatus::Ready
                && ready.stats.requestRevision == 2
                && ready.stats.candidateBytes == 32
                && beforeStop.pendingBytes == 32
                && beforeStop.runningBytes == 0
                && isStopCompleted && stopped.activeBytes == 0
                && stopped.pendingBytes == 0 && stopped.runningBytes == 0
                && stopped.cacheBytes == 0,
            "Replacing an Iso task releases its optional reservation")
            ? 0 : 1;
    }

    {
        auto lane = std::make_shared<ManualRenderLane>();
        RenderResourceCoordinator resources(
            [lane](RenderLaneWork work) {
                return lane->Start(std::move(work));
            });
        auto channel = resources.CreateTaskChannel(
            RenderProductKind::IsoSurface);
        bool isOptionalSet = false;
        bool isStopStarted = false;
        bool isStopped = false;
        bool isLateOptionalRejected = false;
        RenderTaskRequest request;
        request.requestRevision = 1;
        request.estimatedBytes = 16;
        request.work = [&](RenderTaskToken token) {
            isOptionalSet = token.TrySetActualBytes(128);
            isStopStarted = resources.StartStop();
            isStopped = token.GetIsStopped();
            isLateOptionalRejected = !token.TrySetActualBytes(256);
        };
        const bool isAccepted = channel->StartTask(std::move(request))
            == RenderTaskAdmission::Accepted;
        const bool isSent = lane->SendOne();
        const bool isStopCompleted = resources.Stop(
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        const auto stopped = resources.GetResourceState();
        failureCount += GetCaseResult(
            isAccepted && isSent && isOptionalSet && isStopStarted
                && isStopped && isLateOptionalRejected && isStopCompleted
                && stopped.activeBytes == 0 && stopped.pendingBytes == 0
                && stopped.runningBytes == 0 && stopped.cacheBytes == 0,
            "Stop cancels optional Iso reservations and clears resource counters")
            ? 0 : 1;
    }
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
    request.key.inputStamp = { GetTestDataRef(1) };
    request.key.inputIdentity = image.GetPointer();
    request.key.inputMTime = image->GetMTime();
    request.key.inputScalarMTime =
        image->GetPointData()->GetScalars()->GetMTime();
    request.key.outputDimensions = { 12, 12, 12 };
    request.key.isoValue = isoValue;
    return request;
}

int GetInitialIsoStageFailCount()
{
    int failures = 0;
    for (const bool hasExplicitIso : {false, true}) {
        auto data = std::make_shared<TransitionData>();
        const bool seeded = data->SetOwnedImage(BuildIsoImage());
        const auto snapshot = data->GetPrimaryImage();
        auto lane = std::make_shared<ManualRenderLane>();
        auto resources = std::make_shared<RenderResourceCoordinator>(
            [lane](RenderLaneWork work) { return lane->Start(std::move(work)); });
        auto services = std::make_shared<RenderStrategyServices>();
        services->resources = resources;
        services->isHostDriven = true;
        auto events = std::make_shared<SharedStateBroadcaster>();
        AppServiceArgs args;
        args.dataManager = data;
        args.interactionState = std::make_shared<SharedInteractionState>(events);
        args.eventSource = events;
        args.renderServices = services;
        auto ports = CreateAppPorts(std::move(args));
        auto window = vtkSmartPointer<vtkRenderWindow>::New();
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        bool valid = seeded && snapshot && ports.renderBind->SetRenderTarget(window, renderer);
        PreInitConfig config;
        config.vizMode = VizMode::IsoSurface;
        config.hasIso = hasExplicitIso;
        config.isoThreshold = 0.25;
        AppViewUpdate quality;
        quality.volumeQuality = VolumeQuality::Ultra;
        valid = ports.app.view->SetViewConfig(config)
            && ports.app.view->SendViewUpdate(quality) && valid;
        double range[2] = {};
        snapshot->image->GetScalarRange(range);
        const auto expected = hasExplicitIso ? config.isoThreshold : range[0] * 0.5 + range[1] * 0.5;
        constexpr std::uint64_t transaction = 81;
        valid = ports.dataStage->StartDataStage(snapshot, transaction) == DataStageStatus::Preparing
            && lane->GetStartCount() == 1 && lane->SendOne() && valid;
        valid = ports.dataStage->SetDataStageReady(snapshot, transaction) == DataStageStatus::Ready && valid;
        auto key = GetIsoRequest(snapshot->image, 1, VolumeQuality::Ultra, expected).key;
        key.inputStamp = {snapshot->data->self};
        const auto product = resources->GetIsoSurfaceProduct(key);
        auto zeroKey = key;
        zeroKey.isoValue = 0.0;
        valid = product && product->isoValue == expected
            && !resources->GetIsoSurfaceProduct(zeroKey)
            && resources->GetResourceState().productBuildCount == 1 && valid;
        // 模拟跨视图事务补偿：默认值不能在正式数据发布前写回。
        valid = ports.dataStage->SetViewStage(snapshot, transaction)
            && ports.dataStage->ResetViewStage(transaction)
            && ports.dataStage->ClearDataStage(transaction)
            && ports.app.view->GetViewState().isoThreshold == (hasExplicitIso ? config.isoThreshold : 0.0)
            && ports.app.view->GetViewState().hasExplicitIso == hasExplicitIso && valid;
        const bool committed = ports.dataStage->StartDataStage(snapshot, transaction + 1) == DataStageStatus::Preparing
            && ports.dataStage->SetDataStageReady(snapshot, transaction + 1) == DataStageStatus::Ready
            && ports.dataStage->SetViewStage(snapshot, transaction + 1);
        valid = committed && valid;
        if (committed) {
            ports.dataStage->SetDataStageComplete(transaction + 1);
            (void)ports.interaction.update->SendUpdates();
        }
        valid = ports.app.view->GetViewState().isoThreshold == expected
            && ports.app.view->GetViewState().hasExplicitIso == hasExplicitIso
            && lane->GetStartCount() == 1
            && resources->GetResourceState().productBuildCount == 1 && valid;
        if (!hasExplicitIso) {
            const auto old = ports.app.view->GetViewState();
            AppViewUpdate explicitIso;
            explicitIso.isoThreshold = 0.0;
            valid = ports.app.view->SendViewUpdate(explicitIso)
                && ports.app.view->SetViewState(old, ports.app.view->GetViewState().revision)
                && !ports.app.view->GetViewState().hasExplicitIso && valid;
        }
        auto replacement = BuildIsoImage();
        auto* scalars = replacement->GetPointData()->GetScalars();
        for (vtkIdType index = 0; index < scalars->GetNumberOfTuples(); ++index)
            scalars->SetComponent(index, 0, scalars->GetComponent(index, 0) + 100.0);
        scalars->Modified(); replacement->Modified();
        valid = data->SetOwnedImage(replacement) && valid;
        const auto replacementSnapshot = data->GetPrimaryImage();
        const auto nextExpected = hasExplicitIso ? expected : expected + 100.0;
        valid = ports.dataStage->StartDataStage(replacementSnapshot, transaction + 2) == DataStageStatus::Preparing
            && lane->SendOne()
            && ports.dataStage->SetDataStageReady(replacementSnapshot, transaction + 2) == DataStageStatus::Ready
            && ports.dataStage->ClearDataStage(transaction + 2)
            && ports.app.view->GetViewState().isoThreshold == expected && valid;
        const bool replaced = ports.dataStage->StartDataStage(replacementSnapshot, transaction + 3) == DataStageStatus::Preparing
            && ports.dataStage->SetDataStageReady(replacementSnapshot, transaction + 3) == DataStageStatus::Ready
            && ports.dataStage->SetViewStage(replacementSnapshot, transaction + 3);
        if (replaced) ports.dataStage->SetDataStageComplete(transaction + 3);
        valid = replaced && ports.app.view->GetViewState().isoThreshold == nextExpected
            && ports.app.view->GetViewState().hasExplicitIso == hasExplicitIso && valid;
        failures += GetCaseResult(valid, hasExplicitIso
            ? "Explicit initial ISO builds once and survives data-stage rollback"
            : "First ISO product uses native midpoint without a zero-ISO cache; rollback preserves automatic intent") ? 0 : 1;
    }
    return failures;
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

    if (buildResult.product && buildResult.product->surface) {
        // BuildIsoImage 在网格边上的零交点由 4.75 / (4.75 + 3.25) 线性插值确定。
        const std::array<double, 6> expected{1.59375, 9.40625, 1.59375, 9.40625, 1.59375, 9.40625};
        double bounds[6]{};
        buildResult.product->surface->GetCellsBounds(bounds);
        const bool isCorrect = std::equal(expected.begin(), expected.end(), bounds);
        auto changed = vtkSmartPointer<vtkPolyData>::New();
        changed->DeepCopy(buildResult.product->surface);
        changed->ComputeCellsBounds();
        for (vtkIdType index = 0; index < changed->GetNumberOfPoints(); ++index) {
            double point[3];
            changed->GetPoint(index, point);
            point[0] += 2.0;
            changed->GetPoints()->SetPoint(index, point);
        }
        changed->GetPoints()->Modified();
        changed->GetCellsBounds(bounds);
        auto translated = expected;
        translated[0] += 2.0; translated[1] += 2.0;
        failureCount += GetCaseResult(isCorrect
            && std::equal(translated.begin(), translated.end(), bounds),
            "Iso cell bounds preserve analytic grid intersections and invalidate after private geometry changes") ? 0 : 1;
    }

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
        GetTestDataRef(1) });
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
            && active.stats.cpuBindUs > 0 && active.stats.gpuUploadUs == 0
            && active.stats.firstRenderUs == 17,
        "Iso Low to Ultra to High commits only the latest valid revision")
        ? 0 : 1;

    const std::size_t startsBeforeDrag = lane->GetStartCount();
    const auto buildsBeforeDrag = resources->GetResourceState().productBuildCount;
    bool allAccepted = true;
    for (std::uint64_t index = 0; index < 100; ++index) {
        params.isoValue = static_cast<double>(index) * 0.01;
        allAccepted = strategy.SetVisualState(params, UpdateFlags::IsoValue) && allAccepted;
    }
    const bool oneQueued = lane->GetPendingCount() == 1 && lane->GetStartCount() == startsBeforeDrag + 1;
    (void)lane->SendOne(); // 被替换的首项不会执行昂贵构建。
    (void)resources->SendTasks();
    (void)lane->SendOne();
    const bool latestCommitted = strategy.SetProductCommit();
    const auto finalState = strategy.GetTransitionState();
    auto latest = GetIsoRequest(image, finalState.stats.requestRevision, VolumeQuality::High, params.isoValue);
    latest.key.outputDimensions = strategy.GetLodDimensions(VolumeQuality::High);
    const auto finalProduct = resources->GetIsoSurfaceProduct(latest.key);
    failureCount += GetCaseResult(allAccepted && oneQueued && latestCommitted
        && finalState.status == RenderProductStatus::Active && finalProduct && finalProduct->isoValue == .99
        && finalProduct->surface == mapper->GetInput()
        && resources->GetResourceState().productBuildCount == buildsBeforeDrag + 1,
        "One hundred ISO updates admit and commit the final threshold with one actual build") ? 0 : 1;
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
    request.key.inputStamp = { GetTestDataRef(1) };
    request.key.inputIdentity = image.GetPointer();
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

int GetIsoIndexProfileFailCount()
{
    const auto getEnvironment = [](const char* name) {
        char* buffer = nullptr; std::size_t count = 0;
        if (_dupenv_s(&buffer, &count, name) != 0) return std::string{};
        std::unique_ptr<char, decltype(&std::free)> value(buffer, &std::free);
        return value ? std::string(value.get()) : std::string{};
    };
    const auto path = getEnvironment("MVVCVTK_TEST_ISO_PROFILE_RAW");
    if (path.empty()) return 0;
    const auto dimensionsText = getEnvironment("MVVCVTK_TEST_ISO_PROFILE_DIMENSIONS");
    const auto spacingText = getEnvironment("MVVCVTK_TEST_ISO_PROFILE_SPACING");
    const auto originText = getEnvironment("MVVCVTK_TEST_ISO_PROFILE_ORIGIN");
    const auto isoText = getEnvironment("MVVCVTK_TEST_ISO_PROFILE_VALUE");
    if (dimensionsText.empty() || spacingText.empty() || originText.empty() || isoText.empty())
        return GetCaseResult(false, "Explicit Iso profile requires dimensions, spacing, origin and scalar threshold") ? 0 : 1;
    try {
        std::string dimensionsValue(dimensionsText), spacingValue(spacingText), originValue(originText);
        std::replace(dimensionsValue.begin(), dimensionsValue.end(), ',', ' ');
        std::replace(spacingValue.begin(), spacingValue.end(), ',', ' ');
        std::replace(originValue.begin(), originValue.end(), ',', ' ');
        std::istringstream dimensionsInput(dimensionsValue), spacingInput(spacingValue), originInput(originValue);
        std::array<int, 3> dimensions{}; std::array<double, 3> spacing{}, origin{};
        dimensionsInput >> dimensions[0] >> dimensions[1] >> dimensions[2];
        spacingInput >> spacing[0] >> spacing[1] >> spacing[2];
        originInput >> origin[0] >> origin[1] >> origin[2];
        const double iso = std::stod(isoText);
        const auto count = RenderWorkBudget::GetVoxelCount(dimensions);
        if (!dimensionsInput || !spacingInput || !originInput || !count || !std::isfinite(iso)
            || std::any_of(spacing.begin(), spacing.end(), [](double value) {return !std::isfinite(value) || value <= 0;})
            || std::any_of(origin.begin(), origin.end(), [](double value) {return !std::isfinite(value);})
            || *count > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()) / sizeof(float))
            return GetCaseResult(false, "Explicit Iso profile parameters are valid") ? 0 : 1;
        const auto bytes = *count * sizeof(float);
        std::ifstream stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() != static_cast<std::streamoff>(bytes))
            return GetCaseResult(false, "Explicit Iso profile has exact Float32 RAW storage") ? 0 : 1;
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(dimensions.data()); image->SetSpacing(spacing.data()); image->SetOrigin(origin.data());
        image->AllocateScalars(VTK_FLOAT, 1);
        stream.seekg(0); stream.read(static_cast<char*>(image->GetScalarPointer()), static_cast<std::streamsize>(bytes));
        if (!stream) return GetCaseResult(false, "Explicit Iso profile reads the complete RAW input") ? 0 : 1;
        image->GetPointData()->GetScalars()->Modified(); image->Modified();
        auto request = GetIsoRequest(image, 1, VolumeQuality::Ultra, iso);
        request.key.outputDimensions = dimensions;
        auto lane = std::make_shared<ManualRenderLane>();
        RenderResourceCoordinator resources([lane](RenderLaneWork work) {return lane->Start(std::move(work));});
        auto channel = resources.CreateTaskChannel(RenderProductKind::IsoSurface);
        IsoSurfaceBuildResult result;
        RenderTaskRequest work; work.requestRevision = 1;
        work.estimatedBytes = *IsoSurfaceProductBuilder::GetEstimatedBytes(request);
        work.work = [&](RenderTaskToken token) {result = IsoSurfaceProductBuilder().BuildProduct(request, token);};
        const auto admission = channel->StartTask(std::move(work));
        const bool sent = lane->SendOne();
        const bool valid = admission == RenderTaskAdmission::Accepted && sent && result.product
            && result.product->surface && result.product->triangleIndicesTime != 0;
        if (valid) {
            std::cout << "ISO_INDEX_PROFILE dimensions=" << dimensions[0] << ',' << dimensions[1] << ',' << dimensions[2]
                << " iso=" << iso << " points=" << result.product->surface->GetNumberOfPoints()
                << " polygons=" << result.product->surface->GetNumberOfPolys()
                << " emitted_triangles=" << result.product->triangleIndices.size() / 3
                << " index_capacity_bytes=" << result.product->triangleIndices.capacity() * sizeof(unsigned int)
                << " mesh_bytes=" << static_cast<std::uint64_t>(result.product->surface->GetActualMemorySize()) * 1024
                << " product_bytes=" << result.product->actualBytes << '\n';
        }
        result.product.reset(); (void)channel->Stop(); channel.reset();
        const auto released = resources.GetResourceState();
        return GetCaseResult(valid && released.runningBytes == 0 && released.pendingBytes == 0,
            "Explicit private Iso profile records actual triangle storage and releases its product") ? 0 : 1;
    }
    catch (...) {
        return GetCaseResult(false, "Explicit Iso profile completes without an exception") ? 0 : 1;
    }
}

int GetIsoIndexFailCount()
{
    int failures = 0;
    auto image = BuildIsoImage();
    const auto request = GetIsoRequest(image, 1, VolumeQuality::Ultra, 0.0);
    const auto prepared = IsoSurfaceProductBuilder().BuildProduct(request, RenderTaskToken{});
    if (!prepared.product || !prepared.product->surface) {
        return GetCaseResult(false, "Iso index fixture builds a complete CPU product") ? 0 : 1;
    }
    const auto meshBytes = static_cast<std::uint64_t>(prepared.product->surface->GetActualMemorySize()) * 1024;
    std::vector<unsigned int> reference;
    vtkOpenGLIndexBufferObject::AppendTriangleIndexBuffer(reference,
        prepared.product->surface->GetPolys(), prepared.product->surface->GetPoints(), 0, nullptr, nullptr);
    failures += GetCaseResult(!reference.empty()
        && prepared.product->triangleIndices == reference
        && prepared.product->triangleIndicesTime == prepared.product->surface->GetMeshMTime()
        && prepared.product->actualBytes == meshBytes + prepared.product->triangleIndices.capacity() * sizeof(unsigned int),
        "Iso batches preserve VTK triangle order, unsigned index type and zero vertex offset with exact retained bytes") ? 0 : 1;

    auto budgetLane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator budgetResources([budgetLane](RenderLaneWork work) {return budgetLane->Start(std::move(work));});
    auto budgetChannel = budgetResources.CreateTaskChannel(RenderProductKind::IsoSurface);
    bool wasBudgetSet = false;
    IsoSurfaceBuildResult fallback;
    RenderTaskRequest limited; limited.requestRevision = 1;
    limited.estimatedBytes = *IsoSurfaceProductBuilder::GetEstimatedBytes(request);
    // 等 mesh 已按旧路径计费后启用恰好容纳它的限制，确定性拒绝可选缓存。
    limited.onNeeded = [&] {
        if (!wasBudgetSet && budgetChannel->GetState().stats.candidateBytes == meshBytes) {
            wasBudgetSet = budgetResources.SetCpuBudgetBytes(budgetResources.GetResourceState().runningBytes);
        }
        return true;
    };
    limited.work = [&](RenderTaskToken token) {fallback = IsoSurfaceProductBuilder().BuildProduct(request, token);};
    const auto admitted = budgetChannel->StartTask(std::move(limited));
    const bool wasSent = budgetLane->SendOne();
    failures += GetCaseResult(admitted == RenderTaskAdmission::Accepted && wasSent && wasBudgetSet
        && fallback.product && fallback.failureReason == RenderProductFailure::None
        && fallback.product->triangleIndicesTime == 0 && fallback.product->triangleIndices.empty()
        && fallback.product->actualBytes == meshBytes
        && budgetChannel->GetState().status == RenderProductStatus::Ready,
        "An insufficient optional Iso budget keeps the original mesh product successful") ? 0 : 1;

    auto cancelledLane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator cancelledResources([cancelledLane](RenderLaneWork work) {return cancelledLane->Start(std::move(work));});
    auto cancelledChannel = cancelledResources.CreateTaskChannel(RenderProductKind::IsoSurface);
    bool sawReservation = false;
    IsoSurfaceBuildResult cancelled;
    RenderTaskRequest cancelWork; cancelWork.requestRevision = 1;
    cancelWork.estimatedBytes = *IsoSurfaceProductBuilder::GetEstimatedBytes(request);
    cancelWork.onNeeded = [&] {
        sawReservation = cancelledChannel->GetState().stats.candidateBytes > meshBytes;
        return !sawReservation;
    };
    cancelWork.work = [&](RenderTaskToken token) {cancelled = IsoSurfaceProductBuilder().BuildProduct(request, token);};
    const auto cancelAdmission = cancelledChannel->StartTask(std::move(cancelWork));
    const bool wasCancelSent = cancelledLane->SendOne();
    const auto cancelState = cancelledResources.GetResourceState();
    failures += GetCaseResult(cancelAdmission == RenderTaskAdmission::Accepted && wasCancelSent && sawReservation
        && !cancelled.product && cancelled.failureReason == RenderProductFailure::Cancelled
        && cancelState.runningBytes == 0 && cancelState.pendingBytes == 0,
        "Cancellation after an Iso index reservation discards the private candidate and returns its bytes") ? 0 : 1;

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1); window->SetSize(64, 64); window->AddRenderer(renderer);
    auto capture = vtkSmartPointer<vtkWindowToImageFilter>::New();
    capture->SetInput(window); capture->ReadFrontBufferOff(); capture->SetInputBufferTypeToRGBA();
    const auto pixels = [&] {
        window->Render(); window->WaitForCompletion(); capture->Modified(); capture->Update();
        auto* values = capture->GetOutput()->GetPointData()->GetScalars();
        const auto* begin = static_cast<const unsigned char*>(values->GetVoidPointer(0));
        return std::vector<unsigned char>(begin, begin + values->GetNumberOfValues());
    };
    const auto checkRender = [&](const std::shared_ptr<const IsoSurfaceProduct>& product, const bool checkModes) {
        if (!product) return false;
        auto lane = std::make_shared<ManualRenderLane>();
        auto resources = std::make_shared<RenderResourceCoordinator>([lane](RenderLaneWork work) {return lane->Start(std::move(work));});
        auto services = std::make_shared<RenderStrategyServices>(); services->resources = resources;
        IsoSurfaceStrategy strategy(services);
        RenderParams params; params.volumeQuality = VolumeQuality::Ultra; params.isoValue = 0.0;
        bool valid = resources->SetIsoSurfaceProduct(request.key, product)
            && strategy.SetRenderInputStamp(request.key.inputStamp)
            && strategy.SetVisualState(params, UpdateFlags::Quality | UpdateFlags::IsoValue)
            && strategy.SetInputData(image, nullptr) && strategy.SetProductCommit();
        auto* actor = vtkActor::SafeDownCast(strategy.GetMainProp());
        if (!actor) return false;
        auto optimized = vtkSmartPointer<vtkMapper>(actor->GetMapper());
        auto legacy = vtkSmartPointer<vtkOpenGLPolyDataMapper>::New();
        legacy->SetInputData(product->surface); legacy->ScalarVisibilityOff();
        legacy->SetVBOShiftScaleMethod(vtkOpenGLPolyDataMapper::DISABLE_SHIFT_SCALE);
        actor->GetProperty()->SetColor(.2, .7, .9);
        strategy.AttachRenderer(renderer);
        renderer->GetActiveCamera()->SetPosition(20, 18, 22);
        renderer->ResetCamera();
        const auto samePixels = [&](const char* stage) {
            // 原生 mapper 的首次上下文/光照初始化也可能产生末位变化；两边同样
            // 初始化一次，再要求各自连续两帧及两路径之间逐字节相等。
            actor->SetMapper(legacy); (void)pixels(); const auto expected = pixels(); const auto legacyRepeat = pixels();
            actor->SetMapper(optimized); (void)pixels(); const auto actual = pixels(); const auto optimizedRepeat = pixels();
            bool hasVisiblePixel = false;
            for (std::size_t index = 0; index + 3 < expected.size(); index += 4)
                hasVisiblePixel = hasVisiblePixel || expected[index] || expected[index + 1] || expected[index + 2];
            const bool same = hasVisiblePixel && !expected.empty() && actual == expected
                && legacyRepeat == expected && optimizedRepeat == actual;
            if (!same) {
                std::size_t different = 0;
                for (std::size_t index = 0; index < std::min(expected.size(), actual.size()); ++index)
                    if (expected[index] != actual[index]) ++different;
                std::cerr << "ISO_INDEX_PIXEL_DIFF stage=" << stage << " visible=" << hasVisiblePixel
                    << " expected_bytes=" << expected.size() << " actual_bytes=" << actual.size()
                    << " differing_bytes=" << different << " legacy_repeat_equal=" << (expected == legacyRepeat)
                    << " optimized_repeat_equal=" << (actual == optimizedRepeat)
                    << " actual_matches_legacy_repeat=" << (actual == legacyRepeat) << '\n';
                for (std::size_t index = 0, shown = 0; index < std::min(expected.size(), actual.size()) && shown < 8; ++index) {
                    if (expected[index] != actual[index]) {
                        std::cerr << "ISO_INDEX_PIXEL_VALUE offset=" << index << " expected=" << int(expected[index])
                            << " actual=" << int(actual[index]) << '\n';
                        ++shown;
                    }
                }
            }
            return same;
        };
        valid = samePixels("surface") && valid;
        if (checkModes) {
            actor->GetProperty()->SetRepresentationToWireframe(); valid = samePixels("wireframe") && valid;
            actor->GetProperty()->SetRepresentationToSurface();
            actor->GetProperty()->EdgeVisibilityOn(); valid = samePixels("edges") && valid;
            actor->GetProperty()->EdgeVisibilityOff();
            actor->GetProperty()->VertexVisibilityOn(); valid = samePixels("vertices") && valid;
            actor->GetProperty()->VertexVisibilityOff(); valid = samePixels("surface-return") && valid;
            // GPU 资源被释放后，同一不可变 CPU 产品必须重新上传，不能沿用旧 handle。
            optimized->ReleaseGraphicsResources(window); valid = samePixels("resource-reset") && valid;
        }
        strategy.DetachRenderer(renderer);
        legacy->ReleaseGraphicsResources(window); optimized->ReleaseGraphicsResources(window);
        return valid;
    };
    failures += GetCaseResult(checkRender(prepared.product, true),
        "Prepared Iso indices and representation/edge/vertex/resource-reset fallbacks match nonempty legacy RGBA exactly") ? 0 : 1;
    failures += GetCaseResult(checkRender(fallback.product, false),
        "Optional Iso budget fallback renders exactly the same nonempty RGBA as the original mapper") ? 0 : 1;

    auto changed = vtkSmartPointer<vtkPolyData>::New(); changed->DeepCopy(prepared.product->surface);
    auto fewerCells = vtkSmartPointer<vtkCellArray>::New();
    auto* originalCells = changed->GetPolys();
    for (vtkIdType cell = 0; cell < originalCells->GetNumberOfCells(); cell += 4) {
        vtkIdType count = 0; const vtkIdType* ids = nullptr;
        originalCells->GetCellAtId(cell, count, ids); fewerCells->InsertNextCell(count, ids);
    }
    changed->SetPolys(fewerCells);
    auto stale = std::make_shared<IsoSurfaceProduct>(*prepared.product);
    stale->surface = changed;
    stale->actualBytes = static_cast<std::uint64_t>(changed->GetActualMemorySize()) * 1024
        + stale->triangleIndices.capacity() * sizeof(unsigned int);
    failures += GetCaseResult(stale->triangleIndicesTime != changed->GetMeshMTime()
        && checkRender(stale, false),
        "Changed private Iso topology invalidates prepared indices and preserves legacy RGBA") ? 0 : 1;

    auto leaseLane = std::make_shared<ManualRenderLane>();
    auto leaseResources = std::make_shared<RenderResourceCoordinator>([leaseLane](RenderLaneWork work) {return leaseLane->Start(std::move(work));});
    auto leaseServices = std::make_shared<RenderStrategyServices>(); leaseServices->resources = leaseResources;
    auto strategy = std::make_unique<IsoSurfaceStrategy>(leaseServices);
    RenderParams params; params.volumeQuality = VolumeQuality::Ultra; params.isoValue = 0.0;
    bool validLease = strategy->SetRenderInputStamp(request.key.inputStamp)
        && strategy->SetVisualState(params, UpdateFlags::Quality | UpdateFlags::IsoValue)
        && strategy->SetInputData(image, nullptr) && leaseLane->SendOne() && strategy->SetProductCommit();
    auto retained = leaseResources->GetIsoSurfaceProduct(request.key);
    std::weak_ptr<const IsoSurfaceProduct> weakProduct = retained;
    const auto retainedBytes = retained ? retained->actualBytes : 0;
    auto retainedActor = vtkSmartPointer<vtkActor>(vtkActor::SafeDownCast(strategy->GetMainProp()));
    retained.reset(); strategy.reset();
    (void)leaseResources->StartStop();
    const auto beforeRelease = leaseResources->GetResourceState();
    validLease = validLease && !weakProduct.expired() && beforeRelease.pendingBytes == retainedBytes;
    retainedActor = nullptr;
    const auto afterRelease = leaseResources->GetResourceState();
    failures += GetCaseResult(validLease && weakProduct.expired() && afterRelease.pendingBytes == 0
        && afterRelease.activeBytes == 0 && afterRelease.runningBytes == 0 && afterRelease.cacheBytes == 0,
        "An externally retained Iso actor owns its CPU indices until the final mapper reader releases them") ? 0 : 1;
    return failures + GetIsoIndexProfileFailCount();
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
            && nativeResult.product->volume != image
            && nativeResult.product->volume->GetScalarPointer() == image->GetScalarPointer()
            && nativeResult.product->volume->GetPointData()->GetScalars() != image->GetPointData()->GetScalars()
            && denoiseResult.product
            && denoiseResult.product->volume
            && denoiseResult.product->volume != image
            && denoiseResult.product->volume->GetNumberOfPoints()
                == image->GetNumberOfPoints(),
        "Volume Ultra shares native storage through private scalar metadata when denoise is disabled")
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

int GetVolumeRangeFailCount()
{
    int failures = 0;
    for (const int type : {VTK_FLOAT, VTK_DOUBLE, VTK_UNSIGNED_CHAR, VTK_SHORT, VTK_UNSIGNED_LONG_LONG}) {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(4, 2, 2);
        image->AllocateScalars(type, 3);
        auto* source = image->GetPointData()->GetScalars();
        for (vtkIdType tuple = 0; tuple < source->GetNumberOfTuples(); ++tuple) {
            source->SetComponent(tuple, 0, tuple +
                (type == VTK_UNSIGNED_CHAR || type == VTK_UNSIGNED_LONG_LONG ? 0.0 : -7.0));
            source->SetComponent(tuple, 1, tuple * 3.0);
            source->SetComponent(tuple, 2, 42.0);
        }
        if (type == VTK_FLOAT || type == VTK_DOUBLE) {
            source->SetComponent(0, 0, std::numeric_limits<double>::quiet_NaN());
            source->SetComponent(1, 0, -std::numeric_limits<double>::infinity());
            source->SetComponent(2, 0, std::numeric_limits<double>::infinity());
            for (vtkIdType tuple = 0; tuple < source->GetNumberOfTuples(); ++tuple) {
                source->SetComponent(tuple, 1, std::numeric_limits<double>::quiet_NaN());
                source->SetComponent(tuple, 2, tuple % 2 ? std::numeric_limits<double>::infinity()
                    : -std::numeric_limits<double>::infinity());
            }
        }
        source->Modified();
        auto reference = vtkSmartPointer<vtkDataArray>::Take(source->NewInstance());
        reference->DeepCopy(source);
        const auto sourceTime = source->GetMTime();
        const auto result = VolumeLodProductBuilder().BuildProduct(
            GetVolumeRequest(image, nullptr, 1, VolumeQuality::Ultra, {4, 2, 2}, false), RenderTaskToken{});
        auto* prepared = result.product ? result.product->volume->GetPointData()->GetScalars() : nullptr;
        bool valid = prepared && prepared != source && prepared->GetVoidPointer(0) == source->GetVoidPointer(0)
            && source->GetMTime() == sourceTime
            && !source->GetInformation()->Has(vtkAbstractArray::PER_FINITE_COMPONENT())
            && prepared->GetInformation()->Has(vtkAbstractArray::PER_COMPONENT())
            && prepared->GetInformation()->Has(vtkAbstractArray::PER_FINITE_COMPONENT());
        for (int component = 0; component < 3 && prepared; ++component) {
            double expected[2]{}, actual[2]{};
            reference->GetRange(expected, component); prepared->GetRange(actual, component);
            valid = std::equal(expected, expected + 2, actual) && valid;
            reference->GetFiniteRange(expected, component); prepared->GetFiniteRange(actual, component);
            valid = std::equal(expected, expected + 2, actual) && valid;
        }
        failures += GetCaseResult(valid,
            "Volume scalar candidates retain VTK ordinary/finite component semantics without changing published metadata") ? 0 : 1;
        if (prepared) {
            prepared->Modified();
            failures += GetCaseResult(!prepared->GetInformation()->Has(vtkAbstractArray::PER_COMPONENT())
                && !prepared->GetInformation()->Has(vtkAbstractArray::PER_FINITE_COMPONENT()),
                "VTK Modified invalidates both prepared component range caches") ? 0 : 1;
        }
    }
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(128, 128, 32);
    image->AllocateScalars(VTK_FLOAT, 1);
    image->GetPointData()->GetScalars()->Fill(3.0);
    auto request = GetVolumeRequest(image, nullptr, 1, VolumeQuality::Ultra, {128, 128, 32}, false);
    auto lane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator resources([lane](RenderLaneWork work) {return lane->Start(std::move(work));});
    auto channel = resources.CreateTaskChannel(RenderProductKind::VolumeLod);
    int checks = 0;
    VolumeLodBuildResult cancelled;
    RenderTaskRequest work; work.requestRevision = 1;
    // 在已进入构建后的协作检查点撤销需要者，不用 sleep 猜线程交错。
    work.onNeeded = [&] {return ++checks < 7;};
    work.work = [&](RenderTaskToken token) {cancelled = VolumeLodProductBuilder().BuildProduct(request, token);};
    const auto accepted = channel->StartTask(std::move(work));
    const auto sent = lane->SendOne();
    failures += GetCaseResult(accepted == RenderTaskAdmission::Accepted && sent && checks >= 7
        && !cancelled.product && cancelled.failureReason == RenderProductFailure::Cancelled
        && !image->GetPointData()->GetScalars()->GetInformation()->Has(vtkAbstractArray::PER_FINITE_COMPONENT()),
        "Cancellation during scalar preparation discards private range metadata and keeps the published input") ? 0 : 1;

    auto fallbackImage = vtkSmartPointer<vtkImageData>::New();
    fallbackImage->SetDimensions(32, 32, 32);
    auto fallbackValues = vtkSmartPointer<RangeReadProbe>::New();
    fallbackValues->SetNumberOfTuples(32 * 32 * 32);
    fallbackValues->Fill(128.0);
    fallbackImage->GetPointData()->SetScalars(fallbackValues);
    RangeReadProbe::shouldThrow = true;
    auto fallback = VolumeLodProductBuilder().BuildProduct(
        GetVolumeRequest(fallbackImage, nullptr, 2, VolumeQuality::Ultra, {32, 32, 32}, false), RenderTaskToken{});
    const bool wasFallback = !RangeReadProbe::shouldThrow && fallback.product
        && fallback.failureReason == RenderProductFailure::None
        && !fallback.product->volume->GetPointData()->GetScalars()->GetInformation()
            ->Has(vtkAbstractArray::PER_FINITE_COMPONENT());
    RangeReadProbe::shouldThrow = false;
    bool isRendered = false;
    if (fallback.product) {
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        auto window = vtkSmartPointer<vtkRenderWindow>::New();
        window->SetOffScreenRendering(1); window->SetSize(32, 32); window->AddRenderer(renderer);
        auto mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
        mapper->SetUseJittering(false);
        auto volume = vtkSmartPointer<vtkVolume>::New(); volume->SetMapper(mapper);
        auto color = vtkSmartPointer<vtkColorTransferFunction>::New();
        color->AddRGBPoint(0, .8, .4, .2); color->AddRGBPoint(255, .8, .4, .2);
        auto opacity = vtkSmartPointer<vtkPiecewiseFunction>::New();
        opacity->AddPoint(0, 0); opacity->AddPoint(255, 1);
        volume->GetProperty()->SetColor(color); volume->GetProperty()->SetScalarOpacity(opacity);
        renderer->AddVolume(volume); mapper->SetInputData(fallbackImage); renderer->ResetCamera();
        auto capture = vtkSmartPointer<vtkWindowToImageFilter>::New(); capture->SetInput(window);
        capture->ReadFrontBufferOff(); capture->SetInputBufferTypeToRGBA();
        window->Render(); window->WaitForCompletion(); capture->Update();
        auto* pixels = capture->GetOutput()->GetPointData()->GetScalars();
        const auto pixelBytes = static_cast<std::size_t>(pixels->GetNumberOfValues());
        const auto* before = static_cast<const unsigned char*>(pixels->GetVoidPointer(0));
        const std::vector<unsigned char> expected(before, before + pixelBytes);
        mapper->ReleaseGraphicsResources(window);
        mapper->SetInputData(fallback.product->volume);
        window->Render(); window->WaitForCompletion(); capture->Modified(); capture->Update();
        pixels = capture->GetOutput()->GetPointData()->GetScalars();
        double range[2]{}; fallback.product->volume->GetPointData()->GetScalars()->GetFiniteRange(range, 0);
        isRendered = range[0] == 128 && range[1] == 128
            && pixels->GetNumberOfValues() == static_cast<vtkIdType>(expected.size())
            && std::equal(expected.begin(), expected.end(), static_cast<unsigned char*>(pixels->GetVoidPointer(0)));
        renderer->RemoveVolume(volume); mapper->ReleaseGraphicsResources(window);
    }
    failures += GetCaseResult(wasFallback && isRendered,
        "Unavailable optional range cache uses the original owner path and renders identical pixels without rejecting display") ? 0 : 1;
    vtkWeakPointer<vtkDataArray> borrowed = fallbackValues;
    fallbackImage = nullptr; fallbackValues = nullptr;
    const bool hasOwner = borrowed.GetPointer() != nullptr;
    fallback.product.reset();
    failures += GetCaseResult(hasOwner && !borrowed.GetPointer(),
        "Native scalar storage stays owned until the last private candidate is released") ? 0 : 1;
    return failures;
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
        GetTestDataRef(1) });
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
            && active.stats.cpuBindUs > 0 && active.stats.gpuUploadUs == 0
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
    product->inputStamp = { GetTestDataRef(1) };
    product->requestedQuality = VolumeQuality::High;
    product->outputDimensions = { 12, 12, 12 };
    product->volume = image;
    product->actualBytes = bytes;
    return product;
}

int GetSharedBuildAndDefaultPolicyFailCount()
{
    int failures = 0;
    auto lane = std::make_shared<ManualRenderLane>();
    auto resources = std::make_shared<RenderResourceCoordinator>([lane](RenderLaneWork task) { return lane->Start(std::move(task)); });
    auto image = BuildIsoImage();
    auto first = resources->CreateTaskChannel(RenderProductKind::VolumeLod);
    auto second = resources->CreateTaskChannel(RenderProductKind::VolumeLod);
    auto request = GetVolumeRequest(image, nullptr, 1, VolumeQuality::High, {6, 6, 6}, false);
    request.resources = resources;
    int firstCalls = 0, secondCalls = 0;
    std::shared_ptr<const VolumeLodProduct> product;
    const auto a = resources->StartVolumeProduct(first, request, [&](auto, auto) { ++firstCalls; });
    request.requestRevision = 2;
    const auto b = resources->StartVolumeProduct(second, request, [&](auto result, auto) { ++secondCalls; product = result.product; });
    first->Stop();
    lane->SendOne();
    failures += GetCaseResult(a == RenderTaskAdmission::Accepted && b == RenderTaskAdmission::Replaced
        && lane->GetStartCount() == 1 && firstCalls == 0 && secondCalls == 1 && product
        && resources->GetVolumeProduct(request.key) == product
        && second->SetActiveBytes(2, product->actualBytes, product.get()) && second->CompleteActiveBytes(2),
        "Equal products build once; cancelling one waiter preserves others and CPU-ready cache") ? 0 : 1;
    auto isoRequest = GetIsoRequest(image, 3, VolumeQuality::High, 0.0);
    isoRequest.key.outputDimensions = {6, 6, 6}; isoRequest.resources = resources;
    auto iso = resources->CreateTaskChannel(RenderProductKind::IsoSurface);
    std::shared_ptr<const IsoSurfaceProduct> mesh;
    (void)resources->StartIsoSurfaceProduct(iso, isoRequest, [&](auto result, auto) { mesh = result.product; });
    lane->SendOne();
    auto scalar = resources->GetVolumeProduct(request.key);
    isoRequest.key.isoValue = 2.0; isoRequest.requestRevision = 4;
    (void)resources->StartIsoSurfaceProduct(iso, isoRequest, [&](auto result, auto) { mesh = result.product; });
    lane->SendOne();
    failures += GetCaseResult(mesh && scalar == product && resources->GetVolumeProduct(request.key) == product
        && resources->GetResourceState().scalarResampleBuildCount == 1
        && resources->GetResourceState().productBuildCount == 3
        && resources->GetResourceState().joinedBuildCount == 1,
        "ISO threshold changes reuse the same prepared scalar storage") ? 0 : 1;
    auto unrestricted = resources->CreateTaskChannel(RenderProductKind::VolumeLod);
    bool ran = false;
    RenderTaskRequest diagnostic;
    diagnostic.requestRevision = 1;
    diagnostic.estimatedBytes = std::numeric_limits<std::uint64_t>::max();
    diagnostic.work = [&](RenderTaskToken token) {
        ran = token.SetEstimatedBytes(std::numeric_limits<std::uint64_t>::max()) && token.SetActualBytes(16);
    };
    const auto accepted = unrestricted->StartTask(std::move(diagnostic)); lane->SendOne();
    failures += GetCaseResult(accepted == RenderTaskAdmission::Accepted && ran
        && unrestricted->SetActiveBytes(1, 16) && unrestricted->CompleteActiveBytes(1)
        && resources->GetResourceState().isCpuBudgetEnforced == false,
        "Default estimates never veto valid work or active commits") ? 0 : 1;
    unrestricted->Stop();
    failures += GetCaseResult(!unrestricted->SetCachedActive(2, 16), "Stopped channels cannot commit cached products") ? 0 : 1;
    return failures;
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
        isActualAdmitted = token.SetActualBytes(1200);
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
            && preparingResources.pendingBytes == 0
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
    const RenderInputStamp stamp{ GetTestDataRef(9) };
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

int GetWorkingSetFailCount()
{
    int failures = 0;
    auto image = BuildIsoImage();
    auto lane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator resources([lane](RenderLaneWork work) { return lane->Start(std::move(work)); });
    auto volume = VolumeLodBuildRequest{};
    volume.input = image;
    volume.requestRevision = 1;
    volume.key.outputDimensions = {2, 2, 2};
    const auto plainBytes = VolumeLodProductBuilder::GetEstimatedBytes(volume);
    volume.key.isDenoiseOn = true;
    const auto denoiseBytes = VolumeLodProductBuilder::GetEstimatedBytes(volume);
    auto channel = resources.CreateTaskChannel(RenderProductKind::VolumeLod);
    (void)resources.SetCpuBudgetBytes(*plainBytes + 1);
    VolumeLodBuildResult result;
    RenderTaskRequest task;
    task.requestRevision = 1;
    task.estimatedBytes = *plainBytes;
    task.work = [&](RenderTaskToken token) { result = VolumeLodProductBuilder().BuildProduct(volume, token); };
    const bool isStarted = channel->StartTask(std::move(task)) == RenderTaskAdmission::Accepted;
    (void)lane->SendOne();
    failures += GetCaseResult(denoiseBytes && *denoiseBytes > *plainBytes && isStarted
        && result.failureReason == RenderProductFailure::ResourceRejected && !result.product,
        "Low LOD admission still reserves full-resolution double diffusion workspace before Update") ? 0 : 1;

    auto isoLane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator isoResources([isoLane](RenderLaneWork work) { return isoLane->Start(std::move(work)); });
    const auto isoRequest = GetIsoRequest(image, 1, VolumeQuality::Ultra, 0);
    const auto imageBytes = IsoSurfaceProductBuilder::GetEstimatedBytes(isoRequest);
    (void)isoResources.SetCpuBudgetBytes(*imageBytes);
    auto isoChannel = isoResources.CreateTaskChannel(RenderProductKind::IsoSurface);
    IsoSurfaceBuildResult isoResult;
    task = {};
    task.requestRevision = 1;
    task.estimatedBytes = *imageBytes;
    task.work = [&](RenderTaskToken token) { isoResult = IsoSurfaceProductBuilder().BuildProduct(isoRequest, token); };
    const bool isIsoStarted = isoChannel->StartTask(std::move(task)) == RenderTaskAdmission::Accepted;
    (void)isoLane->SendOne();
    failures += GetCaseResult(isIsoStarted && !isoResult.product
        && isoResult.failureReason == RenderProductFailure::ResourceRejected,
        "Iso counting reserves mesh and coexisting workspace before FlyingEdges allocation") ? 0 : 1;

    auto holdLane = std::make_shared<ManualRenderLane>();
    RenderResourceCoordinator holdResources([holdLane](RenderLaneWork work) { return holdLane->Start(std::move(work)); });
    (void)holdResources.SetCpuBudgetBytes(1000);
    auto holdChannel = holdResources.CreateTaskChannel(RenderProductKind::VolumeLod);
    std::shared_ptr<const int> product;
    bool hasPeak = false;
    bool hasDestructorPeak = false;
    struct Capture final {
        RenderResourceCoordinator* resources;
        bool* observed;
        ~Capture() { *observed = resources->GetResourceState().runningBytes == 800; }
    };
    auto capture = std::make_shared<Capture>();
    capture->resources = &holdResources;
    capture->observed = &hasDestructorPeak;
    task = {};
    task.requestRevision = 1;
    task.estimatedBytes = 800;
    task.work = [&, capture](RenderTaskToken token) {
        (void)token.SetActualBytes(16);
        product = std::make_shared<const int>(1);
        (void)token.SetProductOwner(product, 16);
        hasPeak = holdResources.GetResourceState().runningBytes == 800;
        (void)holdChannel->Stop();
    };
    capture.reset();
    (void)holdChannel->StartTask(std::move(task));
    (void)holdLane->SendOne();
    const auto held = holdResources.GetResourceState();
    product.reset();
    const auto released = holdResources.GetResourceState();
    failures += GetCaseResult(hasPeak && hasDestructorPeak && held.runningBytes == 0
        && held.pendingBytes == 16 && released.pendingBytes == 0,
        "Peak survives callable destruction and cancelled product stays charged until last reader exits") ? 0 : 1;
    RenderWorkBudget overflow;
    failures += GetCaseResult(!overflow.Add((std::numeric_limits<std::uint64_t>::max)(), 2)
        && !overflow.GetBytes(), "Working-set arithmetic rejects overflow") ? 0 : 1;
    vtkNew<vtkImageData> malformed;
    malformed->SetDimensions(2, 2, 2); malformed->AllocateScalars(VTK_FLOAT, 1);
    const bool validStorage = RenderWorkBudget::GetScalarStorageValid(malformed);
    malformed->GetPointData()->GetScalars()->SetNumberOfTuples(1);
    const bool rejectsTruncated = !RenderWorkBudget::GetScalarStorageValid(malformed);
    malformed->SetDimensions((std::numeric_limits<int>::max)(), (std::numeric_limits<int>::max)(), 3);
    failures += GetCaseResult(validStorage && rejectsTruncated
        && !RenderWorkBudget::GetScalarStorageValid(malformed),
        "Structural scalar checks reject truncated and overflowing grids without a budget cap") ? 0 : 1;
    return failures;
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

int GetSliceTextFailCount()
{
    int failures = 0;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    const std::array<Orientation, 3> orientations{
        Orientation::Top_down, Orientation::Front_back, Orientation::Left_right};
    const std::array<const char*, 3> labels{
        u8"场景 坐标系\n33.789012 mm", u8"场景 坐标系\n22.456789 mm", u8"场景 坐标系\n11.123456 mm"};
    for (std::size_t index = 0; index < orientations.size(); ++index) {
        SliceStrategy slice(orientations[index]);
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        slice.AttachRenderer(renderer);
        vtkTextActor* text = nullptr;
        renderer->GetViewProps()->InitTraversal();
        while (auto* prop = renderer->GetViewProps()->GetNextProp())
            if (auto* candidate = vtkTextActor::SafeDownCast(prop)) text = candidate;
        const bool isInitiallyHidden = text && !text->GetVisibility();
        slice.SetInputData(image);
        const bool isPendingHidden = text && !text->GetVisibility();
        RenderParams params;
        params.cursor = {11.123456, 22.456789, 33.789012};
        // 模型平移不能被再次加到场景切片坐标上。
        params.modelMatrix[3] = 100;
        const bool isApplied = slice.SetVisualState(params, UpdateFlags::Transform | UpdateFlags::Cursor);
        const bool isWorldPosition = text && text->GetInput()
            && std::string(text->GetInput()) == labels[index]
            && text->GetVisibility() && !text->GetPickable();
        const bool hasChineseFont = text && text->GetTextProperty()->GetFontFamily() == VTK_FONT_FILE
            && text->GetTextProperty()->GetFontFile();
        slice.DetachRenderer(renderer);
        failures += GetCaseResult(isInitiallyHidden && isPendingHidden && isApplied && isWorldPosition
            && hasChineseFont && renderer->GetViewProps()->GetNumberOfItems() == 0,
            "Chinese slice text uses the actual world plane and detaches with its view") ? 0 : 1;
    }
    return failures;
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
    return GetSharedBuildAndDefaultPolicyFailCount()
        + GetGpuHeadroomFailCount()
        + GetLatestTransitionFailCount()
        + GetWorkingSetFailCount()
        + GetTransitionValueFailCount()
        + GetDefaultStrategyFailCount()
        + GetRenderLaneFailCount()
        + GetRenderStopFailCount()
        + GetOptionalCpuBudgetFailCount()
        + GetExecutorLaneFailCount()
        + GetInitialIsoStageFailCount()
        + GetIsoBuilderFailCount()
        + GetIsoStrategyTaskFailCount()
        + GetIsoIndexFailCount()
        + GetVolumeBuilderFailCount()
        + GetVolumeRangeFailCount()
        + GetVolumeStrategyTaskFailCount()
        + GetResourceAccountingFailCount()
        + GetSharedCacheAndGpuFailCount()
        + GetCompositeAtomicFailCount()
        + GetSliceTextFailCount()
        + GetSliceAndPlaneCacheFailCount();
}
