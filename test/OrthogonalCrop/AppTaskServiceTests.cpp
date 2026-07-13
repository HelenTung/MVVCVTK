#include "Tasks/AppDataExportTaskService.h"
#include "Tasks/AppDataLoadTaskService.h"
#include "Services/AppService.h"
#include "AppStateEvents.h"
#include "PlanarTestSuites.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

class AppTaskCases final {
public:

class ExportDataStub final : public AbstractDataManager
{
public:
    vtkSmartPointer<vtkImageData> GetVtkImage() const override
    {
        vtkSmartPointer<vtkImageData> image;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            image = m_image;
        }
        if (!image) {
            return nullptr;
        }
        auto imageCopy = vtkSmartPointer<vtkImageData>::New();
        imageCopy->DeepCopy(image);
        m_imageReadCount.fetch_add(1);
        return imageCopy;
    }

    ImageState GetImageState() const override
    {
        ImageState imageState;
        imageState.image = GetVtkImage();
        if (!imageState.image) {
            return imageState;
        }
        int dims[3] = { 0, 0, 0 };
        imageState.image->GetDimensions(dims);
        imageState.dims = { dims[0], dims[1], dims[2] };
        imageState.spacing = GetSpacing();
        imageState.scalarRange = GetScalarRange();
        imageState.version = GetDataVersion();
        return imageState;
    }

protected:
    ImageSnapshot GetImageSnapshot() const override
    {
        auto imageState = std::make_shared<ImageState>();
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            imageState->image = m_image;
            imageState->spacing = m_spacing;
        }
        if (imageState->image) {
            int dims[3] = { 0, 0, 0 };
            imageState->image->GetDimensions(dims);
            imageState->dims = { dims[0], dims[1], dims[2] };
            imageState->scalarRange = { 1.0, 4.0 };
            imageState->version = m_version.load();
        }
        m_snapshotReadCount.fetch_add(1);
        return imageState;
    }

public:

    std::array<double, 2> GetScalarRange() const override
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        return m_image ? std::array<double, 2>{ 1.0, 4.0 } : std::array<double, 2>{};
    }

    std::array<double, 3> GetSpacing() const override
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        return m_spacing;
    }

    DataVersion GetDataVersion() const override
    {
        return m_version.load();
    }

    bool SetSpacing(const std::array<double, 3>& spacing) override
    {
        vtkSmartPointer<vtkImageData> image;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            if (!m_image) {
                return false;
            }
            m_spacing = spacing;
            image = m_image;
        }
        image->SetSpacing(spacing.data());
        return true;
    }

    bool SetDataLoaded(
        const std::string&,
        const std::array<float, 3>&,
        const std::array<float, 3>&) override
    {
        if (m_isLoadThrow) {
            throw std::runtime_error("synthetic file load failure");
        }
        return false;
    }

    bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>&,
        const std::array<float, 3>&) override
    {
        if (m_isBufferThrow) {
            throw std::runtime_error("synthetic buffer load failure");
        }
        if (!data || dims != std::array<int, 3>{ 2, 2, 1 }) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_buffer.assign(data, data + 4);
        }
        m_hasPending.store(true);
        return true;
    }

    bool SetCurrentFromPending() override
    {
        if (!m_hasPending.exchange(false)) {
            return false;
        }

        std::vector<float> buffer;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            buffer = m_buffer;
        }
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(2, 2, 1);
        image->SetSpacing(1.0, 1.0, 1.0);
        image->SetOrigin(0.0, 0.0, 0.0);
        image->AllocateScalars(VTK_FLOAT, 1);
        std::copy(buffer.begin(), buffer.end(), static_cast<float*>(image->GetScalarPointer()));
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_image = std::move(image);
            m_spacing = { 1.0, 1.0, 1.0 };
        }
        m_version.fetch_add(1);
        return true;
    }

    bool ExportData(
        const std::string& path,
        const std::array<double, 16>&) override
    {
        return path == "A";
    }

    bool ExportSlices(
        const std::string&,
        Orientation,
        const WindowLevelParams&,
        const std::array<double, 16>&) override
    {
        return false;
    }

    std::vector<float> GetBuffer() const
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        return m_buffer;
    }

    bool GetPendingReady() const
    {
        return m_hasPending.load();
    }

    int GetImageReadCount() const
    {
        return m_imageReadCount.load();
    }

    int GetSnapshotReadCount() const
    {
        return m_snapshotReadCount.load();
    }

    void SetLoadThrow(bool isEnabled)
    {
        m_isLoadThrow = isEnabled;
    }

    void SetBufferThrow(bool isEnabled)
    {
        m_isBufferThrow = isEnabled;
    }

private:
    mutable std::mutex m_dataMutex;
    std::vector<float> m_buffer;
    vtkSmartPointer<vtkImageData> m_image;
    std::array<double, 3> m_spacing = { 1.0, 1.0, 1.0 };
    std::atomic<bool> m_hasPending{ false };
    std::atomic<DataVersion> m_version{ 0 };
    mutable std::atomic<int> m_imageReadCount{ 0 };
    mutable std::atomic<int> m_snapshotReadCount{ 0 };
    bool m_isLoadThrow = false;
    bool m_isBufferThrow = false;
};

void SetExpect(bool isExpected, const std::string& message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void StartTaskOrder(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto service = std::make_shared<AppDataExportTaskService>(dataManager, sharedState);

    std::vector<char> callbackOrder;
    std::optional<bool> resultA;
    std::optional<bool> resultB;

    auto taskA = service->BuildDataTask("A", [&](bool isSuccess) {
        callbackOrder.push_back('A');
        resultA = isSuccess;
    });
    auto taskB = service->BuildDataTask("B", [&](bool isSuccess) {
        callbackOrder.push_back('B');
        resultB = isSuccess;
    });

    SetExpect(taskA.has_value(), "export task A should be built.", failureCount);
    SetExpect(taskB.has_value(), "export task B should be built.", failureCount);
    if (!taskA || !taskB) {
        return;
    }

    (*taskB)();
    (*taskA)();
    if (service->ResetSaveCallback()) {
        service->SendSaveCallback();
    }

    SetExpect(
        callbackOrder == std::vector<char>{ 'B', 'A' },
        "export callbacks should run once each in completion order B,A.",
        failureCount);
    SetExpect(
        resultA.has_value() && *resultA,
        "export task A callback should receive its successful result.",
        failureCount);
    SetExpect(
        resultB.has_value() && !*resultB,
        "export task B callback should receive its failed result.",
        failureCount);
}

void StartReloadOwnership(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    AppDataLoadTaskService service(dataManager, sharedState);

    std::vector<float> source{ 1.0f, 2.0f, 3.0f, 4.0f };
    auto task = service.BuildReloadFromBufferTask(
        source.data(),
        { 2, 2, 1 },
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        nullptr);

    source.assign(4, -1.0f);
    SetExpect(task.has_value(), "reload task should accept one owned voxel snapshot.", failureCount);
    if (!task) {
        return;
    }

    (*task)();
    SetExpect(
        dataManager->GetBuffer() == std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f },
        "reload worker must retain the construction-time voxel snapshot.",
        failureCount);
}

void StartLoadAdmission(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto service = std::make_shared<AppDataLoadTaskService>(dataManager, sharedState);
    const std::array<float, 4> reloadData = { 1.0f, 2.0f, 3.0f, 4.0f };

    std::atomic<bool> isStarted{ false };
    std::atomic<int> readyCount{ 0 };
    std::optional<std::packaged_task<void()>> fileTask;
    std::optional<std::packaged_task<void()>> reloadTask;
    std::array<std::optional<bool>, 2> callbackResults;

    auto waitStart = [&]() {
        readyCount.fetch_add(1);
        while (!isStarted.load()) {
            std::this_thread::yield();
        }
    };
    std::thread fileThread([&]() {
        waitStart();
        fileTask = service->BuildLoadFileTask(
            "missing.raw",
            { 1.0f, 1.0f, 1.0f },
            { 0.0f, 0.0f, 0.0f },
            [&](bool isSuccess) { callbackResults[0] = isSuccess; });
    });
    std::thread reloadThread([&]() {
        waitStart();
        reloadTask = service->BuildReloadFromBufferTask(
            reloadData.data(),
            { 2, 2, 1 },
            { 1.0f, 1.0f, 1.0f },
            { 0.0f, 0.0f, 0.0f },
            [&](bool isSuccess) { callbackResults[1] = isSuccess; });
    });

    while (readyCount.load() != 2) {
        std::this_thread::yield();
    }
    isStarted.store(true);
    fileThread.join();
    reloadThread.join();

    const bool isFileAccepted = fileTask.has_value();
    const bool isReloadAccepted = reloadTask.has_value();
    SetExpect(
        isFileAccepted != isReloadAccepted,
        "concurrent File/Reload requests must admit exactly one load transaction.",
        failureCount);
    SetExpect(
        !sharedState->StartLoad(LoadEventKind::File)
            && !sharedState->StartLoad(LoadEventKind::Reload),
        "an active load must reject both async and synchronous reload admission paths.",
        failureCount);
    const float rejectedSource = 1.0f;
    auto rejectedReload = service->BuildReloadFromBufferTask(
        &rejectedSource,
        { 100000, 100000, 10 },
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        nullptr);
    SetExpect(
        !rejectedReload.has_value(),
        "a rejected large reload must fail admission before reading or copying its raw buffer.",
        failureCount);

    if (isFileAccepted) {
        sharedState->SetFileLoadFailed();
    }
    else {
        sharedState->SetReloadLoadFailed();
    }
    SetExpect(
        !sharedState->StartLoad(LoadEventKind::File),
        "a worker terminal state must not release admission before main-thread consumption.",
        failureCount);
    const auto activeKind = isFileAccepted ? LoadEventKind::File : LoadEventKind::Reload;
    SetExpect(
        sharedState->ResetLoad(activeKind)
            && sharedState->StartLoad(LoadEventKind::File),
        "main-thread reset must release the completed load transaction.",
        failureCount);
    sharedState->SetFileLoadFailed();
    sharedState->ResetLoad(LoadEventKind::File);

    if (isFileAccepted) {
        service->SetFileLoadCallbackReady(true);
    }
    if (isReloadAccepted) {
        service->SetReloadReady(true);
    }
    if (service->ResetFileCallback()) {
        service->SendFileLoadCallback();
    }
    if (service->ResetReloadCallback()) {
        service->SendReloadCallback();
    }

    SetExpect(
        callbackResults[0].has_value() && callbackResults[1].has_value(),
        "accepted and rejected requests must retain their own callbacks.",
        failureCount);
    SetExpect(
        callbackResults[0].value_or(false) == isFileAccepted
            && callbackResults[1].value_or(false) == isReloadAccepted,
        "a rejected request must not overwrite the accepted request callback result.",
        failureCount);
}

void StartNullRejection(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    AppDataLoadTaskService service(dataManager, sharedState);
    std::optional<bool> activeResult;

    auto activeTask = service.BuildLoadFileTask(
        "active.raw",
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        [&](bool isSuccess) { activeResult = isSuccess; });
    auto rejectedTask = service.BuildLoadFileTask(
        "rejected.raw",
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        nullptr);

    SetExpect(activeTask.has_value() && !rejectedTask.has_value(),
        "a second same-kind request must be rejected without replacing the active task.",
        failureCount);
    service.SetFileLoadCallbackReady(true);
    if (service.ResetFileCallback()) {
        service.SendFileLoadCallback();
    }
    SetExpect(activeResult.has_value() && *activeResult,
        "a null rejected callback must not steal the active same-kind callback.",
        failureCount);
    sharedState->SetFileLoadFailed();
    sharedState->ResetLoad(LoadEventKind::File);
}

void StartWorkerFailure(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    AppDataLoadTaskService service(dataManager, sharedState);

    dataManager->SetLoadThrow(true);
    auto fileTask = service.BuildLoadFileTask(
        "throw.raw",
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        nullptr);
    SetExpect(fileTask.has_value(), "throwing file worker should be constructed.", failureCount);
    if (fileTask) {
        (*fileTask)();
    }
    SetExpect(sharedState->GetFileLoadState() == LoadState::Failed,
        "a file worker exception must publish the failed terminal state.",
        failureCount);
    sharedState->ResetLoad(LoadEventKind::File);

    dataManager->SetBufferThrow(true);
    const std::array<float, 4> reloadData = { 1.0f, 2.0f, 3.0f, 4.0f };
    auto reloadTask = service.BuildReloadFromBufferTask(
        reloadData.data(),
        { 2, 2, 1 },
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        nullptr);
    SetExpect(reloadTask.has_value(), "throwing reload worker should be constructed.", failureCount);
    if (reloadTask) {
        (*reloadTask)();
    }
    SetExpect(sharedState->GetReloadLoadState() == LoadState::Failed,
        "a reload worker exception must publish the failed terminal state.",
        failureCount);
    SetExpect(sharedState->ResetLoad(LoadEventKind::Reload)
        && sharedState->StartLoad(LoadEventKind::File),
        "worker exception cleanup must allow the next load transaction.",
        failureCount);
    sharedState->SetFileLoadFailed();
    sharedState->ResetLoad(LoadEventKind::File);
}

void StartVizLoadFailure(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto eventHub = std::make_shared<SharedStateBroadcaster>();
    auto sharedState = std::make_shared<SharedInteractionState>(eventHub);
    VizService service(dataManager, sharedState, eventHub);
    service.SetRenderContext(nullptr, nullptr);

    std::optional<bool> fileResult;
    std::optional<bool> reloadResult;
    std::optional<bool> retryResult;
    bool isReloadAccepted = false;
    const std::array<float, 4> reloadSource = { 1.0f, 2.0f, 3.0f, 4.0f };
    dataManager->SetBufferThrow(true);

    service.LoadFileAsync(
        "missing.raw",
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        [&](bool isSuccess) {
            fileResult = isSuccess;
            isReloadAccepted = service.ReloadFromBufferAsync(
                reloadSource.data(),
                { 2, 2, 1 },
                { 1.0f, 1.0f, 1.0f },
                { 0.0f, 0.0f, 0.0f },
                [&](bool isReloadSuccess) { reloadResult = isReloadSuccess; });
        });

    const auto sendUntilCallback = [&](const auto& hasResult) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!hasResult() && std::chrono::steady_clock::now() < deadline) {
            service.SendUpdates();
            std::this_thread::yield();
        }
        return hasResult();
    };

    SetExpect(sendUntilCallback([&]() { return fileResult.has_value(); }),
        "VizService should consume the file failure and deliver its callback.", failureCount);
    SetExpect(service.GetFileLoadState() == LoadState::Failed
        && fileResult.has_value() && !*fileResult && isReloadAccepted,
        "file failure callback must run false and reenter one reload transaction.",
        failureCount);

    SetExpect(sendUntilCallback([&]() { return reloadResult.has_value(); }),
        "VizService should consume the reentered reload failure and deliver its callback.", failureCount);
    SetExpect(service.GetReloadLoadState() == LoadState::Failed
        && reloadResult.has_value() && !*reloadResult,
        "callback reentry reload must retain its event kind and false callback.",
        failureCount);

    service.LoadFileAsync(
        "retry.raw",
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        [&](bool isSuccess) { retryResult = isSuccess; });
    SetExpect(sendUntilCallback([&]() { return retryResult.has_value(); }),
        "completed failure transactions must allow a later file request.", failureCount);
    SetExpect(service.GetFileLoadState() == LoadState::Failed
        && retryResult.has_value() && !*retryResult,
        "retry file failure callback must be delivered exactly as false.",
        failureCount);
}

void StartMultiViewLoad(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto eventHub = std::make_shared<SharedStateBroadcaster>();
    auto sharedState = std::make_shared<SharedInteractionState>(eventHub);

    VizService ownerService(dataManager, sharedState, eventHub);
    VizService laggingService(dataManager, sharedState, eventHub);
    std::atomic<bool> isGateEntered{ false };
    std::atomic<bool> isGateReleased{ false };
    auto gateOwner = std::make_shared<int>(0);
    // owner 先注册；gate 在 B 前阻塞旧终态，确定性验证广播未完成时 admission 不可释放。
    ownerService.SetRenderContext(nullptr, nullptr);
    eventHub->SetObserver(gateOwner, [&](UpdateFlags flags) {
        if ((flags & UpdateFlags::LoadFailed) == UpdateFlags::None
            || (flags & UpdateFlags::FileLoad) == UpdateFlags::None) {
            return;
        }
        isGateEntered.store(true);
        while (!isGateReleased.load()) {
            std::this_thread::yield();
        }
    });
    laggingService.SetRenderContext(nullptr, nullptr);

    std::optional<bool> fileResult;
    std::optional<bool> reloadResult;
    bool isReloadAccepted = false;
    const std::array<float, 4> reloadSource = { 1.0f, 2.0f, 3.0f, 4.0f };

    ownerService.LoadFileAsync(
        "missing-multi-view.raw",
        { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f },
        [&](bool isSuccess) {
            fileResult = isSuccess;
            isReloadAccepted = ownerService.ReloadFromBufferAsync(
                reloadSource.data(),
                { 2, 2, 1 },
                { 1.0f, 1.0f, 1.0f },
                { 0.0f, 0.0f, 0.0f },
                [&](bool isReloadSuccess) { reloadResult = isReloadSuccess; });
        });

    const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!isGateEntered.load() && std::chrono::steady_clock::now() < gateDeadline) {
        std::this_thread::yield();
    }
    SetExpect(isGateEntered.load(),
        "File failure broadcast should reach the deterministic observer gate.",
        failureCount);
    ownerService.SendUpdates();
    SetExpect(!fileResult.has_value()
        && !sharedState->StartLoad(LoadEventKind::Reload),
        "owner must not reset admission before every observer receives the terminal payload.",
        failureCount);
    isGateReleased.store(true);

    const auto sendOwnerUntil = [&](const auto& hasResult) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!hasResult() && std::chrono::steady_clock::now() < deadline) {
            ownerService.SendUpdates();
            std::this_thread::yield();
        }
        return hasResult();
    };

    SetExpect(sendOwnerUntil([&]() { return fileResult.has_value(); })
        && fileResult.has_value() && !*fileResult && isReloadAccepted,
        "only the owner view may release File admission and reenter Reload.",
        failureCount);

    const auto pendingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!dataManager->GetPendingReady()
        && std::chrono::steady_clock::now() < pendingDeadline) {
        std::this_thread::yield();
    }
    SetExpect(dataManager->GetPendingReady(),
        "reentered Reload should publish one pending image.", failureCount);

    const int imageReadsBefore = dataManager->GetImageReadCount();
    const int snapshotReadsBefore = dataManager->GetSnapshotReadCount();
    laggingService.SendUpdates();
    SetExpect(dataManager->GetSnapshotReadCount() > snapshotReadsBefore
            && dataManager->GetImageReadCount() == imageReadsBefore,
        "a lagging non-owner view must converge through the shared snapshot without a public deep copy.",
        failureCount);
    SetExpect(!sharedState->StartLoad(LoadEventKind::File),
        "a non-owner view must not release the owner's Reload admission.",
        failureCount);

    SetExpect(sendOwnerUntil([&]() { return reloadResult.has_value(); })
        && reloadResult.has_value() && *reloadResult,
        "the owner view must receive the matching successful Reload callback.",
        failureCount);

    SetExpect(sharedState->StartLoad(LoadEventKind::Reload),
        "a Host-owned Reload should be accepted after the owner callback.",
        failureCount);
    sharedState->SetReloadLoadFailed();
    laggingService.SendUpdates();
    ownerService.SendUpdates();
    SetExpect(!sharedState->StartLoad(LoadEventKind::File),
        "render views must not release a Host-owned transaction.",
        failureCount);
    SetExpect(sharedState->ResetLoad(LoadEventKind::Reload)
        && sharedState->StartLoad(LoadEventKind::File)
        && sharedState->ResetLoad(LoadEventKind::File),
        "Host must release its transaction after every render view has converged.",
        failureCount);
}

    int GetFailCount()
    {
        int failureCount = 0;
        StartTaskOrder(failureCount);
        StartReloadOwnership(failureCount);
        StartLoadAdmission(failureCount);
        StartNullRejection(failureCount);
        StartWorkerFailure(failureCount);
        StartVizLoadFailure(failureCount);
        StartMultiViewLoad(failureCount);
        return failureCount;
    }
};

int AppTaskSuite::GetFailCount() const
{
    return AppTaskCases().GetFailCount();
}
