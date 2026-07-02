#include "Services/GapAnalysisService.h"

#include "Algorithms/VoidDetector.h"

#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

class GapAnalysisCompletionCallbackState {
public:
    void SetCallback(std::function<void(bool)> callback) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_callback = std::move(callback);
    }

    void SetCallbackReady(bool success) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_callback) {
            return;
        }

        m_pendingCallback = std::move(m_callback);
        m_callback = nullptr;
        m_pendingResult = success;
        m_hasPendingCallback.store(true);
    }

    bool ConsumePendingCallback() {
        return m_hasPendingCallback.exchange(false);
    }

    void ExecutePendingCallback() {
        std::function<void(bool)> callback;
        bool success = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            callback = std::move(m_pendingCallback);
            m_pendingCallback = nullptr;
            success = m_pendingResult;
        }

        // 回调可能触发 UI、VTK 或服务调用；锁外执行避免插件内部状态和宿主回调互相重入死锁。
        if (callback) {
            callback(success);
        }
    }

private:
    std::mutex m_mutex;
    std::function<void(bool)> m_callback;
    std::function<void(bool)> m_pendingCallback;
    bool m_pendingResult{ false };
    std::atomic<bool> m_hasPendingCallback{ false };
};

GapAnalysisService::GapAnalysisService()
    : m_completionCallbackState(std::make_unique<GapAnalysisCompletionCallbackState>()) {
}

GapAnalysisService::~GapAnalysisService() {
    CancelRun();
    WaitForWorkerThread();
}

bool GapAnalysisService::SetInputImage(vtkSmartPointer<vtkImageData> image) {
    VolumeBuffer snapshot;
    if (!BuildVolumeBuffer(std::move(image), snapshot)) {
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

void GapAnalysisService::SetSurfaceParams(const SurfaceParams& p) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_surfParams = p;
}

void GapAnalysisService::SetAdvancedParams(const AdvancedSurfaceParams& p) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_advParams = p;
}

void GapAnalysisService::SetVoidParams(const VoidDetectionParams& p) {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    m_voidParams = p;
}

void GapAnalysisService::RunAsync(std::function<void(bool success)> onComplete) {
    std::lock_guard<std::mutex> workerLock(m_workerMutex);
    if (GetAnalysisState() == GapAnalysisState::Running) {
        return;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_completionCallbackState->SetCallback(std::move(onComplete));

    auto inputSnapshot = GetInputSnapshot();
    if (!inputSnapshot || !inputSnapshot->HasVoxelData()) {
        SetAnalysisState(GapAnalysisState::Failed);
        m_completionCallbackState->SetCallbackReady(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_result = {};
    }

    m_cancelFlag.store(false);
    SetAnalysisState(GapAnalysisState::Running);

    // worker 只捕获不可变快照和参数副本；服务成员只用于写回最终状态和结果。
    // 这让后台计算和 App/DataManager/VTK 输入对象生命周期彻底脱钩。
    m_workerThread = std::thread(
        &GapAnalysisService::RunAnalysisWorker,
        this,
        std::move(inputSnapshot),
        GetParameterSnapshot());
}

void GapAnalysisService::CancelRun() {
    m_cancelFlag.store(true);
}

bool GapAnalysisService::ConsumePendingCompletionCallback() {
    return m_completionCallbackState->ConsumePendingCallback();
}

void GapAnalysisService::ExecutePendingCompletionCallback() {
    m_completionCallbackState->ExecutePendingCallback();
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
    if (!m_result.succeeded || !m_result.labelImage) {
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

GapAnalysisService::VolumeBufferSnapshot GapAnalysisService::GetInputSnapshot() const {
    std::lock_guard<std::mutex> lk(m_inputMutex);
    return m_inputSnapshot;
}

GapAnalysisService::ParameterSnapshot GapAnalysisService::GetParameterSnapshot() const {
    std::lock_guard<std::mutex> lk(m_paramsMutex);
    return { m_surfParams, m_advParams, m_voidParams };
}

void GapAnalysisService::RunAnalysisWorker(
    VolumeBufferSnapshot inputSnapshot,
    ParameterSnapshot params) {
    bool ok = false;

    if (!inputSnapshot || !inputSnapshot->HasVoxelData() || m_cancelFlag.load()) {
        SetAnalysisState(GapAnalysisState::Idle);
        m_completionCallbackState->SetCallbackReady(false);
        return;
    }

    try {
        const VolumeBuffer& volBuf = *inputSnapshot;

        auto interior = VoidDetector::CreateInteriorMask(volBuf, params.surfParams.isoValue);
        if (!m_cancelFlag.load()) {
            auto candidates = VoidDetector::ExtractCandidates(volBuf, interior, params.voidParams);
            if (!m_cancelFlag.load()) {
                GapAnalysisResult result;
                result.voids = VoidDetector::LabelAndAnalyze(
                    volBuf,
                    candidates,
                    params.voidParams,
                    result.labelVolume);
                result.labelImage = BuildLabelImage(result.labelVolume, volBuf);
                result.succeeded = true;

                {
                    std::lock_guard<std::mutex> lk(m_resultMutex);
                    m_result = std::move(result);
                }
                ok = true;
            }
        }
    }
    catch (const std::exception&) {
        ok = false;
    }
    catch (...) {
        ok = false;
    }

    SetAnalysisState(ok ? GapAnalysisState::Succeeded : GapAnalysisState::Failed);
    m_completionCallbackState->SetCallbackReady(ok);
}

void GapAnalysisService::WaitForWorkerThread() {
    std::lock_guard<std::mutex> lk(m_workerMutex);
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void GapAnalysisService::SetAnalysisState(GapAnalysisState state) {
    m_analysisState.store(static_cast<int>(state));
}

bool GapAnalysisService::BuildVolumeBuffer(
    vtkSmartPointer<vtkImageData> image,
    VolumeBuffer& out) {
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

vtkSmartPointer<vtkImageData> GapAnalysisService::BuildLabelImage(
    const std::vector<int>& labelVolume,
    const VolumeBuffer& volBuf) {
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
