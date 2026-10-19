#include "App/Services/AppServiceFactory.h"
#include "App/Services/AppPorts.h"
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "Data/DataConverters.h"
#include "Data/DataManager.h"
#include "Render/Support/BaseVisualStrategy.h"

#include <vtkImageData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using ImageFactory = std::function<double(int, int, int)>;

class TestData final : public RawVolumeDataManager {
public:
    using BaseDataManager::SetOwnedImage;

    bool SetImageWithMask(
        vtkSmartPointer<vtkImageData> image,
        vtkSmartPointer<vtkImageData> mask)
    {
        if (!SetLoadImage(std::move(image), std::move(mask))) return false;
        const auto stage = GetLoadStage();
        VtkImageGridSnapshot published;
        return stage && SetLoadCommit(stage, published) && published;
    }
};

class PassiveStrategy final : public BaseVisualStrategy {
public:
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    bool SetProductCommit() override { return true; }
    RenderTransitionState GetTransitionState() const override { return {}; }
};

class RenderWindowProbe final : public vtkRenderWindow {
public:
    static RenderWindowProbe* New() { return new RenderWindowProbe; }
    vtkTypeMacro(RenderWindowProbe, vtkRenderWindow);
    void Render() override { ++renderCount; }

    int renderCount = 0;
};

class WorkerGate final : public std::enable_shared_from_this<WorkerGate> {
public:
    std::thread Start(AppWorkerWork work)
    {
        const auto self = shared_from_this();
        {
            const std::lock_guard<std::mutex> lock(mutex);
            ++scheduled;
        }
        return std::thread([self, work = std::move(work)]() mutable {
            {
                std::unique_lock<std::mutex> lock(self->mutex);
                ++self->started;
                self->changed.notify_all();
                self->changed.wait(lock, [&] { return self->released; });
            }
            work();
        });
    }

    bool WaitForWorkers(const Clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_until(lock, deadline, [&] { return started >= scheduled; });
    }

    void Release()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t scheduled = 0;
    std::size_t started = 0;
    bool released = false;
};

class OwnerWake final {
public:
    void Notify()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            ++generation;
        }
        changed.notify_all();
    }

    std::uint64_t GetGeneration() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return generation;
    }

    bool WaitForChange(const std::uint64_t previous, const Clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_until(lock, deadline, [&] { return generation != previous; });
    }

    bool WaitForGeneration(const std::uint64_t target, const Clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_until(lock, deadline, [&] { return generation >= target; });
    }

private:
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t generation = 0;
};

class CaptureCerr final {
public:
    CaptureCerr() : previous(std::cerr.rdbuf(buffer.rdbuf())) {}
    ~CaptureCerr() { std::cerr.rdbuf(previous); std::cerr << buffer.str(); }
    std::string Text() const { return buffer.str(); }

private:
    std::ostringstream buffer;
    std::streambuf* previous;
};

vtkSmartPointer<vtkImageData> MakeImage(const ImageFactory& valueAt, const int side = 16)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(4, 4 + side - 1, -2, -2 + side - 1, 1, side);
    image->SetSpacing(2.0, 3.0, 4.0);
    image->SetOrigin(10.0, 20.0, 30.0);
    const double direction[9]{0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* values = static_cast<float*>(image->GetScalarPointer());
    if (!values) return nullptr;
    for (int z = 0; z < side; ++z)
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                values[x + side * (y + side * z)] = static_cast<float>(valueAt(x, y, z));
    image->GetPointData()->GetScalars()->Modified();
    image->Modified();
    return image;
}

vtkSmartPointer<vtkImageData> MakeBimodalImage()
{
    return MakeImage([](const int x, const int y, const int z) {
        if (x == 2 && y == 2 && z == 2) return -900.0;
        return x < 8 ? 0.0 : 1000.0;
    });
}

vtkSmartPointer<vtkImageData> MakeShiftedBimodalImage()
{
    return MakeImage([](const int x, const int y, const int z) {
        if (x == 2 && y == 2 && z == 2) return -800.0;
        return x < 8 ? 100.0 : 1100.0;
    });
}

vtkSmartPointer<vtkImageData> MakeValidityMask(vtkImageData* image)
{
    if (!image) return nullptr;
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* bytes = static_cast<unsigned char*>(mask->GetScalarPointer());
    if (!bytes) return nullptr;
    std::fill_n(bytes, image->GetNumberOfPoints(), static_cast<unsigned char>(255));
    bytes[1] = 0;
    mask->GetPointData()->GetScalars()->Modified();
    mask->Modified();
    return mask;
}

bool SetBimodalSource(TestData& data, vtkSmartPointer<vtkImageData> image = {})
{
    if (!image) image = MakeBimodalImage();
    auto mask = MakeValidityMask(image);
    return mask && data.SetImageWithMask(std::move(image), std::move(mask));
}

bool SetShiftedBimodalSource(TestData& data)
{
    auto image = MakeShiftedBimodalImage();
    auto mask = MakeValidityMask(image);
    return mask && data.SetImageWithMask(std::move(image), std::move(mask));
}

vtkSmartPointer<vtkImageData> MakeSinglePeakImage()
{
    // 每个粗桶都有连续支持，避免稀疏离散灰阶本身产生多个分离尖峰。
    // 两端各至少 17 个体素，使范围探测的最多 16 个修剪不改变 [0,511]。
    constexpr std::size_t sampleCount = 32U * 32U * 32U;
    std::array<double, 512> density{};
    double total = 0;
    for (std::size_t i = 0; i < density.size(); ++i) {
        const double position = (static_cast<double>(i) - 255.5) / 80.0;
        density[i] = std::exp(-0.5 * position * position);
        total += density[i];
    }
    std::array<std::size_t, 512> populations{};
    std::size_t assigned = 0;
    for (std::size_t i = 0; i < populations.size() / 2; ++i) {
        const auto count = 17 + static_cast<std::size_t>((sampleCount - 17 * populations.size()) * density[i] / total);
        populations[i] = populations[populations.size() - 1 - i] = count;
        assigned += 2 * count;
    }
    for (std::size_t i = 0; assigned < sampleCount; ++i, assigned += 2) {
        ++populations[i % 256];
        ++populations[511 - i % 256];
    }
    std::vector<float> values;
    values.reserve(sampleCount);
    for (std::size_t i = 0; i < populations.size(); ++i)
        values.insert(values.end(), populations[i], static_cast<float>(i));
    std::size_t index = 0;
    return MakeImage([&](const int, const int, const int) { return values[index++]; }, 32);
}

class RuntimeFixture final {
public:
    RuntimeFixture(
        std::shared_ptr<TestData> source,
        std::shared_ptr<HistogramConverter> converter,
        bool holdWorkers = true)
        : data(std::move(source)), histogram(std::move(converter)), gate(std::make_shared<WorkerGate>())
    {
        AppServiceArgs args;
        args.dataManager = data;
        state = std::make_shared<SharedInteractionState>(events);
        args.interactionState = state;
        args.eventSource = events;
        args.histogram = histogram;
        args.onWorkAvailable = [wake = wake] { wake->Notify(); };
        if (holdWorkers) {
            args.workerStart = [workers = gate](AppWorkerWork work) {
                return workers->Start(std::move(work));
            };
        }
        args.strategyCreate = [count = strategyCreations](const VizMode) -> std::shared_ptr<AbstractVisualStrategy> {
            ++*count;
            return std::make_shared<PassiveStrategy>();
        };
        ports = CreateAppPorts(std::move(args));
        if (holdWorkers && !gate->WaitForWorkers(Clock::now() + std::chrono::seconds(5))) {
            gate->Release();
            if (ports.taskControl) {
                (void)ports.taskControl->SetTaskStopping();
                (void)ports.taskControl->StopTasks(Clock::now() + std::chrono::seconds(5));
            }
            throw std::runtime_error("app task workers did not reach the controlled gate");
        }

        renderer = vtkSmartPointer<vtkRenderer>::New();
        window = vtkSmartPointer<RenderWindowProbe>::New();
        if (!ports.renderBind->SetRenderTarget(window, renderer))
            throw std::runtime_error("passive render target binding failed");
    }

    ~RuntimeFixture()
    {
        // 析构执行器前先释放门，断言提前失败也不能留下阻塞的所属工作线程。
        gate->Release();
        if (ports.taskControl) {
            (void)ports.taskControl->SetTaskStopping();
            (void)ports.taskControl->StopTasks(Clock::now() + std::chrono::seconds(5));
        }
    }

    bool SetExplicitView()
    {
        PreInitConfig config;
        config.vizMode = VizMode::IsoSurface;
        config.hasIso = true;
        config.isoThreshold = 123.0;
        config.hasWindowLevel = true;
        config.windowLevel = {77.0, 66.0};
        config.hasVolumeTransferFunction = true;
        config.volumeTransferFunction = MakeExplicitTransferFunction();
        return ports.app.view->SetViewConfig(config);
    }

    static VolumeTransferFunction MakeExplicitTransferFunction()
    {
        VolumeTransferFunction transfer;
        transfer.colorNodes = {{-900.0, 0.1, 0.2, 0.3}, {1000.0, 0.8, 0.7, 0.6}};
        transfer.opacityNodes = {{-900.0, 0.0}, {1000.0, 0.75}};
        return transfer;
    }

    bool BeginCandidate(
        const VtkImageGridSnapshot& snapshot,
        const std::uint64_t revision)
    {
        if (ports.dataStage->StartDataStage(snapshot, revision) != DataStageStatus::Preparing)
            return false;
        const auto ready = ports.dataStage->SetDataStageReady(snapshot, revision);
        return ready == DataStageStatus::Ready;
    }

    bool CommitCandidate(const VtkImageGridSnapshot& snapshot, const std::uint64_t revision)
    {
        if (!ports.dataStage->SetViewStage(snapshot, revision)) return false;
        ports.dataStage->SetDataStageComplete(revision);
        // 完整 owner 更新只接纳独立默认候选；所有 worker 仍可被 gate 阻塞。
        (void)ports.interaction.update->SendUpdates();
        return true;
    }

    bool WaitForPresentation(const DataRevisionRef& revision)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            const auto before = wake->GetGeneration();
            (void)ports.interaction.update->SendUpdates();
            const auto state = ports.app.view->GetViewState();
            if (GetPeakResult() && state.dataRevision == revision && !state.isPresentationPending) return true;
            if (!wake->WaitForChange(before, deadline)) return false;
            if (Clock::now() >= deadline) return false;
        }
    }

    bool WaitForValues(
        const DataRevisionRef& revision,
        const double iso,
        const double width,
        const double center)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            const auto before = wake->GetGeneration();
            (void)ports.interaction.update->SendUpdates();
            const auto state = ports.app.view->GetViewState();
            if (GetPeakResult() && state.dataRevision == revision && !state.isPresentationPending
                && state.isoThreshold == iso && state.windowLevel.windowWidth == width
                && state.windowLevel.windowCenter == center) return true;
            if (!wake->WaitForChange(before, deadline)) return false;
            if (Clock::now() >= deadline) return false;
        }
    }

    bool WaitForPeakCompletion()
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            const auto before = wake->GetGeneration();
            if (GetPeakResult()) return true;
            if (!wake->WaitForChange(before, deadline)) return false;
            if (Clock::now() >= deadline) return false;
            if (wake->GetGeneration() == before) continue;
        }
    }

    std::optional<AutoIsoResult> GetPeakResult() const
    {
        const auto stamp = ports.featureView->GetRenderInputStamp();
        if (!stamp) return {};
        const auto snapshot = data->GetImageGrid(data->GetDataGraph(), stamp->dataRevision);
        const auto payload = snapshot
            ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(snapshot->data->payload) : nullptr;
        if (!payload) return {};
        std::shared_ptr<AutoIsoTask> task;
        // 只领取已有的请求/缓存；测试观察不允许启动计算。
        const auto admission = histogram->StartAutoIso(stamp->dataRevision, payload,
            [](RenderLaneWork) { return TaskAdmissionResult::InvalidRequest; }, task);
        return admission == TaskAdmissionResult::Accepted && task ? task->GetResult() : std::nullopt;
    }

    std::shared_ptr<TestData> data;
    std::shared_ptr<HistogramConverter> histogram;
    std::shared_ptr<WorkerGate> gate;
    std::shared_ptr<std::atomic<unsigned>> strategyCreations = std::make_shared<std::atomic<unsigned>>(0);
    std::shared_ptr<OwnerWake> wake = std::make_shared<OwnerWake>();
    std::shared_ptr<SharedStateBroadcaster> events = std::make_shared<SharedStateBroadcaster>();
    std::shared_ptr<SharedInteractionState> state;
    vtkSmartPointer<RenderWindowProbe> window;
    vtkSmartPointer<vtkRenderer> renderer;
    AppFactoryResult ports;
};

bool CheckCommittedInput(RuntimeFixture& fixture, const VtkImageGridSnapshot& snapshot)
{
    const auto state = fixture.ports.app.view->GetViewState();
    const auto input = fixture.ports.featureView->GetRenderInputStamp();
    const auto& model = fixture.ports.interaction.model;
    return snapshot && state.dataRevision == snapshot->data->self && input
        && input->dataRevision == snapshot->data->self
        && model->GetPointVisible({16.0, 28.0, 34.0})
        && !model->GetPointVisible({16.0, 30.0, 34.0})
        && !model->GetPointVisible({16.0, 100.0, 34.0});
}

bool Check(bool condition, const char* message, int& failures)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool CheckAutomaticValues(
    RuntimeFixture& fixture,
    const double expectedIso,
    const double expectedWidth,
    const double expectedCenter)
{
    const auto state = fixture.ports.app.view->GetViewState();
    return state.isoThreshold == expectedIso
        && state.windowLevel.windowWidth == expectedWidth
        && state.windowLevel.windowCenter == expectedCenter;
}

void TestBimodalDefaultAndCache(int& failures)
{
    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "bimodal source publication", failures);
    auto histogram = std::make_shared<HistogramConverter>();
    RuntimeFixture first(data, histogram);
    const auto sourceA = data->GetPrimaryImage();
    Check(bool(sourceA), "bimodal snapshot exists", failures);
    Check(first.BeginCandidate(sourceA, 101), "first source stage becomes ready without histogram work", failures);
    Check(first.CommitCandidate(sourceA, 101), "first source commits while all workers remain gated", failures);
    const auto committedA = first.ports.app.view->GetViewState();
    Check(CheckCommittedInput(first, sourceA)
        && committedA.isoThreshold == 50.0
        && committedA.windowLevel.windowWidth == 1900.0
        && committedA.windowLevel.windowCenter == 50.0,
        "input identity, masked picking, and range midpoint are visible before peak work starts", failures);
    const auto beforeTicks = histogram->GetPeakBuildCount();
    for (int tick = 0; tick < 8; ++tick) (void)first.ports.interaction.update->SendUpdates();
    Check(histogram->GetPeakBuildCount() == beforeTicks
        && CheckCommittedInput(first, sourceA)
        && CheckAutomaticValues(first, 50.0, 1900.0, 50.0),
        "owner ticks do not wait for or rescan a held presentation estimate", failures);
    first.gate->Release();
    Check(first.WaitForPeakCompletion(), "the pure estimate can complete without an owner update", failures);
    (void)first.ports.interaction.update->SendPendingUpdates();
    Check(CheckAutomaticValues(first, 50.0, 1900.0, 50.0),
        "frame freeze consumes no late worker defaults and preserves the frozen presentation", failures);
    Check(first.WaitForValues(sourceA->data->self, 500.0, 1900.0, 500.0),
        "completed bimodal estimate updates ISO and window center to T", failures);
    const auto peakBuilds = histogram->GetPeakBuildCount();
    Check(peakBuilds == beforeTicks,
        "one committed source creates one peak estimate", failures);

    DataReadyState current;
    current.dataRevision = sourceA->data->self;
    current.bindingRevision = sourceA->binding->revision;
    current.scalarRange = { -900.0, 1000.0 };
    current.spacing = { 2.0, 3.0, 4.0 };
    first.state->SetImageDataReady(current);
    bool resetAccepted = false;
    std::thread resetter([&] {
        AppViewUpdate reset;
        reset.windowLevelMode = WindowLevelMode::Auto;
        resetAccepted = first.ports.app.view->SendViewUpdate(reset);
    });
    resetter.join();
    Check(resetAccepted && first.WaitForValues(sourceA->data->self, 500.0, 1900.0, 500.0),
        "a non-owner auto reset returns through owner presentation without reading owner-only peak state", failures);

    RuntimeFixture second(data, histogram);
    auto recordCopy = std::make_shared<DataRevision>(*sourceA->data);
    recordCopy->payload = sourceA->data->payload->CreateSnapshot();
    auto snapshotCopy = std::make_shared<VtkImageGridView>(*sourceA);
    snapshotCopy->data = recordCopy;
    Check(second.BeginCandidate(snapshotCopy, 102), "same-source reboxed stage becomes ready", failures);
    Check(second.CommitCandidate(snapshotCopy, 102), "same-source second view commits before release", failures);
    Check(CheckCommittedInput(second, snapshotCopy),
        "second view publishes the exact shared data revision immediately", failures);
    second.gate->Release();
    Check(second.WaitForValues(sourceA->data->self, 500.0, 1900.0, 500.0),
        "same-source reboxed view reaches the cached automatic value", failures);
    Check(histogram->GetPeakBuildCount() == peakBuilds,
        "same source revision reuses one peak estimate across views and payload reboxing", failures);

    Check(SetShiftedBimodalSource(*data), "shifted source publication", failures);
    const auto sourceB = data->GetPrimaryImage();
    Check(bool(sourceB), "shifted source snapshot exists", failures);
    RuntimeFixture secondSource(data, histogram);
    Check(secondSource.BeginCandidate(sourceB, 103), "shifted source stage becomes ready", failures);
    Check(secondSource.CommitCandidate(sourceB, 103), "shifted source stage commits while its worker is held", failures);
    const auto sourceBState = secondSource.ports.app.view->GetViewState();
    Check(CheckCommittedInput(secondSource, sourceB)
        && sourceBState.isoThreshold == 150.0
        && sourceBState.windowLevel.windowWidth == 1900.0
        && sourceBState.windowLevel.windowCenter == 150.0,
        "source B identity and synchronous midpoint commit before its scan", failures);
    secondSource.gate->Release();
    Check(secondSource.WaitForValues(sourceB->data->self, 600.0, 1900.0, 600.0),
        "new source revision recomputes the shared ISO and center", failures);
    Check(histogram->GetPeakBuildCount() == peakBuilds + 1,
        "new source revision invalidates the previous estimate exactly once", failures);

    RuntimeFixture restoredView(data, histogram);
    Check(restoredView.BeginCandidate(sourceA, 104), "retained source A can be staged again", failures);
    Check(restoredView.CommitCandidate(sourceA, 104), "retained source A stage commits before gate release", failures);
    const auto restoredA = restoredView.ports.app.view->GetViewState();
    Check(CheckCommittedInput(restoredView, sourceA)
        && restoredA.isoThreshold == 500.0
        && restoredA.windowLevel.windowCenter == 500.0,
        "restoring source A publishes its identity and consumes an already completed cached default", failures);
    restoredView.gate->Release();
    Check(restoredView.WaitForValues(sourceA->data->self, 500.0, 1900.0, 500.0),
        "returning to source A restores its automatic peak value", failures);
    Check(histogram->GetPeakBuildCount() == peakBuilds + 1,
        "restoring source A reuses its previous estimate", failures);

    DataReadyState selected;
    selected.dataRevision = sourceB->data->self;
    selected.bindingRevision = sourceB->binding->revision;
    selected.scalarRange = { -800.0, 1100.0 };
    selected.spacing = { 2.0, 3.0, 4.0 };
    first.state->SetImageDataReady(selected);
    AppViewUpdate reset;
    reset.windowLevelMode = WindowLevelMode::Auto;
    Check(first.ports.app.view->SendViewUpdate(reset)
        && first.ports.app.view->GetViewState().windowLevel.windowCenter == 150.0
        && first.ports.app.view->GetViewState().windowLevel.windowWidth == 1900.0,
        "auto reset between Data.Select and view commit uses the new range instead of the old peak center", failures);
    Check(first.window->renderCount == 0 && second.window->renderCount == 0
        && secondSource.window->renderCount == 0 && restoredView.window->renderCount == 0,
        "default calculation and view staging never call Render", failures);
}

void TestPeakFallbacks(int& failures)
{
    const auto runFallback = [&](vtkSmartPointer<vtkImageData> image,
                                 const double low, const double high,
                                 const char* label) {
        auto data = std::make_shared<TestData>();
        Check(data->SetOwnedImage(std::move(image)), label, failures);
        RuntimeFixture fixture(data, std::make_shared<HistogramConverter>());
        const auto snapshot = data->GetPrimaryImage();
        CaptureCerr diagnostic;
        const bool began = fixture.BeginCandidate(snapshot, 201);
        const bool committed = began && fixture.CommitCandidate(snapshot, 201);
        const double midpoint = low * 0.5 + high * 0.5;
        const auto committedState = fixture.ports.app.view->GetViewState();
        Check(committed && committedState.dataRevision == snapshot->data->self
            && committedState.isoThreshold == midpoint
            && committedState.windowLevel.windowWidth == std::max(0.01, high - low)
            && committedState.windowLevel.windowCenter == midpoint,
            "fallback input commits with synchronous midpoint while workers remain gated", failures);
        fixture.gate->Release();
        const bool completed = committed
            && fixture.WaitForPresentation(snapshot->data->self);
        Check(completed,
            "fallback completion wake drains before inspecting its diagnostic", failures);
        Check(CheckAutomaticValues(fixture, midpoint, std::max(0.01, high - low), midpoint),
            label, failures);
        Check(diagnostic.Text().find("自动峰值不可用，沿用范围中点") != std::string::npos,
            "peak fallback emits its diagnostic", failures);
        Check(fixture.window->renderCount == 0, "fallback path never calls Render", failures);
    };

    runFallback(MakeImage([](const int, const int, const int) { return 10.0; }),
        10.0, 10.0, "constant source falls back to its scalar-range midpoint");
    runFallback(MakeSinglePeakImage(), 0.0, 511.0,
        "unimodal source falls back to its distinct scalar-range midpoint");
}

void TestExplicitAndInFlightOverride(int& failures)
{
    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "explicit source publication", failures);
    RuntimeFixture explicitFixture(data, std::make_shared<HistogramConverter>());
    Check(explicitFixture.SetExplicitView(), "explicit view parameters configure", failures);
    const auto source = data->GetPrimaryImage();
    Check(explicitFixture.BeginCandidate(source, 301), "explicit stage starts", failures);
    Check(explicitFixture.CommitCandidate(source, 301), "explicit stage commits", failures);
    const auto explicitState = explicitFixture.ports.app.view->GetViewState();
    Check(explicitState.isoThreshold == 123.0
        && explicitState.windowLevel.windowWidth == 77.0
        && explicitState.windowLevel.windowCenter == 66.0
        && !explicitState.isTransferAuto
        && explicitState.volumeTransferFunction.colorNodes.size() == 2
        && explicitState.volumeTransferFunction.colorNodes[0].scalar == -900.0
        && explicitState.volumeTransferFunction.colorNodes[1].scalar == 1000.0
        && explicitState.volumeTransferFunction.opacityNodes.size() == 2,
        "explicit ISO, window, and transfer function are visible at commit", failures);
    explicitFixture.gate->Release();
    Check(explicitFixture.window->renderCount == 0, "explicit path never calls Render", failures);

    auto pendingData = std::make_shared<TestData>();
    Check(SetBimodalSource(*pendingData), "in-flight source publication", failures);
    RuntimeFixture pending(pendingData, std::make_shared<HistogramConverter>());
    const auto pendingSource = pendingData->GetPrimaryImage();
    Check(pending.BeginCandidate(pendingSource, 302)
        && pending.CommitCandidate(pendingSource, 302),
        "auto source stage commits while peak worker is still gated", failures);
    const auto preEdit = pending.ports.app.view->GetViewState();
    Check(preEdit.dataRevision == pendingSource->data->self
        && preEdit.isoThreshold == 50.0
        && preEdit.windowLevel.windowWidth == 1900.0
        && preEdit.windowLevel.windowCenter == 50.0,
        "synchronous midpoint presentation and source identity precede background T", failures);
    AppViewUpdate edit;
    edit.isoThreshold = 321.0;
    edit.windowLevel = WindowLevelParams{88.0, 44.0};
    edit.volumeTransferFunction = RuntimeFixture::MakeExplicitTransferFunction();
    Check(pending.ports.app.view->SendViewUpdate(edit), "manual values update during peak computation", failures);
    pending.gate->Release();
    Check(pending.WaitForPresentation(pendingSource->data->self)
        && CheckAutomaticValues(pending, 321.0, 88.0, 44.0),
        "cancelled automatic result drains after an in-flight user edit", failures);
    const auto finalState = pending.ports.app.view->GetViewState();
    Check(finalState.isoThreshold == 321.0
        && finalState.windowLevel.windowWidth == 88.0
        && finalState.windowLevel.windowCenter == 44.0
        && !finalState.isTransferAuto
        && finalState.volumeTransferFunction.colorNodes.size() == 2
        && finalState.volumeTransferFunction.colorNodes[0].scalar == -900.0,
        "late automatic result preserves user ISO/window and explicit TF", failures);
    Check(pending.window->renderCount == 0, "in-flight override path never calls Render", failures);

    RuntimeFixture readyEdit(data, std::make_shared<HistogramConverter>());
    Check(readyEdit.BeginCandidate(source, 303)
        && readyEdit.CommitCandidate(source, 303),
        "late window edit input commits before statistics", failures);
    const auto created = readyEdit.strategyCreations->load();
    AppViewUpdate windowEdit;
    windowEdit.windowLevel = WindowLevelParams{99.0, 33.0};
    const bool edited = readyEdit.ports.app.view->SendViewUpdate(windowEdit);
    readyEdit.gate->Release();
    const bool completed = edited
        && readyEdit.WaitForValues(source->data->self, 500.0, 99.0, 33.0)
        && CheckAutomaticValues(readyEdit, 500.0, 99.0, 33.0);
    const auto editedState = readyEdit.ports.app.view->GetViewState();
    Check(completed && editedState.windowLevel.windowWidth == 99.0
        && editedState.windowLevel.windowCenter == 33.0
        && readyEdit.strategyCreations->load() == created && readyEdit.window->renderCount == 0,
        "manual window edit after data commit remains authoritative without rebuilding strategy", failures);
}

void TestSuppliedAutoWindowAndSourceReset(int& failures)
{
    for (const bool fromWorker : {false, true}) {
        auto data = std::make_shared<TestData>();
        Check(SetBimodalSource(*data), "supplied Auto source publication", failures);
        RuntimeFixture fixture(data, std::make_shared<HistogramConverter>());
        const auto source = data->GetPrimaryImage();
        Check(fixture.BeginCandidate(source, 901) && fixture.CommitCandidate(source, 901),
            "supplied Auto input commits while peak worker remains held", failures);
        AppViewUpdate update;
        update.windowLevelMode = WindowLevelMode::Auto;
        update.windowLevel = WindowLevelParams{77.0, 66.0};
        bool accepted = false;
        if (fromWorker) {
            std::thread sender([&] { accepted = fixture.ports.app.view->SendViewUpdate(update); });
            sender.join();
        } else accepted = fixture.ports.app.view->SendViewUpdate(update);
        const auto before = fixture.ports.app.view->GetViewState();
        Check(accepted && before.windowLevelMode == WindowLevelMode::Auto
            && before.windowLevel.windowWidth == 77.0 && before.windowLevel.windowCenter == 66.0,
            "Auto plus a concrete window accepts both supplied values", failures);
        fixture.gate->Release();
        Check(fixture.WaitForPresentation(source->data->self),
            "supplied Auto case consumes the completed peak result", failures);
        const auto after = fixture.ports.app.view->GetViewState();
        std::cout << "Auto supplied (" << (fromWorker ? "non-owner" : "owner")
            << "): accepted=" << before.windowLevel.windowWidth << ',' << before.windowLevel.windowCenter
            << " completed=" << after.windowLevel.windowWidth << ',' << after.windowLevel.windowCenter << '\n';
        Check(after.windowLevelMode == WindowLevelMode::Auto
            && after.windowLevel.windowWidth == 77.0 && after.windowLevel.windowCenter == 66.0
            && after.isoThreshold == 500.0,
            "late peaks preserve supplied Auto width and center while automatic ISO still updates", failures);

        if (!fromWorker) {
            Check(SetShiftedBimodalSource(*data), "supplied Auto later source publication", failures);
            const auto next = data->GetPrimaryImage();
            Check(fixture.BeginCandidate(next, 902) && fixture.CommitCandidate(next, 902)
                && fixture.WaitForValues(next->data->self, 600.0, 1900.0, 600.0)
                && fixture.ports.app.view->GetViewState().windowLevelMode == WindowLevelMode::Auto,
                "normal new-source Auto reset retains c722 behavior rather than making a supplied value permanent", failures);
        } else {
            fixture.state->SetScalarRange(100.0, 177.0);
            AppViewUpdate pureAuto;
            pureAuto.windowLevelMode = WindowLevelMode::Auto;
            Check(fixture.ports.app.view->SendViewUpdate(pureAuto)
                && fixture.WaitForValues(source->data->self, 500.0, 77.0, 500.0),
                "a later pure Auto request resumes cached center defaults without replacing its chosen width", failures);
        }
        Check(fixture.window->renderCount == 0, "supplied Auto regression never calls Render", failures);
    }
}

void TestAutoWindowSnapshotRestore(int& failures)
{
    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "Auto snapshot source publication", failures);
    RuntimeFixture fixture(data, std::make_shared<HistogramConverter>());
    const auto source = data->GetPrimaryImage();
    Check(fixture.BeginCandidate(source, 903) && fixture.CommitCandidate(source, 903),
        "Auto snapshot input commits while peak worker remains held", failures);
    AppViewUpdate supplied;
    supplied.windowLevelMode = WindowLevelMode::Auto;
    supplied.windowLevel = WindowLevelParams{91.0, 73.0};
    Check(fixture.ports.app.view->SendViewUpdate(supplied), "saved Auto values are accepted", failures);
    const auto saved = fixture.ports.app.view->GetViewState();
    AppViewUpdate pureAuto;
    pureAuto.windowLevelMode = WindowLevelMode::Auto;
    Check(fixture.ports.app.view->SendViewUpdate(pureAuto), "pure Auto changes the values before restoration", failures);
    const auto changed = fixture.ports.app.view->GetViewState();
    Check(fixture.ports.app.view->SetViewState(saved, changed.revision),
        "actual saved Auto snapshot restores through SetViewState", failures);
    const auto restored = fixture.ports.app.view->GetViewState();
    Check(restored.windowLevelMode == WindowLevelMode::Auto
        && restored.windowLevel.windowWidth == 91.0 && restored.windowLevel.windowCenter == 73.0,
        "Auto snapshot values are visible before releasing the worker", failures);
    fixture.gate->Release();
    Check(fixture.WaitForPresentation(source->data->self), "Auto snapshot drains the peak completion", failures);
    const auto after = fixture.ports.app.view->GetViewState();
    std::cout << "Auto snapshot: restored=" << restored.windowLevel.windowWidth << ','
        << restored.windowLevel.windowCenter << " completed=" << after.windowLevel.windowWidth << ','
        << after.windowLevel.windowCenter << '\n';
    Check(after.windowLevelMode == WindowLevelMode::Auto
        && after.windowLevel.windowWidth == 91.0 && after.windowLevel.windowCenter == 73.0
        && after.dataRevision == saved.dataRevision,
        "late peak completion preserves both window values restored from an Auto snapshot", failures);
    Check(fixture.window->renderCount == 0, "Auto snapshot regression never calls Render", failures);
}

void TestPureAutoPreservesWindowWidth(int& failures)
{
    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "pure Auto width source publication", failures);
    RuntimeFixture fixture(data, std::make_shared<HistogramConverter>());
    const auto source = data->GetPrimaryImage();
    Check(fixture.BeginCandidate(source, 904) && fixture.CommitCandidate(source, 904),
        "pure Auto width input commits while peak worker remains held", failures);
    // c722 的无参 Auto 从共享范围选择窗宽；迟到选峰不得改回 payload 的全范围窗宽。
    fixture.state->SetScalarRange(100.0, 177.0);
    AppViewUpdate pureAuto;
    pureAuto.windowLevelMode = WindowLevelMode::Auto;
    Check(fixture.ports.app.view->SendViewUpdate(pureAuto), "pure Auto without concrete values is accepted", failures);
    const auto before = fixture.ports.app.view->GetViewState();
    Check(before.windowLevelMode == WindowLevelMode::Auto
        && before.windowLevel.windowWidth == 77.0 && before.windowLevel.windowCenter == 138.5,
        "pure Auto uses the unchanged c722 range policy before peak completion", failures);
    fixture.gate->Release();
    Check(fixture.WaitForPresentation(source->data->self), "pure Auto width drains the peak completion", failures);
    const auto after = fixture.ports.app.view->GetViewState();
    std::cout << "Pure Auto: accepted=" << before.windowLevel.windowWidth << ',' << before.windowLevel.windowCenter
        << " completed=" << after.windowLevel.windowWidth << ',' << after.windowLevel.windowCenter << '\n';
    Check(after.windowLevelMode == WindowLevelMode::Auto
        && after.windowLevel.windowWidth == 77.0 && after.windowLevel.windowCenter == 500.0,
        "late pure Auto peak changes only the center and retains the accepted width", failures);
    Check(fixture.window->renderCount == 0, "pure Auto width regression never calls Render", failures);
}

void TestRejectedAutoWindowPreservesIntent(int& failures)
{
    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "rejected Auto source publication", failures);
    RuntimeFixture fixture(data, std::make_shared<HistogramConverter>());
    const auto source = data->GetPrimaryImage();
    Check(fixture.BeginCandidate(source, 905) && fixture.CommitCandidate(source, 905),
        "rejected Auto input commits while peak worker remains held", failures);
    const auto before = fixture.ports.app.view->GetViewState();
    AppViewUpdate rejected;
    rejected.windowLevelMode = WindowLevelMode::Auto;
    rejected.windowLevel = WindowLevelParams{77.0, 66.0};
    VolumeTransferFunction outsideRange;
    outsideRange.colorNodes = {{2000.0, 0.0, 0.0, 0.0}, {3000.0, 1.0, 1.0, 1.0}};
    outsideRange.opacityNodes = {{2000.0, 0.0}, {3000.0, 1.0}};
    rejected.volumeTransferFunction = outsideRange;
    Check(!fixture.ports.app.view->SendViewUpdate(rejected),
        "compound update rejects a transfer function that does not overlap the data range", failures);
    const auto rolledBack = fixture.ports.app.view->GetViewState();
    Check(rolledBack.windowLevelMode == before.windowLevelMode
        && rolledBack.windowLevel.windowWidth == before.windowLevel.windowWidth
        && rolledBack.windowLevel.windowCenter == before.windowLevel.windowCenter,
        "rejected compound update restores the previous window values", failures);
    fixture.gate->Release();
    Check(fixture.WaitForPresentation(source->data->self), "rejected Auto case drains peak completion", failures);
    const auto after = fixture.ports.app.view->GetViewState();
    std::cout << "Rejected Auto: completed=" << after.windowLevel.windowWidth << ','
        << after.windowLevel.windowCenter << '\n';
    Check(after.windowLevelMode == WindowLevelMode::Auto
        && after.windowLevel.windowWidth == 1900.0 && after.windowLevel.windowCenter == 500.0,
        "rejected supplied values do not replace the previous pure Auto intent", failures);
    Check(fixture.window->renderCount == 0, "rejected Auto regression never calls Render", failures);
}

void TestCancellationAndStop(int& failures)
{
    auto changingData = std::make_shared<TestData>();
    Check(SetBimodalSource(*changingData), "switching source A publication", failures);
    RuntimeFixture changing(changingData, std::make_shared<HistogramConverter>());
    const auto sourceA = changingData->GetPrimaryImage();
    Check(changing.BeginCandidate(sourceA, 390)
        && changing.CommitCandidate(sourceA, 390),
        "source A data transaction commits while its estimate is held", failures);
    const auto visibleA = changing.ports.app.view->GetViewState();
    Check(CheckCommittedInput(changing, sourceA)
        && visibleA.isoThreshold == 50.0 && visibleA.windowLevel.windowCenter == 50.0,
        "source A identity and midpoint are visible before releasing its estimate", failures);
    Check(SetShiftedBimodalSource(*changingData),
        "source B replaces A before its estimate completes", failures);
    const auto sourceB = changingData->GetPrimaryImage();
    Check(changing.BeginCandidate(sourceB, 391)
        && changing.CommitCandidate(sourceB, 391),
        "source B data transaction commits while both estimates are still gated", failures);
    const auto visibleB = changing.ports.app.view->GetViewState();
    Check(CheckCommittedInput(changing, sourceB)
        && visibleB.dataRevision == sourceB->data->self
        && visibleB.isoThreshold == 150.0
        && visibleB.windowLevel.windowWidth == 1900.0
        && visibleB.windowLevel.windowCenter == 150.0,
        "source B identity and synchronous midpoint replace A before T is available", failures);
    changing.gate->Release();
    Check(changing.WaitForValues(sourceB->data->self, 600.0, 1900.0, 600.0),
        "the committed source B result is applied independently of the cancelled A task", failures);
    Check(changing.ports.taskControl->StopTasks(Clock::now() + std::chrono::seconds(10)),
        "all old-source workers exit before the stale-result assertion", failures);
    (void)changing.ports.interaction.update->SendUpdates();
    Check(CheckAutomaticValues(changing, 600.0, 1900.0, 600.0)
        && changing.ports.app.view->GetViewState().dataRevision == sourceB->data->self,
        "late A result cannot overwrite the committed B ISO or window center", failures);
    Check(changing.window->renderCount == 0, "source replacement never calls Render", failures);

    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "cancel source publication", failures);
    RuntimeFixture fixture(data, std::make_shared<HistogramConverter>());
    const auto original = fixture.ports.app.view->GetViewState();
    const auto snapshot = data->GetPrimaryImage();
    Check(fixture.BeginCandidate(snapshot, 401), "uncommitted candidate becomes ready", failures);
    Check(fixture.ports.app.view->GetViewState().dataRevision != snapshot->data->self,
        "ready candidate has not changed visible input identity", failures);
    Check(fixture.ports.dataStage->ClearDataStage(401), "pending data stage cancellation succeeds", failures);
    fixture.gate->Release();
    for (int tick = 0; tick < 4; ++tick) (void)fixture.ports.interaction.update->SendUpdates();
    const auto afterCancel = fixture.ports.app.view->GetViewState();
    Check(afterCancel.dataRevision == original.dataRevision
        && afterCancel.isoThreshold == original.isoThreshold
        && afterCancel.windowLevel.windowCenter == original.windowLevel.windowCenter,
        "cancelling an uncommitted data stage publishes no automatic presentation candidate", failures);
    Check(fixture.histogram->GetPeakBuildCount() == 0,
        "uncommitted candidate never starts histogram work", failures);
    Check(fixture.window->renderCount == 0, "cancel path never calls Render", failures);
    Check(SetShiftedBimodalSource(*data), "retry source publication after cancellation", failures);
    const auto retrySource = data->GetPrimaryImage();
    Check(fixture.BeginCandidate(retrySource, 403)
        && fixture.CommitCandidate(retrySource, 403),
        "new source retries and commits before the worker gate opens", failures);
    Check(CheckCommittedInput(fixture, retrySource)
        && fixture.ports.app.view->GetViewState().isoThreshold == 150.0,
        "retry input identity and midpoint are immediately visible", failures);
    fixture.gate->Release();
    Check(fixture.WaitForValues(retrySource->data->self, 600.0, 1900.0, 600.0),
        "retry publishes defaults for the new source instead of stale cancelled output", failures);

    auto stopData = std::make_shared<TestData>();
    Check(SetBimodalSource(*stopData), "stop source publication", failures);
    RuntimeFixture stopping(stopData, std::make_shared<HistogramConverter>());
    const auto stopSnapshot = stopData->GetPrimaryImage();
    Check(stopping.BeginCandidate(stopSnapshot, 402)
        && stopping.CommitCandidate(stopSnapshot, 402),
        "stop source formally commits before stopping its automatic estimate", failures);
    const auto beforeStop = stopping.ports.app.view->GetViewState();
    Check(CheckCommittedInput(stopping, stopSnapshot)
        && beforeStop.isoThreshold == 50.0
        && beforeStop.windowLevel.windowCenter == 50.0,
        "Stop begins with the formally visible input and synchronous midpoint", failures);
    const bool stoppingAccepted = stopping.ports.taskControl
        && stopping.ports.taskControl->SetTaskStopping();
    stopping.gate->Release();
    const bool stopped = stopping.ports.taskControl
        && stopping.ports.taskControl->StopTasks(Clock::now() + std::chrono::seconds(10));
    Check(stoppingAccepted && stopped, "StopTasks joins held workers before its absolute deadline", failures);
    (void)stopping.ports.interaction.update->SendUpdates();
    const auto afterStop = stopping.ports.app.view->GetViewState();
    Check(afterStop.dataRevision == stopSnapshot->data->self
        && afterStop.isoThreshold == beforeStop.isoThreshold
        && afterStop.windowLevel.windowCenter == beforeStop.windowLevel.windowCenter,
        "stopped worker cannot write back pending automatic values", failures);
    Check(stopping.window->renderCount == 0, "stop path never calls Render", failures);
}

void TestPreparationOwnership(int& failures)
{
    auto data = std::make_shared<TestData>();
    Check(SetBimodalSource(*data), "automatic refresh source publication", failures);
    auto histogram = std::make_shared<HistogramConverter>();
    RuntimeFixture automatic(data, histogram);
    const auto source = data->GetPrimaryImage();
    AppViewUpdate refresh;
    refresh.mode = VizMode::Volume;
    Check(automatic.ports.app.view->SendViewUpdate(refresh), "refresh trigger switches away from the initial mode", failures);
    refresh.mode = VizMode::IsoSurface;
    Check(automatic.ports.app.view->SendViewUpdate(refresh), "refresh trigger restores the requested ISO mode", failures);
    for (int tick = 0; tick < 4; ++tick) (void)automatic.ports.interaction.update->SendUpdates();
    const auto heldState = automatic.ports.app.view->GetViewState();
    const auto buildsBeforeRelease = histogram->GetPeakBuildCount();
    const auto strategiesBeforeRelease = automatic.strategyCreations->load();
    Check(heldState.dataRevision == source->data->self
        && heldState.isoThreshold == 50.0
        && heldState.windowLevel.windowWidth == 1900.0
        && heldState.windowLevel.windowCenter == 50.0
        && !heldState.isPresentationPending,
        "mode reset commits the source and exposes its midpoint while its worker is held", failures);
    for (int tick = 0; tick < 8; ++tick) (void)automatic.ports.interaction.update->SendUpdates();
    Check(histogram->GetPeakBuildCount() == buildsBeforeRelease,
        "repeated owner ticks do not scan the same pending source", failures);
    automatic.gate->Release();
    const bool presented = automatic.WaitForValues(source->data->self, 500.0, 1900.0, 500.0);
    const auto completedBuilds = histogram->GetPeakBuildCount();
    for (int tick = 0; tick < 8; ++tick) (void)automatic.ports.interaction.update->SendUpdates();
    Check(presented && completedBuilds == buildsBeforeRelease
        && histogram->GetPeakBuildCount() == completedBuilds
        && automatic.strategyCreations->load() == strategiesBeforeRelease
        && CheckAutomaticValues(automatic, 500.0, 1900.0, 500.0),
        "completed same-source reset is not rescanned or rebuilt by later owner ticks", failures);

    auto externalData = std::make_shared<TestData>();
    Check(SetBimodalSource(*externalData), "external stage source publication", failures);
    RuntimeFixture external(externalData, std::make_shared<HistogramConverter>());
    const auto externalSource = externalData->GetPrimaryImage();
    Check(external.BeginCandidate(externalSource, 701), "external candidate reaches ready without stats", failures);
    AppViewUpdate background;
    background.background = BackgroundColor{0.1, 0.2, 0.3};
    Check(external.ports.app.view->SendViewUpdate(background), "external ready stage receives an ordinary visual update", failures);
    for (int tick = 0; tick < 4; ++tick) (void)external.ports.interaction.update->SendUpdates();
    const bool kept = external.ports.dataStage->GetDataStageStatus(701) == DataStageStatus::Ready
        && external.ports.app.view->GetViewState().dataRevision != externalSource->data->self;
    external.gate->Release();
    Check(kept && external.CommitCandidate(externalSource, 701)
        && external.WaitForValues(externalSource->data->self, 500.0, 1900.0, 500.0),
        "ordinary updates cannot take ownership of an external ready data-stage transaction", failures);
    Check(automatic.window->renderCount == 0 && external.window->renderCount == 0,
        "ownership regression never calls Render", failures);
}
} // namespace

int GetAutoDisplayDefaultsFailCount()
{
    int failures = 0;
    TestBimodalDefaultAndCache(failures);
    TestPeakFallbacks(failures);
    TestExplicitAndInFlightOverride(failures);
    TestSuppliedAutoWindowAndSourceReset(failures);
    TestAutoWindowSnapshotRestore(failures);
    TestPureAutoPreservesWindowWidth(failures);
    TestRejectedAutoWindowPreservesIntent(failures);
    TestCancellationAndStop(failures);
    TestPreparationOwnership(failures);
    return failures;
}
