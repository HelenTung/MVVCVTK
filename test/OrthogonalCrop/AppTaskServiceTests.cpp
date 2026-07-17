#include "Tasks/AppDataLoadTaskService.h"
#include "AppState.h"
#include "AppStateEvents.h"
#include "Data/VolumeTypes.h"
#include "PlanarTestSuites.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void SetExpect(bool isPassed, const char* message, int& failureCount)
{
    if (isPassed) return;
    ++failureCount;
    std::cerr << "[AppTaskTests] " << message << '\n';
}

class DataStub final : public AbstractDataManager {
protected:
    ImageSnapshot GetImageSnapshot() const override { return {}; }

public:
    vtkSmartPointer<vtkImageData> GetVtkImage() const override { return nullptr; }
    ImageState GetImageState() const override { return {}; }
    std::array<double, 2> GetScalarRange() const override { return { 0.0, 0.0 }; }
    std::array<double, 3> GetSpacing() const override { return { 1.0, 1.0, 1.0 }; }
    bool SetSpacing(const std::array<double, 3>&) override { return true; }
    DataVersion GetDataVersion() const override { return 0; }

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
        return isLoadSuccess;
    }

    bool SetCurrentFromPending(bool& hasPending) override
    {
        hasPending = false;
        return true;
    }
    bool ClearPending() override { return true; }
    bool ExportData(const std::string&, const std::array<double, 16>&) override { return false; }
    bool ExportSlices(const std::string&, Orientation, const WindowLevelParams&,
        const std::array<double, 16>&) override { return false; }

    std::string loadedPath;
    std::array<int, 3> loadedDims{};
    std::vector<float> loadedVoxels;
    bool isLoadSuccess = true;
    bool isThrowNeeded = false;
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
        (*reloadTask)();
        SetExpect(result.get() && dataManager->loadedVoxels
            == std::vector<float>({ 0, 1, 2, 3, 4, 5, 6, 7 }),
            "task must retain voxels after caller storage is destroyed", failureCount);
    }

    auto fileTask = service.BuildLoadFileTask("volume.raw", *layout);
    SetExpect(fileTask.has_value(), "file task must be built", failureCount);
    if (fileTask) {
        auto result = fileTask->get_future();
        (*fileTask)();
        SetExpect(result.get() && dataManager->loadedPath == "volume.raw"
            && dataManager->loadedDims == std::array<int, 3>{ 2, 2, 2 },
            "file task must retain path and layout", failureCount);
    }

    dataManager->isThrowNeeded = true;
    auto failedTask = service.BuildLoadFileTask("throw.raw", *layout);
    if (failedTask) {
        auto result = failedTask->get_future();
        (*failedTask)();
        SetExpect(!result.get(), "worker exceptions must become false", failureCount);
    }
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
}

int AppTaskSuite::GetFailCount() const
{
    int failureCount = 0;
    StartVolumeTypes(failureCount);
    StartOwningTasks(failureCount);
    StartStateGate(failureCount);
    return failureCount;
}
