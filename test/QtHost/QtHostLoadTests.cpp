#include "QtHostMethodCases.h"

#include "App/AppState.h"
#include "App/Services/AppPorts.h"
#include "Data/DataManager.h"
#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/LoadCommitCoordinator.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

HostCoreServices GetLoadCore(
    std::shared_ptr<RawVolumeDataManager> dataManager = {})
{
    HostCoreServices core;
    core.sharedDataMgr = dataManager
        ? std::move(dataManager)
        : std::make_shared<RawVolumeDataManager>();
    core.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    core.sharedState =
        std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);
    return core;
}

std::vector<HostRenderViewConfig> GetLoadViews()
{
    HostRenderViewConfig primary;
    primary.id = "load-primary";
    primary.role = HostRenderViewRole::Primary3D;
    primary.window.viewInit.viewMode = HostRenderMode::Volume;
    primary.renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    primary.renderWindow->SetOffScreenRendering(1);
    HostRenderViewConfig auxiliary;
    auxiliary.id = "load-aux";
    auxiliary.role = HostRenderViewRole::Auxiliary;
    auxiliary.window.viewInit.viewMode = HostRenderMode::IsoSurface;
    auxiliary.renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    auxiliary.renderWindow->SetOffScreenRendering(1);
    return { std::move(primary), std::move(auxiliary) };
}

HostReloadRequest GetReload()
{
    HostReloadRequest reload;
    reload.voxels = {
        0.0f, 1.0f, 2.0f, 3.0f,
        4.0f, 5.0f, 6.0f, 7.0f
    };
    reload.geometry = {
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    return reload;
}

bool SendReload(
    HostCommandRouter& router,
    HostViewRuntimeRegistry& views,
    HostReloadRequest reload)
{
    const HostViewTarget primary{
        "load-primary", false,
        HostRenderViewRole::Primary3D };
    const HostViewTarget auxiliary{
        "load-aux", false,
        HostRenderViewRole::Auxiliary };
    bool isComplete = false;
    bool isSucceeded = false;
    if (!router.Dispatch(
            std::move(reload),
            [&isComplete, &isSucceeded](const bool value) {
                isSucceeded = value;
                isComplete = true;
            })) {
        return false;
    }
    constexpr int pollCount = 1000;
    for (int poll = 0; !isComplete && poll < pollCount; ++poll) {
        (void)views.SendViewUpdates(primary);
        (void)views.SendViewUpdates(auxiliary);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return isComplete && isSucceeded;
}

bool GetStageFinalizeValid()
{
    class StageStub final : public AppDataStagePort {
    public:
        explicit StageStub(std::shared_ptr<AbstractDataManager> data)
            : m_data(std::move(data))
        {
        }

        bool BuildDataStage(const TrustedImageSnapshot& snapshot) override
        {
            if (m_isBuilt || !snapshot) return false;
            m_snapshot = snapshot;
            m_isBuilt = true;
            return true;
        }

        bool SetViewStage(const TrustedImageSnapshot& snapshot) override
        {
            if (!m_isBuilt || m_isCommitted
                || snapshot != m_snapshot) {
                return false;
            }
            m_isCommitted = true;
            return true;
        }

        bool ResetViewStage() override
        {
            ++m_resetCount;
            m_isCommitted = false;
            return true;
        }

        bool ClearDataStage() override
        {
            ++m_clearCount;
            return false;
        }

        void SetDataStageComplete() noexcept override
        {
            ++m_completeCount;
            m_hasPublished = m_data
                && m_data->GetImageSnapshot() == m_snapshot;
            m_isCommitted = false;
            m_isBuilt = false;
            m_snapshot.reset();
        }

        bool GetIsComplete() const noexcept
        {
            return m_completeCount == 1
                && m_clearCount == 0
                && m_resetCount == 0
                && m_hasPublished
                && !m_isBuilt
                && !m_isCommitted;
        }

    private:
        std::shared_ptr<AbstractDataManager> m_data;
        TrustedImageSnapshot m_snapshot;
        int m_completeCount = 0;
        int m_clearCount = 0;
        int m_resetCount = 0;
        bool m_hasPublished = false;
        bool m_isBuilt = false;
        bool m_isCommitted = false;
    };

    auto data = std::make_shared<RawVolumeDataManager>();
    const auto initial = data->GetImageSnapshot();
    const auto layout = VolumeLayout::Create(
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {});
    auto buffer = layout
        ? VolumeBuffer::Create(
            std::vector<float>{
                0.0f, 1.0f, 2.0f, 3.0f,
                4.0f, 5.0f, 6.0f, 7.0f },
            *layout)
        : std::optional<VolumeBuffer>{};
    if (!initial || !buffer || !data->SetFromBuffer(*buffer)) {
        return false;
    }

    auto first = std::make_shared<StageStub>(data);
    auto second = std::make_shared<StageStub>(data);
    LoadCommitRequest request;
    request.loadKind = LoadEventKind::Reload;
    request.stages = { first, second };
    LoadCommitCoordinator coordinator(data);
    const bool isCommitted = coordinator.SetLoadCommit(request);
    const auto current = data->GetImageSnapshot();
    return isCommitted
        && current
        && current != initial
        && current->version == initial->version + 1
        && first->GetIsComplete()
        && second->GetIsComplete();
}

bool GetMultiViewLoadValid(const bool isAuxStopped)
{
    auto core = GetLoadCore();
    HostViewRuntimeRegistry views;
    if (!views.Build(core, GetLoadViews())
        || !views.SetInteractorsReady()) {
        return false;
    }

    const HostViewTarget primary{
        "load-primary", false,
        HostRenderViewRole::Primary3D };
    const HostViewTarget auxiliary{
        "load-aux", false,
        HostRenderViewRole::Auxiliary };
    const auto initial = core.sharedDataMgr->GetImageSnapshot();
    if (!initial) return false;

    const auto directory = views.GetViewDirectory();
    if (isAuxStopped) {
        const auto current = directory.lock();
        const auto route = current
            ? current->GetViewRoute(auxiliary)
            : std::optional<HostViewRoute>{};
        if (!route || !route->stopView || !route->stopView()) {
            return false;
        }
    }

    HostCommandRouter router(directory);
    bool isComplete = false;
    bool isSucceeded = false;
    if (!router.Dispatch(
            GetReload(),
            [&isComplete, &isSucceeded](const bool value) {
                isSucceeded = value;
                isComplete = true;
            })) {
        return false;
    }
    constexpr int pollCount = 1000;
    for (int poll = 0; !isComplete && poll < pollCount; ++poll) {
        (void)views.SendViewUpdates(primary);
        if (!isAuxStopped) {
            (void)views.SendViewUpdates(auxiliary);
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }

    const auto current = core.sharedDataMgr->GetImageSnapshot();
    if (isAuxStopped) {
        return isComplete
            && !isSucceeded
            && current == initial
            && !core.sharedDataMgr->GetPendingSnapshot();
    }

    (void)views.SendViewUpdates(primary);
    (void)views.SendViewUpdates(auxiliary);
    const auto states = views.GetViewStates();
    double imageCenter[3] = {};
    current->image->GetCenter(imageCenter);
    const std::array<double, 3> expectedCursor = {
        imageCenter[0], imageCenter[1], imageCenter[2] };
    return isComplete
        && isSucceeded
        && current
        && current->version == initial->version + 1
        && states.size() == 2
        && states[0].scalarRange
            == std::array<double, 2>{ 0.0, 7.0 }
        && states[1].scalarRange
            == std::array<double, 2>{ 0.0, 7.0 }
        && states[0].windowLevel.windowWidth == 7.0
        && states[0].windowLevel.windowCenter == 3.5
        && states[1].windowLevel.windowWidth == 7.0
        && states[1].windowLevel.windowCenter == 3.5
        && core.sharedState->GetDataVersion() == current->version
        && core.sharedState->GetCursorWorld() == expectedCursor
        && core.sharedState->GetCursorRawWorld() == expectedCursor
        && core.sharedState->GetCursorAxis() == -1;
}

bool GetWindowLevelIntentValid()
{
    auto core = GetLoadCore();
    auto configs = GetLoadViews();
    configs[0].window.viewInit.windowLevel = { 80.0, 20.0 };
    configs[0].window.viewInit.hasWindowLevel = true;
    configs[1].window.viewInit.viewMode =
        HostRenderMode::SliceTopDown;
    HostViewRuntimeRegistry views;
    if (!views.Build(core, configs)
        || !views.SetInteractorsReady()) {
        return false;
    }

    const HostViewTarget primary{
        "load-primary", false,
        HostRenderViewRole::Primary3D };
    const HostViewTarget auxiliary{
        "load-aux", false,
        HostRenderViewRole::Auxiliary };
    HostCommandRouter router(views.GetViewDirectory());
    if (!SendReload(router, views, GetReload())) return false;
    auto primaryState = views.GetViewState(primary);
    auto auxiliaryState = views.GetViewState(auxiliary);
    const bool hasInitialIntent = primaryState
        && auxiliaryState
        && primaryState->windowLevel.windowWidth == 80.0
        && primaryState->windowLevel.windowCenter == 20.0
        && auxiliaryState->windowLevel.windowWidth == 7.0
        && auxiliaryState->windowLevel.windowCenter == 3.5;

    HostViewSetRequest manual;
    manual.targetView = auxiliary;
    manual.windowLevel = HostWindowLevelParams{ 12.0, 6.0 };
    if (!hasInitialIntent
        || !router.Dispatch(std::move(manual))) {
        return false;
    }

    HostReloadRequest next = GetReload();
    next.voxels = {
        -4.0f, -3.0f, -2.0f, -1.0f,
        0.0f, 1.0f, 2.0f, 3.0f
    };
    if (!SendReload(router, views, std::move(next))) return false;
    primaryState = views.GetViewState(primary);
    auxiliaryState = views.GetViewState(auxiliary);
    const bool hasManualReload = primaryState
        && auxiliaryState
        && primaryState->windowLevel.windowWidth == 80.0
        && primaryState->windowLevel.windowCenter == 20.0
        && auxiliaryState->windowLevel.windowWidth == 12.0
        && auxiliaryState->windowLevel.windowCenter == 6.0;

    HostViewResetRequest reset;
    reset.targetView = auxiliary;
    if (!hasManualReload
        || !router.Dispatch(std::move(reset))) {
        return false;
    }
    auxiliaryState = views.GetViewState(auxiliary);
    const bool hasAutoReset = auxiliaryState
        && auxiliaryState->windowLevel.windowWidth == 7.0
        && auxiliaryState->windowLevel.windowCenter == -0.5;

    HostReloadRequest constant = GetReload();
    constant.voxels.assign(8, 12.0f);
    if (!hasAutoReset
        || !SendReload(router, views, std::move(constant))) {
        return false;
    }
    primaryState = views.GetViewState(primary);
    auxiliaryState = views.GetViewState(auxiliary);
    return primaryState
        && auxiliaryState
        && primaryState->windowLevel.windowWidth == 80.0
        && primaryState->windowLevel.windowCenter == 20.0
        && std::isfinite(
            auxiliaryState->windowLevel.windowWidth)
        && auxiliaryState->windowLevel.windowWidth > 0.0
        && auxiliaryState->windowLevel.windowCenter == 12.0;
}

bool GetPublishLastValid()
{
    class GateDataManager final : public RawVolumeDataManager {
    public:
        std::function<bool()> beforePublish;

        bool SetCurrentFromPending(
            const TrustedImageSnapshot& expectedPending,
            TrustedImageSnapshot& publishedSnapshot) override
        {
            if (beforePublish && !beforePublish()) {
                publishedSnapshot.reset();
                return false;
            }
            return BaseDataManager::SetCurrentFromPending(
                expectedPending, publishedSnapshot);
        }
    };

    auto dataManager = std::make_shared<GateDataManager>();
    auto core = GetLoadCore(dataManager);
    HostViewRuntimeRegistry views;
    if (!views.Build(core, GetLoadViews())
        || !views.SetInteractorsReady()) {
        return false;
    }

    const HostViewTarget primary{
        "load-primary", false,
        HostRenderViewRole::Primary3D };
    const HostViewTarget auxiliary{
        "load-aux", false,
        HostRenderViewRole::Auxiliary };
    const auto initial = dataManager->GetImageSnapshot();
    if (!initial) return false;

    const std::array<double, 3> oldCursor = { 11.0, 12.0, 13.0 };
    core.sharedState->SetCursorRawWorld(
        oldCursor[0], oldCursor[1], oldCursor[2]);
    core.sharedState->SetCursorAxis(2);
    core.sharedState->SetCursorWorld(
        oldCursor[0], oldCursor[1], oldCursor[2]);
    const DataVersion oldSharedVersion =
        core.sharedState->GetDataVersion();
    auto eventOwner = std::make_shared<int>(0);
    int dataEventCount = 0;
    core.sharedStateBroadcaster->SetObserver(
        eventOwner,
        [&dataEventCount](const UpdateFlags flags) {
            if ((flags & (UpdateFlags::DataReady | UpdateFlags::Cursor))
                != UpdateFlags::None) {
                ++dataEventCount;
            }
        });

    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool hasPublishGate = false;
    bool hasReaderSample = false;
    bool hasViewsCommitted = false;
    bool hasGpuWarmup = false;
    bool hasSharedUnchangedAtGate = false;
    std::atomic<bool> isWriterDone = false;
    DataVersion minVersion = initial->version;
    DataVersion maxVersion = initial->version;
    bool hasVersionDrop = false;

    int warmupCount = 0;
    auto warmupObserver = vtkSmartPointer<vtkCallbackCommand>::New();
    warmupObserver->SetClientData(&warmupCount);
    warmupObserver->SetCallback(
        [](vtkObject*, unsigned long eventId,
            void* clientData, void*) {
            if (eventId == vtkCommand::StartEvent && clientData) {
                ++(*static_cast<int*>(clientData));
            }
        });
    const auto endpoints = views.BuildEndpoints();
    std::vector<std::pair<vtkRenderWindow*, unsigned long>> warmupTags;
    for (const auto& endpoint : endpoints) {
        if (!endpoint.renderWindow) continue;
        warmupTags.push_back({
            endpoint.renderWindow,
            endpoint.renderWindow->AddObserver(
                vtkCommand::StartEvent, warmupObserver) });
    }

    dataManager->beforePublish = [&]() {
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            hasPublishGate = true;
            gateChanged.notify_all();
            gateChanged.wait(lock, [&]() { return hasReaderSample; });
        }
        const auto states = views.GetViewStates();
        hasViewsCommitted = states.size() == 2
            && states[0].dataVersion == initial->version + 1
            && states[1].dataVersion == initial->version + 1;
        hasGpuWarmup = warmupCount >= 2;
        hasSharedUnchangedAtGate =
            core.sharedState->GetDataVersion() == oldSharedVersion
            && core.sharedState->GetCursorWorld() == oldCursor
            && core.sharedState->GetCursorRawWorld() == oldCursor
            && core.sharedState->GetCursorAxis() == 2
            && dataEventCount == 0;
        // 模拟最终 DataManager CAS 冲突；此时全部 View 已提交候选，
        // Host 必须回滚 View，但 current 从未向读者发布新版本。
        return false;
    };

    std::thread reader([&]() {
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            gateChanged.wait(lock, [&]() {
                return hasPublishGate || isWriterDone.load();
            });
            if (!hasPublishGate) return;
        }

        DataVersion lastVersion = initial->version;
        bool isFirstSample = true;
        do {
            const DataVersion version = dataManager->GetDataVersion();
            minVersion = std::min(minVersion, version);
            maxVersion = std::max(maxVersion, version);
            hasVersionDrop = hasVersionDrop || version < lastVersion;
            lastVersion = version;
            if (isFirstSample) {
                std::lock_guard<std::mutex> lock(gateMutex);
                hasReaderSample = true;
                isFirstSample = false;
                gateChanged.notify_all();
            }
            std::this_thread::yield();
        } while (!isWriterDone.load());
    });

    HostCommandRouter router(views.GetViewDirectory());
    bool isComplete = false;
    bool isSucceeded = false;
    const bool isDispatched = router.Dispatch(
        GetReload(),
        [&isComplete, &isSucceeded](const bool value) {
            isSucceeded = value;
            isComplete = true;
        });
    constexpr int pollCount = 1000;
    for (int poll = 0; isDispatched && !isComplete
        && poll < pollCount; ++poll) {
        (void)views.SendViewUpdates(primary);
        (void)views.SendViewUpdates(auxiliary);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    isWriterDone = true;
    gateChanged.notify_all();
    reader.join();
    for (const auto& [renderWindow, tag] : warmupTags) {
        if (renderWindow && tag != 0) renderWindow->RemoveObserver(tag);
    }
    warmupObserver->SetClientData(nullptr);

    const auto current = dataManager->GetImageSnapshot();
    const auto states = views.GetViewStates();
    const bool hasViewsReset = states.size() == 2
        && states[0].dataVersion == initial->version
        && states[1].dataVersion == initial->version;
    return isDispatched
        && isComplete
        && !isSucceeded
        && hasPublishGate
        && hasViewsCommitted
        && hasGpuWarmup
        && hasSharedUnchangedAtGate
        && hasViewsReset
        && current == initial
        && core.sharedState->GetDataVersion() == oldSharedVersion
        && core.sharedState->GetCursorWorld() == oldCursor
        && core.sharedState->GetCursorRawWorld() == oldCursor
        && core.sharedState->GetCursorAxis() == 2
        && dataEventCount == 0
        && minVersion == initial->version
        && maxVersion == initial->version
        && !hasVersionDrop
        && !dataManager->GetPendingSnapshot();
}

bool GetGpuWarmupFailValid()
{
    auto core = GetLoadCore();
    HostViewRuntimeRegistry views;
    if (!views.Build(core, GetLoadViews())
        || !views.SetInteractorsReady()) {
        return false;
    }
    const auto initial = core.sharedDataMgr->GetImageSnapshot();
    const auto endpoints = views.BuildEndpoints();
    if (!initial || endpoints.size() != 2
        || !endpoints[1].renderWindow) {
        return false;
    }

    int warmupCount = 0;
    auto errorInjector = vtkSmartPointer<vtkCallbackCommand>::New();
    errorInjector->SetClientData(&warmupCount);
    errorInjector->SetCallback(
        [](vtkObject* caller, unsigned long eventId,
            void* clientData, void*) {
            if (eventId != vtkCommand::StartEvent
                || !caller || !clientData) {
                return;
            }
            ++(*static_cast<int*>(clientData));
            caller->InvokeEvent(vtkCommand::ErrorEvent);
        });
    const unsigned long injectorTag =
        endpoints[1].renderWindow->AddObserver(
            vtkCommand::StartEvent, errorInjector);

    const HostViewTarget primary{
        "load-primary", false,
        HostRenderViewRole::Primary3D };
    const HostViewTarget auxiliary{
        "load-aux", false,
        HostRenderViewRole::Auxiliary };
    HostCommandRouter router(views.GetViewDirectory());
    bool isComplete = false;
    bool isSucceeded = false;
    const bool isDispatched = router.Dispatch(
        GetReload(),
        [&isComplete, &isSucceeded](const bool value) {
            isSucceeded = value;
            isComplete = true;
        });
    constexpr int pollCount = 1000;
    for (int poll = 0; isDispatched && !isComplete
        && poll < pollCount; ++poll) {
        (void)views.SendViewUpdates(primary);
        (void)views.SendViewUpdates(auxiliary);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    endpoints[1].renderWindow->RemoveObserver(injectorTag);
    errorInjector->SetClientData(nullptr);

    const auto current = core.sharedDataMgr->GetImageSnapshot();
    const auto states = views.GetViewStates();
    return isDispatched
        && isComplete
        && !isSucceeded
        && warmupCount >= 1
        && current == initial
        && states.size() == 2
        && states[0].dataVersion == initial->version
        && states[1].dataVersion == initial->version
        && !core.sharedDataMgr->GetPendingSnapshot();
}

bool GetImageReadStateValid()
{
    using ValueOwner =
        decltype(ImageReadState{}.values)::element_type;
    static_assert(std::is_const_v<ValueOwner>);

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->SetSpacing(0.5, 0.75, 1.25);
    image->SetOrigin(2.0, 3.0, 4.0);
    image->AllocateScalars(VTK_FLOAT, 2);
    constexpr std::size_t valueCount = 16;
    std::array<float, valueCount> source{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<float>(index);
    }
    std::memcpy(
        image->GetScalarPointer(),
        source.data(),
        sizeof(source));

    RawVolumeDataManager dataManager;
    bool hasPending = false;
    if (!dataManager.SetImageSnapshot(image)
        || !dataManager.SetCurrentFromPending(hasPending)
        || !hasPending) {
        return false;
    }
    const auto first = dataManager.GetImageReadState();
    if (!first || !first->values
        || first->extent != std::array<int, 6>{ 0, 1, 0, 1, 0, 1 }
        || first->dims != std::array<int, 3>{ 2, 2, 2 }
        || first->spacing != std::array<double, 3>{ 0.5, 0.75, 1.25 }
        || first->origin != std::array<double, 3>{ 2.0, 3.0, 4.0 }
        || first->valueType != ImageValueType::Float32
        || first->byteOrder != ImageByteOrder::Native
        || first->tupleOrder
            != ImageTupleOrder::XFastestInterleaved
        || first->componentBytes != sizeof(float)
        || first->componentCount != 2
        || first->values->size() != sizeof(source)
        || std::memcmp(
            first->values->data(), source.data(), sizeof(source)) != 0
        || first->validityMask) {
        return false;
    }

    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->SetDimensions(2, 2, 2);
    mask->SetSpacing(image->GetSpacing());
    mask->SetOrigin(image->GetOrigin());
    mask->SetDirectionMatrix(image->GetDirectionMatrix());
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    const std::array<std::uint8_t, 8> maskValues = {
        0, 1, 2, 3, 4, 5, 6, 7
    };
    std::memcpy(
        mask->GetScalarPointer(),
        maskValues.data(),
        maskValues.size());
    const auto expectedSnapshot = dataManager.GetImageSnapshot();
    TrustedImageState maskedState = *expectedSnapshot;
    maskedState.validityMask = mask;
    TrustedImageSnapshot maskedSnapshot;
    if (!dataManager.SetCurrentData(
            std::move(maskedState),
            expectedSnapshot,
            maskedSnapshot)) {
        return false;
    }
    constexpr std::size_t maskBytes = 8;
    const auto tooLarge = dataManager.GetImageReadResult(
        sizeof(source) + maskBytes - 1);
    const auto exactRead = dataManager.GetImageReadResult(
        sizeof(source) + maskBytes);
    if (tooLarge.error != ImageReadError::TooLarge
        || tooLarge.requiredBytes != sizeof(source) + maskBytes
        || tooLarge.state
        || exactRead.error != ImageReadError::None
        || exactRead.requiredBytes != sizeof(source) + maskBytes
        || !exactRead.state
        || !exactRead.state->validityMask
        || exactRead.state->validityMask->size() != maskBytes
        || std::memcmp(
            exactRead.state->validityMask->data(),
            maskValues.data(),
            maskValues.size()) != 0) {
        return false;
    }

    ImageReadRequest regionRequest;
    regionRequest.region = ImageReadRegion{
        { 1, 0, 0 }, { 1, 2, 2 }
    };
    constexpr std::size_t regionValueBytes =
        4 * 2 * sizeof(float);
    constexpr std::size_t regionMaskBytes = 4;
    regionRequest.maxBytes = regionValueBytes + regionMaskBytes;
    const auto regionRead = dataManager.GetImageReadResult(
        regionRequest, TaskStopToken{});
    const std::array<float, 8> expectedRegion = {
        2.0f, 3.0f, 6.0f, 7.0f,
        10.0f, 11.0f, 14.0f, 15.0f
    };
    const std::array<std::uint8_t, 4> expectedMask = {
        1, 3, 5, 7
    };
    if (regionRead.error != ImageReadError::None
        || regionRead.requiredBytes
            != regionValueBytes + regionMaskBytes
        || !regionRead.state
        || regionRead.state->dims
            != std::array<int, 3>{ 1, 2, 2 }
        || regionRead.state->extent
            != std::array<int, 6>{ 0, 0, 0, 1, 0, 1 }
        || regionRead.state->origin
            != std::array<double, 3>{ 2.5, 3.0, 4.0 }
        || regionRead.state->region.offset
            != std::array<std::size_t, 3>{ 1, 0, 0 }
        || regionRead.state->voxelCount != 4
        || !regionRead.state->values
        || regionRead.state->values->size() != regionValueBytes
        || std::memcmp(
            regionRead.state->values->data(),
            expectedRegion.data(),
            regionValueBytes) != 0
        || !regionRead.state->validityMask
        || std::memcmp(
            regionRead.state->validityMask->data(),
            expectedMask.data(),
            expectedMask.size()) != 0) {
        return false;
    }

    ImageReadRequest invalidRegion;
    invalidRegion.region = ImageReadRegion{
        { 0, 0, 0 }, { 0, 1, 1 }
    };
    ImageReadRequest overflowRegion;
    overflowRegion.region = ImageReadRegion{
        { std::numeric_limits<std::size_t>::max(), 0, 0 },
        { 1, 1, 1 }
    };
    if (dataManager.GetImageReadResult(
            invalidRegion, TaskStopToken{}).error
            != ImageReadError::InvalidRegion
        || dataManager.GetImageReadResult(
            overflowRegion, TaskStopToken{}).error
            != ImageReadError::InvalidRegion) {
        return false;
    }

    ImageReadRequest chunkRequest;
    constexpr std::size_t voxelBytes = 2 * sizeof(float) + 1;
    chunkRequest.maxBytes = voxelBytes * 2;
    std::vector<std::uint8_t> chunkValues;
    std::vector<std::uint8_t> chunkMask;
    std::size_t voxelOffset = 0;
    bool isChunkDone = false;
    while (!isChunkDone) {
        const auto chunk = dataManager.GetImageReadChunk(
            chunkRequest, voxelOffset, TaskStopToken{});
        if (chunk.error != ImageReadError::None
            || chunk.requiredBytes != sizeof(source) + maskBytes
            || !chunk.state
            || chunk.state->voxelOffset != voxelOffset
            || chunk.state->voxelCount != 2
            || !chunk.state->values
            || !chunk.state->validityMask) {
            return false;
        }
        chunkValues.insert(
            chunkValues.end(),
            chunk.state->values->begin(),
            chunk.state->values->end());
        chunkMask.insert(
            chunkMask.end(),
            chunk.state->validityMask->begin(),
            chunk.state->validityMask->end());
        voxelOffset = chunk.nextVoxelOffset;
        isChunkDone = chunk.isDone;
    }
    ImageReadRequest tinyChunk;
    tinyChunk.maxBytes = voxelBytes - 1;
    const auto tooSmallChunk = dataManager.GetImageReadChunk(
        tinyChunk, 0, TaskStopToken{});
    TaskStopSource stoppedSource;
    (void)stoppedSource.Stop();
    const auto cancelledRead = dataManager.GetImageReadResult(
        ImageReadRequest{}, stoppedSource.GetToken());
    if (voxelOffset != 8
        || chunkValues.size() != sizeof(source)
        || std::memcmp(
            chunkValues.data(), source.data(), sizeof(source)) != 0
        || chunkMask != std::vector<std::uint8_t>(
            maskValues.begin(), maskValues.end())
        || tooSmallChunk.error != ImageReadError::TooLarge
        || tooSmallChunk.requiredBytes != sizeof(source) + maskBytes
        || cancelledRead.error != ImageReadError::Cancelled) {
        return false;
    }

    auto replacement = vtkSmartPointer<vtkImageData>::New();
    replacement->SetDimensions(2, 2, 2);
    replacement->AllocateScalars(VTK_FLOAT, 2);
    std::array<float, valueCount> replacementValues{};
    replacementValues.fill(-1.0f);
    std::memcpy(
        replacement->GetScalarPointer(),
        replacementValues.data(),
        sizeof(replacementValues));
    hasPending = false;
    if (!dataManager.SetImageSnapshot(replacement)
        || !dataManager.SetCurrentFromPending(hasPending)
        || !hasPending) {
        return false;
    }
    const auto second = dataManager.GetImageReadState();
    return second
        && second->values
        && second->version > first->version
        && std::memcmp(
            first->values->data(), source.data(), sizeof(source)) == 0
        && std::memcmp(
            second->values->data(),
            replacementValues.data(),
            sizeof(replacementValues)) == 0;
}

bool GetHostResultValid()
{
    VtkAppHostSession emptySession(HostSessionConfig{});
    int emptyCount = 0;
    HostResult emptyResult;
    const bool isEmptySent = emptySession.SendRequestResult(
        GetReload(),
        [&emptyCount, &emptyResult](HostResult result) {
            ++emptyCount;
            emptyResult = std::move(result);
        });
    if (isEmptySent || emptyCount != 1
        || emptyResult.isSucceeded
        || emptyResult.errorCode != HostErrorCode::SessionNotReady
        || emptySession.GetImageReadState()) {
        return false;
    }

    HostSessionConfig config;
    auto views = GetLoadViews();
    config.renderViews.push_back(std::move(views.front()));
    VtkAppHostSession session(std::move(config));
    if (!session.BuildSession()) return false;

    const auto* endpoint = session.GetPrimaryEndpoint();
    if (!endpoint || !endpoint->interactor) return false;
    endpoint->interactor->Initialize();
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView.viewId = "load-primary";
    if (!session.AttachTimer(timer)) return false;
    const auto sendTimer = [&]() {
        int timerId = endpoint->interactor->GetTimerEventId();
        if (timerId == 0) {
            for (int candidate = 1; candidate <= 64; ++candidate) {
                if (endpoint->interactor->GetTimerDuration(candidate) != 0) {
                    timerId = candidate;
                    break;
                }
            }
        }
        if (timerId == 0) return false;
        endpoint->interactor->InvokeEvent(
            vtkCommand::TimerEvent, &timerId);
        return true;
    };

    int syncSuccessCount = 0;
    HostResult syncSuccess;
    HostViewSetRequest viewRequest;
    viewRequest.targetView.viewId = "load-primary";
    viewRequest.opacity = 0.75;
    const bool isSyncSuccessSent = session.SendRequestResult(
        std::move(viewRequest),
        [&syncSuccessCount, &syncSuccess](HostResult value) {
            ++syncSuccessCount;
            syncSuccess = std::move(value);
        });
    if (!isSyncSuccessSent || syncSuccessCount != 1
        || !syncSuccess.isSucceeded
        || syncSuccess.errorCode != HostErrorCode::None) {
        return false;
    }

    int syncFailCount = 0;
    HostResult syncFail;
    HostViewSetRequest missingView;
    missingView.targetView.viewId = "missing-view";
    missingView.opacity = 0.5;
    const bool isSyncFailSent = session.SendRequestResult(
        std::move(missingView),
        [&syncFailCount, &syncFail](HostResult value) {
            ++syncFailCount;
            syncFail = std::move(value);
        });
    if (!isSyncFailSent || syncFailCount != 1
        || syncFail.isSucceeded
        || syncFail.errorCode != HostErrorCode::OperationFailed) {
        return false;
    }

    HostReloadRequest invalidReload;
    invalidReload.voxels.assign(7, 1.0f);
    invalidReload.geometry = {
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    int resultCount = 0;
    HostResult result;
    const bool isSent = session.SendRequestResult(
        std::move(invalidReload),
        [&resultCount, &result](HostResult value) {
            ++resultCount;
            result = std::move(value);
        });
    const bool isValid = !isSent
        && resultCount == 1
        && !result.isSucceeded
        && result.errorCode == HostErrorCode::RequestRejected;
    if (!isValid) return false;

    int asyncSuccessCount = 0;
    HostResult asyncSuccess;
    const bool isAsyncSuccessSent = session.SendRequestResult(
        GetReload(),
        [&asyncSuccessCount, &asyncSuccess](HostResult value) {
            ++asyncSuccessCount;
            asyncSuccess = std::move(value);
        });
    constexpr int pollCount = 1000;
    for (int poll = 0; isAsyncSuccessSent
        && asyncSuccessCount == 0 && poll < pollCount; ++poll) {
        if (!sendTimer()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!isAsyncSuccessSent || asyncSuccessCount != 1
        || !asyncSuccess.isSucceeded
        || asyncSuccess.errorCode != HostErrorCode::None) {
        return false;
    }

    int imageReadCount = 0;
    ImageReadResult imageRead;
    const auto imageAdmission = session.StartImageRead(
        ImageReadRequest{},
        [&imageReadCount, &imageRead](ImageReadResult value) {
            ++imageReadCount;
            imageRead = std::move(value);
        });
    int rejectedReadCount = 0;
    const auto busyAdmission = session.StartImageRead(
        ImageReadRequest{},
        [&rejectedReadCount](ImageReadResult) {
            ++rejectedReadCount;
        });
    for (int poll = 0; imageReadCount == 0
        && poll < pollCount; ++poll) {
        if (!sendTimer()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int poll = 0; poll < 3; ++poll) {
        if (!sendTimer()) return false;
    }
    if (imageAdmission != ImageReadAdmission::Accepted
        || busyAdmission != ImageReadAdmission::Busy
        || imageReadCount != 1
        || rejectedReadCount != 0
        || imageRead.error != ImageReadError::None
        || !imageRead.state
        || imageRead.state->dims
            != std::array<int, 3>{ 2, 2, 2 }
        || !imageRead.state->values
        || imageRead.state->values->size()
            != 8 * sizeof(float)) {
        return false;
    }

    int asyncFailCount = 0;
    HostResult asyncFail;
    HostLoadRequest missingLoad;
    missingLoad.filePath = "missing-result.raw";
    missingLoad.geometry = {
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    const bool isAsyncFailSent = session.SendRequestResult(
        std::move(missingLoad),
        [&asyncFailCount, &asyncFail](HostResult value) {
            ++asyncFailCount;
            asyncFail = std::move(value);
        });
    for (int poll = 0; isAsyncFailSent
        && asyncFailCount == 0 && poll < pollCount; ++poll) {
        if (!sendTimer()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int poll = 0; poll < 3; ++poll) {
        if (!sendTimer()) return false;
    }
    int stoppedReadCount = 0;
    ImageReadResult stoppedRead;
    const auto stoppedReadAdmission = session.StartImageRead(
        ImageReadRequest{},
        [&stoppedReadCount, &stoppedRead](ImageReadResult value) {
            ++stoppedReadCount;
            stoppedRead = std::move(value);
        });
    const bool isStopped = session.Stop();
    return isStopped
        && stoppedReadAdmission == ImageReadAdmission::Accepted
        && stoppedReadCount == 1
        && (stoppedRead.error == ImageReadError::None
            || stoppedRead.error == ImageReadError::Cancelled)
        && isAsyncFailSent
        && asyncFailCount == 1
        && !asyncFail.isSucceeded
        && asyncFail.errorCode == HostErrorCode::OperationFailed;
}

} // namespace

int GetLoadFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    int failureCount = 0;
    HostLoadRequest missingLoad;
    missingLoad.geometry = {
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(missingLoad)),
        "Load missing path rejection") ? 0 : 1;
    HostLoadRequest unicodeLoad;
    unicodeLoad.geometry = {
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    unicodeLoad.filePath = u8"不存在/体数据 é.raw";
    failureCount += GetCaseResult(
        !session.SendRequest(
            HostLoadRequest(unicodeLoad)),
        "Load UTF-8 missing path rejection") ? 0 : 1;

    HostRenderViewConfig view;
    view.id = "utf8-load";
    view.role = HostRenderViewRole::Primary3D;
    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession unicodeSession(std::move(config));
    failureCount += GetCaseResult(
        unicodeSession.BuildSession()
            && unicodeSession.SendRequest(
                std::move(unicodeLoad)),
        "Load UTF-8 request facade acceptance") ? 0 : 1;

    HostReloadRequest reload;
    reload.voxels.assign(7, 1.0f);
    reload.geometry = {
        { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(reload)),
        "Reload voxel-count rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        GetMultiViewLoadValid(false),
        "Multi-view reload publishes one shared version after every stage is ready") ? 0 : 1;
    failureCount += GetCaseResult(
        GetWindowLevelIntentValid(),
        "Window/level auto intent follows data while manual intent survives reload") ? 0 : 1;
    failureCount += GetCaseResult(
        GetMultiViewLoadValid(true),
        "Multi-view reload failure keeps the previous current snapshot") ? 0 : 1;
    failureCount += GetCaseResult(
        GetPublishLastValid(),
        "Multi-view failed publish keeps concurrent readers monotonic") ? 0 : 1;
    failureCount += GetCaseResult(
        GetStageFinalizeValid(),
        "Published load uses noexcept stage finalization instead of fallible cleanup") ? 0 : 1;
    failureCount += GetCaseResult(
        GetGpuWarmupFailValid(),
        "No-effect GPU warm-up ErrorEvent rolls back every view") ? 0 : 1;
    failureCount += GetCaseResult(
        GetImageReadStateValid(),
        "Read image contract owns immutable bytes without VTK identity") ? 0 : 1;
    failureCount += GetCaseResult(
        GetHostResultValid(),
        "Typed host result reports one stable failure reason") ? 0 : 1;
    return failureCount;
}
