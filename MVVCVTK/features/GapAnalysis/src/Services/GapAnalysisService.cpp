#include "Services/GapAnalysisService.h"

#include "GapInputBuffer.h"
#include "GapKernelBridge.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Strategies/GapOverlayStrategies.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// GapAnalysis 的并发与显示编排边界：后台 worker 只消费不可变体素/参数快照并一次性提交结果；
// 宿主 view 线程通过独立状态机消费终态、创建 overlay。分析状态、显示阶段和 overlay 可见意图互不推导。
class GapAnalysisService::Impl final {
public:
    Impl() = default;
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    bool SetGapInput(vtkSmartPointer<vtkImageData> image);
    void SetSurface(const GapSurfaceParams& params);
    void SetVoid(const GapVoidParams& params);
    bool StartAsync(std::function<void(bool isSuccess)> onComplete);
    void StopAsync();
    bool GetDoneEvent();
    void SendCallback();
    GapAnalysisState GetAnalysisState() const;
    std::vector<VoidRegion> GetVoidRegions() const;
    GapStatistics GetStatistics() const;
    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const;
    vtkSmartPointer<vtkImageData> BuildLabelImage() const;

    bool StartView(GapViewRequest request, std::function<void(bool)> onComplete);
    bool SwitchOverlay();
    bool ExitView();
    void ClearView();
    bool GetViewOn() const;
    bool GetDisplayTickNeeded() const;
    void OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage);

private:
    class KernelModule final {
    public:
        explicit KernelModule(HMODULE module) noexcept
            : m_module(module)
        {
        }

        ~KernelModule() noexcept
        {
            if (m_module) {
                FreeLibrary(m_module);
            }
        }

        KernelModule(const KernelModule&) = delete;
        KernelModule& operator=(const KernelModule&) = delete;
        KernelModule(KernelModule&&) = delete;
        KernelModule& operator=(KernelModule&&) = delete;

        HMODULE Get() const noexcept
        {
            return m_module;
        }

    private:
        HMODULE m_module = nullptr;
    };

    struct InputData final {
        GapInputBuffer volume;
        vtkSmartPointer<vtkImageData> image;
    };

    using InputSnapshot = std::shared_ptr<const InputData>;

    struct KernelBatch final {
        GapKernelHeader header{};
        std::vector<GapKernelRegion> regions;
        std::vector<std::int32_t> labels;
        std::uint64_t expectedLabelCount = 0;
    };

    enum class GapViewPhase {
        // 没有活动显示会话。
        Idle,
        // worker 已接纳任务，tick 只轮询原子终态。
        AwaitingResult,
        // 本次终态已回调并处理，后续 tick 不重复挂载或报错。
        Consumed
    };

    struct GapParamSnapshot {
        // StartAsync 从 m_paramsMutex 下复制；worker 将 isoValue 映射为 DefX 表面阈值。
        GapSurfaceParams surfParams;
        // 随任务按值冻结；worker 只把灰度边界和最小体积映射给私有算法内核。
        GapVoidParams voidParams;
    };

    struct GapOverlayBinding {
        // 已挂载 overlay 的宿主 service 共享 owner；SetOverlayOff 用它执行 Remove 后清空 binding。
        std::shared_ptr<OverlayService> service;
        // 与 service 中同一 overlay 实例的共享 owner，确保 RemoveOverlay 前对象仍有效。
        std::shared_ptr<FeatureOverlay> overlay;
    };

    void SetCompletionCallback(std::function<void(bool)> callback);
    void SetCallbackReady(bool isSuccess);
    bool GetMeshVisible(vtkSmartPointer<vtkPolyData> voidMesh) const;
    bool GetLabelExtent(vtkSmartPointer<vtkImageData> labelImage) const;
    static std::optional<std::filesystem::path> GetModulePath(
        HMODULE module);
    static std::optional<std::filesystem::path> GetRuntimePath(
        const std::filesystem::path& directory);
    static std::optional<std::wstring> GetEnvValue(
        const wchar_t* name);
    static bool GetPathEqual(
        const std::filesystem::path& left,
        const std::filesystem::path& right);
    static std::optional<std::filesystem::path> GetKernelPath();
    bool BuildInputBuffer(
        vtkSmartPointer<vtkImageData> image,
        GapInputBuffer& out) const;
    bool BuildInputSnapshot(
        vtkSmartPointer<vtkImageData> image,
        vtkSmartPointer<vtkImageData> validityMask,
        InputSnapshot& out) const;
    bool BuildKernelResult(
        const GapInputBuffer& volume,
        const GapParamSnapshot& params,
        vtkImageData* image,
        KernelBatch& result) const;
    static std::int32_t MVVCVTK_GAP_KERNEL_CALL SetKernelResult(
        const GapKernelResultView* result,
        void* context) noexcept;
    bool BuildResultPayload(
        const KernelBatch& batch,
        vtkImageData* inputImage,
        GapAnalysisResult& result) const;
    bool GetRequestValid(
        const GapSurfaceConfig& surface,
        const GapVoidParams& voidParams) const;
    vtkSmartPointer<vtkImageData> BuildLabelImage(
        const std::vector<std::int32_t>& labelVolume,
        vtkImageData* inputImage) const;

    InputSnapshot GetInputSnapshot() const;
    GapParamSnapshot GetParamSnapshot() const;
    void StartWorker(
        InputSnapshot inputSnapshot,
        GapParamSnapshot params);
    void StopWorker();
    void SetAnalysisState(GapAnalysisState state);

    bool SetDisplayView();
    bool SetOverlayOff() noexcept;
    bool SetStoredView();
    bool ExitViewState();
    void ClearDisplayState();
    bool SetViewThread();
    bool GetViewBound() const;
    bool GetViewThreadReady() const;
    bool GetViewThread() const;
    bool ClearViewThread();
    double GetDisplayIso(
        const GapInputBuffer& inputSnapshot,
        const GapSurfaceConfig& surface) const;

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
    // 两种输入入口最终都提交同一只读 float VTK 快照及其连续体素别名。
    InputSnapshot m_inputSnapshot;

    // paramsMutex 把两个可变参数对象作为同一份任务配置复制边界。
    mutable std::mutex m_paramsMutex;
    // 输入标量域表面参数；GetParamSnapshot 按值冻结，worker 当前消费其中 isoValue。
    GapSurfaceParams m_surfParams;
    // 灰度边界与最小体积由私有内核消费；其余 ABI 保留字段不参与计算。
    GapVoidParams m_voidParams;

    // resultMutex 保护完整结果 payload；读取入口只在锁内取得值或 VTK owner，复制/构建均在锁外。
    mutable std::mutex m_resultMutex;
    // 最近一次 worker 提交的结果真源；新任务启动前清空，mesh/label 显示缓存均从它派生。
    GapAnalysisResult m_result;

    // workerMutex 只串行化 std::thread 槽的 join/替换，不保护算法 payload。
    mutable std::mutex m_workerMutex;
    // Impl 唯一拥有的 worker 线程；复用或析构前由 owner 接管点 join，禁止越过 Impl 生命周期。
    std::thread m_workerThread;
    // stopMutex 只线性化取消与终态发布；算法内部仍通过原子值协作式观察。
    mutable std::mutex m_stopMutex;
    // 取消请求轴：StopAsync 置位，下一次被接受的 StartAsync 清零。
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
    // 成功结果派生的 2D label image 强引用缓存；完整继承输入快照几何，生命周期和 mesh 缓存一致。
    vtkSmartPointer<vtkImageData> m_displayLabelImage;
    // StartView 保存的 ISO 来源配方；接纳 worker 前用冻结输入的 min/max 解析，结束会话时清空。
    GapSurfaceConfig m_displaySurfaceConfig;
    // StartView 保存的 void 参数值副本；接纳 worker 时同步写入参数槽，结束会话时清空。
    GapVoidParams m_displayVoidParams;
    std::function<void(bool)> m_viewCallback;
    bool m_isExitPending = false;
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

std::optional<std::filesystem::path>
GapAnalysisService::Impl::GetModulePath(HMODULE module)
{
    constexpr std::size_t initialSize = 1024;
    constexpr std::size_t maxSize = 32768;
    std::vector<wchar_t> buffer(initialSize);
    while (buffer.size() <= maxSize) {
        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(
            module,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return std::nullopt;
        }
        if (copied < buffer.size()) {
            return std::filesystem::path(
                std::wstring(buffer.data(), copied));
        }
        if (buffer.size() == maxSize) {
            return std::nullopt;
        }
        buffer.resize((std::min)(buffer.size() * 2, maxSize));
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
GapAnalysisService::Impl::GetRuntimePath(
    const std::filesystem::path& directory)
{
    if (directory.empty() || !directory.is_absolute()) {
        return std::nullopt;
    }

    std::error_code error;
    const auto kernelCandidate =
        directory / L"MVVCVTKGapKernel.dll";
    if (!std::filesystem::is_regular_file(kernelCandidate, error)
        || error) {
        return std::nullopt;
    }
    const auto kernelPath = std::filesystem::canonical(
        kernelCandidate, error);
    if (error) {
        return std::nullopt;
    }

    const auto defxPath =
        kernelPath.parent_path() / L"DefXAnalysis.dll";
    if (!std::filesystem::is_regular_file(defxPath, error)
        || error) {
        return std::nullopt;
    }
    return kernelPath;
}

std::optional<std::wstring>
GapAnalysisService::Impl::GetEnvValue(const wchar_t* name)
{
    if (!name || *name == L'\0') {
        return std::nullopt;
    }

    SetLastError(ERROR_SUCCESS);
    DWORD bufferSize = GetEnvironmentVariableW(name, nullptr, 0);
    if (bufferSize == 0) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND
            ? std::nullopt : std::optional<std::wstring>(L"");
    }

    for (;;) {
        std::vector<wchar_t> buffer(bufferSize);
        const DWORD copied = GetEnvironmentVariableW(
            name, buffer.data(), bufferSize);
        if (copied == 0) {
            return std::wstring{};
        }
        if (copied < bufferSize) {
            return std::wstring(buffer.data(), copied);
        }
        bufferSize = copied;
    }
}

bool GapAnalysisService::Impl::GetPathEqual(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) {
        return true;
    }

    const auto leftPath = left.lexically_normal().native();
    const auto rightPath = right.lexically_normal().native();
    return CompareStringOrdinal(
        leftPath.c_str(), static_cast<int>(leftPath.size()),
        rightPath.c_str(), static_cast<int>(rightPath.size()),
        TRUE) == CSTR_EQUAL;
}

std::optional<std::filesystem::path>
GapAnalysisService::Impl::GetKernelPath()
{
    // 1. SDK/宿主通过 Feature 专属环境变量给出受信任绝对目录；
    // 一旦显式配置，非法路径必须失败，不能再回退到其他搜索来源。
    const auto runtimeValue = GetEnvValue(
        L"MVVCVTK_GAP_RUNTIME_DIR");
    if (runtimeValue) {
        const std::filesystem::path runtimeDir(*runtimeValue);
        const auto configuredPath = GetRuntimePath(runtimeDir);
        if (!configuredPath) {
            std::wcerr
                << L"[GapAnalysis] Invalid MVVCVTK_GAP_RUNTIME_DIR: "
                << runtimeDir.native() << L'\n' << std::flush;
            return std::nullopt;
        }
        return configuredPath;
    }

    // 2. 源码构建或随应用部署只接受进程目录中的成对 runtime。
    const auto processPath = GetModulePath(nullptr);
    if (processPath) {
        const auto localPath = GetRuntimePath(
            processPath->parent_path());
        if (localPath) {
            return localPath;
        }
    }
    return std::nullopt;
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

bool GapAnalysisService::SetGapInput(vtkSmartPointer<vtkImageData> image)
{
    return m_impl->SetGapInput(std::move(image));
}

void GapAnalysisService::SetSurface(const GapSurfaceParams& params)
{
    m_impl->SetSurface(params);
}

void GapAnalysisService::SetVoid(const GapVoidParams& params)
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

GapStatistics GapAnalysisService::GetStatistics() const
{
    return m_impl->GetStatistics();
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
    GapViewRequest request,
    std::function<void(bool)> onComplete)
{
    return m_impl->StartView(std::move(request), std::move(onComplete));
}

bool GapAnalysisService::SwitchOverlay()
{
    return m_impl->SwitchOverlay();
}

bool GapAnalysisService::ExitView()
{
    return m_impl->ExitView();
}

void GapAnalysisService::ClearView()
{
    m_impl->ClearView();
}

bool GapAnalysisService::GetViewOn() const
{
    return m_impl->GetViewOn();
}

bool GapAnalysisService::GetDisplayTickNeeded() const
{
    return m_impl->GetDisplayTickNeeded();
}

void GapAnalysisService::OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage)
{
    m_impl->OnDisplayTick(std::move(inputImage));
}

bool GapAnalysisService::Impl::SetGapInput(vtkSmartPointer<vtkImageData> image) {
    // 输入替换分三条路径：
    // A. 转换失败：退休旧快照，非 Running 时发布 Failed；运行中任务继续持有自己的旧 owner。
    // B. owner 分配失败：与转换失败保持相同状态语义。
    // C. 成功：与 StartAsync 在 workerMutex 下串行，替换输入；非 Running 时同时退休旧结果并回到 Idle。
    InputSnapshot snapshot;
    if (!BuildInputSnapshot(std::move(image), nullptr, snapshot)) {
        InputSnapshot retiredSnapshot;
        GapAnalysisResult retiredResult;
        {
            std::lock_guard<std::mutex> workerLock(m_workerMutex);
            {
                std::lock_guard<std::mutex> inputLock(m_inputMutex);
                retiredSnapshot = std::move(m_inputSnapshot);
            }
            if (GetAnalysisState() != GapAnalysisState::Running) {
                std::lock_guard<std::mutex> resultLock(m_resultMutex);
                retiredResult = std::move(m_result);
                m_result = {};
                SetAnalysisState(GapAnalysisState::Failed);
            }
        }
        return false;
    }

    // 输入、旧结果和执行状态作为一次提交与 StartAsync 串行，不能覆盖刚发布的 Running。
    InputSnapshot retiredSnapshot;
    GapAnalysisResult retiredResult;
    {
        std::lock_guard<std::mutex> workerLock(m_workerMutex);
        {
            std::lock_guard<std::mutex> inputLock(m_inputMutex);
            retiredSnapshot = std::move(m_inputSnapshot);
            m_inputSnapshot = std::move(snapshot);
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

void GapAnalysisService::Impl::SetSurface(const GapSurfaceParams& params) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_surfParams = params;
}

void GapAnalysisService::Impl::SetVoid(const GapVoidParams& params) {
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

    // 输入已由隔离入口固化为最小只读快照；worker 只持有不可变 owner。
    auto inputSnapshot = GetInputSnapshot();
    if (!inputSnapshot || !inputSnapshot->volume.GetVoxelReady()
        || !inputSnapshot->image) {
        SetAnalysisState(GapAnalysisState::Failed);
        SetCallbackReady(false);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_result = {};
    }

    // 3. 结果清场完成后再发布新执行状态；与 StopAsync 串行后，
    // 新任务的取消起点和 Running 状态属于同一提交。
    {
        std::lock_guard<std::mutex> stopLock(m_stopMutex);
        m_isStopping.store(false);
        SetAnalysisState(GapAnalysisState::Running);
    }

    // 4. worker 只捕获不可变输入快照和参数值副本。线程创建失败必须同步回滚 Running，
    // 否则服务会留下一个永远没有 worker 的假运行状态。
    try {
        m_workerThread = std::thread(
            &GapAnalysisService::Impl::StartWorker,
            this,
            std::move(inputSnapshot),
            GetParamSnapshot());
    }
    catch (...) {
        SetAnalysisState(GapAnalysisState::Failed);
        SetCallbackReady(false);
        return false;
    }
    return true;
}

void GapAnalysisService::Impl::StopAsync() {
    // 取消与 worker 的成功发布线性化；返回仍不表示线程已退出。
    std::lock_guard<std::mutex> stopLock(m_stopMutex);
    m_isStopping.store(true);
}

GapAnalysisState GapAnalysisService::Impl::GetAnalysisState() const {
    return static_cast<GapAnalysisState>(m_analysisState.load());
}

std::vector<VoidRegion> GapAnalysisService::Impl::GetVoidRegions() const {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    return m_result.voids;
}

GapStatistics GapAnalysisService::Impl::GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_result.isSucceeded
        ? m_result.statistics : GapStatistics{};
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

    // labelImage 中 0 为背景、正整数为任一区域；等值 0.5 把所有正标签合并成一张空洞外表面。
    // 结果不保留区域间的标签边界，也不在此计算法线；当前显示路径把它作为 3D overlay 输入。
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
    GapViewRequest request,
    std::function<void(bool)> onComplete) {
    // 新会话采用“局部准备 -> worker 接纳 -> 可见状态提交”三段事务：
    // 1. 在不改变旧会话的前提下验证线程、参数、目标并冻结 image+mask。
    // 2. 串行领取 worker/callback 槽；只有线程对象成功启动才算接纳。
    // 3. 接纳后才卸载旧 overlay，并一次性提交新 target、callback 和显示阶段。
    if (!GetViewThreadReady()) {
        std::cerr << "[GapAnalysis] Display activation rejected: view thread mismatch." << std::endl;
        return false;
    }
    if (!GetRequestValid(request.surface, request.voidParams)) {
        return false;
    }

    std::vector<std::shared_ptr<OverlayService>> meshTargets;
    meshTargets.reserve(request.meshTargets.size());
    for (auto& target : request.meshTargets) {
        if (target) {
            meshTargets.push_back(std::move(target));
        }
    }

    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>>
        sliceTargets;
    sliceTargets.reserve(request.sliceTargets.size());
    for (auto& target : request.sliceTargets) {
        if (target.second) {
            sliceTargets.push_back(std::move(target));
        }
    }

    if (meshTargets.empty() && sliceTargets.empty()) {
        std::cerr << "[GapAnalysis] Display activation skipped: no overlay target was provided." << std::endl;
        return false;
    }

    InputSnapshot inputSnapshot;
    if (!BuildInputSnapshot(
            std::move(request.inputImage),
            std::move(request.validityMask),
            inputSnapshot)) {
        return false;
    }

    GapParamSnapshot params = GetParamSnapshot();
    params.surfParams = {};
    params.surfParams.isoValue = static_cast<float>(
        GetDisplayIso(inputSnapshot->volume, request.surface));
    params.surfParams.background = request.surface.backgroundMean;
    params.surfParams.material = request.surface.materialMean;
    params.voidParams = request.voidParams;
    if (!std::isfinite(params.surfParams.isoValue)
        || !std::isfinite(params.surfParams.background)
        || !std::isfinite(params.surfParams.material)) {
        return false;
    }

    const bool wasViewBound = GetViewBound();
    if (!SetViewThread()) {
        return false;
    }

    GapAnalysisResult retiredResult;
    {
        std::lock_guard<std::mutex> workerLock(m_workerMutex);
        if (GetAnalysisState() == GapAnalysisState::Running
            || m_viewPhase.load() == GapViewPhase::AwaitingResult
            || m_isExitPending) {
            if (!wasViewBound) {
                ClearViewThread();
            }
            return false;
        }

        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }

        {
            std::lock_guard<std::mutex> callbackLock(m_callbackMutex);
            if (m_completionCallback || m_nextCallback
                || m_hasCallback.load()) {
                if (!wasViewBound) {
                    ClearViewThread();
                }
                return false;
            }
        }

        // 取消锁覆盖 Running 提交和线程接纳；构造失败时可以恢复原状态，
        // 而不会抹除一个已经返回给调用方的并发 StopAsync。
        std::lock_guard<std::mutex> stopLock(m_stopMutex);
        const auto oldState = GetAnalysisState();
        const bool wasStopping = m_isStopping.load();
        {
            std::lock_guard<std::mutex> resultLock(m_resultMutex);
            retiredResult = std::move(m_result);
            m_result = {};
        }
        m_isStopping.store(false);
        SetAnalysisState(GapAnalysisState::Running);

        try {
            m_workerThread = std::thread(
                &GapAnalysisService::Impl::StartWorker,
                this,
                inputSnapshot,
                params);
        }
        catch (...) {
            {
                std::lock_guard<std::mutex> resultLock(m_resultMutex);
                m_result = std::move(retiredResult);
            }
            m_isStopping.store(wasStopping);
            SetAnalysisState(oldState);
            if (!wasViewBound) {
                ClearViewThread();
            }
            return false;
        }
    }

    // worker 已被接纳；RemoveOverlay 的 noexcept 契约保证旧 overlay 清理不会重新
    // 打开异常出口，因此从这里开始可以连续提交新会话且不再返回 false。
    SetOverlayOff();
    {
        std::lock_guard<std::mutex> inputLock(m_inputMutex);
        m_inputSnapshot = inputSnapshot;
    }
    {
        std::lock_guard<std::mutex> paramsLock(m_paramsMutex);
        m_surfParams = params.surfParams;
        m_voidParams = params.voidParams;
    }
    m_meshTargets = std::move(meshTargets);
    m_sliceTargets = std::move(sliceTargets);
    m_displayVoidMesh = nullptr;
    m_displayLabelImage = nullptr;
    m_displaySurfaceConfig = request.surface;
    m_displayVoidParams = request.voidParams;
    m_viewCallback = std::move(onComplete);
    m_isExitPending = false;
    m_viewPhase.store(GapViewPhase::AwaitingResult);
    m_isOverlayOn = true;
    std::cout << "[GapAnalysis] Display mode requested. Analysis worker accepted." << std::endl;
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
        m_isOverlayOn = false;
        m_isExitPending = true;
        StopAsync();
    }
    if (isActive || hasCachedResult || hasRemoved) {
        std::cout << "[GapAnalysis] Display mode exited. Void overlays are hidden." << std::endl;
    }
    return isActive || hasCachedResult || hasRemoved;
}

bool GapAnalysisService::Impl::GetViewOn() const {
    return m_viewPhase.load() != GapViewPhase::Idle && !m_isExitPending;
}

bool GapAnalysisService::Impl::GetDisplayTickNeeded() const {
    return m_viewPhase.load() != GapViewPhase::Idle;
}

void GapAnalysisService::Impl::ClearView()
{
    if (GetViewBound() && !GetViewThread()) {
        std::cerr << "[GapAnalysis] ClearView rejected: view thread mismatch." << std::endl;
        return;
    }

    // 清理顺序：先请求取消并 join worker，确认不再写结果后卸载 overlay；随后释放 callback、
    // 输入快照、显示缓存和 view 线程绑定。调用方必须在已绑定的宿主线程协调活动显示会话。
    StopAsync();
    StopWorker();
    SetOverlayOff();
    m_viewCallback = nullptr;
    {
        std::lock_guard<std::mutex> callbackLock(m_callbackMutex);
        m_completionCallback = nullptr;
        m_nextCallback = nullptr;
        m_isNextOk = false;
        m_hasCallback.store(false);
    }
    m_isExitPending = false;
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        m_inputSnapshot.reset();
    }
    {
        std::lock_guard<std::mutex> resultLock(m_resultMutex);
        m_result = {};
    }
    SetAnalysisState(GapAnalysisState::Idle);
    ClearDisplayState();
    if (GetViewBound()) {
        ClearViewThread();
    }
}

void GapAnalysisService::Impl::OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage) {
    (void)inputImage;
    if (!GetViewThread()) {
        return;
    }
    if (m_viewPhase.load() == GapViewPhase::Idle) {
        return;
    }

    // 1. Consumed 只阻止重复挂载；退出请求仍必须经过下方 join 与状态清理。
    if (m_viewPhase.load() == GapViewPhase::Consumed
        && !m_isExitPending) {
        return;
    }

    // 2. 显示线程先用原子执行状态判断是否到达终态；Idle/Running 均没有可消费的终态结果。
    const GapAnalysisState state = GetAnalysisState();
    if (state == GapAnalysisState::Running) {
        return;
    }
    StopWorker();

    auto callback = std::move(m_viewCallback);
    if (m_isExitPending || state == GapAnalysisState::Idle) {
        m_isExitPending = false;
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_inputSnapshot.reset();
        }
        ClearDisplayState();
        ClearViewThread();
        if (callback) { try { callback(false); } catch (...) {} }
        return;
    }

    // 3A. 失败只记一次日志并关闭本次消费，不挂载任何 overlay。
    if (state == GapAnalysisState::Failed) {
        std::cerr << "[GapAnalysis] Analysis failed; overlay will not be attached." << std::endl;
        m_viewPhase.store(GapViewPhase::Consumed);
        if (callback) { try { callback(false); } catch (...) {} }
        return;
    }

    // 3B. 成功结果在当前 tick 缓存并按 overlay 可见意图挂载；无论目标是否可显示都不重复消费。
    const bool isDisplayed = SetDisplayView();
    m_viewPhase.store(GapViewPhase::Consumed);
    if (callback) { try { callback(isDisplayed); } catch (...) {} }
}

GapAnalysisService::Impl::InputSnapshot GapAnalysisService::Impl::GetInputSnapshot() const {
    std::lock_guard<std::mutex> lk(m_inputMutex);
    return m_inputSnapshot;
}

GapAnalysisService::Impl::GapParamSnapshot GapAnalysisService::Impl::GetParamSnapshot() const {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    return { m_surfParams, m_voidParams };
}

void GapAnalysisService::Impl::StartWorker(
    InputSnapshot inputSnapshot,
    GapParamSnapshot params) {
    // worker 只使用按值参数和共享只读输入快照；中间产物保持局部，完整结果在 resultMutex 下单次发布。
    // DefX 同步调用前后及 DTO 投影阶段观察取消；取消和算法异常都映射为 Failed。
    bool isSuccess = false;
    GapAnalysisResult result;

    if (!inputSnapshot || !inputSnapshot->volume.GetVoxelReady()
        || !inputSnapshot->image || m_isStopping.load()) {
        SetAnalysisState(GapAnalysisState::Idle);
        SetCallbackReady(false);
        return;
    }

    try {
        KernelBatch batch;

        // 1. bridge 在自己的 DLL 内完成唯一一次 DefX 调用，并同步复制 header、region 和原始标签。
        // DefX 没有取消入口，因此这里只能在调用前后观察停止标志。
        const bool hasKernelResult = !m_isStopping.load()
            && BuildKernelResult(
                inputSnapshot->volume,
                params,
                inputSnapshot->image,
                batch)
            && !m_isStopping.load();

        // 2. Feature 只验证并投影供应商 DTO；不得再执行阈值、腐蚀、连通域、过滤或重编号。
        if (hasKernelResult
            && BuildResultPayload(
                batch,
                inputSnapshot->image,
                result)
            && !m_isStopping.load()) {
            result.isSucceeded = true;
            isSuccess = true;
        }
    }
    catch (const std::exception&) {
        isSuccess = false;
    }
    catch (...) {
        isSuccess = false;
    }

    // 终态发布与 StopAsync 串行：取消若先取得锁，本轮不得再提交成功；
    // worker 若先提交，StopAsync 只会在完整结果、状态和 callback 门铃发布后返回。
    {
        std::lock_guard<std::mutex> stopLock(m_stopMutex);
        if (m_isStopping.load()) {
            isSuccess = false;
        }
        if (isSuccess) {
            std::lock_guard<std::mutex> resultLock(m_resultMutex);
            m_result = std::move(result);
        }
        SetAnalysisState(isSuccess ? GapAnalysisState::Succeeded : GapAnalysisState::Failed);
        SetCallbackReady(isSuccess);
    }
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

bool GapAnalysisService::Impl::SetDisplayView() {
    if (m_viewPhase.load() == GapViewPhase::Idle) {
        return false;
    }

    // 主线程先从已提交结果构建显示缓存，再移除旧 binding；overlay 隐藏时缓存仍保留。
    m_displayVoidMesh = BuildVoidMesh();
    m_displayLabelImage = BuildLabelImage();
    SetOverlayOff();
    if (!m_isOverlayOn) {
        std::cout << "[GapAnalysis] Analysis completed, but overlays are hidden. Use the host overlay switch command to show them." << std::endl;
        return true;
    }

    try {
        return SetStoredView();
    }
    catch (...) {
        SetOverlayOff();
        return false;
    }
}

bool GapAnalysisService::Impl::SetOverlayOff() noexcept {
    bool hasRemoved = false;
    for (const auto& binding : m_displayOverlayBindings) {
        if (!binding.service || !binding.overlay) {
            continue;
        }
        binding.service->RemoveOverlay(binding.overlay);
        hasRemoved = true;
    }
    m_displayOverlayBindings.clear();
    return hasRemoved;
}

bool GapAnalysisService::Impl::SetStoredView() {
    // 1. 先对称卸载旧 binding，保证重复显示/切换可见性不会累积 prop。
    // 2. mesh 与 label 两类 artifact 独立判定、独立挂载；缺少其中一类不阻止另一类显示。
    // 3. 每次成功 Attach 都记录同一 service/strategy 对，供 SetOverlayOff 精确 Remove。
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
            if (service->AttachOverlay(overlay)) {
                m_displayOverlayBindings.push_back({ service, overlay });
                hasMeshAdded = true;
            }
        }
    }

    if (hasSliceInput) {
        for (const auto& target : m_sliceTargets) {
            if (!target.second) {
                continue;
            }
            auto overlay = std::make_shared<GapSliceOverlayStrategy>(target.first);
            overlay->SetInputData(m_displayLabelImage);
            if (target.second->AttachOverlay(overlay)) {
                m_displayOverlayBindings.push_back({ target.second, overlay });
                hasSliceAdded = true;
            }
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
    m_displaySurfaceConfig = {};
    m_displayVoidParams = {};
    m_viewCallback = nullptr;
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

bool GapAnalysisService::Impl::GetViewBound() const
{
    std::lock_guard<std::mutex> lock(m_viewThreadMutex);
    return m_viewThreadId != std::thread::id{};
}

bool GapAnalysisService::Impl::GetViewThreadReady() const
{
    const auto currentThreadId = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(m_viewThreadMutex);
    return m_viewThreadId == std::thread::id{}
        || m_viewThreadId == currentThreadId;
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

double GapAnalysisService::Impl::GetDisplayIso(
    const GapInputBuffer& inputSnapshot,
    const GapSurfaceConfig& surface) const {
    if (surface.isoMode == GapIsoMode::AbsoluteValue) {
        return surface.absoluteIsoValue;
    }

    // DataRangeRatio 的数学含义：
    // iso = min + (max - min) * ratio。ratio 只表达上位机配方，真实阈值在 feature 拿到当前数据快照后解析。
    return inputSnapshot.minVal
        + (inputSnapshot.maxVal - inputSnapshot.minVal) * surface.dataRangeRatio;
}

bool GapAnalysisService::Impl::BuildInputSnapshot(
    vtkSmartPointer<vtkImageData> image,
    vtkSmartPointer<vtkImageData> validityMask,
    InputSnapshot& out) const
{
    out.reset();
    // DefX 没有有效域输入。静默忽略或在 Feature 内裁剪标签都会形成第二套算法语义，
    // 因此任何非空 mask 都在 worker 接纳前明确拒绝。
    if (!image || validityMask) {
        return false;
    }

    try {
        auto* sourceScalars = image->GetPointData()
            ? image->GetPointData()->GetScalars() : nullptr;
        if (!sourceScalars
            || sourceScalars->GetNumberOfComponents() != 1) {
            return false;
        }

        auto imageCopy = vtkSmartPointer<vtkImageData>::New();
        if (sourceScalars->GetDataType() == VTK_FLOAT) {
            imageCopy->DeepCopy(image);
        }
        else {
            // DefX 固定消费连续 float；非 float 输入在接纳前一次性转换。
            imageCopy->CopyStructure(image);
            imageCopy->AllocateScalars(VTK_FLOAT, 1);
            auto* targetScalars = static_cast<float*>(
                imageCopy->GetScalarPointer());
            const auto pointCount = image->GetNumberOfPoints();
            if (!targetScalars || pointCount <= 0
                || sourceScalars->GetNumberOfTuples() < pointCount) {
                return false;
            }
            for (vtkIdType index = 0; index < pointCount; ++index) {
                targetScalars[index] = static_cast<float>(
                    sourceScalars->GetTuple1(index));
            }
            imageCopy->Modified();
        }

        auto workerImage = imageCopy;
        GapInputBuffer volume;
        if (!BuildInputBuffer(std::move(imageCopy), volume)) {
            return false;
        }
        auto snapshot = std::make_shared<InputData>();
        snapshot->volume = std::move(volume);
        snapshot->image = std::move(workerImage);
        out = std::move(snapshot);
        return out && out->volume.GetVoxelReady() && out->image;
    }
    catch (const std::bad_alloc&) {
        out.reset();
        return false;
    }
}
std::int32_t MVVCVTK_GAP_KERNEL_CALL
GapAnalysisService::Impl::SetKernelResult(
    const GapKernelResultView* result,
    void* context) noexcept
{
    if (!result || !context
        || result->abiVersion != GapKernelAbiVersion
        || result->structSize != sizeof(GapKernelResultView)
        || result->headerSize != sizeof(GapKernelHeader)
        || result->regionSize != sizeof(GapKernelRegion)
        || !result->header
        || (result->regionCount != 0 && !result->regions)
        || (result->labelCount != 0 && !result->labels)
        || result->labelCount
            != static_cast<KernelBatch*>(context)
                ->expectedLabelCount
        || result->regionCount > result->labelCount
        || result->regionCount
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::ptrdiff_t>::max)())
        || result->labelCount
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::ptrdiff_t>::max)())) {
        return 0;
    }

    try {
        KernelBatch batch;
        batch.expectedLabelCount = result->labelCount;
        batch.header = *result->header;
        if (result->regionCount != 0) {
            batch.regions.assign(
                result->regions,
                result->regions
                    + static_cast<std::size_t>(result->regionCount));
        }
        if (result->labelCount != 0) {
            batch.labels.assign(
                result->labels,
                result->labels
                    + static_cast<std::size_t>(result->labelCount));
        }
        *static_cast<KernelBatch*>(context) = std::move(batch);
        return 1;
    }
    catch (...) {
        return 0;
    }
}

bool GapAnalysisService::Impl::BuildKernelResult(
    const GapInputBuffer& volume,
    const GapParamSnapshot& params,
    vtkImageData* image,
    KernelBatch& result) const
{
    result = {};
    if (!image || !volume.GetVoxelReady()
        || !std::isfinite(volume.minVal)
        || !std::isfinite(volume.maxVal)
        || !std::isfinite(params.surfParams.isoValue)
        || !std::isfinite(params.surfParams.background)
        || !std::isfinite(params.surfParams.material)
        || !std::isfinite(params.voidParams.minVolumeMM3)
        || volume.minVal > volume.maxVal
        || params.surfParams.background > params.surfParams.material
        || params.voidParams.minVolumeMM3 < 0.0
        || params.voidParams.minVolumeMM3
            > static_cast<double>(
                (std::numeric_limits<float>::max)())) {
        return false;
    }

    const auto dimX = static_cast<std::uint64_t>(volume.dims[0]);
    const auto dimY = static_cast<std::uint64_t>(volume.dims[1]);
    const auto dimZ = static_cast<std::uint64_t>(volume.dims[2]);
    if (volume.dims[0] <= 0 || volume.dims[1] <= 0
        || volume.dims[2] <= 0
        || dimX > (std::numeric_limits<std::uint64_t>::max)() / dimY
        || dimX * dimY
            > (std::numeric_limits<std::uint64_t>::max)() / dimZ) {
        return false;
    }
    const auto voxelCount = dimX * dimY * dimZ;
    if (voxelCount > static_cast<std::uint64_t>(
            (std::numeric_limits<vtkIdType>::max)())) {
        return false;
    }
    result.expectedLabelCount = voxelCount;

    int imageDims[3] = {};
    int imageExtent[6] = {};
    double imageSpacing[3] = {};
    double imageOrigin[3] = {};
    image->GetDimensions(imageDims);
    image->GetExtent(imageExtent);
    image->GetSpacing(imageSpacing);
    image->GetOrigin(imageOrigin);
    auto* scalars = image->GetPointData()
        ? image->GetPointData()->GetScalars() : nullptr;
    if (!scalars || scalars->GetDataType() != VTK_FLOAT
        || scalars->GetNumberOfComponents() != 1
        || scalars->GetNumberOfTuples()
            != static_cast<vtkIdType>(voxelCount)
        || scalars->GetVoidPointer(0) != volume.GetVoxelData()) {
        return false;
    }

    GapKernelRequest request{};
    request.abiVersion = GapKernelAbiVersion;
    request.structSize = static_cast<std::uint32_t>(
        sizeof(GapKernelRequest));
    request.voxelData = volume.GetVoxelData();
    request.voxelCount = voxelCount;
    for (int axis = 0; axis < 3; ++axis) {
        if (imageDims[axis] != volume.dims[axis]
            || imageSpacing[axis] != volume.spacing[axis]
            || imageOrigin[axis] != volume.origin[axis]) {
            return false;
        }
        request.dims[axis] = imageDims[axis];
        request.spacing[axis] = imageSpacing[axis];
        request.origin[axis] = imageOrigin[axis];
    }
    std::copy_n(imageExtent, 6, request.extent);

    const auto* direction = image->GetDirectionMatrix();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            request.direction[row * 3 + column] = direction
                ? direction->GetElement(row, column)
                : static_cast<double>(row == column);
        }
    }
    request.backgroundMean = params.surfParams.background;
    request.materialMean = params.surfParams.material;
    request.isoThreshold = params.surfParams.isoValue;
    request.minVolumeMM3 = static_cast<float>(
        params.voidParams.minVolumeMM3);
    request.numberRuns = 1;
    request.isFilterEnabled = params.voidParams.isFilterEnabled ? 1U : 0U;

    const auto kernelPath = GetKernelPath();
    if (!kernelPath) {
        std::cerr
            << "[GapAnalysis] A unique private kernel runtime was not found.\n"
            << std::flush;
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    KernelModule module(LoadLibraryExW(
        kernelPath->c_str(),
        nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH));
    if (!module.Get()) {
        const DWORD error = GetLastError();
        std::wcerr
            << L"[GapAnalysis] Private kernel load failed: "
            << kernelPath->native()
            << L" | win32=" << error << L'\n' << std::flush;
        return false;
    }

    // 完整路径锁定 bridge 本体；加载后再核对实际模块与同目录 DefX，
    // 防止已加载的同名依赖绕过 runtime 对选择。
    const auto loadedKernelPath = GetModulePath(module.Get());
    const HMODULE defxModule = GetModuleHandleW(L"DefXAnalysis.dll");
    const auto loadedDefxPath = defxModule
        ? GetModulePath(defxModule) : std::nullopt;
    const auto expectedDefxPath =
        kernelPath->parent_path() / L"DefXAnalysis.dll";
    if (!loadedKernelPath
        || !GetPathEqual(*kernelPath, *loadedKernelPath)
        || !loadedDefxPath
        || !GetPathEqual(expectedDefxPath, *loadedDefxPath)) {
        std::cerr
            << "[GapAnalysis] Private kernel runtime identity mismatch.\n"
            << std::flush;
        return false;
    }

    std::wcerr
        << L"[GapAnalysis] Private kernel loaded: "
        << loadedKernelPath->native() << L'\n' << std::flush;
    const auto buildResult = reinterpret_cast<GapKernelEntry>(
        GetProcAddress(module.Get(), "BuildGapResult"));
    if (!buildResult) {
        std::cerr
            << "[GapAnalysis] Private kernel entry is unavailable.\n"
            << std::flush;
        return false;
    }
    const bool isBuilt = buildResult
        (
            &request,
            &GapAnalysisService::Impl::SetKernelResult,
            &result) != 0;
    if (!isBuilt) {
        result = {};
    }
    return isBuilt;
}
bool GapAnalysisService::Impl::BuildResultPayload(
    const KernelBatch& batch,
    vtkImageData* inputImage,
    GapAnalysisResult& result) const
{
    result = {};
    if (!inputImage
        || batch.header.regionCount < 0
        || static_cast<std::uint64_t>(batch.header.regionCount)
            != static_cast<std::uint64_t>(batch.regions.size())
        || batch.header.totalVoxelCount < 0) {
        return false;
    }

    int inputDims[3] = {};
    int inputExtent[6] = {};
    inputImage->GetDimensions(inputDims);
    inputImage->GetExtent(inputExtent);
    std::uint64_t voxelCount = 1;
    for (int axis = 0; axis < 3; ++axis) {
        if (inputDims[axis] <= 0
            || batch.header.dims[axis] != inputDims[axis]) {
            return false;
        }
        const auto dimension = static_cast<std::uint64_t>(
            inputDims[axis]);
        if (voxelCount
            > (std::numeric_limits<std::uint64_t>::max)()
                / dimension) {
            return false;
        }
        voxelCount *= dimension;
    }
    if (batch.labels.size() != voxelCount
        || static_cast<std::uint64_t>(
            batch.header.totalVoxelCount) > voxelCount) {
        return false;
    }

    const float headerValues[]{
        batch.header.materialMean,
        batch.header.materialStd,
        batch.header.airMean,
        batch.header.airStd,
        batch.header.computedThreshold,
        batch.header.materialVolumeMM3,
        batch.header.defectVolumeMM3,
        batch.header.defectVolumeRatio,
        batch.header.objectMean,
        batch.header.objectStd
    };
    for (const float value : headerValues) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    if (batch.header.materialVolumeMM3 < 0.0f
        || batch.header.defectVolumeMM3 < 0.0f
        || batch.header.defectVolumeRatio < 0.0f) {
        return false;
    }

    GapAnalysisResult candidate;
    candidate.voids.reserve(batch.regions.size());
    std::unordered_set<std::int32_t> regionIds;
    std::uint64_t voidVoxelCount = 0;
    for (const auto& source : batch.regions) {
        if (m_isStopping.load()) {
            return false;
        }
        if (source.id <= 0 || source.voxelCount < 0
            || !regionIds.insert(source.id).second) {
            return false;
        }

        const float regionValues[]{
            source.volumeMM3,
            source.equivalentDiameterMM,
            source.radiusMM,
            source.diameterMM,
            source.minGray,
            source.maxGray,
            source.meanGray,
            source.stdDevGray,
            source.grayDeviation,
            source.gapMM,
            source.compactness,
            source.surfaceAreaMM2,
            source.sphericity,
            source.pcaDeviation1,
            source.pcaDeviation2,
            source.pcaDeviation3,
            source.pcaMaxDeviationRatio,
            source.pcaMinDeviationRatio,
            source.projectedAreaXMM2,
            source.projectedAreaYMM2,
            source.projectedAreaZMM2,
            source.projectedSizeX,
            source.projectedSizeY,
            source.projectedSizeZ,
            source.defectProbability
        };
        for (const float value : regionValues) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        for (const double value : source.centerMM) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        if (source.volumeMM3 < 0.0f
            || source.equivalentDiameterMM < 0.0f
            || source.radiusMM < 0.0f
            || source.diameterMM < 0.0f
            || source.surfaceAreaMM2 < 0.0f) {
            return false;
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (source.bbox[axis * 2] > source.bbox[axis * 2 + 1]
                || source.bbox[axis * 2] < inputExtent[axis * 2]
                || source.bbox[axis * 2 + 1]
                    > inputExtent[axis * 2 + 1]
                || source.seedVoxel[axis] < inputExtent[axis * 2]
                || source.seedVoxel[axis]
                    > inputExtent[axis * 2 + 1]) {
                return false;
            }
        }
        const auto count = static_cast<std::uint64_t>(
            source.voxelCount);
        if (voidVoxelCount
            > (std::numeric_limits<std::uint64_t>::max)() - count) {
            return false;
        }
        voidVoxelCount += count;

        VoidRegion region;
        region.id = source.id;
        region.voxelCount = source.voxelCount;
        region.volumeMM3 = source.volumeMM3;
        region.equivalentDiameterMM = source.equivalentDiameterMM;
        region.radiusMM = source.radiusMM;
        region.diameterMM = source.diameterMM;
        std::copy_n(source.centerMM, 3, region.centerMM.begin());
        std::copy_n(source.centroidMM, 3, region.centroidMM.begin());
        std::copy_n(source.bbox, 6, region.bbox.begin());
        std::copy_n(source.seedVoxel, 3, region.seedVoxel.begin());
        region.minGray = source.minGray;
        region.maxGray = source.maxGray;
        region.meanGray = source.meanGray;
        region.stdDevGray = source.stdDevGray;
        region.grayDeviation = source.grayDeviation;
        region.gapMM = source.gapMM;
        region.compactness = source.compactness;
        region.surfaceAreaMM2 = source.surfaceAreaMM2;
        region.sphericity = source.sphericity;
        region.pcaDeviation = {
            source.pcaDeviation1,
            source.pcaDeviation2,
            source.pcaDeviation3
        };
        region.pcaMaxDeviationRatio =
            source.pcaMaxDeviationRatio;
        region.pcaMinDeviationRatio =
            source.pcaMinDeviationRatio;
        region.projectedAreaXMM2 = source.projectedAreaXMM2;
        region.projectedAreaYMM2 = source.projectedAreaYMM2;
        region.projectedAreaZMM2 = source.projectedAreaZMM2;
        region.projectedSize = {
            source.projectedSizeX,
            source.projectedSizeY,
            source.projectedSizeZ
        };
        region.defectProbability = source.defectProbability;
        candidate.voids.push_back(region);
    }

    std::unordered_map<std::int32_t, std::uint64_t> labelCounts;
    for (std::size_t index = 0; index < batch.labels.size(); ++index) {
        if ((index & 4095U) == 0U && m_isStopping.load()) {
            return false;
        }
        const auto label = batch.labels[index];
        if (label < 0
            || (label > 0
                && regionIds.find(label) == regionIds.end())) {
            return false;
        }
        if (label > 0) {
            auto& count = labelCounts[label];
            if (count
                == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            ++count;
        }
    }
    for (const auto& region : batch.regions) {
        const auto iterator = labelCounts.find(region.id);
        const std::uint64_t labelCount = iterator == labelCounts.end()
            ? 0 : iterator->second;
        if (labelCount != static_cast<std::uint64_t>(
                region.voxelCount)) {
            return false;
        }
    }

    const auto totalVoxelCount = static_cast<std::uint64_t>(
        batch.header.totalVoxelCount);
    if (voidVoxelCount > totalVoxelCount
        || totalVoxelCount
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || voidVoxelCount
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return false;
    }

    candidate.labelImage = BuildLabelImage(
        batch.labels, inputImage);
    if (!candidate.labelImage) {
        return false;
    }
    candidate.statistics.objectVoxelCount =
        static_cast<std::size_t>(
            totalVoxelCount - voidVoxelCount);
    candidate.statistics.voidVoxelCount =
        static_cast<std::size_t>(voidVoxelCount);
    candidate.statistics.objectVolumeMM3 =
        static_cast<double>(batch.header.materialVolumeMM3);
    candidate.statistics.voidVolumeMM3 =
        static_cast<double>(batch.header.defectVolumeMM3);
    candidate.statistics.porosityRatio =
        static_cast<double>(batch.header.defectVolumeRatio);
    result = std::move(candidate);
    return true;
}

bool GapAnalysisService::Impl::GetRequestValid(
    const GapSurfaceConfig& surface,
    const GapVoidParams& voidParams) const
{
    if (!std::isfinite(surface.dataRangeRatio)
        || !std::isfinite(surface.absoluteIsoValue)
        || !std::isfinite(surface.backgroundMean)
        || !std::isfinite(surface.materialMean)
        || !std::isfinite(voidParams.minVolumeMM3)) {
        return false;
    }

    switch (surface.isoMode) {
    case GapIsoMode::DataRangeRatio:
        if (surface.dataRangeRatio < 0.0
            || surface.dataRangeRatio > 1.0) {
            return false;
        }
        break;
    case GapIsoMode::AbsoluteValue:
        break;
    default:
        return false;
    }

    return surface.backgroundMean <= surface.materialMean
        && voidParams.minVolumeMM3 >= 0.0
        && voidParams.minVolumeMM3
            <= static_cast<double>(
                (std::numeric_limits<float>::max)())
        && (voidParams.minVolumeMM3 == 0.0
            || voidParams.minVolumeMM3
                >= static_cast<double>(
                    (std::numeric_limits<float>::denorm_min)()));
}

bool GapAnalysisService::Impl::BuildInputBuffer(
    vtkSmartPointer<vtkImageData> image,
    GapInputBuffer& out) const
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
    if (!scalars || scalars->GetNumberOfComponents() != 1
        || scalars->GetDataType() != VTK_FLOAT) {
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
        || scalars->GetNumberOfTuples()
            != static_cast<vtkIdType>(expectedCount)) {
        return false;
    }

    out.dims = { dims[0], dims[1], dims[2] }; // voxel index 布局为 [x, y, z]。

    double spacing[3] = { 1.0, 1.0, 1.0 };
    image->GetSpacing(spacing);
    // 此处信任 DataManager 生产的 spacing/origin 已合法；DefX 请求入口会再次核对 VTK 几何。
    out.spacing = { spacing[0], spacing[1], spacing[2] }; // 输入 physical 坐标每 voxel 间距，沿 [x, y, z]。

    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetOrigin(origin);
    out.origin = { origin[0], origin[1], origin[2] }; // 输入 physical 坐标原点，沿 [x, y, z]。

    // BuildInputSnapshot 已把所有标量归一到同一份连续 float 快照；这里只扫描数值域并建立只读别名。
    const auto* source = static_cast<const float*>(
        scalars->GetVoidPointer(0));
    if (!source) {
        return false;
    }
    float minValue = (std::numeric_limits<float>::max)();
    float maxValue = (std::numeric_limits<float>::lowest)();
    for (std::size_t index = 0; index < expectedCount; ++index) {
        if (!std::isfinite(source[index])) {
            return false;
        }
        minValue = (std::min)(minValue, source[index]);
        maxValue = (std::max)(maxValue, source[index]);
    }
    out.minVal = minValue;
    out.maxVal = maxValue;
    try {
        std::shared_ptr<const void> imageOwner =
            std::make_shared<vtkSmartPointer<vtkImageData>>(
                std::move(image));
        return out.SetVoxels(std::move(imageOwner), source);
    }
    catch (const std::bad_alloc&) {
        return false;
    }
}

vtkSmartPointer<vtkImageData> GapAnalysisService::Impl::BuildLabelImage(
    const std::vector<std::int32_t>& labelVolume,
    vtkImageData* inputImage) const
{
    // 原始标签值和 x-fast 布局保持不变；CopyStructure 保留完整 extent、spacing、
    // origin 与 direction，避免 Feature 重新构造几何后产生 overlay 错位。
    if (!inputImage || labelVolume.empty()
        || labelVolume.size()
            > static_cast<std::size_t>(
                (std::numeric_limits<vtkIdType>::max)())
        || inputImage->GetNumberOfPoints()
            != static_cast<vtkIdType>(labelVolume.size())) {
        return nullptr;
    }

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->CopyStructure(inputImage);
    image->AllocateScalars(VTK_INT, 1);
    auto* labelPtr = static_cast<int*>(image->GetScalarPointer());
    if (!labelPtr) {
        return nullptr;
    }

    static_assert(sizeof(int) == sizeof(std::int32_t));
    std::copy(labelVolume.begin(), labelVolume.end(), labelPtr);
    image->Modified();
    return image;
}
