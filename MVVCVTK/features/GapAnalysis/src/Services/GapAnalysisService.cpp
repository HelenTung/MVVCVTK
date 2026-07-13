#include "Services/GapAnalysisService.h"

#include "Algorithms/VolumeBuffer.h"
#include "Algorithms/VoidDetector.h"
#include "AppInterfaces.h"
#include "Render/Strategies/GapOverlayStrategies.h"

#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

class GapAnalysisService::Impl final {
public:
    Impl() = default;
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    bool SetInputImage(vtkSmartPointer<vtkImageData> image);
    bool SetInputSnapshot(vtkSmartPointer<vtkImageData> image);
    void SetSurface(const SurfaceParams& params);
    void SetAdvanced(const AdvancedSurfaceParams& params);
    void SetVoid(const VoidDetectionParams& params);
    bool StartAsync(std::function<void(bool isSuccess)> onComplete);
    void StopAsync();
    bool GetDoneEvent();
    void SendCallback();
    GapAnalysisState GetAnalysisState() const;
    std::vector<VoidRegion> GetVoidRegions() const;
    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const;

    bool StartView(
        const GapAnalysisSurfaceRequest& surfaceRequest,
        const VoidDetectionParams& voidParams,
        const std::vector<std::shared_ptr<OverlayService>>& meshOverlayTargets,
        const std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>>& sliceOverlayTargets,
        std::function<void(double isoValue)> onIsoValueResolved);
    bool SwitchOverlay();
    bool ExitView();
    bool GetViewOn() const;
    void OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage);

private:
    using VolumeBufferSnapshot = std::shared_ptr<const VolumeBuffer>;

    enum class GapViewPhase {
        Idle,
        AwaitingInput,
        AwaitingResult,
        Consumed
    };

    struct GapParamSnapshot {
        // StartAsync 从 m_paramsMutex 下复制；worker 当前只消费 isoValue。
        SurfaceParams surfParams;
        // 随任务按值冻结，避免后续 setter 改写；当前 worker 尚未接入法向精化，不消费这些字段。
        AdvancedSurfaceParams advParams;
        // 随任务按值冻结；worker 消费 grayMax、erosionIterations 与 minVolumeMM3。
        VoidDetectionParams voidParams;
    };

    struct GapOverlayBinding {
        // 已挂载 overlay 的宿主 service 共享 owner；SetOverlayOff 用它执行 Remove 后清空 binding。
        std::shared_ptr<OverlayService> service;
        // 与 service 中同一策略实例的共享 owner，确保 RemoveOverlayStrategy 前对象仍有效。
        std::shared_ptr<AbstractVisualStrategy> overlayStrategy;
    };

    void SetCompletionCallback(std::function<void(bool)> callback);
    void SetCallbackReady(bool isSuccess);
    bool GetMeshVisible(vtkSmartPointer<vtkPolyData> voidMesh) const;
    bool GetLabelExtent(vtkSmartPointer<vtkImageData> labelImage) const;
    bool BuildVolumeBuffer(
        vtkSmartPointer<vtkImageData> image,
        VolumeBuffer& out) const;
    vtkSmartPointer<vtkImageData> BuildLabelImage(
        const std::vector<int>& labelVolume,
        const VolumeBuffer& volBuf) const;

    VolumeBufferSnapshot GetInputSnapshot() const;
    GapParamSnapshot GetParamSnapshot() const;
    void StartWorker(
        VolumeBufferSnapshot inputSnapshot,
        GapParamSnapshot params);
    void StopWorker();
    void SetAnalysisState(GapAnalysisState state);

    bool StartRun(vtkSmartPointer<vtkImageData> inputImage);
    void SetDisplayView();
    bool SetOverlayOff();
    bool SetStoredView();
    bool ExitViewState();
    void ClearDisplayState();
    bool SetViewThread();
    bool GetViewThread() const;
    bool ClearViewThread();
    double GetDisplayIso(const VolumeBuffer& inputSnapshot) const;

    // callbackMutex 同时保护 active/pending callback 与 pending success payload。
    std::mutex m_callbackMutex;
    // 当前任务尚未完成的 callback；StartAsync 替换，SetCallbackReady 完成时移出。
    std::function<void(bool)> m_completionCallback;
    // 已完成 callback 的 pending 槽；存在未消费回调时 StartAsync 拒绝新任务，避免结果被覆盖。
    std::function<void(bool)> m_nextCallback;
    // 与 m_nextCallback 同一锁事务发布的单任务结果快照。
    bool m_isNextOk = false;
    // pending callback 门铃；SetCallbackReady 最后置位，GetDoneEvent 用 exchange 保证一次消费。
    std::atomic<bool> m_hasCallback{ false };

    // inputMutex 保护当前不可变体素快照 shared owner；worker 按值领取 owner 后不再访问该槽。
    mutable std::mutex m_inputMutex;
    // 两种输入入口最终都提交只读体素 owner；float 共享其受控 VTK owner，转换输入独占 vector。
    VolumeBufferSnapshot m_inputSnapshot;

    // paramsMutex 把三个可变参数对象作为同一份任务配置复制边界。
    mutable std::mutex m_paramsMutex;
    // 输入标量域表面参数；GetParamSnapshot 按值冻结，worker 当前消费其中 isoValue。
    SurfaceParams m_surfParams;
    // 法向精化参数的预留值快照；当前 worker 不消费，不能描述为已生效配置。
    AdvancedSurfaceParams m_advParams;
    // 空隙候选与保留参数；GetParamSnapshot 按值冻结后由当前 worker 消费已接入字段。
    VoidDetectionParams m_voidParams;

    // resultMutex 保护完整结果 payload；读取入口只在锁内取得值或 VTK owner，复制/构建均在锁外。
    mutable std::mutex m_resultMutex;
    // 最近一次 worker 提交的结果真源；新任务启动前清空，mesh/label 显示缓存均从它派生。
    GapAnalysisResult m_result;

    // workerMutex 只串行化 std::thread 槽的 join/替换，不保护算法 payload。
    mutable std::mutex m_workerMutex;
    // Impl 唯一拥有的 worker 线程；复用或析构前由 owner 接管点 join，禁止越过 Impl 生命周期。
    std::thread m_workerThread;
    // 取消请求轴：StopAsync 置位，下一次被接受的 StartAsync 清零；worker 仅在阶段边界观察它。
    std::atomic<bool> m_isStopping{ false };
    // 分析执行轴：入口发布 Idle/Running/前置失败，worker 发布终态；它不表达 view/overlay 是否开启。
    std::atomic<int> m_analysisState{ static_cast<int>(GapAnalysisState::Idle) };

    // 显示会话只允许绑定的宿主线程访问；线程 id 由独立 mutex 保护，VTK/overlay 调用不持该锁。
    mutable std::mutex m_viewThreadMutex;
    std::thread::id m_viewThreadId;
    // StartView 过滤并保留 3D target 的 shared owner，ClearDisplayState 统一释放。
    std::vector<std::shared_ptr<OverlayService>> m_meshTargets;
    // 2D target 的 [orientation, service] shared owner；orientation 在创建 slice overlay 时固化。
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> m_sliceTargets;
    // 当前已实际 Attach 的 service/strategy 对；SetOverlayOff 逐项 Remove 后清空。
    std::vector<GapOverlayBinding> m_displayOverlayBindings;
    // 成功结果派生的 3D void mesh 强引用缓存；隐藏 overlay 时保留，退出或新会话时清空。
    vtkSmartPointer<vtkPolyData> m_displayVoidMesh;
    // 成功结果派生的 2D label image 强引用缓存；沿用输入快照的 dimensions/spacing/origin，生命周期和 mesh 缓存一致。
    vtkSmartPointer<vtkImageData> m_displayLabelImage;
    // StartView 保存的 ISO 来源配方；StartRun 拿到输入 min/max 后解析，结束会话时清空。
    GapAnalysisSurfaceRequest m_displaySurfaceRequest;
    // StartView 保存的 void 参数值副本；StartRun 写入 worker 参数槽，结束会话时清空。
    VoidDetectionParams m_displayVoidParams;
    // 可选 ISO 回调值副本；StartRun 在宿主 display tick 解析后同步调用，ClearDisplayState 释放。
    std::function<void(double isoValue)> m_isoCallback;
    // 显示会话阶段只表达输入等待、结果等待与终态消费，不混入 worker 或 overlay 显隐状态。
    std::atomic<GapViewPhase> m_viewPhase{ GapViewPhase::Idle };
    // overlay 可见意图轴：用户可独立切换；关闭只卸载显示，不取消分析或丢弃缓存结果。
    bool m_isOverlayOn = false;
};

void GapAnalysisService::Impl::SetCompletionCallback(std::function<void(bool)> callback)
{
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_completionCallback = std::move(callback);
}

void GapAnalysisService::Impl::SetCallbackReady(bool isSuccess)
{
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    if (!m_completionCallback) {
        return;
    }

    // 完成路径把任务回调与 isSuccess 移入 pending 槽；门铃最后置位，供宿主线程领取。
    m_nextCallback = std::move(m_completionCallback);
    m_completionCallback = nullptr;
    m_isNextOk = isSuccess;
    m_hasCallback.store(true);
}

bool GapAnalysisService::Impl::GetDoneEvent()
{
    // exchange 保证同一 pending callback 只向轮询方报告一次；payload 仍由 SendCallback 在锁内取走。
    return m_hasCallback.exchange(false);
}

void GapAnalysisService::Impl::SendCallback()
{
    std::function<void(bool)> callback;
    bool isSuccess = false;
    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        callback = std::move(m_nextCallback);
        m_nextCallback = nullptr;
        isSuccess = m_isNextOk;
    }

    // 回调可能触发 UI、VTK 或服务调用；锁外执行避免插件内部状态和宿主回调互相重入死锁。
    if (callback) {
        callback(isSuccess);
    }
}

bool GapAnalysisService::Impl::GetMeshVisible(vtkSmartPointer<vtkPolyData> voidMesh) const
{
    return voidMesh
        && voidMesh->GetNumberOfPoints() > 0
        && voidMesh->GetNumberOfCells() > 0;
}

bool GapAnalysisService::Impl::GetLabelExtent(vtkSmartPointer<vtkImageData> labelImage) const
{
    if (!labelImage) {
        return false;
    }

    int labelDims[3] = { 0, 0, 0 };
    labelImage->GetDimensions(labelDims);
    return labelDims[0] > 0 && labelDims[1] > 0 && labelDims[2] > 0;
}

GapAnalysisService::Impl::~Impl()
{
    if (GetViewOn()) {
        if (GetViewThread()) {
            ExitViewState();
        }
        else {
            std::cerr << "[GapAnalysis] Active view must exit on its bound host thread before destruction."
                << std::endl;
            assert(false && "GapAnalysisService destroyed before owner-thread ExitView");
        }
    }
    StopAsync();
    StopWorker();
}

GapAnalysisService::GapAnalysisService()
    : m_impl(std::make_unique<Impl>())
{
}

GapAnalysisService::~GapAnalysisService() = default;

bool GapAnalysisService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    return m_impl->SetInputImage(std::move(image));
}

bool GapAnalysisService::SetInputSnapshot(vtkSmartPointer<vtkImageData> image)
{
    return m_impl->SetInputSnapshot(std::move(image));
}

void GapAnalysisService::SetSurface(const SurfaceParams& params)
{
    m_impl->SetSurface(params);
}

void GapAnalysisService::SetAdvanced(const AdvancedSurfaceParams& params)
{
    m_impl->SetAdvanced(params);
}

void GapAnalysisService::SetVoid(const VoidDetectionParams& params)
{
    m_impl->SetVoid(params);
}

bool GapAnalysisService::StartAsync(std::function<void(bool isSuccess)> onComplete)
{
    return m_impl->StartAsync(std::move(onComplete));
}

void GapAnalysisService::StopAsync()
{
    m_impl->StopAsync();
}

bool GapAnalysisService::GetDoneEvent()
{
    return m_impl->GetDoneEvent();
}

void GapAnalysisService::SendCallback()
{
    m_impl->SendCallback();
}

GapAnalysisState GapAnalysisService::GetAnalysisState() const
{
    return m_impl->GetAnalysisState();
}

std::vector<VoidRegion> GapAnalysisService::GetVoidRegions() const
{
    return m_impl->GetVoidRegions();
}

vtkSmartPointer<vtkPolyData> GapAnalysisService::BuildVoidMesh() const
{
    return m_impl->BuildVoidMesh();
}

vtkSmartPointer<vtkImageData> GapAnalysisService::BuildLabelImage() const
{
    return m_impl->BuildLabelImage();
}

bool GapAnalysisService::StartView(
    const GapAnalysisSurfaceRequest& surfaceRequest,
    const VoidDetectionParams& voidParams,
    const std::vector<std::shared_ptr<OverlayService>>& meshOverlayTargets,
    const std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>>& sliceOverlayTargets,
    std::function<void(double isoValue)> onIsoValueResolved)
{
    return m_impl->StartView(
        surfaceRequest,
        voidParams,
        meshOverlayTargets,
        sliceOverlayTargets,
        std::move(onIsoValueResolved));
}

bool GapAnalysisService::SwitchOverlay()
{
    return m_impl->SwitchOverlay();
}

bool GapAnalysisService::ExitView()
{
    return m_impl->ExitView();
}

bool GapAnalysisService::GetViewOn() const
{
    return m_impl->GetViewOn();
}

void GapAnalysisService::OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage)
{
    m_impl->OnDisplayTick(std::move(inputImage));
}

bool GapAnalysisService::Impl::SetInputImage(vtkSmartPointer<vtkImageData> image) {
    if (!image) {
        return SetInputSnapshot(nullptr);
    }

    // 公共兼容入口不能借用外部可变 VTK 对象；DeepCopy 完成后再进入只读快照链。
    try {
        auto imageCopy = vtkSmartPointer<vtkImageData>::New();
        imageCopy->DeepCopy(image);
        return SetInputSnapshot(std::move(imageCopy));
    }
    catch (const std::bad_alloc&) {
        return SetInputSnapshot(nullptr);
    }
}

bool GapAnalysisService::Impl::SetInputSnapshot(vtkSmartPointer<vtkImageData> image) {
    VolumeBuffer snapshot;
    if (!BuildVolumeBuffer(std::move(image), snapshot)) {
        VolumeBufferSnapshot retiredSnapshot;
        {
            std::lock_guard<std::mutex> workerLock(m_workerMutex);
            {
                std::lock_guard<std::mutex> inputLock(m_inputMutex);
                retiredSnapshot = std::move(m_inputSnapshot);
            }
            if (GetAnalysisState() != GapAnalysisState::Running) {
                SetAnalysisState(GapAnalysisState::Failed);
            }
        }
        return false;
    }

    VolumeBufferSnapshot snapshotPtr;
    try {
        snapshotPtr = std::make_shared<VolumeBuffer>(std::move(snapshot));
    }
    catch (const std::bad_alloc&) {
        VolumeBufferSnapshot retiredSnapshot;
        {
            std::lock_guard<std::mutex> workerLock(m_workerMutex);
            {
                std::lock_guard<std::mutex> inputLock(m_inputMutex);
                retiredSnapshot = std::move(m_inputSnapshot);
            }
            if (GetAnalysisState() != GapAnalysisState::Running) {
                SetAnalysisState(GapAnalysisState::Failed);
            }
        }
        return false;
    }

    // 输入、旧结果和执行状态作为一次提交与 StartAsync 串行，不能覆盖刚发布的 Running。
    VolumeBufferSnapshot retiredSnapshot;
    GapAnalysisResult retiredResult;
    {
        std::lock_guard<std::mutex> workerLock(m_workerMutex);
        {
            std::lock_guard<std::mutex> inputLock(m_inputMutex);
            retiredSnapshot = std::move(m_inputSnapshot);
            m_inputSnapshot = std::move(snapshotPtr);
        }
        if (GetAnalysisState() != GapAnalysisState::Running) {
            std::lock_guard<std::mutex> resultLock(m_resultMutex);
            retiredResult = std::move(m_result);
            m_result = {};
            SetAnalysisState(GapAnalysisState::Idle);
        }
    }
    return true;
}

void GapAnalysisService::Impl::SetSurface(const SurfaceParams& params) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_surfParams = params;
}

void GapAnalysisService::Impl::SetAdvanced(const AdvancedSurfaceParams& params) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_advParams = params;
}

void GapAnalysisService::Impl::SetVoid(const VoidDetectionParams& params) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_voidParams = params;
}

bool GapAnalysisService::Impl::StartAsync(std::function<void(bool isSuccess)> onComplete) {
    // 1. workerMutex 串行化线程对象接管；Running 请求不替换当前任务或 callback。
    std::lock_guard<std::mutex> workerLock(m_workerMutex);
    if (GetAnalysisState() == GapAnalysisState::Running) {
        return false;
    }

    // pending callback 尚未由宿主消费时拒绝新任务，保证单槽结果不会被后一任务覆盖。
    {
        std::lock_guard<std::mutex> callbackLock(m_callbackMutex);
        if (m_completionCallback || m_nextCallback) {
            return false;
        }
    }

    // 2. 上一轮已结束线程在复用 std::thread 前必须 join；此处不会与新 worker 并行。
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    // 回调先进入任务槽；输入校验失败时，若存在回调也按同一 pending 通道发布 false。
    SetCompletionCallback(std::move(onComplete));

    // 输入已由隔离或受控入口固化为 VolumeBuffer；worker 只持有不可变快照 owner。
    auto inputSnapshot = GetInputSnapshot();
    if (!inputSnapshot || !inputSnapshot->GetVoxelReady()) {
        SetAnalysisState(GapAnalysisState::Failed);
        SetCallbackReady(false);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_result = {};
    }

    // 3. 结果清场完成后再发布新执行状态；清除的只是上一轮取消请求，不改变显示状态轴。
    m_isStopping.store(false);
    SetAnalysisState(GapAnalysisState::Running);

    // 4. worker 只捕获不可变输入快照和参数值副本；共享交互仅为读取取消请求并提交结果、状态和 callback 门铃。
    m_workerThread = std::thread(
        &GapAnalysisService::Impl::StartWorker,
        this,
        std::move(inputSnapshot),
        GetParamSnapshot());
    return true;
}

void GapAnalysisService::Impl::StopAsync() {
    // 这里只发布协作式取消；worker 在算法阶段之间观察，调用方不能把返回视为线程已退出。
    m_isStopping.store(true);
}

GapAnalysisState GapAnalysisService::Impl::GetAnalysisState() const {
    return static_cast<GapAnalysisState>(m_analysisState.load());
}

std::vector<VoidRegion> GapAnalysisService::Impl::GetVoidRegions() const {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    return m_result.voids;
}

vtkSmartPointer<vtkPolyData> GapAnalysisService::Impl::BuildVoidMesh() const {
    vtkSmartPointer<vtkImageData> labelImage;
    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        if (!m_result.isSucceeded || !m_result.labelImage) {
            return nullptr;
        }
        labelImage = m_result.labelImage;
    }

    auto fe = vtkSmartPointer<vtkFlyingEdges3D>::New();
    fe->SetInputData(labelImage);
    fe->SetValue(0, 0.5); // label > 0 即为空洞区域
    fe->ComputeNormalsOff();
    fe->Update();
    return fe->GetOutput();
}

vtkSmartPointer<vtkImageData> GapAnalysisService::Impl::BuildLabelImage() const {
    vtkSmartPointer<vtkImageData> labelImage;
    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        labelImage = m_result.labelImage;
    }
    if (!labelImage) {
        return nullptr;
    }

    auto imageCopy = vtkSmartPointer<vtkImageData>::New();
    imageCopy->DeepCopy(labelImage);
    return imageCopy;
}

bool GapAnalysisService::Impl::StartView(
    const GapAnalysisSurfaceRequest& surfaceRequest,
    const VoidDetectionParams& voidParams,
    const std::vector<std::shared_ptr<OverlayService>>& meshOverlayTargets,
    const std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>>& sliceOverlayTargets,
    std::function<void(double isoValue)> onIsoValueResolved) {
    if (!SetViewThread()) {
        std::cerr << "[GapAnalysis] Display activation rejected: view thread mismatch." << std::endl;
        return false;
    }
    // 新显示请求先卸载旧 overlay；正在运行的 worker 只收到取消请求，线程所有权仍由 StartAsync/析构收口。
    SetOverlayOff();
    if (GetAnalysisState() == GapAnalysisState::Running) {
        StopAsync();
    }

    m_meshTargets.clear();
    m_meshTargets.reserve(meshOverlayTargets.size());
    for (const auto& target : meshOverlayTargets) {
        if (target) {
            m_meshTargets.push_back(target);
        }
    }

    m_sliceTargets.clear();
    m_sliceTargets.reserve(sliceOverlayTargets.size());
    for (const auto& target : sliceOverlayTargets) {
        if (target.second) {
            m_sliceTargets.push_back(target);
        }
    }

    if (m_meshTargets.empty() && m_sliceTargets.empty()) {
        std::cerr << "[GapAnalysis] Display activation skipped: no overlay target was provided." << std::endl;
        ClearDisplayState();
        ClearViewThread();
        return false;
    }

    // 以下字段发布一份新的显示会话：参数/target 已就绪，但输入快照要等主线程 tick 提供。
    m_displayVoidMesh = nullptr;
    m_displayLabelImage = nullptr;
    m_displaySurfaceRequest = surfaceRequest;
    m_displayVoidParams = voidParams;
    m_isoCallback = std::move(onIsoValueResolved);
    m_viewPhase.store(GapViewPhase::AwaitingInput);
    m_isOverlayOn = true;
    std::cout << "[GapAnalysis] Display mode requested. Analysis will start after volume data is ready." << std::endl;
    return true;
}

bool GapAnalysisService::Impl::SwitchOverlay() {
    if (!GetViewThread()) {
        return false;
    }
    if (m_viewPhase.load() == GapViewPhase::Idle) {
        std::cerr << "[GapAnalysis] Overlay switch ignored: display mode is not active." << std::endl;
        return false;
    }

    // 可见意图独立于 worker 和显示完成状态；隐藏时保留 mesh/label 缓存，重新开启可直接挂载。
    m_isOverlayOn = !m_isOverlayOn;
    // A. 切到隐藏时只卸载 binding，不清除已经完成的显示缓存。
    if (!m_isOverlayOn) {
        SetOverlayOff();
        std::cout << "[GapAnalysis] Overlays hidden. Use the host overlay switch command to show them again." << std::endl;
        return true;
    }

    // B. 切到显示时，结果未完成就等待 tick；已有缓存则立即重新挂载。
    if (!m_displayVoidMesh && !m_displayLabelImage) {
        std::cout << "[GapAnalysis] Overlays enabled. They will appear after analysis completes." << std::endl;
        return true;
    }

    return SetStoredView();
}

bool GapAnalysisService::Impl::ExitView() {
    if (!GetViewThread()) {
        return false;
    }
    return ExitViewState();
}

bool GapAnalysisService::Impl::ExitViewState() {
    const bool isActive = m_viewPhase.load() != GapViewPhase::Idle;
    const bool hasCachedResult = m_displayVoidMesh != nullptr || m_displayLabelImage != nullptr;
    // 1. overlay 必须先从各目标卸载，避免 ClearDisplayState 丢失 binding 后无法移除。
    const bool hasRemoved = SetOverlayOff();
    if (isActive) {
        // 2. 退出只发布 worker 停止请求；不在主线程 tick 中阻塞等待。
        StopAsync();
    }
    // 3. 显示会话不再保留输入 owner；worker 已按值持有自己的快照，可继续安全收尾。
    VolumeBufferSnapshot inputSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        inputSnapshot = std::move(m_inputSnapshot);
    }
    inputSnapshot.reset();
    // 4. 清除显示轴和缓存；分析执行轴由 worker 自己发布终态。
    ClearDisplayState();
    ClearViewThread();
    if (isActive || hasCachedResult || hasRemoved) {
        std::cout << "[GapAnalysis] Display mode exited. Void overlays are hidden." << std::endl;
    }
    return isActive || hasCachedResult || hasRemoved;
}

bool GapAnalysisService::Impl::GetViewOn() const {
    return m_viewPhase.load() != GapViewPhase::Idle;
}

void GapAnalysisService::Impl::OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage) {
    if (!GetViewThread()) {
        return;
    }
    if (m_viewPhase.load() == GapViewPhase::Idle) {
        return;
    }

    // 1. 只有输入转换、ISO 解析和 worker 启动均被真实接纳，才进入结果等待阶段。
    if (m_viewPhase.load() == GapViewPhase::AwaitingInput) {
        if (GetAnalysisState() != GapAnalysisState::Running
            && StartRun(std::move(inputImage))) {
            m_viewPhase.store(GapViewPhase::AwaitingResult);
        }
        return;
    }

    // 2. Consumed 明确表示本显示请求的终态已经处理，后续 tick 不重复挂载或记录失败。
    if (m_viewPhase.load() == GapViewPhase::Consumed) {
        return;
    }

    // 3. 显示线程先用原子执行状态判断是否到达终态；Idle/Running 均没有可消费的终态结果。
    const GapAnalysisState state = GetAnalysisState();
    if (state == GapAnalysisState::Idle || state == GapAnalysisState::Running) {
        return;
    }

    // 可选任务 callback 与结果挂载都由当前宿主 tick 消费；SendCallback 会在 callbackMutex 锁外执行。
    if (GetDoneEvent()) {
        SendCallback();
    }

    // 4A. 失败只记一次日志并关闭本次消费，不挂载任何 overlay。
    if (state == GapAnalysisState::Failed) {
        std::cerr << "[GapAnalysis] Analysis failed; overlay will not be attached." << std::endl;
        m_viewPhase.store(GapViewPhase::Consumed);
        return;
    }

    // 4B. 成功结果在当前 tick 缓存并按 overlay 可见意图挂载；无论目标是否可显示都不重复消费。
    SetDisplayView();
    m_viewPhase.store(GapViewPhase::Consumed);
}

GapAnalysisService::Impl::VolumeBufferSnapshot GapAnalysisService::Impl::GetInputSnapshot() const {
    std::lock_guard<std::mutex> lk(m_inputMutex);
    return m_inputSnapshot;
}

GapAnalysisService::Impl::GapParamSnapshot GapAnalysisService::Impl::GetParamSnapshot() const {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    return { m_surfParams, m_advParams, m_voidParams };
}

void GapAnalysisService::Impl::StartWorker(
    VolumeBufferSnapshot inputSnapshot,
    GapParamSnapshot params) {
    bool isSuccess = false;

    // worker 尚未进入算法时若快照无效或已收到取消请求，以 Idle 结束并为可选 callback 记录失败。
    if (!inputSnapshot || !inputSnapshot->GetVoxelReady() || m_isStopping.load()) {
        SetAnalysisState(GapAnalysisState::Idle);
        SetCallbackReady(false);
        return;
    }

    try {
        const VolumeBuffer& volBuf = *inputSnapshot;

        // 1. 先从只读体素快照构造内部区域；取消只在阶段边界协作式生效。
        auto interior = VoidDetector::CreateInteriorMask(volBuf, params.surfParams.isoValue);
        if (!m_isStopping.load()) {
            // 2. 候选检测只消费本任务参数副本，不读取随后可能更新的服务参数。
            auto candidates = VoidDetector::BuildCandidates(volBuf, interior, params.voidParams);
            if (!m_isStopping.load()) {
                // 3. 区域、label volume 与 label image 先在 worker 局部完整构造，再一次性提交。
                GapAnalysisResult result;
                result.voids = VoidDetector::BuildRegions(
                    volBuf,
                    candidates,
                    params.voidParams,
                    result.labelVolume);
                result.labelImage = BuildLabelImage(result.labelVolume, volBuf);
                result.isSucceeded = true;

                {
                    std::lock_guard<std::mutex> lk(m_resultMutex);
                    m_result = std::move(result);
                }
                isSuccess = true;
            }
        }
    }
    catch (const std::exception&) {
        isSuccess = false;
    }
    catch (...) {
        isSuccess = false;
    }

    // 终态先于可选 callback 门铃发布；宿主观察到门铃时可以读取一致的执行状态和结果。
    SetAnalysisState(isSuccess ? GapAnalysisState::Succeeded : GapAnalysisState::Failed);
    SetCallbackReady(isSuccess);
}

void GapAnalysisService::Impl::StopWorker() {
    std::lock_guard<std::mutex> lk(m_workerMutex);
    if (m_workerThread.joinable()) {
        // join 只由 owner 接管点调用，确保 Impl 销毁或复用线程槽前 worker 已结束。
        m_workerThread.join();
    }
}

void GapAnalysisService::Impl::SetAnalysisState(GapAnalysisState state) {
    m_analysisState.store(static_cast<int>(state));
}

bool GapAnalysisService::Impl::StartRun(vtkSmartPointer<vtkImageData> inputImage) {
    // 1A. 普通非空输入保持公共隔离语义；1B. 空输入复用宿主预装的受控只读快照。
    if (inputImage && !SetInputImage(std::move(inputImage))) {
        return false;
    }

    const auto inputSnapshot = GetInputSnapshot();
    if (!inputSnapshot || !inputSnapshot->GetVoxelReady()) {
        return false;
    }

    // 2. ISO 在本次输入标量范围上解析；回调也在当前 display tick 直接执行，不进入 worker。
    const double isoValue = GetDisplayIso(*inputSnapshot);
    if (m_isoCallback) {
        m_isoCallback(isoValue);
    }

    // 3. 写入本次显示参数后启动异步任务；StartAsync 会再次领取输入与参数快照。
    SurfaceParams surfaceParams;
    surfaceParams.isoValue = static_cast<float>(isoValue);
    SetSurface(surfaceParams);
    SetVoid(m_displayVoidParams);
    return StartAsync(nullptr);
}

void GapAnalysisService::Impl::SetDisplayView() {
    if (m_viewPhase.load() == GapViewPhase::Idle) {
        return;
    }

    // 主线程先从已提交结果构建显示缓存，再移除旧 binding；overlay 隐藏时缓存仍保留。
    m_displayVoidMesh = BuildVoidMesh();
    m_displayLabelImage = BuildLabelImage();
    SetOverlayOff();
    if (!m_isOverlayOn) {
        std::cout << "[GapAnalysis] Analysis completed, but overlays are hidden. Use the host overlay switch command to show them." << std::endl;
        return;
    }

    SetStoredView();
}

bool GapAnalysisService::Impl::SetOverlayOff() {
    bool hasRemoved = false;
    for (const auto& binding : m_displayOverlayBindings) {
        if (!binding.service || !binding.overlayStrategy) {
            continue;
        }
        binding.service->RemoveOverlayStrategy(binding.overlayStrategy);
        hasRemoved = true;
    }
    m_displayOverlayBindings.clear();
    return hasRemoved;
}

bool GapAnalysisService::Impl::SetStoredView() {
    SetOverlayOff();

    const bool hasMeshInput = GetMeshVisible(m_displayVoidMesh);
    const bool hasSliceInput = GetLabelExtent(m_displayLabelImage);
    bool hasMeshAdded = false;
    bool hasSliceAdded = false;

    if (hasMeshInput) {
        for (const auto& service : m_meshTargets) {
            if (!service) {
                continue;
            }
            auto overlay = std::make_shared<GapMeshOverlayStrategy>();
            overlay->SetInputData(m_displayVoidMesh);
            service->AttachOverlayStrategy(overlay);
            m_displayOverlayBindings.push_back({ service, overlay });
            hasMeshAdded = true;
        }
    }

    if (hasSliceInput) {
        for (const auto& target : m_sliceTargets) {
            if (!target.second) {
                continue;
            }
            auto overlay = std::make_shared<GapSliceOverlayStrategy>(target.first);
            overlay->SetInputData(m_displayLabelImage);
            target.second->AttachOverlayStrategy(overlay);
            m_displayOverlayBindings.push_back({ target.second, overlay });
            hasSliceAdded = true;
        }
    }

    if (!hasMeshAdded) {
        std::cerr << "[GapAnalysis] Analysis produced no 3D void mesh overlay target." << std::endl;
    }
    if (!hasSliceAdded) {
        std::cerr << "[GapAnalysis] Analysis produced no 2D label overlay target." << std::endl;
    }

    if (!m_displayOverlayBindings.empty()) {
        int labelDims[3] = { 0, 0, 0 };
        if (m_displayLabelImage) {
            m_displayLabelImage->GetDimensions(labelDims);
        }
        const vtkIdType meshPoints = m_displayVoidMesh ? m_displayVoidMesh->GetNumberOfPoints() : 0;
        const vtkIdType meshCells = m_displayVoidMesh ? m_displayVoidMesh->GetNumberOfCells() : 0;
        std::cout << "[GapAnalysis] Overlays shown: mesh points = "
            << meshPoints << ", mesh cells = " << meshCells
            << ", label dims = " << labelDims[0] << "x" << labelDims[1] << "x" << labelDims[2]
            << std::endl;
    }
    return !m_displayOverlayBindings.empty();
}

void GapAnalysisService::Impl::ClearDisplayState() {
    m_meshTargets.clear();
    m_sliceTargets.clear();
    m_displayVoidMesh = nullptr;
    m_displayLabelImage = nullptr;
    m_displaySurfaceRequest = {};
    m_displayVoidParams = {};
    m_isoCallback = nullptr;
    // 显示阶段与可见意图在会话结束时一起复位；worker 状态保持独立。
    m_viewPhase.store(GapViewPhase::Idle);
    m_isOverlayOn = false;
}

bool GapAnalysisService::Impl::SetViewThread()
{
    const auto currentThreadId = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(m_viewThreadMutex);
    if (m_viewThreadId == std::thread::id{}) {
        m_viewThreadId = currentThreadId;
    }
    return m_viewThreadId == currentThreadId;
}

bool GapAnalysisService::Impl::GetViewThread() const
{
    std::lock_guard<std::mutex> lock(m_viewThreadMutex);
    return m_viewThreadId == std::this_thread::get_id();
}

bool GapAnalysisService::Impl::ClearViewThread()
{
    std::lock_guard<std::mutex> lock(m_viewThreadMutex);
    if (m_viewThreadId != std::this_thread::get_id()) {
        return false;
    }
    m_viewThreadId = {};
    return true;
}

double GapAnalysisService::Impl::GetDisplayIso(const VolumeBuffer& inputSnapshot) const {
    if (m_displaySurfaceRequest.isoMode == GapAnalysisIsoValueMode::AbsoluteValue) {
        return m_displaySurfaceRequest.absoluteIsoValue;
    }

    // DataRangeRatio 的数学含义：
    // iso = min + (max - min) * ratio。ratio 只表达上位机配方，真实阈值在 feature 拿到当前数据快照后解析。
    return inputSnapshot.minVal
        + (inputSnapshot.maxVal - inputSnapshot.minVal) * m_displaySurfaceRequest.dataRangeRatio;
}

bool GapAnalysisService::Impl::BuildVolumeBuffer(
    vtkSmartPointer<vtkImageData> image,
    VolumeBuffer& out) const
{
    if (!image) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }

    auto scalars = image->GetPointData() ? image->GetPointData()->GetScalars() : nullptr;
    if (!scalars || scalars->GetNumberOfComponents() != 1) {
        return false;
    }

    const auto dimX = static_cast<std::size_t>(dims[0]);
    const auto dimY = static_cast<std::size_t>(dims[1]);
    const auto dimZ = static_cast<std::size_t>(dims[2]);
    const auto maxCount = (std::numeric_limits<std::size_t>::max)();
    if (dimX > maxCount / dimY || dimX * dimY > maxCount / dimZ) {
        return false;
    }
    const auto expectedCount = dimX * dimY * dimZ;
    if (expectedCount > static_cast<std::size_t>((std::numeric_limits<vtkIdType>::max)())
        || scalars->GetNumberOfTuples() < static_cast<vtkIdType>(expectedCount)) {
        return false;
    }

    out.dims = { dims[0], dims[1], dims[2] }; // voxel index 布局为 [x, y, z]。

    double spacing[3] = { 1.0, 1.0, 1.0 };
    image->GetSpacing(spacing);
    out.spacing = { spacing[0], spacing[1], spacing[2] }; // 输入 physical 坐标每 voxel 间距，沿 [x, y, z]。

    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetOrigin(origin);
    out.origin = { origin[0], origin[1], origin[2] }; // 输入 physical 坐标原点，沿 [x, y, z]。

    // RawVolumeDataManager 当前产出 VTK_FLOAT；该路径只扫描数值域，不再分配或复制整卷体素。
    if (scalars->GetDataType() == VTK_FLOAT) {
        const auto* source = static_cast<const float*>(scalars->GetVoidPointer(0));
        if (!source) {
            return false;
        }

        const auto minMax = std::minmax_element(source, source + expectedCount);
        out.minVal = *minMax.first;
        out.maxVal = *minMax.second;
        try {
            std::shared_ptr<const void> imageOwner =
                std::make_shared<vtkSmartPointer<vtkImageData>>(std::move(image));
            return out.SetSharedVoxels(std::move(imageOwner), source);
        }
        catch (const std::bad_alloc&) {
            return false;
        }
    }

    // 插件接入非 float VTK image 时仍转换成连续 float 存储，算法层无需扩散 scalar 类型分支。
    std::vector<float> voxels;
    try {
        voxels.resize(expectedCount);
        for (std::size_t index = 0; index < expectedCount; ++index) {
            voxels[index] = static_cast<float>(scalars->GetTuple1(static_cast<vtkIdType>(index)));
        }
    }
    catch (const std::bad_alloc&) {
        return false;
    }

    const auto minMax = std::minmax_element(voxels.begin(), voxels.end());
    out.minVal = minMax.first == voxels.end() ? 0.0f : *minMax.first;
    out.maxVal = minMax.second == voxels.end() ? 0.0f : *minMax.second;
    out.SetOwnedVoxels(std::move(voxels));

    return true;
}

vtkSmartPointer<vtkImageData> GapAnalysisService::Impl::BuildLabelImage(
    const std::vector<int>& labelVolume,
    const VolumeBuffer& volBuf) const
{
    if (labelVolume.empty()) {
        return nullptr;
    }

    const auto total = static_cast<std::size_t>(volBuf.dims[0])
        * static_cast<std::size_t>(volBuf.dims[1])
        * static_cast<std::size_t>(volBuf.dims[2]);
    if (labelVolume.size() < total) {
        return nullptr;
    }

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(volBuf.dims[0], volBuf.dims[1], volBuf.dims[2]);
    image->SetSpacing(volBuf.spacing[0], volBuf.spacing[1], volBuf.spacing[2]);
    image->SetOrigin(volBuf.origin[0], volBuf.origin[1], volBuf.origin[2]);
    image->AllocateScalars(VTK_INT, 1);

    auto* labelPtr = static_cast<int*>(image->GetScalarPointer());
    if (!labelPtr) {
        return nullptr;
    }

    std::copy_n(labelVolume.begin(), total, labelPtr);
    image->Modified();
    return image;
}
