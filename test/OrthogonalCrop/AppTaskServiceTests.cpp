#include "Tasks/AppDataExportTaskService.h"
#include "Tasks/AppDataLoadTaskService.h"
#include "../TestDataPort.h"
#include "Algorithms/CropAlgorithm.h"
#include "AppState.h"
#include "AppStateEvents.h"
#include "App/Services/AppServiceFactory.h"
#include "App/Services/AppPorts.h"
#include "Data/DataConverters.h"
#include "Data/DataManager.h"
#include "Data/DataPayloads.h"
#include "Data/VtkDataBridge.h"
#include "Data/VolumeTypes.h"
#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/Types/HostRequestTypes.h"
#include "PlanarTestSuites.h"
#include "Render/CropShaderController.h"
#include "Render/Contracts/RenderStrategyFactory.h"
#include "Render/Strategies/CompositeStrategy.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"
#include "Render/Strategies/SliceStrategy.h"
#include "Render/Strategies/VolumeStrategy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCallbackCommand.h>
#include <vtkCell.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkMatrix3x3.h>
#include <vtkMatrix4x4.h>
#include <vtkOBJReader.h>
#include <vtkPNGReader.h>
#include <vtkPLYReader.h>
#include <vtkPointData.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSTLReader.h>
#include <vtkTriangleFilter.h>
#include <vtkTransform.h>
#include <vtkUnsignedCharArray.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkWeakPointer.h>

namespace {
void SetExpect(bool isPassed, const char* message, int& failureCount)
{
    if (isPassed) return;
    ++failureCount;
    std::cerr << "[AppTaskTests] " << message << '\n';
}

StrategyCreate GetStrategyFactory()
{
    return [](const VizMode mode)
        -> std::shared_ptr<AbstractVisualStrategy> {
        switch (mode) {
        case VizMode::Volume:
            return std::make_shared<VolumeStrategy>();
        case VizMode::IsoSurface:
            return std::make_shared<IsoSurfaceStrategy>();
        case VizMode::SliceTop_down:
            return std::make_shared<SliceStrategy>(Orientation::Top_down);
        case VizMode::SliceFront_back:
            return std::make_shared<SliceStrategy>(Orientation::Front_back);
        case VizMode::SliceLeft_right:
            return std::make_shared<SliceStrategy>(Orientation::Left_right);
        case VizMode::CompositeVolume:
            return std::make_shared<CompositeStrategy>(
                std::make_shared<VolumeStrategy>());
        case VizMode::CompositeIsoSurface:
            return std::make_shared<CompositeStrategy>(
                std::make_shared<IsoSurfaceStrategy>());
        default:
            return nullptr;
        }
    };
}

class FailVisualStrategy final : public VolumeStrategy {
public:
    explicit FailVisualStrategy(
        std::shared_ptr<std::atomic<int>> attachCount)
        : m_attachCount(std::move(attachCount))
    {
    }

    void AttachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (m_attachCount) ++(*m_attachCount);
        VolumeStrategy::AttachRenderer(std::move(renderer));
        throw std::runtime_error("intentional strategy attach failure");
    }

private:
    std::shared_ptr<std::atomic<int>> m_attachCount;
};

struct VisualStateCapture final {
    RenderParams params;
    UpdateFlags flags = UpdateFlags::None;
    int setCount = 0;
};

class CaptureVisualStrategy final : public VolumeStrategy {
public:
    explicit CaptureVisualStrategy(
        std::shared_ptr<VisualStateCapture> capture)
        : m_capture(std::move(capture))
    {
    }

    bool SetVisualState(
        const RenderParams& params,
        const UpdateFlags flags) override
    {
        if (m_capture) {
            m_capture->params = params;
            m_capture->flags = flags;
            ++m_capture->setCount;
        }
        return VolumeStrategy::SetVisualState(params, flags);
    }

private:
    std::shared_ptr<VisualStateCapture> m_capture;
};

class RejectQualityStrategy final : public VolumeStrategy {
public:
    explicit RejectQualityStrategy(
        std::shared_ptr<std::atomic<int>> rejectCount)
        : m_rejectCount(std::move(rejectCount))
    {
    }

    bool SetVisualState(
        const RenderParams& params,
        const UpdateFlags flags) override
    {
        if (flags == UpdateFlags::Quality) {
            if (m_rejectCount) ++(*m_rejectCount);
            return false;
        }
        return VolumeStrategy::SetVisualState(params, flags);
    }

private:
    std::shared_ptr<std::atomic<int>> m_rejectCount;
};

struct QualitySwitchGate final {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<VolumeQuality> qualities;
    bool isArmed = false;
    bool isStarted = false;
    bool isReleased = false;
    bool hasTimedOut = false;
};

class WaitQualityStrategy final : public VolumeStrategy {
public:
    explicit WaitQualityStrategy(
        std::shared_ptr<QualitySwitchGate> gate)
        : m_gate(std::move(gate))
    {
    }

    bool SetVisualState(
        const RenderParams& params,
        const UpdateFlags flags) override
    {
        if (m_gate
            && (flags & UpdateFlags::Quality) != UpdateFlags::None) {
            std::unique_lock<std::mutex> lock(m_gate->mutex);
            m_gate->qualities.push_back(params.volumeQuality);
            if (m_gate->isArmed) {
                m_gate->isArmed = false;
                m_gate->isStarted = true;
                m_gate->condition.notify_all();
                constexpr auto waitLimit = std::chrono::seconds(5);
                if (!m_gate->condition.wait_for(
                        lock, waitLimit,
                        [&]() { return m_gate->isReleased; })) {
                    m_gate->hasTimedOut = true;
                }
            }
        }
        return VolumeStrategy::SetVisualState(params, flags);
    }

private:
    std::shared_ptr<QualitySwitchGate> m_gate;
};

class DataStub final : public BaseDataManager {
public:
    bool SetSpacing(const std::array<double, 3>& spacing) override
    {
        if (setSpacingCall) return setSpacingCall(spacing);
        return GetPrimaryImage()
            ? BaseDataManager::SetSpacing(spacing) : true;
    }

    bool SetDataLoaded(const std::string& path, const VolumeLayout& layout) override
    {
        loadedPath = path;
        loadedDims = layout.GetDimensions();
        if (isThrowNeeded) throw std::runtime_error("load failure");
        return isLoadSuccess;
    }

    bool SetFromBuffer(const VolumeBuffer& buffer) override
    {
        loadedVoxels = buffer.GetVoxels();
        loadedDims = buffer.GetLayout().GetDimensions();
        if (isThrowNeeded) throw std::runtime_error("reload failure");
        if (!isLoadSuccess) return false;
        const auto& layout = buffer.GetLayout();
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(layout.GetDimensions().data());
        image->SetSpacing(
            layout.GetSpacing()[0],
            layout.GetSpacing()[1],
            layout.GetSpacing()[2]);
        image->SetOrigin(
            layout.GetOrigin()[0],
            layout.GetOrigin()[1],
            layout.GetOrigin()[2]);
        image->AllocateScalars(VTK_FLOAT, 1);
        auto* values = static_cast<float*>(image->GetScalarPointer());
        if (!values) return false;
        std::copy(loadedVoxels.begin(), loadedVoxels.end(), values);
        return SetLoadImage(std::move(image));
    }

    bool SetFromBuffer(
        const VolumeBuffer& buffer,
        const TaskStopToken& stopToken) override
    {
        return !stopToken.GetIsStopped()
            && SetFromBuffer(buffer)
            && !stopToken.GetIsStopped();
    }
    bool ExportData(
        const VtkImageGridSnapshot& snapshot,
        const std::string& outputDir,
        const DataExportParams& params) override
    {
        exportedSnapshot = snapshot;
        exportedDir = outputDir;
        exportedParams = params;
        return true;
    }
    bool ExportData(
        const VtkImageGridSnapshot& snapshot,
        const std::string& outputDir,
        const DataExportParams& params,
        const TaskStopToken& stopToken) override
    {
        if (exportCall) return exportCall(stopToken);
        return AbstractDataManager::ExportData(
            snapshot,
            outputDir,
            params,
            stopToken);
    }
    bool ExportSlices(const std::string&, Orientation, const WindowLevelParams&,
        const std::array<double, 16>&) override { return false; }

    VtkImageGridSnapshot exportedSnapshot;
    std::string exportedDir;
    DataExportParams exportedParams;
    std::string loadedPath;
    std::array<int, 3> loadedDims{};
    std::vector<float> loadedVoxels;
    std::function<bool(const std::array<double, 3>&)>
        setSpacingCall;
    std::function<bool(const TaskStopToken&)> exportCall;
    bool isLoadSuccess = true;
    bool isThrowNeeded = false;

    bool SetPrimaryForTest(
        vtkSmartPointer<vtkImageData> image,
        vtkSmartPointer<vtkImageData> mask = {})
    {
        if (!SetLoadImage(std::move(image), std::move(mask))) return false;
        const auto stage = GetLoadStage();
        VtkImageGridSnapshot published;
        return stage && SetLoadCommit(stage, published) && published;
    }
};

class DataManagerProbe final : public BaseDataManager {
public:
    bool SetDataLoaded(
        const std::string&,
        const VolumeLayout&) override
    {
        return false;
    }

    bool SetInitial(
        vtkSmartPointer<vtkImageData> image,
        vtkSmartPointer<vtkImageData> mask = {})
    {
        if (!mask) return SetOwnedImage(std::move(image));
        if (!SetLoadImage(std::move(image), std::move(mask))) return false;
        const auto stage = GetLoadStage();
        VtkImageGridSnapshot published;
        return stage && SetLoadCommit(stage, published) && published;
    }

    bool SetCandidate(
        vtkImageData* image,
        vtkImageData* mask,
        const VtkImageGridSnapshot& expected,
        VtkImageGridSnapshot& published)
    {
        published.reset();
        auto payload = m_bridge.CreateImagePayload(image, mask);
        if (!payload || !expected || !expected->binding) return false;
        const auto entity = CreateDataEntityId();
        const DataRevisionRef ref{ entity, 1 };
        DataTransaction transaction;
        transaction.outputs.push_back(DataRevisionDraft{
            entity, 0, DataTypes::imageGrid3D, {}, std::move(payload), {} });
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(primaryVolumeBinding),
            expected->binding->revision,
            true,
            expected->binding->target,
            ref });
        if (SetDataCommit(std::move(transaction)).status
            != DataCommitStatus::Succeeded) {
            return false;
        }
        published = GetPrimaryImage();
        return published != nullptr;
    }

    VtkImageGridSnapshot GetPrimary() const
    {
        return GetPrimaryImage();
    }

private:
    VtkDataBridge m_bridge;
};

void StartVolumeTypes(int& failureCount)
{
    SetExpect(!VolumeLayout::Create({ 0, 2, 3 }, { 1, 1, 1 }, { 0, 0, 0 }),
        "zero dimension must fail", failureCount);
    SetExpect(!VolumeLayout::Create({ 2, 2, 3 }, { 1, 0, 1 }, { 0, 0, 0 }),
        "non-positive spacing must fail", failureCount);
    const auto layout = VolumeLayout::Create(
        { 2, 2, 3 }, { 0.5f, 1.0f, 2.0f }, { 3.0f, 4.0f, 5.0f });
    SetExpect(layout && layout->GetVoxelCount() == 12
        && layout->GetByteCount() == 12 * sizeof(float),
        "valid layout counts must be exact", failureCount);
    if (!layout) return;
    SetExpect(!VolumeBuffer::Create(std::vector<float>(11), *layout)
        && !VolumeBuffer::Create(std::vector<float>(13), *layout),
        "short and long owning buffers must fail", failureCount);
}

void StartOwningTasks(int& failureCount)
{
    auto dataManager = std::make_shared<DataStub>();
    AppDataLoadTaskService service(dataManager);
    auto layout = VolumeLayout::Create(
        { 2, 2, 2 }, { 1, 1, 1 }, { 0, 0, 0 });
    if (!layout) {
        ++failureCount;
        return;
    }

    std::vector<float> source{ 0, 1, 2, 3, 4, 5, 6, 7 };
    auto buffer = VolumeBuffer::Create(std::move(source), *layout);
    auto reloadTask = buffer
        ? service.BuildReloadTask(std::move(*buffer)) : std::nullopt;
    SetExpect(reloadTask.has_value(), "owning reload task must be built", failureCount);
    if (reloadTask) {
        auto result = reloadTask->get_future();
        (*reloadTask)(TaskStopToken{});
        SetExpect(result.get() && dataManager->loadedVoxels
            == std::vector<float>({ 0, 1, 2, 3, 4, 5, 6, 7 }),
            "task must retain voxels after caller storage is destroyed", failureCount);
    }

    auto fileTask = service.BuildLoadFileTask("volume.raw", *layout);
    SetExpect(fileTask.has_value(), "file task must be built", failureCount);
    if (fileTask) {
        auto result = fileTask->get_future();
        (*fileTask)(TaskStopToken{});
        SetExpect(result.get() && dataManager->loadedPath == "volume.raw"
            && dataManager->loadedDims == std::array<int, 3>{ 2, 2, 2 },
            "file task must retain path and layout", failureCount);
    }

    dataManager->isThrowNeeded = true;
    auto failedTask = service.BuildLoadFileTask("throw.raw", *layout);
    if (failedTask) {
        auto result = failedTask->get_future();
        (*failedTask)(TaskStopToken{});
        SetExpect(!result.get(), "worker exceptions must become false", failureCount);
    }
}

vtkSmartPointer<vtkImageData> BuildExportImage()
{
    auto image =
        vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->SetSpacing(0.5, 1.0, 1.5);
    image->AllocateScalars(VTK_FLOAT, 1);
    for (int z = 0; z < 4; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                image->SetScalarComponentFromFloat(
                    x, y, z, 0,
                    static_cast<float>(x + y + z));
            }
        }
    }
    return image;
}

void StartExportSnapshot(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataStub>();
    (void)dataManager->SetPrimaryForTest(BuildExportImage());
    const auto firstState = dataManager->GetPrimaryImage();

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    auto viewState =
        std::make_shared<ViewPresentationState>();
    viewState->SetIsoValue(2.5);
    const std::array<double, 16> firstMatrix = {
        1.0, 0.0, 0.0, 10.0,
        0.0, 1.0, 0.0, 20.0,
        0.0, 0.0, 1.0, 30.0,
        0.0, 0.0, 0.0, 1.0
    };
    state->SetModelMatrix(firstMatrix);
    state->SetScalarRange(0.0, 9.0);
    VolumeTransferFunction firstTransfer;
    firstTransfer.colorNodes = {
        { 0.0, 0.0, 1.0, 0.0 },
        { 9.0, 0.0, 1.0, 1.0 }
    };
    firstTransfer.opacityNodes = {
        { 0.0, 1.0 },
        { 9.0, 1.0 }
    };
    (void)viewState->SetVolumeTransferFunction(firstTransfer);
    AppDataExportTaskService service(
        dataManager, state, viewState);
    auto task = service.BuildDataTask(
        "exports", ".ply");

    (void)dataManager->SetPrimaryForTest(BuildExportImage());
    viewState->SetIsoValue(4.5);
    state->SetModelMatrix({
        1.0, 0.0, 0.0, -10.0,
        0.0, 1.0, 0.0, -20.0,
        0.0, 0.0, 1.0, -30.0,
        0.0, 0.0, 0.0, 1.0
    });
    state->SetScalarRange(-10.0, 10.0);
    VolumeTransferFunction secondTransfer;
    secondTransfer.colorNodes = {
        { -10.0, 1.0, 0.0, 0.0 },
        { 10.0, 0.0, 0.0, 1.0 }
    };
    secondTransfer.opacityNodes = {
        { -10.0, 1.0 },
        { 10.0, 1.0 }
    };
    (void)viewState->SetVolumeTransferFunction(secondTransfer);

    SetExpect(task.has_value(),
        "data export task should accept a valid snapshot",
        failureCount);
    if (!task) return;
    auto result = task->get_future();
    (*task)(TaskStopToken{});
    SetExpect(result.get()
            && dataManager->exportedSnapshot
            && firstState
            && dataManager->exportedSnapshot->data->self
                == firstState->data->self
            && dataManager->exportedDir
                == "exports"
            && dataManager->exportedParams.extension
                == ".ply"
            && dataManager->exportedParams.isoValue == 2.5
            && dataManager->exportedParams.scalarRange
                == std::array<double, 2>{ 0.0, 9.0 }
            && dataManager->exportedParams.modelToWorld
                == firstMatrix
            && dataManager->exportedParams
                .volumeTransferFunction.colorNodes.size() == 2
            && dataManager->exportedParams
                .volumeTransferFunction.colorNodes[0].g == 1.0
            && dataManager->exportedParams
                .volumeTransferFunction.colorNodes[1].b == 1.0,
        "data export must preserve target and admission-time snapshots",
        failureCount);
}

void StartBoundedTasks(int& failureCount)
{
    TaskStopSource source;
    const auto token = source.GetToken();
    SetExpect(!token.GetIsStopped()
            && source.Stop()
            && token.GetIsStopped(),
        "stop token must share cancellation state",
        failureCount);

    auto dataManager = std::make_shared<DataStub>();
    (void)dataManager->SetPrimaryForTest(BuildExportImage());

    std::mutex taskMutex;
    std::condition_variable taskChanged;
    int startedCount = 0;
    dataManager->exportCall = [&](const TaskStopToken& stopToken) {
        std::unique_lock<std::mutex> lock(taskMutex);
        ++startedCount;
        taskChanged.notify_all();
        while (!stopToken.GetIsStopped()) {
            taskChanged.wait_for(
                lock, std::chrono::milliseconds(1));
        }
        return false;
    };

    std::atomic<int> workerCount{ 0 };
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState =
        std::make_shared<SharedInteractionState>(broadcaster);
    args.eventSource = broadcaster;
    args.workerStart = [&workerCount](AppWorkerWork work) {
        ++workerCount;
        return std::thread(std::move(work));
    };
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.data || !ports.taskControl) {
        SetExpect(false,
            "bounded executor needs data and task control ports",
            failureCount);
        return;
    }

    std::atomic<int> callbackCount{ 0 };
    const auto callback = [&callbackCount](bool) {
        ++callbackCount;
    };
    const auto first = ports.app.data->ExportDataAsync(
        "bounded-a", ".raw", callback);
    const auto second = ports.app.data->ExportDataAsync(
        "bounded-b", ".raw", callback);
    bool areWorkersStarted = false;
    {
        std::unique_lock<std::mutex> lock(taskMutex);
        areWorkersStarted = taskChanged.wait_for(
            lock,
            std::chrono::seconds(1),
            [&startedCount] { return startedCount == 2; });
    }
    const auto third = ports.app.data->ExportDataAsync(
        "bounded-c", ".raw", callback);
    const bool isStopStarted = ports.taskControl->SetTaskStopping();
    const bool isStopped = ports.taskControl->StopTasks(
        std::chrono::steady_clock::now()
            + std::chrono::seconds(1));
    const auto afterStop = ports.app.data->ExportDataAsync(
        "bounded-d", ".raw", callback);
    SetExpect(workerCount.load() == 3
            && first == TaskAdmissionResult::Accepted
            && second == TaskAdmissionResult::Accepted
            && areWorkersStarted
            && third == TaskAdmissionResult::QueueFull
            && isStopStarted && isStopped
            && afterStop == TaskAdmissionResult::Stopping
            && callbackCount.load() == 0,
        "fixed workers must bound admission and cooperatively stop",
        failureCount);

    auto blockedData = std::make_shared<DataStub>();
    (void)blockedData->SetPrimaryForTest(BuildExportImage());
    std::mutex blockMutex;
    std::condition_variable blockChanged;
    bool isBlockStarted = false;
    bool isBlockReleased = false;
    blockedData->exportCall = [&](const TaskStopToken&) {
        std::unique_lock<std::mutex> lock(blockMutex);
        isBlockStarted = true;
        blockChanged.notify_all();
        blockChanged.wait(lock, [&isBlockReleased] {
            return isBlockReleased;
        });
        return false;
    };

    auto blockedBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    AppServiceArgs blockedArgs;
    blockedArgs.dataManager = blockedData;
    blockedArgs.interactionState =
        std::make_shared<SharedInteractionState>(
            blockedBroadcaster);
    blockedArgs.eventSource = blockedBroadcaster;
    auto blockedPorts = CreateAppPorts(std::move(blockedArgs));
    const auto blockedAdmission = blockedPorts.app.data
        ? blockedPorts.app.data->ExportDataAsync(
            "blocked", ".raw", {})
        : TaskAdmissionResult::Unavailable;
    bool didBlockStart = false;
    {
        std::unique_lock<std::mutex> lock(blockMutex);
        didBlockStart = blockChanged.wait_for(
            lock,
            std::chrono::seconds(1),
            [&isBlockStarted] { return isBlockStarted; });
    }
    const bool isDeadlineKept = blockedPorts.taskControl
        && blockedPorts.taskControl->SetTaskStopping()
        && !blockedPorts.taskControl->StopTasks(
            std::chrono::steady_clock::now()
                + std::chrono::milliseconds(50));
    {
        const std::lock_guard<std::mutex> lock(blockMutex);
        isBlockReleased = true;
    }
    blockChanged.notify_all();
    const bool isRetryStopped = blockedPorts.taskControl
        && blockedPorts.taskControl->StopTasks(
            std::chrono::steady_clock::now()
                + std::chrono::seconds(1));
    SetExpect(blockedAdmission == TaskAdmissionResult::Accepted
            && didBlockStart
            && isDeadlineKept
            && isRetryStopped,
        "stop deadline must retain a blocked executor for retry",
        failureCount);
}

void StartExportFiles(int& failureCount)
{
    DataManagerProbe dataManager;
    SetExpect(
        dataManager.SetInitial(BuildExportImage()),
        "data export needs an image snapshot",
        failureCount);
    const auto snapshot =
        dataManager.GetPrimary();
    const auto uniqueId =
        std::chrono::steady_clock::now()
            .time_since_epoch().count();
    const auto outputDir =
        std::filesystem::temp_directory_path()
        / std::filesystem::u8path(
            u8"MVVCVTK_网格")
        / std::to_string(uniqueId);
    std::error_code error;
    std::filesystem::create_directories(
        outputDir, error);
    const std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 10.0,
        0.0, 1.0, 0.0, 20.0,
        0.0, 0.0, 1.0, 30.0,
        0.0, 0.0, 0.0, 1.0
    };
    DataExportParams params;
    params.isoValue = 2.5;
    params.modelToWorld = modelToWorld;
    params.scalarRange = { 0.0, 9.0 };
    params.volumeTransferFunction.colorNodes = {
        { 0.0, 0.0, 1.0, 0.0 },
        { 9.0, 0.0, 1.0, 1.0 }
    };
    params.volumeTransferFunction.opacityNodes = {
        { 0.0, 1.0 },
        { 9.0, 1.0 }
    };

    const auto plyPath =
        outputDir / "4x4x4_transform.ply";
    const auto stlPath =
        outputDir / "4x4x4_transform.stl";
    const auto objPath =
        outputDir / "4x4x4_transform.obj";
    params.extension = ".ply";
    const bool isPlySaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    params.extension = ".stl";
    const bool isStlSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    params.extension = ".obj";
    const bool isObjSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    SetExpect(!error && isPlySaved && isStlSaved && isObjSaved,
        "PLY, STL and OBJ should use one export entry",
        failureCount);

    auto plyReader =
        vtkSmartPointer<vtkPLYReader>::New();
    plyReader->SetFileName(
        plyPath.u8string().c_str());
    plyReader->Update();
    auto stlReader =
        vtkSmartPointer<vtkSTLReader>::New();
    stlReader->SetFileName(
        stlPath.u8string().c_str());
    stlReader->Update();
    auto objReader =
        vtkSmartPointer<vtkOBJReader>::New();
    objReader->SetFileName(
        objPath.u8string().c_str());
    objReader->Update();
    const auto getIsMeshValid =
        [](vtkPolyData* mesh) {
            if (!mesh
                || mesh->GetNumberOfPoints() == 0
                || mesh->GetNumberOfCells() == 0) {
                return false;
            }
            double bounds[6] = {};
            mesh->GetBounds(bounds);
            return bounds[0] >= 10.0
                && bounds[2] >= 20.0
                && bounds[4] >= 30.0;
        };
    SetExpect(
        getIsMeshValid(plyReader->GetOutput())
            && getIsMeshValid(
                stlReader->GetOutput())
            && getIsMeshValid(
                objReader->GetOutput()),
        "mesh files should contain baked world coordinates",
        failureCount);

    vtkPolyData* plyMesh = plyReader->GetOutput();
    auto* plyRgb = plyMesh && plyMesh->GetPointData()
        ? vtkUnsignedCharArray::SafeDownCast(
            plyMesh->GetPointData()->GetScalars())
        : nullptr;
    SetExpect(
        plyRgb && plyRgb->GetName()
            && std::string(plyRgb->GetName()) == "RGB"
            && plyRgb->GetNumberOfComponents() == 3
            && plyRgb->GetNumberOfTuples()
                == plyMesh->GetNumberOfPoints(),
        "PLY should round-trip one RGB tuple per mesh point",
        failureCount);
    bool isPlyRgbMatched = plyRgb != nullptr;
    if (plyRgb) {
        for (vtkIdType pointId = 0;
            pointId < plyRgb->GetNumberOfTuples();
            ++pointId) {
            unsigned char rgb[3] = {};
            plyRgb->GetTypedTuple(pointId, rgb);
            isPlyRgbMatched = isPlyRgbMatched
                && rgb[0] == 0 && rgb[1] == 255
                && rgb[2] >= 70 && rgb[2] <= 71;
        }
    }
    SetExpect(
        isPlyRgbMatched,
        "PLY RGB should match the frozen isosurface-scalar transfer function",
        failureCount);

    auto expectedIso =
        vtkSmartPointer<vtkFlyingEdges3D>::New();
    expectedIso->SetInputData(snapshot->image);
    expectedIso->SetValue(0, params.isoValue);
    expectedIso->ComputeNormalsOn();
    expectedIso->ComputeGradientsOff();
    auto expectedTriangles =
        vtkSmartPointer<vtkTriangleFilter>::New();
    expectedTriangles->SetInputConnection(
        expectedIso->GetOutputPort());
    expectedTriangles->Update();
    vtkPolyData* expectedMesh =
        expectedTriangles->GetOutput();
    vtkPolyData* objMesh = objReader->GetOutput();
    vtkPolyData* stlMesh = stlReader->GetOutput();
    vtkDataArray* expectedScalars = expectedMesh
        && expectedMesh->GetPointData()
        ? expectedMesh->GetPointData()->GetScalars()
        : nullptr;
    bool hasExpectedIsoScalars = expectedScalars
        && expectedScalars->GetNumberOfComponents() > 0
        && expectedScalars->GetNumberOfTuples()
            == expectedMesh->GetNumberOfPoints();
    if (hasExpectedIsoScalars) {
        for (vtkIdType pointId = 0;
            pointId < expectedScalars->GetNumberOfTuples();
            ++pointId) {
            hasExpectedIsoScalars = hasExpectedIsoScalars
                && std::abs(
                    expectedScalars->GetComponent(pointId, 0)
                        - params.isoValue)
                    <= 1e-5;
        }
    }
    SetExpect(
        hasExpectedIsoScalars,
        "the isosurface fixture should preserve point scalars at the frozen iso",
        failureCount);

    // writer 可以合法重排 point/cell id；以量化后的世界坐标多重集比较实际几何。
    using PointKey = std::array<long long, 3>;
    using TriangleKey = std::array<PointKey, 3>;
    constexpr double coordinateScale = 1e5;
    const auto getPointKey =
        [&](const double point[3], bool isExpected) {
            PointKey key = {};
            for (int axis = 0; axis < 3; ++axis) {
                const double worldValue = point[axis]
                    + (isExpected
                        ? modelToWorld[axis * 4 + 3]
                        : 0.0);
                key[static_cast<std::size_t>(axis)] =
                    std::llround(worldValue * coordinateScale);
            }
            return key;
        };
    const auto getPointKeys =
        [&](vtkPolyData* mesh, bool isExpected) {
            std::vector<PointKey> points;
            if (!mesh || mesh->GetNumberOfPoints() == 0) {
                return points;
            }
            points.reserve(static_cast<std::size_t>(
                mesh->GetNumberOfPoints()));
            for (vtkIdType pointId = 0;
                pointId < mesh->GetNumberOfPoints();
                ++pointId) {
                double point[3] = {};
                mesh->GetPoint(pointId, point);
                if (!std::isfinite(point[0])
                    || !std::isfinite(point[1])
                    || !std::isfinite(point[2])) {
                    points.clear();
                    return points;
                }
                points.push_back(
                    getPointKey(point, isExpected));
            }
            std::sort(points.begin(), points.end());
            return points;
        };
    const auto expectedPoints =
        getPointKeys(expectedMesh, true);
    const bool isGeometryMatched =
        !expectedPoints.empty()
        && getPointKeys(plyMesh, false) == expectedPoints
        && getPointKeys(objMesh, false) == expectedPoints;
    SetExpect(
        isGeometryMatched,
        "PLY and OBJ should round-trip the real world-space mesh point set",
        failureCount);

    const auto getTriangleKeys =
        [&](vtkPolyData* mesh, bool isExpected) {
            std::vector<TriangleKey> triangles;
            if (!mesh || mesh->GetNumberOfCells() == 0) {
                return triangles;
            }
            triangles.reserve(static_cast<std::size_t>(
                mesh->GetNumberOfCells()));
            for (vtkIdType cellId = 0;
                cellId < mesh->GetNumberOfCells(); ++cellId) {
                vtkCell* cell = mesh->GetCell(cellId);
                if (!cell || cell->GetNumberOfPoints() != 3) {
                    triangles.clear();
                    return triangles;
                }
                TriangleKey triangle = {};
                for (vtkIdType corner = 0; corner < 3;
                    ++corner) {
                    double point[3] = {};
                    mesh->GetPoint(
                        cell->GetPointId(corner), point);
                    if (!std::isfinite(point[0])
                        || !std::isfinite(point[1])
                        || !std::isfinite(point[2])) {
                        triangles.clear();
                        return triangles;
                    }
                    triangle[static_cast<std::size_t>(corner)] =
                        getPointKey(point, isExpected);
                }
                std::sort(triangle.begin(), triangle.end());
                triangles.push_back(triangle);
            }
            std::sort(triangles.begin(), triangles.end());
            return triangles;
        };
    const auto expectedTopology =
        getTriangleKeys(expectedMesh, true);
    SetExpect(
        !expectedTopology.empty()
            && getTriangleKeys(plyMesh, false)
                == expectedTopology
            && getTriangleKeys(objMesh, false)
                == expectedTopology
            && getTriangleKeys(stlMesh, false)
                == expectedTopology,
        "PLY, OBJ and STL should round-trip the real triangle geometry",
        failureCount);

    bool isPlyTriangulated = plyMesh != nullptr;
    if (plyMesh) {
        for (vtkIdType cellId = 0;
            cellId < plyMesh->GetNumberOfCells();
            ++cellId) {
            vtkCell* cell = plyMesh->GetCell(cellId);
            isPlyTriangulated = isPlyTriangulated
                && cell && cell->GetNumberOfPoints() == 3;
        }
    }
    const auto getHasUnitNormals =
        [](vtkPolyData* mesh) {
            vtkDataArray* normals = mesh
                && mesh->GetPointData()
                ? mesh->GetPointData()->GetNormals()
                : nullptr;
            bool hasNormals = normals
                && normals->GetNumberOfComponents() == 3
                && normals->GetNumberOfTuples()
                    == mesh->GetNumberOfPoints();
            if (!hasNormals) {
                return false;
            }
            for (vtkIdType pointId = 0;
                pointId < normals->GetNumberOfTuples();
                ++pointId) {
                double normal[3] = {};
                normals->GetTuple(pointId, normal);
                const double length = std::sqrt(
                    normal[0] * normal[0]
                    + normal[1] * normal[1]
                    + normal[2] * normal[2]);
                hasNormals = hasNormals
                    && std::isfinite(length)
                    && std::abs(length - 1.0) <= 1e-5;
            }
            return hasNormals;
        };
    SetExpect(
        isPlyTriangulated
            && getHasUnitNormals(plyMesh)
            && getHasUnitNormals(objMesh),
        "PLY and OBJ should round-trip unit point normals",
        failureCount);

    params.extension = ".ply";
    params.volumeTransferFunction.colorNodes.clear();
    const bool isGrayPlySaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    auto grayPlyReader =
        vtkSmartPointer<vtkPLYReader>::New();
    grayPlyReader->SetFileName(
        plyPath.u8string().c_str());
    grayPlyReader->Update();
    auto* grayRgb = grayPlyReader->GetOutput()
        && grayPlyReader->GetOutput()->GetPointData()
        ? vtkUnsignedCharArray::SafeDownCast(
            grayPlyReader->GetOutput()
                ->GetPointData()->GetScalars())
        : nullptr;
    bool isGrayRgbMatched = isGrayPlySaved
        && grayRgb && grayRgb->GetNumberOfComponents() == 3;
    if (isGrayRgbMatched) {
        for (vtkIdType pointId = 0;
            pointId < grayRgb->GetNumberOfTuples();
            ++pointId) {
            unsigned char rgb[3] = {};
            grayRgb->GetTypedTuple(pointId, rgb);
            isGrayRgbMatched = isGrayRgbMatched
                && rgb[0] >= 70 && rgb[0] <= 71
                && rgb[0] == rgb[1]
                && rgb[1] == rgb[2];
        }
    }
    SetExpect(
        isGrayRgbMatched,
        "PLY should use a scalar-range grayscale fallback when no TF is frozen",
        failureCount);

    params.volumeTransferFunction.colorNodes = {
        { 0.0, 0.0, 1.0, 0.0 },
        { 9.0, 0.0, 1.0, 1.0 }
    };
    params.volumeTransferFunction.opacityNodes = {
        { 0.0, 1.0 },
        { 9.0, 1.0 }
    };
    params.extension = ".raw";
    const bool isRawSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    SetExpect(
        isRawSaved
            && std::filesystem::exists(
                outputDir
                / "4x4x4_transform.raw"),
        "Raw should reuse the same export entry",
        failureCount);

    auto invalidMatrix = modelToWorld;
    invalidMatrix[0] =
        std::numeric_limits<double>::quiet_NaN();
    auto singularMatrix = modelToWorld;
    singularMatrix[0] = 0.0;
    auto projectiveMatrix = modelToWorld;
    projectiveMatrix[12] = 0.1;
    auto smallScaleMatrix = modelToWorld;
    smallScaleMatrix[0] = 1e-8;
    smallScaleMatrix[5] = 1e-8;
    smallScaleMatrix[10] = 1e-8;
    params.extension = ".obj";
    params.modelToWorld = smallScaleMatrix;
    const bool isSmallScaleSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    params.extension = ".raw";
    params.modelToWorld = invalidMatrix;
    const bool isInvalidSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    params.modelToWorld = singularMatrix;
    const bool isSingularSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    params.modelToWorld = projectiveMatrix;
    const bool isProjectiveSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    params.extension = ".ply";
    params.modelToWorld = singularMatrix;
    const bool isMeshSingularSaved = dataManager.ExportData(
        snapshot, outputDir.u8string(), params);
    SetExpect(
        isSmallScaleSaved
            && !isInvalidSaved && !isSingularSaved
            && !isProjectiveSaved
            && !isMeshSingularSaved,
        "data export should accept small affine scales and reject invalid transforms",
        failureCount);

    auto emptyMask =
        vtkSmartPointer<vtkImageData>::New();
    emptyMask->CopyStructure(snapshot->image);
    emptyMask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(
            emptyMask->GetScalarPointer()),
        emptyMask->GetNumberOfPoints(),
        static_cast<unsigned char>(0));
    DataManagerProbe maskedData;
    (void)maskedData.SetInitial(snapshot->image, emptyMask);
    const auto maskedState = maskedData.GetPrimary();
    params.extension = ".ply";
    params.modelToWorld = modelToWorld;
    SetExpect(
        !dataManager.ExportData(
            maskedState,
            outputDir.u8string(), params),
        "mesh export should consume the frozen validity mask",
        failureCount);

    auto partialMask =
        vtkSmartPointer<vtkImageData>::New();
    partialMask->CopyStructure(snapshot->image);
    partialMask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    int maskExtent[6] = {};
    partialMask->GetExtent(maskExtent);
    const int firstValidX =
        (maskExtent[0] + maskExtent[1] + 1) / 2;
    for (int z = maskExtent[4]; z <= maskExtent[5]; ++z) {
        for (int y = maskExtent[2]; y <= maskExtent[3]; ++y) {
            for (int x = maskExtent[0]; x <= maskExtent[1]; ++x) {
                auto* maskValue = static_cast<unsigned char*>(
                    partialMask->GetScalarPointer(x, y, z));
                *maskValue = x >= firstValidX
                    ? static_cast<unsigned char>(255)
                    : static_cast<unsigned char>(0);
            }
        }
    }
    DataManagerProbe partialData;
    (void)partialData.SetInitial(snapshot->image, partialMask);
    const auto partialState = partialData.GetPrimary();
    const auto partialPlyDir = outputDir / "partial-ply";
    const auto partialPlyPath =
        partialPlyDir / "4x4x4_transform.ply";
    const bool isPartialPlySaved = dataManager.ExportData(
        partialState, partialPlyDir.u8string(), params);
    auto partialReader =
        vtkSmartPointer<vtkPLYReader>::New();
    partialReader->SetFileName(
        partialPlyPath.u8string().c_str());
    partialReader->Update();
    vtkPolyData* partialMesh = partialReader->GetOutput();

    params.extension = ".stl";
    const auto partialStlDir = outputDir / "partial-stl";
    const auto partialStlPath =
        partialStlDir / "4x4x4_transform.stl";
    const bool isPartialStlSaved = dataManager.ExportData(
        partialState, partialStlDir.u8string(), params);
    auto partialStlReader =
        vtkSmartPointer<vtkSTLReader>::New();
    partialStlReader->SetFileName(
        partialStlPath.u8string().c_str());
    partialStlReader->Update();
    vtkPolyData* partialStlMesh =
        partialStlReader->GetOutput();

    params.extension = ".obj";
    const auto partialObjDir = outputDir / "partial-obj";
    const auto partialObjPath =
        partialObjDir / "4x4x4_transform.obj";
    const bool isPartialObjSaved = dataManager.ExportData(
        partialState, partialObjDir.u8string(), params);
    auto partialObjReader =
        vtkSmartPointer<vtkOBJReader>::New();
    partialObjReader->SetFileName(
        partialObjPath.u8string().c_str());
    partialObjReader->Update();
    vtkPolyData* partialObjMesh =
        partialObjReader->GetOutput();
    SetExpect(
        isPartialPlySaved
            && std::filesystem::exists(partialPlyPath)
            && partialMesh
            && partialMesh->GetNumberOfCells() > 0
            && plyMesh
            && partialMesh->GetNumberOfCells()
                < plyMesh->GetNumberOfCells()
            && isPartialStlSaved
            && std::filesystem::exists(partialStlPath)
            && partialStlMesh
            && partialStlMesh->GetNumberOfCells() > 0
            && stlMesh
            && partialStlMesh->GetNumberOfCells()
                < stlMesh->GetNumberOfCells()
            && isPartialObjSaved
            && std::filesystem::exists(partialObjPath)
            && partialObjMesh
            && partialObjMesh->GetNumberOfCells() > 0
            && objMesh
            && partialObjMesh->GetNumberOfCells()
                < objMesh->GetNumberOfCells(),
        "a partial validity mask should reduce every exported mesh format",
        failureCount);

    params.extension = ".raw";
    const auto partialRawDir = outputDir / "partial-raw";
    const bool isMaskedRawSaved = dataManager.ExportData(
        partialState, partialRawDir.u8string(), params);
    const auto rawPath =
        partialRawDir / "4x4x4_transform.raw";
    std::array<float, 64> rawValues = {};
    std::ifstream rawFile(rawPath, std::ios::binary);
    rawFile.read(
        reinterpret_cast<char*>(rawValues.data()),
        static_cast<std::streamsize>(
            rawValues.size() * sizeof(float)));
    const std::streamsize rawBytes = rawFile.gcount();
    rawFile.close();
    std::error_code rawSizeError;
    const auto rawSize = std::filesystem::file_size(
        rawPath, rawSizeError);
    SetExpect(
        isMaskedRawSaved
            && !rawSizeError
            && rawSize == rawValues.size() * sizeof(float)
            && rawBytes
                == static_cast<std::streamsize>(
                    rawValues.size() * sizeof(float))
            && rawValues[1] == 0.0f
            && rawValues[2] == 2.0f,
        "RAW export should write masked voxels as background",
        failureCount);
    params.extension = ".ply";

    auto mismatchMask =
        vtkSmartPointer<vtkImageData>::New();
    mismatchMask->DeepCopy(partialMask);
    mismatchMask->SetSpacing(2.0, 1.0, 1.0);
    DataManagerProbe mismatchData;
    (void)mismatchData.SetInitial(snapshot->image, mismatchMask);
    const auto mismatchState = mismatchData.GetPrimary();
    SetExpect(
        !dataManager.ExportData(
            mismatchState,
            outputDir.u8string(), params),
        "mesh export should reject mismatched mask geometry",
        failureCount);

    DataExportParams invalidRangeParams = params;
    invalidRangeParams.scalarRange = { 9.0, 0.0 };
    DataExportParams invalidTfParams = params;
    invalidTfParams.volumeTransferFunction.colorNodes = {
        { 8.0, 0.0, 1.0, 0.0 },
        { 2.0, 0.0, 1.0, 1.0 }
    };
    invalidTfParams.volumeTransferFunction.opacityNodes = {
        { 8.0, 1.0 },
        { 2.0, 1.0 }
    };
    SetExpect(
        !dataManager.ExportData(
            snapshot, outputDir.u8string(),
            invalidRangeParams)
            && !dataManager.ExportData(
                snapshot, outputDir.u8string(),
                invalidTfParams),
        "PLY export should reject invalid scalar and transfer-function metadata",
        failureCount);

    std::filesystem::remove_all(
        outputDir, error);
}

void StartTransformedMaskedRaw(int& failureCount)
{
    // 生产路径不物化输出 mask；此处仅对小体数据用 VTK nearest
    // 生成独立参考结果，核对非零 extent、direction 和 affine 变换。
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(5, 8, -2, 1, 10, 12);
    image->SetOrigin(3.25, -4.5, 2.75);
    image->SetSpacing(0.7, 1.1, 1.3);
    const double direction[9] = {
        0.0, -1.0, 0.0,
        1.0, 0.0, 0.0,
        0.0, 0.0, 1.0
    };
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_FLOAT, 1);
    int imageExtent[6] = {};
    image->GetExtent(imageExtent);
    for (int z = imageExtent[4]; z <= imageExtent[5]; ++z) {
        for (int y = imageExtent[2]; y <= imageExtent[3]; ++y) {
            for (int x = imageExtent[0]; x <= imageExtent[1]; ++x) {
                const float value = static_cast<float>(
                    100 * (z - imageExtent[4])
                    + 10 * (y - imageExtent[2])
                    + (x - imageExtent[0]));
                image->SetScalarComponentFromFloat(
                    x, y, z, 0, value);
            }
        }
    }

    DataManagerProbe dataManager;
    const bool isInitialSet = dataManager.SetInitial(image);
    const auto sourceSnapshot = dataManager.GetPrimary();
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    for (int z = imageExtent[4]; z <= imageExtent[5]; ++z) {
        for (int y = imageExtent[2]; y <= imageExtent[3]; ++y) {
            for (int x = imageExtent[0]; x <= imageExtent[1]; ++x) {
                const int ordinal = x - imageExtent[0]
                    + 2 * (y - imageExtent[2])
                    + 3 * (z - imageExtent[4]);
                auto* maskValue = static_cast<unsigned char*>(
                    mask->GetScalarPointer(x, y, z));
                *maskValue = ordinal % 4 == 0
                    ? static_cast<unsigned char>(0)
                    : static_cast<unsigned char>(255);
            }
        }
    }
    DataManagerProbe maskedData;
    const bool isMaskedSet = maskedData.SetInitial(image, mask);
    const auto maskedState = maskedData.GetPrimary();

    constexpr double radians = 0.37;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const std::array<double, 16> modelToWorld = {
        cosine, -sine, 0.0, 7.0,
        sine, cosine, 0.0, -3.0,
        0.0, 0.0, 1.0, 5.0,
        0.0, 0.0, 0.0, 1.0
    };
    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(modelToWorld.data());
    worldToModelMatrix->Invert();
    auto worldToModelTransform = vtkSmartPointer<vtkTransform>::New();
    worldToModelTransform->SetMatrix(worldToModelMatrix);

    double scalarRange[2] = {};
    image->GetScalarRange(scalarRange);
    auto referenceImageReslice =
        vtkSmartPointer<vtkImageReslice>::New();
    referenceImageReslice->SetInputData(image);
    referenceImageReslice->SetResliceTransform(
        worldToModelTransform);
    referenceImageReslice->SetInterpolationModeToLinear();
    referenceImageReslice->SetOutputDimensionality(3);
    referenceImageReslice->SetAutoCropOutput(true);
    referenceImageReslice->SetBackgroundLevel(scalarRange[0]);
    referenceImageReslice->Update();
    vtkImageData* referenceImage =
        referenceImageReslice->GetOutput();

    auto referenceMaskReslice =
        vtkSmartPointer<vtkImageReslice>::New();
    referenceMaskReslice->SetInputData(mask);
    referenceMaskReslice->SetResliceTransform(
        worldToModelTransform);
    referenceMaskReslice->SetInterpolationModeToNearestNeighbor();
    referenceMaskReslice->SetOutputDimensionality(3);
    referenceMaskReslice->SetBackgroundLevel(0.0);
    if (referenceImage) {
        referenceMaskReslice->SetOutputOrigin(
            referenceImage->GetOrigin());
        referenceMaskReslice->SetOutputSpacing(
            referenceImage->GetSpacing());
        referenceMaskReslice->SetOutputDirection(
            referenceImage->GetDirectionMatrix()->GetData());
        referenceMaskReslice->SetOutputExtent(
            referenceImage->GetExtent());
    }
    referenceMaskReslice->Update();
    vtkImageData* referenceMask =
        referenceMaskReslice->GetOutput();

    int outputDimensions[3] = {};
    int outputExtent[6] = {};
    if (referenceImage) {
        referenceImage->GetDimensions(outputDimensions);
        referenceImage->GetExtent(outputExtent);
    }
    const std::size_t outputVoxelCount =
        outputDimensions[0] > 0
            && outputDimensions[1] > 0
            && outputDimensions[2] > 0
        ? static_cast<std::size_t>(outputDimensions[0])
            * static_cast<std::size_t>(outputDimensions[1])
            * static_cast<std::size_t>(outputDimensions[2])
        : 0;
    const auto uniqueId = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto outputDir = std::filesystem::temp_directory_path()
        / "MVVCVTK_transformed_masked_raw"
        / std::to_string(uniqueId);
    DataExportParams params;
    params.extension = ".raw";
    params.modelToWorld = modelToWorld;
    const bool isSaved = maskedState
        && dataManager.ExportData(
            maskedState, outputDir.u8string(), params);
    const auto outputPath = outputDir
        / (std::to_string(outputDimensions[0]) + "x"
            + std::to_string(outputDimensions[1]) + "x"
            + std::to_string(outputDimensions[2])
            + "_transform.raw");

    std::vector<float> actualValues(outputVoxelCount);
    std::ifstream rawFile(outputPath, std::ios::binary);
    rawFile.read(
        reinterpret_cast<char*>(actualValues.data()),
        static_cast<std::streamsize>(
            actualValues.size() * sizeof(float)));
    const std::streamsize actualBytes = rawFile.gcount();
    rawFile.close();
    std::error_code sizeError;
    const auto actualSize = std::filesystem::file_size(
        outputPath, sizeError);

    bool isContentMatched = referenceImage && referenceMask
        && outputVoxelCount > 0;
    bool hasValidVoxel = false;
    bool hasInvalidVoxel = false;
    std::size_t linearIndex = 0;
    constexpr float tolerance = 1e-5f;
    for (int z = outputExtent[4];
        isContentMatched && z <= outputExtent[5]; ++z) {
        for (int y = outputExtent[2];
            isContentMatched && y <= outputExtent[3]; ++y) {
            for (int x = outputExtent[0];
                isContentMatched && x <= outputExtent[1]; ++x) {
                const bool isValid = referenceMask
                    ->GetScalarComponentAsDouble(x, y, z, 0) != 0.0;
                const float expectedValue = isValid
                    ? static_cast<float>(referenceImage
                        ->GetScalarComponentAsDouble(x, y, z, 0))
                    : static_cast<float>(scalarRange[0]);
                hasValidVoxel = hasValidVoxel || isValid;
                hasInvalidVoxel = hasInvalidVoxel || !isValid;
                isContentMatched = linearIndex < actualValues.size()
                    && std::abs(
                        actualValues[linearIndex] - expectedValue)
                        <= tolerance;
                ++linearIndex;
            }
        }
    }
    SetExpect(
        isInitialSet && isSaved && !sizeError
            && actualSize == outputVoxelCount * sizeof(float)
            && actualBytes == static_cast<std::streamsize>(
                outputVoxelCount * sizeof(float))
            && linearIndex == outputVoxelCount
            && hasValidVoxel && hasInvalidVoxel
            && isContentMatched,
        "transformed RAW mask sampling should match VTK nearest reslicing",
        failureCount);
    std::error_code removeError;
    std::filesystem::remove_all(outputDir, removeError);
}

void StartStateGate(int& failureCount)
{
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto firstOwner = std::make_shared<int>(1);
    auto secondOwner = std::make_shared<int>(2);
    std::atomic<int> secondCount{ 0 };
    broadcaster->SetObserver(firstOwner, [](UpdateFlags) {
        throw std::runtime_error("observer failure");
    });
    broadcaster->SetObserver(secondOwner, [&](UpdateFlags) { ++secondCount; });
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    SetExpect(state->StartLoad(LoadEventKind::Reload),
        "reload admission must start", failureCount);
    bool nestedResult = true;
    auto nestedOwner = std::make_shared<int>(3);
    broadcaster->SetObserver(nestedOwner, [&](UpdateFlags) {
        nestedResult = state->SetReloadLoadFailed();
    });
    SetExpect(state->SetReloadDataReady(0.0, 1.0, { 1.0, 1.0, 1.0 }),
        "outer terminal must publish", failureCount);
    SetExpect(secondCount.load() == 1 && !nestedResult,
        "observer failure and terminal reentry must be isolated", failureCount);
    SetExpect(state->ResetLoad(LoadEventKind::Reload),
        "published terminal must release admission", failureCount);
}

void StartMaskSnapshot(int& failureCount)
{
    DataManagerProbe dataManager;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 1, 1);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* values = static_cast<float*>(
        image->GetScalarPointer());
    values[0] = 0.0f;
    values[1] = 100.0f;
    SetExpect(dataManager.SetInitial(image),
        "initial image snapshot should publish",
        failureCount);

    const auto expected = dataManager.GetPrimary();
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* maskValues =
        static_cast<unsigned char*>(
            mask->GetScalarPointer());
    maskValues[0] = 255;
    maskValues[1] = 0;

    VtkImageGridSnapshot publishedSnapshot;
    SetExpect(dataManager.SetCandidate(
            image,
            mask,
            expected,
            publishedSnapshot),
        "image and validity mask should publish as one CAS batch",
        failureCount);
    const auto current = dataManager.GetPrimary();
    SetExpect(current && publishedSnapshot
            && publishedSnapshot->data->self == current->data->self
            && current->binding->revision
                == expected->binding->revision + 1
            && current->validityMask
            && current->validityMask.GetPointer()
                != mask.GetPointer(),
        "published mask should share one immutable image revision",
        failureCount);
    const auto currentBindingRevision = current && current->binding
        ? current->binding->revision : 0;
    SetExpect(!dataManager.SetCandidate(
            image,
            mask,
            expected,
            publishedSnapshot)
            && !publishedSnapshot
            && dataManager.GetPrimaryBindingRevision()
                == currentBindingRevision,
        "a stale expected snapshot must not replace current image or mask",
        failureCount);

    const auto uniqueId =
        std::chrono::steady_clock::now()
            .time_since_epoch().count();
    const auto outputDir =
        std::filesystem::temp_directory_path()
        / ("MVVCVTK_mask_"
            + std::to_string(uniqueId));
    const std::array<double, 16> identity = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    const bool isExported = dataManager.ExportSlices(
        outputDir.u8string(),
        Orientation::Top_down,
        { 100.0, 50.0 },
        identity);
    auto reader = vtkSmartPointer<vtkPNGReader>::New();
    reader->SetFileName(
        (outputDir / "Top_down_0000.png")
            .u8string().c_str());
    if (isExported) {
        reader->Update();
    }
    auto* output = reader->GetOutput();
    const auto* outputValues =
        output && output->GetNumberOfPoints() == 2
        ? static_cast<const unsigned char*>(
            output->GetScalarPointer())
        : nullptr;
    SetExpect(isExported
            && outputValues
            && outputValues[0] == 0
            && outputValues[1] == 0,
        "slice export should write mask=0 voxels as background",
        failureCount);
    std::error_code error;
    std::filesystem::remove_all(outputDir, error);
}

void StartFactoryAdmission(int& failureCount)
{
    auto dataManager = std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->AllocateScalars(VTK_FLOAT, 1);
    SetExpect(dataManager->SetInitial(image),
        "factory admission needs an initial image",
        failureCount);

    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    const StrategyCreate factory = GetStrategyFactory();
    SetExpect(std::dynamic_pointer_cast<VolumeStrategy>(
            factory(VizMode::Volume))
            && std::dynamic_pointer_cast<IsoSurfaceStrategy>(
                factory(VizMode::IsoSurface))
            && std::dynamic_pointer_cast<SliceStrategy>(
                factory(VizMode::SliceTop_down))
            && std::dynamic_pointer_cast<SliceStrategy>(
                factory(VizMode::SliceFront_back))
            && std::dynamic_pointer_cast<SliceStrategy>(
                factory(VizMode::SliceLeft_right))
            && std::dynamic_pointer_cast<CompositeStrategy>(
                factory(VizMode::CompositeVolume))
            && std::dynamic_pointer_cast<CompositeStrategy>(
                factory(VizMode::CompositeIsoSurface)),
        "test seam should map all seven concrete strategy types",
        failureCount);
    SetExpect(std::dynamic_pointer_cast<VolumeStrategy>(
            CreateRenderStrategy(VizMode::Volume))
            && std::dynamic_pointer_cast<IsoSurfaceStrategy>(
                CreateRenderStrategy(VizMode::IsoSurface))
            && std::dynamic_pointer_cast<SliceStrategy>(
                CreateRenderStrategy(VizMode::SliceTop_down))
            && std::dynamic_pointer_cast<SliceStrategy>(
                CreateRenderStrategy(VizMode::SliceFront_back))
            && std::dynamic_pointer_cast<SliceStrategy>(
                CreateRenderStrategy(VizMode::SliceLeft_right))
            && std::dynamic_pointer_cast<CompositeStrategy>(
                CreateRenderStrategy(VizMode::CompositeVolume))
            && std::dynamic_pointer_cast<CompositeStrategy>(
                CreateRenderStrategy(VizMode::CompositeIsoSurface))
            && !CreateRenderStrategy(
                static_cast<VizMode>(-1)),
        "Render factory should map seven modes and reject invalid values",
        failureCount);
}

void StartObserverGate(int& failureCount)
{
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    AppServiceArgs args;
    args.dataManager = std::make_shared<DataStub>();
    args.interactionState = state;
    args.eventSource = broadcaster;
    auto ports = CreateAppPorts(std::move(args));
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    const bool isBound = ports.renderBind
        && ports.renderBind->SetRenderTarget(
            renderWindow, renderer);

    std::atomic<bool> isStarted{ false };
    std::atomic<bool> isStopped{ false };
    std::atomic<int> sendCount{ 0 };
    std::thread sender([&]() {
        isStarted = true;
        while (!isStopped.load()) {
            broadcaster->SendFlags(UpdateFlags::Material);
            ++sendCount;
        }
    });
    while (!isStarted.load()) {
        std::this_thread::yield();
    }

    // 析构先关闭 observer gate，并等待已进入的回调离开；随后广播只能清理过期订阅。
    ports = {};
    isStopped = true;
    sender.join();
    broadcaster->SendFlags(UpdateFlags::Material);
    SetExpect(isBound && sendCount.load() > 0,
        "App runtime destruction must close concurrent observer callbacks",
        failureCount);
}

void StartCandidateParams(int& failureCount)
{
    auto dataManager = std::make_shared<DataStub>();
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    state->SetScalarRange(100.0, 200.0);
    state->SetCursorRawWorld(9.0, 8.0, 7.0);
    state->SetCursorWorld(9.0, 8.0, 7.0);
    state->SetCursorAxis(2);

    auto capture = std::make_shared<VisualStateCapture>();
    auto sharedHistogram = std::make_shared<HistogramConverter>();
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    args.histogram = sharedHistogram;
    args.strategyCreate = [capture](VizMode)
        -> std::shared_ptr<AbstractVisualStrategy> {
        return std::make_shared<CaptureVisualStrategy>(capture);
    };
    auto ports = CreateAppPorts(std::move(args));
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->SetSize(32, 32);
    renderWindow->AddRenderer(renderer);
    const bool isBound = ports.renderBind
        && ports.renderBind->SetRenderTarget(
            renderWindow, renderer);

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->SetSpacing(2.0, 3.0, 4.0);
    image->AllocateScalars(VTK_FLOAT, 1);
    const std::array<float, 8> values = {
        -3.0f, -2.0f, -1.0f, 0.0f,
        1.0f, 2.0f, 3.0f, 5.0f
    };
    std::copy(
        values.begin(), values.end(),
        static_cast<float*>(image->GetScalarPointer()));
    image->GetPointData()->GetScalars()->Modified();
    image->Modified();

    (void)dataManager->SetPrimaryForTest(image);
    const auto snapshot = dataManager->GetPrimaryImage();
    const bool isBuilt = ports.dataStage
        && ports.dataStage->BuildDataStage(snapshot);
    auto secondCapture = std::make_shared<VisualStateCapture>();
    AppServiceArgs secondArgs;
    secondArgs.dataManager = dataManager;
    secondArgs.interactionState = state;
    secondArgs.eventSource = broadcaster;
    secondArgs.histogram = sharedHistogram;
    secondArgs.strategyCreate = [secondCapture](VizMode)
        -> std::shared_ptr<AbstractVisualStrategy> {
        return std::make_shared<CaptureVisualStrategy>(secondCapture);
    };
    auto secondPorts = CreateAppPorts(std::move(secondArgs));
    auto secondRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto secondWindow = vtkSmartPointer<vtkRenderWindow>::New();
    secondWindow->SetOffScreenRendering(1);
    secondWindow->SetSize(32, 32);
    secondWindow->AddRenderer(secondRenderer);
    const bool isSecondBound = secondPorts.renderBind
        && secondPorts.renderBind->SetRenderTarget(
            secondWindow, secondRenderer);
    const bool isSecondBuilt = secondPorts.dataStage
        && secondPorts.dataStage->BuildDataStage(snapshot);
    const bool isHistogramSkipped = sharedHistogram
        && sharedHistogram->GetBuildCount() == 0;
    const bool isSecondCleared = secondPorts.dataStage
        && secondPorts.dataStage->ClearDataStage();
    const bool hasNextParams = capture->setCount > 0
        && capture->flags == UpdateFlags::All
        && capture->params.scalarRange[0] == -3.0
        && capture->params.scalarRange[1] == 5.0
        && capture->params.cursor
            == std::array<double, 3>{ 1.0, 1.5, 2.0 }
        && capture->params.cursorRaw
            == std::array<double, 3>{ 1.0, 1.5, 2.0 }
        && capture->params.cursorAxis == -1
        && capture->params.windowLevel.windowWidth == 8.0
        && capture->params.windowLevel.windowCenter == 1.0
        && capture->params.volumeTransferFunction.colorNodes.size() == 4
        && capture->params.volumeTransferFunction.opacityNodes.size() == 4
        && capture->params.volumeTransferFunction.colorNodes[0].scalar
            == -3.0
        && capture->params.volumeTransferFunction.colorNodes[1].scalar
            == 1.0
        && std::abs(
            capture->params.volumeTransferFunction.colorNodes[2].scalar
                - 3.8) < 1e-12
        && capture->params.volumeTransferFunction.colorNodes[3].scalar
            == 5.0
        && capture->params.volumeTransferFunction.opacityNodes[0].opacity
            == 0.0
        && capture->params.volumeTransferFunction.opacityNodes[1].opacity
            == 0.0
        && capture->params.volumeTransferFunction.opacityNodes[2].opacity
            == 0.8
        && capture->params.volumeTransferFunction.opacityNodes[3].opacity
            == 1.0;
    const bool isSharedUnchanged =
        state->GetScalarRange()
            == std::array<double, 2>{ 100.0, 200.0 }
        && state->GetCursorWorld()
            == std::array<double, 3>{ 9.0, 8.0, 7.0 }
        && state->GetCursorRawWorld()
            == std::array<double, 3>{ 9.0, 8.0, 7.0 }
        && state->GetCursorAxis() == 2;
    const bool isCleared = ports.dataStage
        && ports.dataStage->ClearDataStage();

    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        values.size(), 12.0f);
    image->GetPointData()->GetScalars()->Modified();
    image->Modified();
    (void)dataManager->SetPrimaryForTest(image);
    const auto constantSnapshot = dataManager->GetPrimaryImage();
    const int oldSetCount = capture->setCount;
    const bool isConstantBuilt = ports.dataStage
        && ports.dataStage->BuildDataStage(constantSnapshot);
    const bool hasConstantWindow = capture->setCount > oldSetCount
        && std::isfinite(
            capture->params.windowLevel.windowWidth)
        && capture->params.windowLevel.windowWidth > 0.0
        && capture->params.windowLevel.windowCenter == 12.0;
    const bool isConstantScanSkipped = sharedHistogram
        && sharedHistogram->GetBuildCount() == 0;
    const bool isConstantCleared = ports.dataStage
        && ports.dataStage->ClearDataStage();

    SetExpect(isBound
            && isBuilt
            && isSecondBound
            && isSecondBuilt
            && isHistogramSkipped
            && isSecondCleared
            && hasNextParams
            && isSharedUnchanged
            && isCleared
            && isConstantBuilt
            && hasConstantWindow
            && isConstantScanSkipped
            && isConstantCleared,
        "candidate strategy must use a range-mapped default TF without scanning the volume",
        failureCount);
}

void StartPipelineRollback(int& failureCount)
{
    auto dataManager = std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    SetExpect(dataManager->SetInitial(image),
        "pipeline rollback needs an initial image",
        failureCount);

    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    auto failAttachCount =
        std::make_shared<std::atomic<int>>(0);
    const StrategyCreate failFactory =
        [failAttachCount](const VizMode mode)
            -> std::shared_ptr<AbstractVisualStrategy> {
        if (mode == VizMode::Volume) {
            return std::make_shared<VolumeStrategy>();
        }
        if (mode == VizMode::IsoSurface) {
            return std::make_shared<FailVisualStrategy>(
                failAttachCount);
        }
        return nullptr;
    };
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    args.strategyCreate = failFactory;
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.view || !ports.interaction.update
        || !ports.interaction.model || !ports.renderBind) {
        SetExpect(false,
            "pipeline rollback needs every narrow port",
            failureCount);
        return;
    }
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    AppViewUpdate update;
    update.mode = VizMode::Volume;
    SetExpect(ports.renderBind->SetRenderTarget(
            renderWindow, renderer)
            && ports.app.view->SendViewUpdate(update)
            && ports.interaction.update->SendUpdates(),
        "pipeline rollback should build its committed Volume strategy",
        failureCount);
    vtkProp3D* oldProp =
        ports.interaction.model->GetMainProp();
    vtkCamera* camera = renderer->GetActiveCamera();
    camera->SetPosition(11.0, -5.0, 17.0);
    camera->SetFocalPoint(2.0, 3.0, 4.0);
    camera->SetViewUp(0.0, 1.0, 0.0);
    camera->SetClippingRange(0.25, 450.0);
    camera->SetParallelScale(3.5);
    camera->SetViewAngle(24.0);
    camera->ParallelProjectionOff();
    std::array<double, 3> oldPosition;
    std::copy_n(
        camera->GetPosition(),
        oldPosition.size(),
        oldPosition.begin());
    std::array<double, 3> oldFocal;
    std::copy_n(
        camera->GetFocalPoint(),
        oldFocal.size(),
        oldFocal.begin());
    std::array<double, 3> oldViewUp;
    std::copy_n(
        camera->GetViewUp(),
        oldViewUp.size(),
        oldViewUp.begin());
    std::array<double, 2> oldClip;
    std::copy_n(
        camera->GetClippingRange(),
        oldClip.size(),
        oldClip.begin());
    const double oldScale = camera->GetParallelScale();
    const double oldAngle = camera->GetViewAngle();

    update.mode = VizMode::IsoSurface;
    const bool isUpdateSent =
        ports.app.view->SendViewUpdate(update)
        && ports.interaction.update->SendUpdates();
    const bool isFailed = isUpdateSent
        && failAttachCount->load() > 0;
    camera = renderer->GetActiveCamera();
    const bool isCameraRestored = camera
        && std::equal(
            oldPosition.begin(),
            oldPosition.end(),
            camera->GetPosition())
        && std::equal(
            oldFocal.begin(),
            oldFocal.end(),
            camera->GetFocalPoint())
        && std::equal(
            oldViewUp.begin(),
            oldViewUp.end(),
            camera->GetViewUp())
        && std::equal(
            oldClip.begin(),
            oldClip.end(),
            camera->GetClippingRange())
        && camera->GetParallelScale() == oldScale
        && camera->GetViewAngle() == oldAngle
        && camera->GetParallelProjection() == 0;
    if (!isCameraRestored && camera) {
        std::cerr << "DIAG_CAMERA_ROLLBACK"
            << " position=" << camera->GetPosition()[0]
            << ',' << camera->GetPosition()[1]
            << ',' << camera->GetPosition()[2]
            << " focal=" << camera->GetFocalPoint()[0]
            << ',' << camera->GetFocalPoint()[1]
            << ',' << camera->GetFocalPoint()[2]
            << " viewUp=" << camera->GetViewUp()[0]
            << ',' << camera->GetViewUp()[1]
            << ',' << camera->GetViewUp()[2]
            << " clip=" << camera->GetClippingRange()[0]
            << ',' << camera->GetClippingRange()[1]
            << " scale=" << camera->GetParallelScale()
            << " angle=" << camera->GetViewAngle()
            << " projection=" << camera->GetParallelProjection()
            << '\n';
    }
    SetExpect(isFailed,
        "fault-injection strategy should make pipeline commit fail",
        failureCount);
    SetExpect(oldProp
            && ports.interaction.model->GetMainProp() == oldProp
            && renderer->HasViewProp(oldProp),
        "failed strategy attach must restore the committed prop",
        failureCount);
    SetExpect(isCameraRestored,
        "failed strategy attach must restore the full committed camera",
        failureCount);

    auto reboundRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto reboundWindow = vtkSmartPointer<vtkRenderWindow>::New();
    reboundWindow->SetOffScreenRendering(1);
    reboundWindow->AddRenderer(reboundRenderer);
    ports.renderBind->SetRenderTarget(
        reboundWindow, reboundRenderer);
    SetExpect(reboundRenderer->GetActiveCamera()
            && reboundRenderer->GetActiveCamera()
                ->GetParallelProjection() == 0
            && ports.interaction.model->GetMainProp() == oldProp,
        "failed pending mode must not pollute committed-mode renderer rebind",
        failureCount);
}

void StartQualityRollback(int& failureCount)
{
    auto dataManager = std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    if (!dataManager->SetInitial(image)) {
        SetExpect(false,
            "quality rollback needs an initial image",
            failureCount);
        return;
    }

    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    auto rejectCount = std::make_shared<std::atomic<int>>(0);
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    args.strategyCreate = [rejectCount](const VizMode mode)
        -> std::shared_ptr<AbstractVisualStrategy> {
        return mode == VizMode::Volume
            ? std::make_shared<RejectQualityStrategy>(rejectCount)
            : nullptr;
    };
    auto ports = CreateAppPorts(std::move(args));
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    AppViewUpdate modeUpdate;
    modeUpdate.mode = VizMode::Volume;
    const bool isBuilt = ports.renderBind
        && ports.renderBind->SetRenderTarget(
            renderWindow, renderer)
        && ports.app.view
        && ports.app.view->SendViewUpdate(modeUpdate)
        && ports.interaction.update
        && ports.interaction.update->SendUpdates();
    if (!isBuilt) {
        SetExpect(false,
            "quality rollback needs a committed Volume strategy",
            failureCount);
        return;
    }

    const auto baseline = ports.app.view->GetViewState();
    constexpr double oldRate = 7.0;
    renderWindow->SetDesiredUpdateRate(oldRate);
    AppViewUpdate qualityUpdate;
    qualityUpdate.volumeQuality = VolumeQuality::Ultra;
    const bool isAccepted =
        ports.app.view->SendViewUpdate(qualityUpdate);
    const bool isRejected =
        !ports.interaction.update->SendUpdates();
    const auto restored = ports.app.view->GetViewState();
    const bool isNotRetried =
        ports.interaction.update->SendUpdates();
    SetExpect(isAccepted
            && isRejected
            && isNotRetried
            && rejectCount->load() == 1
            && baseline.volumeQuality == VolumeQuality::Auto
            && restored.volumeQuality == baseline.volumeQuality
            && renderWindow->GetDesiredUpdateRate() == oldRate,
        "rejected Ultra must keep the applied quality and render rate without fallback or retry",
        failureCount);
}

void StartQualityLatest(int& failureCount)
{
    auto dataManager = std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    if (!dataManager->SetInitial(image)) {
        SetExpect(false,
            "quality latest-value test needs an initial image",
            failureCount);
        return;
    }

    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    auto gate = std::make_shared<QualitySwitchGate>();
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    args.strategyCreate = [gate](const VizMode mode)
        -> std::shared_ptr<AbstractVisualStrategy> {
        return mode == VizMode::Volume
            ? std::make_shared<WaitQualityStrategy>(gate)
            : nullptr;
    };
    auto ports = CreateAppPorts(std::move(args));
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    AppViewUpdate modeUpdate;
    modeUpdate.mode = VizMode::Volume;
    const bool isBuilt = ports.renderBind
        && ports.renderBind->SetRenderTarget(renderWindow, renderer)
        && ports.app.view
        && ports.app.view->SendViewUpdate(modeUpdate)
        && ports.interaction.update
        && ports.interaction.update->SendUpdates();
    if (!isBuilt) {
        SetExpect(false,
            "quality latest-value test needs a committed Volume strategy",
            failureCount);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->isArmed = true;
    }
    AppViewUpdate highUpdate;
    highUpdate.volumeQuality = VolumeQuality::High;
    const bool isHighAccepted =
        ports.app.view->SendViewUpdate(highUpdate);
    bool isLowAccepted = false;
    bool isGateStarted = false;
    std::thread lowThread([&]() {
        {
            std::unique_lock<std::mutex> lock(gate->mutex);
            constexpr auto waitLimit = std::chrono::seconds(5);
            isGateStarted = gate->condition.wait_for(
                lock, waitLimit,
                [&]() { return gate->isStarted; });
        }
        if (isGateStarted) {
            AppViewUpdate lowUpdate;
            lowUpdate.volumeQuality = VolumeQuality::Low;
            isLowAccepted = ports.app.view->SendViewUpdate(lowUpdate);
        }
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->isReleased = true;
        }
        gate->condition.notify_all();
    });

    const bool isHighApplied = ports.interaction.update->SendUpdates();
    const AppViewState highState = ports.app.view->GetViewState();
    lowThread.join();
    const bool isLowApplied = ports.interaction.update->SendUpdates();
    const AppViewState lowState = ports.app.view->GetViewState();
    std::vector<VolumeQuality> qualities;
    bool hasTimedOut = false;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        qualities = gate->qualities;
        hasTimedOut = gate->hasTimedOut;
    }
    SetExpect(isHighAccepted
            && isGateStarted
            && !hasTimedOut
            && isLowAccepted
            && isHighApplied
            && highState.volumeQuality == VolumeQuality::High
            && isLowApplied
            && lowState.volumeQuality == VolumeQuality::Low
            && qualities.size() >= 2
            && qualities[qualities.size() - 2] == VolumeQuality::High
            && qualities.back() == VolumeQuality::Low,
        "applied quality must match each committed mapper snapshot while a newer tier remains pending",
        failureCount);
}

void StartInputSwap(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataManagerProbe>();
    auto firstImage =
        vtkSmartPointer<vtkImageData>::New();
    firstImage->SetDimensions(4, 4, 4);
    firstImage->AllocateScalars(VTK_FLOAT, 1);
    SetExpect(dataManager->SetInitial(firstImage),
        "render input swap needs an initial image",
        failureCount);

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.view || !ports.interaction.update
        || !ports.interaction.model || !ports.renderBind
        || !ports.featureView) {
        SetExpect(false,
            "render input swap needs every narrow port",
            failureCount);
        return;
    }
    auto renderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    const bool isBound = ports.renderBind->SetRenderTarget(
        renderWindow, renderer);
    AppViewUpdate volumeUpdate;
    volumeUpdate.mode = VizMode::Volume;
    broadcaster->SendFlags(UpdateFlags::All);
    SetExpect(isBound
            && ports.app.view->SendViewUpdate(volumeUpdate)
            && ports.interaction.update->SendUpdates(),
        "initial render pipeline should build",
        failureCount);
    auto* firstProp = ports.interaction.model->GetMainProp();
    bool isWrongThreadAccepted = true;
    std::thread wrongThread([&] {
        isWrongThreadAccepted =
            ports.interaction.update->SendUpdates();
    });
    wrongThread.join();
    SetExpect(!isWrongThreadAccepted
            && ports.interaction.model->GetMainProp() == firstProp,
        "reload from a non-owner thread must not touch the VTK pipeline",
        failureCount);
    auto replacementRenderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto replacementWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    replacementWindow->SetOffScreenRendering(1);
    replacementWindow->AddRenderer(
        replacementRenderer);
    bool isWrongBindAccepted = true;
    std::thread wrongBindThread([&] {
        isWrongBindAccepted = ports.renderBind->SetRenderTarget(
            replacementWindow,
            replacementRenderer);
    });
    wrongBindThread.join();
    SetExpect(!isWrongBindAccepted
            && ports.interaction.model->GetMainProp() == firstProp,
        "a non-owner thread must not replace the RenderContext owner",
        failureCount);
    const auto setMode = [&ports](const VizMode mode) {
        AppViewUpdate update;
        update.mode = mode;
        return ports.app.view->SendViewUpdate(update)
            && ports.interaction.update->SendUpdates();
    };
    vtkWeakPointer<vtkProp3D> retiredVolumeProp = firstProp;
    SetExpect(setMode(VizMode::IsoSurface)
            && !retiredVolumeProp
            && ports.interaction.model->GetMainProp(),
        "mode switch should commit a fresh strategy and release the old Volume prop",
        failureCount);
    vtkWeakPointer<vtkProp3D> retiredIsoProp =
        ports.interaction.model->GetMainProp();
    SetExpect(setMode(VizMode::Volume)
            && !retiredIsoProp
            && ports.interaction.model->GetMainProp(),
        "returning to Volume should create a fresh committed strategy",
        failureCount);
    auto* committedProp = ports.interaction.model->GetMainProp();
    auto cropEffect =
        std::make_shared<CropShaderEffect>();
    SetExpect(ports.featureView->AttachRenderEffect(cropEffect),
        "input swap test should attach one crop effect",
        failureCount);

    CropOpItem keepOp;
    keepOp.operationIndex = 1;
    keepOp.geometryType = CropShape::Plane;
    keepOp.removalMode = CropRemovalMode::KeepInside;
    CropOpItem removeOp = keepOp;
    removeOp.operationIndex = 2;
    removeOp.removalMode =
        CropRemovalMode::RemoveInside;
    const auto tableResult =
        CropAlgorithm::BuildPredicateTable(
            { keepOp, removeOp }, 2);
    const auto inputStamp =
        ports.featureView->GetRenderInputStamp();
    CropShaderPayload payload;
    payload.revision = 1;
    if (inputStamp) payload.sourceStamp = *inputStamp;
    payload.nodeCount = 2;
    payload.predicateTable =
        tableResult.predicateTable;
    SetExpect(inputStamp
            && tableResult.isSucceeded
            && cropEffect->SetCropParams(payload),
        "KeepInside and RemoveInside should stage before input replacement",
        failureCount);

    auto nextImage =
        vtkSmartPointer<vtkImageData>::New();
    nextImage->DeepCopy(firstImage);
    auto mask =
        vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(nextImage);
    mask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    auto* maskValues =
        static_cast<unsigned char*>(
            mask->GetScalarPointer());
    std::fill_n(
        maskValues,
        mask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    const auto expected =
        dataManager->GetPrimary();
    VtkImageGridSnapshot published;
    SetExpect(dataManager->SetCandidate(
            nextImage,
            mask,
            expected,
            published),
        "next image and mask should publish",
        failureCount);
    broadcaster->SendFlags(UpdateFlags::All);
    SetExpect(ports.interaction.update->SendUpdates()
            && ports.interaction.model->GetMainProp() == committedProp
            && cropEffect->GetState().status
                != RenderEffectStatus::Failed,
        "input replacement must wait while keeping the current crop binding valid",
        failureCount);
    SetExpect(cropEffect->ClearCropStage(
            payload.revision),
        "input swap test should finish the staged transaction",
        failureCount);
    vtkCamera* activeCamera = renderer->GetActiveCamera();
    activeCamera->SetPosition(8.0, -3.0, 12.0);
    activeCamera->SetFocalPoint(1.0, 2.0, 3.0);
    activeCamera->SetViewUp(0.0, 1.0, 0.0);
    activeCamera->SetClippingRange(0.2, 420.0);
    activeCamera->SetParallelScale(2.75);
    activeCamera->SetViewAngle(27.0);
    activeCamera->ParallelProjectionOff();
    struct CameraProbe final {
        vtkCamera* camera = nullptr;
        std::array<double, 3> position{};
        std::array<double, 3> focalPoint{};
        std::array<double, 3> viewUp{};
        std::array<double, 2> clippingRange{};
        double parallelScale = 0.0;
        double viewAngle = 0.0;
        std::size_t renderCount = 0;
        bool isStable = true;
    } cameraProbe;
    cameraProbe.camera = activeCamera;
    std::copy_n(
        activeCamera->GetPosition(),
        cameraProbe.position.size(),
        cameraProbe.position.begin());
    std::copy_n(
        activeCamera->GetFocalPoint(),
        cameraProbe.focalPoint.size(),
        cameraProbe.focalPoint.begin());
    std::copy_n(
        activeCamera->GetViewUp(),
        cameraProbe.viewUp.size(),
        cameraProbe.viewUp.begin());
    std::copy_n(
        activeCamera->GetClippingRange(),
        cameraProbe.clippingRange.size(),
        cameraProbe.clippingRange.begin());
    cameraProbe.parallelScale = activeCamera->GetParallelScale();
    cameraProbe.viewAngle = activeCamera->GetViewAngle();
    auto renderCallback =
        vtkSmartPointer<vtkCallbackCommand>::New();
    renderCallback->SetClientData(&cameraProbe);
    renderCallback->SetCallback(
        [](vtkObject*, unsigned long eventId,
            void* clientData, void*) {
            if (eventId == vtkCommand::StartEvent
                && clientData) {
                auto* probe = static_cast<CameraProbe*>(clientData);
                ++probe->renderCount;
                probe->isStable = probe->isStable
                    && probe->camera
                    && probe->camera->GetParallelProjection() == 0
                    && std::equal(
                        probe->position.begin(),
                        probe->position.end(),
                        probe->camera->GetPosition())
                    && std::equal(
                        probe->focalPoint.begin(),
                        probe->focalPoint.end(),
                        probe->camera->GetFocalPoint())
                    && std::equal(
                        probe->viewUp.begin(),
                        probe->viewUp.end(),
                        probe->camera->GetViewUp())
                    && std::equal(
                        probe->clippingRange.begin(),
                        probe->clippingRange.end(),
                        probe->camera->GetClippingRange())
                    && probe->parallelScale
                        == probe->camera->GetParallelScale()
                    && probe->viewAngle
                        == probe->camera->GetViewAngle();
            }
        });
    const unsigned long renderTag =
        renderWindow->AddObserver(
            vtkCommand::StartEvent, renderCallback);
    const bool isInputRebuilt =
        ports.interaction.update->SendUpdates();
    renderWindow->RemoveObserver(renderTag);
    renderCallback->SetClientData(nullptr);
    std::cout
        << "DIAG_RENDER_SOURCE: candidate_render="
        << cameraProbe.renderCount << '\n';
    SetExpect(isInputRebuilt
            && cameraProbe.renderCount == 0
            && cameraProbe.isStable,
        "idle candidate should not render or mutate the committed camera",
        failureCount);
    auto* nextProp = ports.interaction.model->GetMainProp();
    SetExpect(committedProp
            && nextProp
            && committedProp != nextProp,
        "same-mode input replacement must swap a prepared strategy instead of mutating the visible strategy",
        failureCount);
}

void StartRenderOwnerGate(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(), 1.0F);
    SetExpect(dataManager->SetInitial(image),
        "owner gate needs an initial image",
        failureCount);

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.view || !ports.interaction.update
        || !ports.interaction.model || !ports.renderBind) {
        SetExpect(false,
            "owner gate needs every narrow port",
            failureCount);
        return;
    }

    auto material = state->GetMaterial();
    material.diffuse = 0.23;
    AppViewUpdate update;
    update.mode = VizMode::Volume;
    update.material = material;
    const bool isStateSet =
        ports.app.view->SendViewUpdate(update);
    broadcaster->SendFlags(UpdateFlags::All);

    SetExpect(isStateSet
            && !ports.interaction.update->SendUpdates()
            && !ports.interaction.model->GetMainProp(),
        "an unbound service must not submit state to VTK",
        failureCount);

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    const bool isSubmitted =
        ports.renderBind->SetRenderTarget(renderWindow, renderer)
        && ports.interaction.update->SendUpdates();
    auto* volume = vtkVolume::SafeDownCast(
        ports.interaction.model->GetMainProp());
    auto* property = volume ? volume->GetProperty() : nullptr;
    SetExpect(isSubmitted && property
            && std::abs(property->GetDiffuse()
                - material.diffuse) < 1e-12,
        "the RenderContext owner must submit pending state",
        failureCount);
}

void StartStrategySwitchSync(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(), 1.0F);
    SetExpect(dataManager->SetInitial(image),
        "strategy switch sync needs an initial image",
        failureCount);

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.view || !ports.interaction.update
        || !ports.interaction.model || !ports.renderBind) {
        SetExpect(false,
            "strategy switch sync needs every narrow port",
            failureCount);
        return;
    }
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    AppViewUpdate update;
    update.mode = VizMode::Volume;
    SetExpect(ports.renderBind->SetRenderTarget(
            renderWindow, renderer)
            && ports.app.view->SendViewUpdate(update)
            && ports.interaction.update->SendUpdates(),
        "strategy switch sync should build the initial volume pipeline",
        failureCount);

    const std::array<VizMode, 7> modes = {
        VizMode::Volume,
        VizMode::IsoSurface,
        VizMode::SliceTop_down,
        VizMode::SliceFront_back,
        VizMode::SliceLeft_right,
        VizMode::CompositeVolume,
        VizMode::CompositeIsoSurface
    };
    for (const VizMode mode : modes) {
        update = {};
        update.mode = mode;
        ports.app.view->SendViewUpdate(update);
        ports.interaction.update->SendUpdates();
        vtkCamera* camera = renderer->GetActiveCamera();
        const bool isSlice = mode == VizMode::SliceTop_down
            || mode == VizMode::SliceFront_back
            || mode == VizMode::SliceLeft_right;
        bool isOrientationValid = camera != nullptr;
        if (camera && isSlice) {
            const double* position = camera->GetPosition();
            const double* focalPoint = camera->GetFocalPoint();
            const double* viewUp = camera->GetViewUp();
            if (mode == VizMode::SliceTop_down) {
                isOrientationValid = position[2] > focalPoint[2]
                    && std::abs(viewUp[1] - 1.0) < 1e-12;
            }
            else if (mode == VizMode::SliceFront_back) {
                isOrientationValid = position[1] > focalPoint[1]
                    && std::abs(viewUp[2] - 1.0) < 1e-12;
            }
            else {
                isOrientationValid = position[0] > focalPoint[0]
                    && std::abs(viewUp[2] - 1.0) < 1e-12;
            }
        }
        SetExpect((isSlice
                || ports.interaction.model->GetMainProp())
                && camera
                && (camera->GetParallelProjection() != 0) == isSlice
                && isOrientationValid,
            "all factory modes should build with service-owned camera semantics",
            failureCount);
    }

    auto firstMaterial = state->GetMaterial();
    firstMaterial.diffuse = 0.23;
    firstMaterial.opacity = 0.61;
    update = {};
    update.material = firstMaterial;
    ports.app.view->SendViewUpdate(update);
    ports.interaction.update->SendUpdates();

    update = {};
    update.mode = VizMode::IsoSurface;
    ports.app.view->SendViewUpdate(update);
    ports.interaction.update->SendUpdates();
    auto* isoActor = vtkActor::SafeDownCast(
        ports.interaction.model->GetMainProp());
    auto* isoProperty = isoActor
        ? isoActor->GetProperty() : nullptr;
    SetExpect(isoProperty
            && std::abs(isoProperty->GetDiffuse()
                - firstMaterial.diffuse) < 1e-12
            && std::abs(isoProperty->GetOpacity()
                - firstMaterial.opacity) < 1e-12,
        "a new strategy must receive the complete shared visual state",
        failureCount);

    auto nextMaterial = firstMaterial;
    nextMaterial.diffuse = 0.41;
    nextMaterial.opacity = 0.78;
    update = {};
    update.material = nextMaterial;
    ports.app.view->SendViewUpdate(update);
    ports.interaction.update->SendUpdates();

    update = {};
    update.mode = VizMode::Volume;
    ports.app.view->SendViewUpdate(update);
    ports.interaction.update->SendUpdates();
    auto* volume = vtkVolume::SafeDownCast(
        ports.interaction.model->GetMainProp());
    auto* volumeProperty = volume
        ? volume->GetProperty() : nullptr;
    SetExpect(volumeProperty
            && std::abs(volumeProperty->GetDiffuse()
                - nextMaterial.diffuse) < 1e-12,
        "a cached strategy must replay state changed while it was inactive",
        failureCount);

    vtkCamera* oldCamera = renderer->GetActiveCamera();
    oldCamera->SetPosition(9.0, -4.0, 13.0);
    oldCamera->SetFocalPoint(1.0, 2.0, 3.0);
    oldCamera->SetViewUp(0.0, 1.0, 0.0);
    oldCamera->ParallelProjectionOff();
    const std::array<double, 3> reboundPosition = {
        oldCamera->GetPosition()[0],
        oldCamera->GetPosition()[1],
        oldCamera->GetPosition()[2]
    };
    const std::array<double, 3> reboundFocal = {
        oldCamera->GetFocalPoint()[0],
        oldCamera->GetFocalPoint()[1],
        oldCamera->GetFocalPoint()[2]
    };

    update = {};
    update.mode = VizMode::SliceTop_down;
    ports.app.view->SendViewUpdate(update);
    auto reboundRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto reboundWindow = vtkSmartPointer<vtkRenderWindow>::New();
    reboundWindow->SetOffScreenRendering(1);
    reboundWindow->AddRenderer(reboundRenderer);
    ports.renderBind->SetRenderTarget(
        reboundWindow, reboundRenderer);
    vtkCamera* reboundCamera = reboundRenderer->GetActiveCamera();
    SetExpect(reboundCamera
            && reboundCamera->GetParallelProjection() == 0
            && std::equal(
                reboundPosition.begin(),
                reboundPosition.end(),
                reboundCamera->GetPosition())
            && std::equal(
                reboundFocal.begin(),
                reboundFocal.end(),
                reboundCamera->GetFocalPoint()),
        "renderer rebind must restore the committed camera and ignore pending mode",
        failureCount);
    ports.interaction.update->SendUpdates();
    SetExpect(reboundCamera->GetParallelProjection() != 0,
        "pending Slice mode should affect camera only after pipeline commit",
        failureCount);

    update = {};
    update.mode = VizMode::Volume;
    ports.app.view->SendViewUpdate(update);
    ports.interaction.update->SendUpdates();
    reboundCamera->SetPosition(8.0, 5.0, 12.0);
    reboundCamera->SetFocalPoint(1.5, 1.5, 1.5);
    const std::array<double, 3> cameraOffset = {
        reboundCamera->GetPosition()[0]
            - reboundCamera->GetFocalPoint()[0],
        reboundCamera->GetPosition()[1]
            - reboundCamera->GetFocalPoint()[1],
        reboundCamera->GetPosition()[2]
            - reboundCamera->GetFocalPoint()[2]
    };
    std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 5.0,
        0.0, 1.0, 0.0, -2.0,
        0.0, 0.0, 1.0, 4.0,
        0.0, 0.0, 0.0, 1.0
    };
    ports.interaction.model->SetModelMatrix(modelToWorld);
    ports.interaction.update->SendUpdates();
    double imageCenter[3] = { 0.0, 0.0, 0.0 };
    image->GetCenter(imageCenter);
    const double* transformedFocal = reboundCamera->GetFocalPoint();
    const double* transformedPosition = reboundCamera->GetPosition();
    SetExpect(std::abs(transformedFocal[0]
                - (imageCenter[0] + 5.0)) < 1e-12
            && std::abs(transformedFocal[1]
                - (imageCenter[1] - 2.0)) < 1e-12
            && std::abs(transformedFocal[2]
                - (imageCenter[2] + 4.0)) < 1e-12
            && std::abs((transformedPosition[0] - transformedFocal[0])
                - cameraOffset[0]) < 1e-12
            && std::abs((transformedPosition[1] - transformedFocal[1])
                - cameraOffset[1]) < 1e-12
            && std::abs((transformedPosition[2] - transformedFocal[2])
                - cameraOffset[2]) < 1e-12,
        "service-owned Transform camera should move center and preserve view offset",
        failureCount);
}

void StartRealCamera(int& failureCount)
{
    char* rawPathValue = nullptr;
    std::size_t rawPathSize = 0;
    (void)_dupenv_s(
        &rawPathValue,
        &rawPathSize,
        "MVVCVTK_REAL_RAW");
    const std::unique_ptr<char, decltype(&std::free)> rawPathOwner(
        rawPathValue,
        &std::free);
    const char* rawPathText = rawPathOwner.get();
    if (!rawPathText || rawPathText[0] == '\0') {
        std::cout << "REAL_DATA_NOT_REQUESTED\n";
        return;
    }

    const std::filesystem::path rawPath =
        std::filesystem::u8path(rawPathText);
    std::error_code error;
    if (!std::filesystem::is_regular_file(rawPath, error)) {
        std::cerr << "REAL_DATA_MISSING: " << rawPathText << '\n';
        ++failureCount;
        return;
    }
    constexpr std::uintmax_t expectedSize = 62500000;
    const std::uintmax_t actualSize =
        std::filesystem::file_size(rawPath, error);
    if (error || actualSize != expectedSize) {
        std::cerr << "REAL_DATA_MISMATCH: size="
            << actualSize << " expected=" << expectedSize << '\n';
        ++failureCount;
        return;
    }

    const auto layout = VolumeLayout::Create(
        { 250, 250, 250 },
        { 0.085F, 0.085F, 0.085F },
        { 0.0F, 0.0F, 0.0F });
    if (!layout) {
        std::cerr << "REAL_DATA_MISMATCH: invalid locked layout\n";
        ++failureCount;
        return;
    }

    auto dataManager = std::make_shared<RawVolumeDataManager>();
    AppDataLoadTaskService loadService(dataManager);
    auto loadTask = loadService.BuildLoadFileTask(
        rawPath.u8string(), *layout);
    if (!loadTask) {
        std::cerr << "REAL_DATA_MISMATCH: load task rejected\n";
        ++failureCount;
        return;
    }
    auto loadResult = loadTask->get_future();
    (*loadTask)(TaskStopToken{});
    if (!loadResult.get()) {
        std::cerr << "REAL_DATA_MISMATCH: production load failed\n";
        ++failureCount;
        return;
    }

    const auto loadStage = dataManager->GetLoadStage();
    VtkImageGridSnapshot imageState;
    const bool isCommitted = loadStage
        && dataManager->SetLoadCommit(loadStage, imageState);
    const auto* imagePayload = imageState && imageState->data
        ? dynamic_cast<const ImageGrid3DPayload*>(
            imageState->data->payload.get())
        : nullptr;
    const bool isGeometryValid = isCommitted
        && imageState && imageState->image && imagePayload
        && imagePayload->GetGeometry().dimensions
            == std::array<int, 3>{ 250, 250, 250 }
        && std::abs(imagePayload->GetGeometry().spacing[0] - 0.085) < 1e-6
        && std::abs(imagePayload->GetGeometry().spacing[1] - 0.085) < 1e-6
        && std::abs(imagePayload->GetGeometry().spacing[2] - 0.085) < 1e-6
        && std::abs(imagePayload->GetGeometry().origin[0] + 21.165) < 1e-5
        && std::abs(imagePayload->GetGeometry().origin[1] + 21.165) < 1e-5
        && std::abs(imagePayload->GetGeometry().origin[2]) < 1e-8
        && std::abs(imagePayload->GetScalarRange()[0]
            - (-0.1316370964050293)) < 1e-7
        && std::abs(imagePayload->GetScalarRange()[1]
            - 0.065924093127250671) < 1e-7;
    SetExpect(isGeometryValid,
        "locked RAW should commit the expected RAS geometry and scalar range",
        failureCount);
    if (!isGeometryValid) {
        std::cerr << "REAL_DATA_MISMATCH: committed geometry/range\n";
        return;
    }

    const int cameraFailureCount = failureCount;
    std::size_t vtkErrorCount = 0;
    auto errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    errorCallback->SetClientData(&vtkErrorCount);
    errorCallback->SetCallback([](
        vtkObject*, unsigned long eventId, void* clientData, void*) {
        if (eventId != vtkCommand::ErrorEvent || !clientData) return;
        ++(*static_cast<std::size_t*>(clientData));
    });

    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    const unsigned long windowErrorTag = renderWindow->AddObserver(
        vtkCommand::ErrorEvent, errorCallback);
    renderWindow->SetOffScreenRendering(1);

    HostCoreServices core;
    core.sharedDataMgr = dataManager;
    core.sharedState = state;
    core.sharedStateBroadcaster = broadcaster;
    HostRenderViewConfig view;
    view.id = "real-camera";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode = HostRenderMode::Volume;
    view.renderWindow = renderWindow;
    HostViewRuntimeRegistry views;
    const bool isViewBuilt = views.Build(core, { view });
    const HostViewTarget target{
        "real-camera", false, HostRenderViewRole::Primary3D
    };
    const auto endpoints = views.BuildEndpoints();
    auto* renderer = endpoints.size() == 1
        ? endpoints.front().renderer
        : nullptr;
    const unsigned long rendererErrorTag = renderer
        ? renderer->AddObserver(vtkCommand::ErrorEvent, errorCallback)
        : 0;
    if (!isViewBuilt || !renderer) {
        if (rendererErrorTag != 0) {
            renderer->RemoveObserver(rendererErrorTag);
        }
        renderWindow->RemoveObserver(windowErrorTag);
        errorCallback->SetClientData(nullptr);
        std::cerr << "REAL_DATA_VTK_ERROR: host view build failed\n";
        ++failureCount;
        return;
    }

    HostCommandRouter router(views.GetViewDirectory());
    broadcaster->SendFlags(UpdateFlags::All);
    SetExpect(views.SendViewUpdates(target),
        "real camera test should build the production Volume pipeline",
        failureCount);
    SetExpect(renderer->GetActiveCamera()->GetParallelProjection() == 0,
        "real Volume mode should use perspective projection",
        failureCount);

    const auto sendMode = [&router, &views, &target](
        const HostRenderMode mode) {
        HostViewSetRequest request;
        request.targetView = target;
        request.mode = mode;
        return router.Dispatch(std::move(request))
            && views.SendViewUpdates(target);
    };
    SetExpect(sendMode(HostRenderMode::SliceTopDown),
        "real camera test should switch through the Host value route",
        failureCount);
    SetExpect(renderer->GetActiveCamera()->GetParallelProjection() != 0,
        "real Slice mode should use parallel projection",
        failureCount);
    SetExpect(sendMode(HostRenderMode::Volume),
        "real camera test should restore Volume through the Host value route",
        failureCount);

    vtkCamera* camera = renderer->GetActiveCamera();
    camera->SetPosition(10.0, -6.0, 15.0);
    camera->SetFocalPoint(0.0, 0.0, 0.0);
    const std::array<double, 3> oldOffset = {
        camera->GetPosition()[0] - camera->GetFocalPoint()[0],
        camera->GetPosition()[1] - camera->GetFocalPoint()[1],
        camera->GetPosition()[2] - camera->GetFocalPoint()[2]
    };
    const std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 2.5,
        0.0, 1.0, 0.0, -1.5,
        0.0, 0.0, 1.0, 3.0,
        0.0, 0.0, 0.0, 1.0
    };
    SetExpect(views.SetModelMatrix(target, modelToWorld)
            && views.SendViewUpdates(target),
        "real Transform should cross only the Host value boundary",
        failureCount);
    renderWindow->Render();
    double modelCenter[3] = { 0.0, 0.0, 0.0 };
    imageState->image->GetCenter(modelCenter);
    const double* focalPoint = camera->GetFocalPoint();
    const double* position = camera->GetPosition();
    SetExpect(std::abs(focalPoint[0]
                - (modelCenter[0] + 2.5)) < 1e-6
            && std::abs(focalPoint[1]
                - (modelCenter[1] - 1.5)) < 1e-6
            && std::abs(focalPoint[2]
                - (modelCenter[2] + 3.0)) < 1e-6
            && std::abs((position[0] - focalPoint[0])
                - oldOffset[0]) < 1e-6
            && std::abs((position[1] - focalPoint[1])
                - oldOffset[1]) < 1e-6
            && std::abs((position[2] - focalPoint[2])
                - oldOffset[2]) < 1e-6,
        "real Transform should preserve camera offset at the transformed RAS center",
        failureCount);

    const std::array<double, 3> savedPosition = {
        position[0], position[1], position[2]
    };
    const std::array<double, 3> savedFocal = {
        focalPoint[0], focalPoint[1], focalPoint[2]
    };
    auto reboundWindow = vtkSmartPointer<vtkRenderWindow>::New();
    const unsigned long reboundWindowErrorTag =
        reboundWindow->AddObserver(
            vtkCommand::ErrorEvent, errorCallback);
    reboundWindow->SetOffScreenRendering(1);
    const bool isRebound = views.SetViewWindow(
        "real-camera", reboundWindow);
    const auto reboundEndpoints = views.BuildEndpoints();
    auto* reboundRenderer = reboundEndpoints.size() == 1
        ? reboundEndpoints.front().renderer
        : nullptr;
    const unsigned long reboundRendererErrorTag = reboundRenderer
        ? reboundRenderer->AddObserver(
            vtkCommand::ErrorEvent, errorCallback)
        : 0;
    reboundWindow->Render();
    vtkCamera* reboundCamera = reboundRenderer
        ? reboundRenderer->GetActiveCamera()
        : nullptr;
    SetExpect(isRebound
            && reboundCamera
            && reboundCamera->GetParallelProjection() == 0
            && std::equal(
                savedPosition.begin(),
                savedPosition.end(),
                reboundCamera->GetPosition())
            && std::equal(
                savedFocal.begin(),
                savedFocal.end(),
                reboundCamera->GetFocalPoint()),
        "real renderer rebind should preserve committed Volume camera state",
        failureCount);

    const bool hasObservers = rendererErrorTag != 0
        && windowErrorTag != 0
        && reboundRendererErrorTag != 0
        && reboundWindowErrorTag != 0;
    const bool isCameraValid = hasObservers
        && vtkErrorCount == 0
        && failureCount == cameraFailureCount;
    renderer->RemoveObserver(rendererErrorTag);
    renderWindow->RemoveObserver(windowErrorTag);
    if (reboundRenderer) {
        reboundRenderer->RemoveObserver(reboundRendererErrorTag);
    }
    reboundWindow->RemoveObserver(reboundWindowErrorTag);
    errorCallback->SetClientData(nullptr);
    if (!isCameraValid) {
        if (failureCount == cameraFailureCount) ++failureCount;
        std::cerr << "REAL_DATA_VTK_ERROR: count=" << vtkErrorCount
            << " observers=" << (hasObservers ? "ready" : "missing")
            << '\n';
        return;
    }
    std::cout << "REAL_DATA_OK: " << rawPathText << '\n';
}

void StartVisualConfigGetters(int& failureCount)
{
    auto dataManager = std::make_shared<DataStub>();
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.view || !ports.app.session
        || !ports.interaction.state) {
        SetExpect(false,
            "view state getters need every narrow port",
            failureCount);
        return;
    }

    PreInitConfig config;
    config.vizMode = VizMode::Volume;
    config.material = { 0.2, 0.6, 0.3, 12.0, 0.8, true };
    config.volumeTransferFunction.colorNodes = {
        { 0.0, 0.2, 0.3, 0.4 },
        { 1.0, 0.8, 0.7, 0.6 }
    };
    config.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.1 },
        { 1.0, 0.9 }
    };
    config.isoThreshold = 12.5;
    config.bgColor = { 0.05, 0.1, 0.15 };
    config.windowLevel = { 80.0, 20.0 };
    config.hasVolumeTransferFunction = true;
    config.hasIso = true;
    config.hasBgColor = true;
    config.hasWindowLevel = true;
    AppSessionUpdate sessionUpdate;
    sessionUpdate.spacing =
        std::array<double, 3>{ 0.5, 1.0, 1.5 };
    const bool isConfigSet =
        ports.app.view->SetViewConfig(config)
        && ports.app.session->SendSessionUpdate(sessionUpdate);
    state->SetScalarRange(-4.0, 88.0);

    auto viewState = ports.app.view->GetViewState();
    SetExpect(isConfigSet
            && viewState.mode == VizMode::Volume
            && viewState.material.opacity == 0.8
            && viewState.material.isShadeOn
            && viewState.volumeTransferFunction.opacityNodes.size() == 2
            && viewState.volumeTransferFunction.opacityNodes[1].opacity == 0.9
            && viewState.isoThreshold == 12.5
            && viewState.background.r == 0.05
            && viewState.spacing[2] == 1.5
            && viewState.windowLevel.windowWidth == 80.0
            && viewState.windowLevelMode == WindowLevelMode::Manual
            && viewState.scalarRange
                == std::array<double, 2>{ -4.0, 88.0 },
        "view state snapshot must mirror visual config and scalar range",
        failureCount);

    const AppViewState manualState = viewState;
    AppViewUpdate autoWindow;
    autoWindow.windowLevelMode = WindowLevelMode::Auto;
    const bool isAutoWindowSet =
        ports.app.view->SendViewUpdate(autoWindow);
    const auto autoState = ports.app.view->GetViewState();
    const bool isManualRestored = ports.app.view->SetViewState(
        manualState, autoState.revision);
    const auto restoredState = ports.app.view->GetViewState();
    SetExpect(isAutoWindowSet
            && autoState.windowLevelMode == WindowLevelMode::Auto
            && autoState.windowLevel.windowWidth == 92.0
            && autoState.windowLevel.windowCenter == 42.0
            && isManualRestored
            && restoredState.windowLevelMode
                == WindowLevelMode::Manual
            && restoredState.windowLevel.windowWidth == 80.0
            && restoredState.windowLevel.windowCenter == 20.0,
        "view snapshot restore must preserve window/level intent",
        failureCount);

    const InteractionSource source{ "getter-test", "view" };
    AppViewUpdate update;
    AppVisibilityUpdate visibility;
    visibility.isRulerVisible = false;
    update.visibility = visibility;
    const bool isStateSet =
        ports.interaction.state->SetInteracting(source, true)
        && ports.app.view->SendViewUpdate(update);
    viewState = ports.app.view->GetViewState();
    SetExpect(isStateSet
            && viewState.isInteracting
            && (viewState.visibilityMask & VisFlags::Ruler) == 0,
        "narrow state and view ports must mirror interaction visibility",
        failureCount);
    update = {};
    auto invalidTransfer = config.volumeTransferFunction;
    invalidTransfer.opacityNodes.resize(1);
    update.volumeTransferFunction = invalidTransfer;
    SetExpect(!ports.app.view->SendViewUpdate(update)
            && ports.app.view->GetViewState()
                .volumeTransferFunction.opacityNodes.size() == 2,
        "view port must reject an incomplete transfer snapshot",
        failureCount);
}

void StartViewEventCommit(int& failureCount)
{
    auto dataManager = std::make_shared<DataStub>();
    const bool hasPrimary = dataManager->SetPrimaryForTest(BuildExportImage());
    const auto primaryBeforeSpacing = dataManager->GetPrimaryImage();
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    AppServiceArgs args;
    args.dataManager = dataManager;
    args.interactionState = state;
    args.eventSource = broadcaster;
    auto ports = CreateAppPorts(std::move(args));
    if (!ports.app.view || !ports.app.session) {
        SetExpect(false,
            "view event commit needs the narrow view port",
            failureCount);
        return;
    }

    auto observerOwner = std::make_shared<int>(0);
    int observerCount = 0;
    UpdateFlags observedFlags = UpdateFlags::None;
    std::array<double, 3> observedSpacing = {};
    std::array<double, 3> observedCursor = {};
    const std::weak_ptr<AppViewPort> weakView = ports.app.view;
    broadcaster->SetObserver(
        observerOwner,
        [&observerCount, &observedFlags,
            &observedSpacing, &observedCursor, weakView](
                const UpdateFlags flags) {
            ++observerCount;
            observedFlags |= flags;
            const auto view = weakView.lock();
            if (view) {
                const auto viewState = view->GetViewState();
                observedSpacing = viewState.spacing;
                observedCursor = viewState.cursorWorld;
            }
        });

    const auto baseline = ports.app.view->GetViewState();
    AppViewUpdate failedUpdate;
    failedUpdate.background = BackgroundColor{ 0.4, 0.5, 0.6 };
    failedUpdate.volumeQuality = static_cast<VolumeQuality>(99);
    const bool isFailed = !ports.app.view->SendViewUpdate(failedUpdate);
    const auto restored = ports.app.view->GetViewState();
    SetExpect(isFailed
            && restored.background.r == baseline.background.r
            && restored.volumeQuality == baseline.volumeQuality
            && observerCount == 0
            && observedFlags == UpdateFlags::None,
        "failed compensated view update must not expose observer frames",
        failureCount);

    AppViewUpdate committedUpdate;
    committedUpdate.background = BackgroundColor{ 0.1, 0.2, 0.3 };
    const bool isCommitted =
        ports.app.view->SendViewUpdate(committedUpdate);
    SetExpect(isCommitted
            && observerCount == 0
            && ports.app.view->GetViewState().revision
                > baseline.revision,
        "successful View update must remain local and advance its revision",
        failureCount);

    observerCount = 0;
    observedFlags = UpdateFlags::None;
    AppSessionUpdate initialSession;
    initialSession.spacing =
        std::array<double, 3>{ 3.0, 3.0, 3.0 };
    const bool isSessionSet =
        ports.app.session->SendSessionUpdate(initialSession);
    const auto sessionState = ports.app.view->GetViewState();
    const auto primaryAfterSpacing = dataManager->GetPrimaryImage();
    SetExpect(hasPrimary && primaryBeforeSpacing && primaryAfterSpacing
            && isSessionSet
            && observerCount == 1
            && (observedFlags & UpdateFlags::Spacing)
                != UpdateFlags::None
            && (observedFlags & UpdateFlags::DataReady)
                != UpdateFlags::None
            && primaryAfterSpacing->data->self
                != primaryBeforeSpacing->data->self
            && primaryAfterSpacing->binding->revision
                > primaryBeforeSpacing->binding->revision
            && state->GetDataRevision()
                == primaryAfterSpacing->data->self
            && state->GetDataBindingRevision()
                == primaryAfterSpacing->binding->revision
            && sessionState.spacing
                == std::array<double, 3>{ 3.0, 3.0, 3.0 },
        "Session spacing must publish its DataGraph identity through the dedicated port.",
        failureCount);

    observerCount = 0;
    observedFlags = UpdateFlags::None;
    std::mutex spacingMutex;
    std::condition_variable spacingChanged;
    bool isSpacingEntered = false;
    bool isSpacingReleased = false;
    std::array<double, 3> dataSpacing = sessionState.spacing;
    dataManager->setSpacingCall = [
        &spacingMutex,
        &spacingChanged,
        &isSpacingEntered,
        &isSpacingReleased,
        &dataSpacing](const std::array<double, 3>& spacing) {
        std::unique_lock<std::mutex> lock(spacingMutex);
        if (spacing == std::array<double, 3>{ 4.0, 4.0, 4.0 }) {
            isSpacingEntered = true;
            spacingChanged.notify_all();
            spacingChanged.wait(lock, [&isSpacingReleased]() {
                return isSpacingReleased;
            });
            return false;
        }
        dataSpacing = spacing;
        return true;
    };
    AppSessionUpdate conflictingUpdate;
    conflictingUpdate.spacing =
        std::array<double, 3>{ 4.0, 4.0, 4.0 };
    bool isConflictingAccepted = true;
    std::thread ownerWriter([&]() {
        isConflictingAccepted =
            ports.app.session->SendSessionUpdate(conflictingUpdate);
    });
    {
        std::unique_lock<std::mutex> lock(spacingMutex);
        spacingChanged.wait(lock, [&isSpacingEntered]() {
            return isSpacingEntered;
        });
    }
    std::thread concurrentWriter([state]() {
        state->SetCursorRawWorld(37.0, 38.0, 39.0);
        state->SetCursorAxis(-1);
        state->SetCursorWorld(37.0, 38.0, 39.0);
    });
    concurrentWriter.join();
    const int observerCountBeforeClose = observerCount;
    {
        const std::lock_guard<std::mutex> lock(spacingMutex);
        isSpacingReleased = true;
    }
    spacingChanged.notify_all();
    ownerWriter.join();
    dataManager->setSpacingCall = {};
    const auto barrierState = ports.app.view->GetViewState();
    SetExpect(!isConflictingAccepted
            && observerCountBeforeClose == 0
            && observerCount == 1
            && (observedFlags & UpdateFlags::Cursor)
                != UpdateFlags::None
            && (observedFlags & UpdateFlags::Spacing)
                == UpdateFlags::None
            && barrierState.spacing == sessionState.spacing
            && observedSpacing == sessionState.spacing
            && dataSpacing == sessionState.spacing
            && barrierState.cursorWorld
                == std::array<double, 3>{ 37.0, 38.0, 39.0 }
            && observedCursor
                == std::array<double, 3>{ 37.0, 38.0, 39.0 },
        "failed Session rollback must preserve a concurrent cursor value",
        failureCount);

    class ThrowingSink final : public IStateEventSink {
    public:
        void SendFlags(UpdateFlags) override
        {
            throw std::runtime_error("intentional sink failure");
        }
    };
    auto throwState = std::make_shared<SharedInteractionState>(
        std::make_shared<ThrowingSink>());
    AppServiceArgs throwArgs;
    throwArgs.dataManager = std::make_shared<DataStub>();
    throwArgs.interactionState = throwState;
    throwArgs.eventSource = std::make_shared<SharedStateBroadcaster>();
    auto throwPorts = CreateAppPorts(std::move(throwArgs));
    AppViewUpdate throwUpdate;
    throwUpdate.background = BackgroundColor{ 0.2, 0.3, 0.4 };
    const auto throwBaseline = throwPorts.app.view
        ? throwPorts.app.view->GetViewState()
        : AppViewState{};
    const bool isThrowCommitted = throwPorts.app.view
        && throwPorts.app.view->SendViewUpdate(throwUpdate);
    const auto throwCommitted = throwPorts.app.view
        ? throwPorts.app.view->GetViewState()
        : AppViewState{};
    SetExpect(isThrowCommitted
            && throwCommitted.revision > throwBaseline.revision
            && throwCommitted.background.r == 0.2,
        "throwing event sink must not change committed view result",
        failureCount);
}
}

int AppTaskSuite::GetFailCount() const
{
    int failureCount = 0;
    StartVolumeTypes(failureCount);
    StartOwningTasks(failureCount);
    StartExportSnapshot(failureCount);
    StartBoundedTasks(failureCount);
    StartExportFiles(failureCount);
    StartTransformedMaskedRaw(failureCount);
    StartStateGate(failureCount);
    StartObserverGate(failureCount);
    StartCandidateParams(failureCount);
    StartMaskSnapshot(failureCount);
    StartFactoryAdmission(failureCount);
    StartPipelineRollback(failureCount);
    StartQualityRollback(failureCount);
    StartQualityLatest(failureCount);
    StartRenderOwnerGate(failureCount);
    StartStrategySwitchSync(failureCount);
    StartInputSwap(failureCount);
    StartRealCamera(failureCount);
    StartVisualConfigGetters(failureCount);
    StartViewEventCommit(failureCount);
    return failureCount;
}
