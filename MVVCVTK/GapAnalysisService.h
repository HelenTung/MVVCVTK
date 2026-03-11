#pragma once
// =====================================================================
// GapAnalysisService.h — IGapAnalysisService 实现
// =====================================================================

#include "GapAnalysisTypes.h"
#include "VolumeBuffer.h"
#include "VoidDetector.h"
#include "SurfaceRefiner.h"
#include "AppInterfaces.h"

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkIntArray.h>
#include <vtkPointData.h>
#include <vtkFlyingEdges3D.h>

#include <mutex>
#include <future>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

class GapAnalysisService : public IGapAnalysisService {
public:
    explicit GapAnalysisService(std::shared_ptr<AbstractDataManager> dataMgr)
        : m_dataMgr(std::move(dataMgr)) {
    }

    ~GapAnalysisService() {
        m_cancelFlag.store(true);
        std::lock_guard<std::mutex> lk(m_futureMutex);
    }

    // ================================================================
    // 【前处理】— 只写参数，零计算，线程安全
    // ================================================================
    void GapPreInit_SetSurfaceParams(const SurfaceParams& p) override {
        std::lock_guard<std::mutex> lk(m_paramsMutex);
        m_surfParams = p;
    }
    void GapPreInit_SetAdvancedParams(const AdvancedSurfaceParams& p) override {
        std::lock_guard<std::mutex> lk(m_paramsMutex);
        m_advParams = p;
    }
    void GapPreInit_SetVoidParams(const VoidDetectionParams& p) override {
        std::lock_guard<std::mutex> lk(m_paramsMutex);
        m_voidParams = p;
    }

    // ================================================================
    // 【触发】— 主动发起后台计算，对齐 LoadFileAsync 线程模式
    // ================================================================
    void RunAsync( // MAIN
        std::function<void(bool success)> onComplete = nullptr) override
    {
        if (m_analysisState.load() == static_cast<int>(GapAnalysisState::Running))
            return;

        auto img = m_dataMgr ? m_dataMgr->GetVtkImage() : nullptr;
        if (!img) {
            m_analysisState.store(static_cast<int>(GapAnalysisState::Failed));
            if (onComplete) onComplete(false);
            return;
        }

        m_cancelFlag.store(false);
        m_analysisState.store(static_cast<int>(GapAnalysisState::Running));

        SurfaceParams         surfP;
        AdvancedSurfaceParams advP;
        VoidDetectionParams   voidP;
        {
            std::lock_guard<std::mutex> lk(m_paramsMutex);
            surfP = m_surfParams;
            advP = m_advParams;
            voidP = m_voidParams;
        }

        std::packaged_task<void()> task(
            [this, img, surfP, advP, voidP, onComplete]() mutable
            {
                bool ok = false;
                if (m_cancelFlag.load()) {
                    m_analysisState.store(static_cast<int>(GapAnalysisState::Idle));
                    if (onComplete) onComplete(false);
                    return;
                }

                VolumeBuffer volBuf;
                if (BuildVolumeBuffer(img, volBuf) && !m_cancelFlag.load()) {
                    auto interior = VoidDetector::CreateInteriorMask(volBuf, surfP.isoValue);
                    if (!m_cancelFlag.load()) {
                        auto candidates = VoidDetector::ExtractCandidates(volBuf, interior, voidP);
                        if (!m_cancelFlag.load()) {
                            GapAnalysisResult result;
                            result.voids = VoidDetector::LabelAndAnalyze(
                                volBuf, candidates, voidP, result.labelVolume);
                            result.succeeded = true;
                            {
                                std::lock_guard<std::mutex> lk(m_resultMutex);
                                m_result = std::move(result);
                                m_volBufSnap = std::move(volBuf);
                            }
                            ok = true;
                        }
                    }
                }

                m_analysisState.store(ok
                    ? static_cast<int>(GapAnalysisState::Succeeded)
                    : static_cast<int>(GapAnalysisState::Failed));
                if (onComplete) onComplete(ok);
            });

        {
            std::lock_guard<std::mutex> lk(m_futureMutex);
            m_future = task.get_future();
        }
        std::thread(std::move(task)).detach();
    }

    void CancelRun() override { m_cancelFlag.store(true); }

    // ================================================================
    // 【查询】
    // ================================================================
    GapAnalysisState GetAnalysisState() const override {
        return static_cast<GapAnalysisState>(m_analysisState.load());
    }

    // ================================================================
    // 【后处理】— 主线程消费，在 PostData 阶段调用
    // ================================================================
    std::vector<VoidRegion> GetVoidRegions() const override {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        return m_result.voids;
    }

    // ── 3D：空洞等值面 Mesh（喂给 IsoSurfaceStrategy）──────────────
    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const override {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        if (!m_result.succeeded || m_result.labelVolume.empty())
            return nullptr;

        auto img = BuildLabelImageInternal();
        if (!img) return nullptr;

        auto fe = vtkSmartPointer<vtkFlyingEdges3D>::New();
        fe->SetInputData(img);
        fe->SetValue(0, 0.5);   // label > 0 即为空洞区域
        fe->ComputeNormalsOff();
        fe->Update();
        return fe->GetOutput();
    }

    // ── 2D：标签体 vtkImageData（喂给 SliceStrategy）───────────────
    // label=0 → 背景（透明）；label>0 → 空洞编号
    // SliceStrategy::SetInputData 接受 vtkImageData，直接传入即可。
    // 调用时机：主线程，GetAnalysisState() == Succeeded 之后
    vtkSmartPointer<vtkImageData> BuildLabelImage() const {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        return BuildLabelImageInternal();
    }

private:
    std::shared_ptr<AbstractDataManager> m_dataMgr;

    mutable std::mutex    m_paramsMutex;
    SurfaceParams         m_surfParams;
    AdvancedSurfaceParams m_advParams;
    VoidDetectionParams   m_voidParams;

    mutable std::mutex  m_resultMutex;
    GapAnalysisResult   m_result;
    VolumeBuffer        m_volBufSnap;

    std::atomic<int>    m_analysisState{ static_cast<int>(GapAnalysisState::Idle) };
    std::atomic<bool>   m_cancelFlag{ false };
    mutable std::mutex  m_futureMutex;
    std::future<void>   m_future;

    // ── 内部辅助：labelVolume → vtkImageData（调用方持有 m_resultMutex）
    vtkSmartPointer<vtkImageData> BuildLabelImageInternal() const {
        if (!m_result.succeeded || m_result.labelVolume.empty())
            return nullptr;

        const auto& b = m_volBufSnap;
        auto img = vtkSmartPointer<vtkImageData>::New();
        img->SetDimensions(b.dims[0], b.dims[1], b.dims[2]);
        img->SetSpacing(b.spacing[0], b.spacing[1], b.spacing[2]);
        img->SetOrigin(b.origin[0], b.origin[1], b.origin[2]);
        img->AllocateScalars(VTK_INT, 1);

        int* ptr = static_cast<int*>(img->GetScalarPointer());
        const size_t total = (size_t)b.dims[0] * b.dims[1] * b.dims[2];
        std::copy(m_result.labelVolume.begin(),
            m_result.labelVolume.begin() + (std::ptrdiff_t)total,
            ptr);
        img->Modified();
        return img;
    }

	// ── 内部辅助：vtkImageData → VolumeBuffer（调用方持有 m_resultMutex）
    static bool BuildVolumeBuffer(
        vtkSmartPointer<vtkImageData> img, VolumeBuffer& out)
    {
        if (!img) return false;
        int d[3]; img->GetDimensions(d);
        out.dims = { d[0], d[1], d[2] };
        double sp[3]; img->GetSpacing(sp);
        out.spacing = { sp[0], sp[1], sp[2] };
        double og[3]; img->GetOrigin(og);
        out.origin = { og[0], og[1], og[2] };

        const size_t total = (size_t)d[0] * d[1] * d[2];
        out.voxels.resize(total);
        for (size_t i = 0; i < total; ++i) {
            const int x = (int)(i % d[0]);
            const int y = (int)((i / d[0]) % d[1]);
            const int z = (int)(i / ((size_t)d[0] * d[1]));
            out.voxels[i] = img->GetScalarComponentAsFloat(x, y, z, 0);
        }
        auto [mn, mx] = std::minmax_element(out.voxels.begin(), out.voxels.end());
        out.minVal = *mn;
        out.maxVal = *mx;
        return true;
    }

public:
	// ── 保存结果（CSV + RAW），调试用，调用方持有 m_resultMutex
    bool saveResults(const std::string& baseName) {
        std::string csvPath = baseName + "_voids.csv";
        FILE* fp = fopen(csvPath.c_str(), "w");
        if (fp) {
           
            fprintf(fp, "ID,VoxelCount,Volume(mm3),EquivDiameter(mm)\n");
            for (const auto& v : m_result.voids)
                fprintf(fp, "%d,%llu,%.4f,%.4f\n", v.id, (unsigned long long)v.voxelCount, v.volumeMM3, v.equivalentDiameterMM); 
            fclose(fp);
        }

        std::string rawPath = baseName + "_label.raw";
        FILE* fr = fopen(rawPath.c_str(), "wb");
        if (fr) { fwrite(m_result.labelVolume.data(), sizeof(int), m_result.labelVolume.size(), fr); fclose(fr); }
        return true;
    }
};