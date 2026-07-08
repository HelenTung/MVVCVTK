#include "Services/GapAnalysisService.h"

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
#include <exception>
#include <iostream>
#include <new>
#include <utility>

class GapAnalysisService::Impl final {
public:
    void SetCompletionCallback(std::function<void(bool)> callback);
    void SetCallbackReady(bool isSuccess);
    bool GetDoneEvent();
    void SendCallback();

    bool GetMeshVisible(vtkSmartPointer<vtkPolyData> voidMesh) const;
    bool GetLabelExtent(vtkSmartPointer<vtkImageData> labelImage) const;
    bool BuildVolumeBuffer(
        vtkSmartPointer<vtkImageData> image,
        VolumeBuffer& out) const;
    vtkSmartPointer<vtkImageData> BuildLabelImage(
        const std::vector<int>& labelVolume,
        const VolumeBuffer& volBuf) const;

private:
    std::mutex m_callbackMutex;
    std::function<void(bool)> m_completionCallback;
    std::function<void(bool)> m_nextCallback;
    bool m_isNextOk = false;
    std::atomic<bool> m_hasCallback{ false };
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

    m_nextCallback = std::move(m_completionCallback);
    m_completionCallback = nullptr;
    m_isNextOk = isSuccess;
    m_hasCallback.store(true);
}

bool GapAnalysisService::Impl::GetDoneEvent()
{
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

GapAnalysisService::GapAnalysisService()
    : m_impl(std::make_unique<GapAnalysisService::Impl>())
{
}

GapAnalysisService::~GapAnalysisService() {
    ExitView();
    StopAsync();
    StopWorker();
}

bool GapAnalysisService::SetInputImage(vtkSmartPointer<vtkImageData> image) {
    VolumeBuffer snapshot;
    if (!m_impl->BuildVolumeBuffer(std::move(image), snapshot)) {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        m_inputSnapshot.reset();
        if (GetAnalysisState() != GapAnalysisState::Running) {
            SetAnalysisState(GapAnalysisState::Failed);
        }
        return false;
    }

    auto snapshotPtr = std::make_shared<VolumeBuffer>(std::move(snapshot));
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        m_inputSnapshot = std::move(snapshotPtr);
    }
    if (GetAnalysisState() != GapAnalysisState::Running) {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_result = {};
        SetAnalysisState(GapAnalysisState::Idle);
    }
    return true;
}

void GapAnalysisService::SetSurface(const SurfaceParams& p) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_surfParams = p;
}

void GapAnalysisService::SetAdvanced(const AdvancedSurfaceParams& p) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_advParams = p;
}

void GapAnalysisService::SetVoid(const VoidDetectionParams& p) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_voidParams = p;
}

void GapAnalysisService::StartAsync(std::function<void(bool isSuccess)> onComplete) {
    std::lock_guard<std::mutex> workerLock(m_workerMutex);
    if (GetAnalysisState() == GapAnalysisState::Running) {
        return;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_impl->SetCompletionCallback(std::move(onComplete));

    auto inputSnapshot = GetInputSnapshot();
    if (!inputSnapshot || !inputSnapshot->GetVoxelReady()) {
        SetAnalysisState(GapAnalysisState::Failed);
        m_impl->SetCallbackReady(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_result = {};
    }

    m_isStopping.store(false);
    SetAnalysisState(GapAnalysisState::Running);

    // worker 只捕获不可变快照和参数副本；服务成员只用于写回最终状态和结果。
    // 这让后台计算和 App/DataManager/VTK 输入对象生命周期彻底脱钩。
    m_workerThread = std::thread(
        &GapAnalysisService::StartWorker,
        this,
        std::move(inputSnapshot),
        GetParamSnapshot());
}

void GapAnalysisService::StopAsync() {
    m_isStopping.store(true);
}

bool GapAnalysisService::GetDoneEvent() {
    return m_impl && m_impl->GetDoneEvent();
}

void GapAnalysisService::SendCallback() {
    if (m_impl) {
        m_impl->SendCallback();
    }
}

GapAnalysisState GapAnalysisService::GetAnalysisState() const {
    return static_cast<GapAnalysisState>(m_analysisState.load());
}

std::vector<VoidRegion> GapAnalysisService::GetVoidRegions() const {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    return m_result.voids;
}

vtkSmartPointer<vtkPolyData> GapAnalysisService::BuildVoidMesh() const {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    if (!m_result.isSucceeded || !m_result.labelImage) {
        return nullptr;
    }

    auto fe = vtkSmartPointer<vtkFlyingEdges3D>::New();
    fe->SetInputData(m_result.labelImage);
    fe->SetValue(0, 0.5); // label > 0 即为空洞区域
    fe->ComputeNormalsOff();
    fe->Update();
    return fe->GetOutput();
}

vtkSmartPointer<vtkImageData> GapAnalysisService::BuildLabelImage() const {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    return m_result.labelImage;
}

bool GapAnalysisService::StartView(
    const GapAnalysisSurfaceRequest& surfaceRequest,
    const VoidDetectionParams& voidParams,
    const std::vector<std::shared_ptr<AbstractAppService>>& meshOverlayTargets,
    const std::vector<std::pair<Orientation, std::shared_ptr<AbstractAppService>>>& sliceOverlayTargets,
    std::function<void(double isoValue)> onIsoValueResolved) {
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
        return false;
    }

    m_displayVoidMesh = nullptr;
    m_displayLabelImage = nullptr;
    m_displaySurfaceRequest = surfaceRequest;
    m_displayVoidParams = voidParams;
    m_isoCallback = std::move(onIsoValueResolved);
    m_isViewOn = true;
    m_isOverlayOn = true;
    m_hasRunRequest = true;
    m_hasDone = false;
    m_hasFailLog = false;
    std::cout << "[GapAnalysis] Display mode requested. Analysis will start after volume data is ready." << std::endl;
    return true;
}

bool GapAnalysisService::SwitchOverlay() {
    if (!m_isViewOn) {
        std::cerr << "[GapAnalysis] Overlay switch ignored: display mode is not active." << std::endl;
        return false;
    }

    m_isOverlayOn = !m_isOverlayOn;
    if (!m_isOverlayOn) {
        SetOverlayOff();
        std::cout << "[GapAnalysis] Overlays hidden. Use the host overlay switch command to show them again." << std::endl;
        return true;
    }

    if (!m_displayVoidMesh && !m_displayLabelImage) {
        std::cout << "[GapAnalysis] Overlays enabled. They will appear after analysis completes." << std::endl;
        return true;
    }

    return SetStoredView();
}

bool GapAnalysisService::ExitView() {
    const bool isActive = m_isViewOn;
    const bool hasCachedResult = m_displayVoidMesh != nullptr || m_displayLabelImage != nullptr;
    const bool hasRemoved = SetOverlayOff();
    if (isActive) {
        StopAsync();
    }
    ClearDisplayState();
    if (isActive || hasCachedResult || hasRemoved) {
        std::cout << "[GapAnalysis] Display mode exited. Void overlays are hidden." << std::endl;
    }
    return isActive || hasCachedResult || hasRemoved;
}

bool GapAnalysisService::GetViewOn() const {
    return m_isViewOn;
}

void GapAnalysisService::OnDisplayTick(vtkSmartPointer<vtkImageData> inputImage) {
    if (!m_isViewOn) {
        return;
    }

    if (m_hasRunRequest) {
        if (GetAnalysisState() != GapAnalysisState::Running
            && StartRun(std::move(inputImage))) {
            m_hasRunRequest = false;
            m_hasDone = false;
            m_hasFailLog = false;
        }
        return;
    }

    if (m_hasDone) {
        return;
    }

    const GapAnalysisState state = GetAnalysisState();
    if (state == GapAnalysisState::Idle || state == GapAnalysisState::Running) {
        return;
    }

    if (GetDoneEvent()) {
        SendCallback();
    }

    if (state == GapAnalysisState::Failed) {
        if (!m_hasFailLog) {
            std::cerr << "[GapAnalysis] Analysis failed; overlay will not be attached." << std::endl;
            m_hasFailLog = true;
        }
        m_hasDone = true;
        return;
    }

    SetDisplayView();
    m_hasDone = true;
}

GapAnalysisService::VolumeBufferSnapshot GapAnalysisService::GetInputSnapshot() const {
    std::lock_guard<std::mutex> lk(m_inputMutex);
    return m_inputSnapshot;
}

GapParamSnapshot GapAnalysisService::GetParamSnapshot() const {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    return { m_surfParams, m_advParams, m_voidParams };
}

void GapAnalysisService::StartWorker(
    VolumeBufferSnapshot inputSnapshot,
    GapParamSnapshot params) {
    bool isSuccess = false;

    if (!inputSnapshot || !inputSnapshot->GetVoxelReady() || m_isStopping.load()) {
        SetAnalysisState(GapAnalysisState::Idle);
        m_impl->SetCallbackReady(false);
        return;
    }

    try {
        const VolumeBuffer& volBuf = *inputSnapshot;

        auto interior = VoidDetector::CreateInteriorMask(volBuf, params.surfParams.isoValue);
        if (!m_isStopping.load()) {
            auto candidates = VoidDetector::BuildCandidates(volBuf, interior, params.voidParams);
            if (!m_isStopping.load()) {
                GapAnalysisResult result;
                result.voids = VoidDetector::BuildRegions(
                    volBuf,
                    candidates,
                    params.voidParams,
                    result.labelVolume);
                result.labelImage = m_impl->BuildLabelImage(result.labelVolume, volBuf);
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

    SetAnalysisState(isSuccess ? GapAnalysisState::Succeeded : GapAnalysisState::Failed);
    m_impl->SetCallbackReady(isSuccess);
}

void GapAnalysisService::StopWorker() {
    std::lock_guard<std::mutex> lk(m_workerMutex);
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void GapAnalysisService::SetAnalysisState(GapAnalysisState state) {
    m_analysisState.store(static_cast<int>(state));
}

bool GapAnalysisService::StartRun(vtkSmartPointer<vtkImageData> inputImage) {
    if (!SetInputImage(std::move(inputImage))) {
        return false;
    }

    const auto inputSnapshot = GetInputSnapshot();
    if (!inputSnapshot || !inputSnapshot->GetVoxelReady()) {
        return false;
    }

    const double isoValue = GetDisplayIso(*inputSnapshot);
    if (m_isoCallback) {
        m_isoCallback(isoValue);
    }

    SurfaceParams surfaceParams;
    surfaceParams.isoValue = static_cast<float>(isoValue);
    SetSurface(surfaceParams);
    SetVoid(m_displayVoidParams);
    StartAsync();
    return true;
}

void GapAnalysisService::SetDisplayView() {
    if (!m_isViewOn) {
        return;
    }

    m_displayVoidMesh = BuildVoidMesh();
    m_displayLabelImage = BuildLabelImage();
    SetOverlayOff();
    if (!m_isOverlayOn) {
        std::cout << "[GapAnalysis] Analysis completed, but overlays are hidden. Use the host overlay switch command to show them." << std::endl;
        return;
    }

    SetStoredView();
}

bool GapAnalysisService::SetOverlayOff() {
    bool hasRemoved = false;
    for (const auto& binding : m_displayOverlayBindings) {
        if (!binding.service || !binding.overlayStrategy) {
            continue;
        }
        binding.service->RemoveOverlayStrategy(binding.overlayStrategy);
        binding.service->SetDirty();
        hasRemoved = true;
    }
    m_displayOverlayBindings.clear();
    return hasRemoved;
}

bool GapAnalysisService::SetStoredView() {
    SetOverlayOff();

    const bool hasMeshInput = m_impl->GetMeshVisible(m_displayVoidMesh);
    const bool hasSliceInput = m_impl->GetLabelExtent(m_displayLabelImage);
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

void GapAnalysisService::ClearDisplayState() {
    m_meshTargets.clear();
    m_sliceTargets.clear();
    m_displayVoidMesh = nullptr;
    m_displayLabelImage = nullptr;
    m_displaySurfaceRequest = {};
    m_displayVoidParams = {};
    m_isoCallback = nullptr;
    m_isViewOn = false;
    m_isOverlayOn = false;
    m_hasRunRequest = false;
    m_hasDone = false;
    m_hasFailLog = false;
}

double GapAnalysisService::GetDisplayIso(const VolumeBuffer& inputSnapshot) const {
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

    const auto expectedCount = static_cast<std::size_t>(dims[0])
        * static_cast<std::size_t>(dims[1])
        * static_cast<std::size_t>(dims[2]);
    if (expectedCount == 0 || scalars->GetNumberOfTuples() < static_cast<vtkIdType>(expectedCount)) {
        return false;
    }

    std::vector<float> voxels;
    try {
        voxels.resize(expectedCount);
    }
    catch (const std::bad_alloc&) {
        return false;
    }

    // RawVolumeDataManager 当前产出 VTK_FLOAT；保留泛型路径是为了插件被其他宿主接入时不误判失败。
    if (scalars->GetDataType() == VTK_FLOAT) {
        const auto* source = static_cast<const float*>(scalars->GetVoidPointer(0));
        if (!source) {
            return false;
        }
        std::copy_n(source, expectedCount, voxels.data());
    }
    else {
        for (std::size_t index = 0; index < expectedCount; ++index) {
            voxels[index] = static_cast<float>(scalars->GetTuple1(static_cast<vtkIdType>(index)));
        }
    }

    out.dims = { dims[0], dims[1], dims[2] };

    double spacing[3] = { 1.0, 1.0, 1.0 };
    image->GetSpacing(spacing);
    out.spacing = { spacing[0], spacing[1], spacing[2] };

    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetOrigin(origin);
    out.origin = { origin[0], origin[1], origin[2] };

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
