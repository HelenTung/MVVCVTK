#pragma once
// =====================================================================
// GapAnalysisService.h — 间隙分析接入层
//
// 职责：桥接 MVVCVTK 数据层与 GapAnalysis 算法层。
//   【前处理】SetParams()：仅写参数，无 VTK 操作
//   【计算】  Run()：纯同步计算，由外部（后台线程）调用
//   【后处理】GetResult()：主线程消费结果，生成 vtkPolyData 供 Strategy 使用
//
// 层次：
//   AbstractDataManager（DataManager 层）
//        ↓ GetVtkImage()
//   GapAnalysisService（算法调度层，无渲染）
//        ↓ GapAnalysisResult
//   MedicalVizService::PostData_RebuildPipeline（渲染层消费）
// =====================================================================

#include "GapAnalysisTypes.h"
#include "VolumeBuffer.h"
#include "VoidDetector.h"
#include "SurfaceRefiner.h"
#include "AppInterfaces.h"

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkImageImport.h>
#include <vtkFlyingEdges3D.h>  // 与项目统一，用 FlyingEdges 替代 MarchingCubes
#include <mutex>
#include <memory>
#include <atomic>

class GapAnalysisService {
public:
    explicit GapAnalysisService(std::shared_ptr<AbstractDataManager> dataMgr)
        : m_dataMgr(std::move(dataMgr))
    {
    }

    // ================================================================
    // 【前处理】— 仅写参数，零 VTK 操作，线程安全
    // 调用时机：数据加载之前或之后均可（与 PreInit_* 同期调用）
    // ================================================================
    void PreInit_SetSurfaceParams(const SurfaceParams& p) {
        std::lock_guard<std::mutex> lk(m_paramsMutex);
        m_surfParams = p;
    }
    void PreInit_SetAdvancedParams(const AdvancedSurfaceParams& p) {
        std::lock_guard<std::mutex> lk(m_paramsMutex);
        m_advParams = p;
    }
    void PreInit_SetVoidDetectionParams(const VoidDetectionParams& p) {
        std::lock_guard<std::mutex> lk(m_paramsMutex);
        m_voidParams = p;
    }

    // ================================================================
    // 【计算】— 纯同步，在后台线程中调用
    // 由 MedicalVizService::LoadFileAsync 的 onComplete 回调触发
    // ================================================================
    bool Run() {
        auto img = m_dataMgr ? m_dataMgr->GetVtkImage() : nullptr;
        if (!img) return false;

        // 将 vtkImageData 转换为 VolumeBuffer（前处理数据桥接）
        if (!BuildVolumeBuffer(img)) return false;

        // 取快照（避免计算期间参数被改写）
        SurfaceParams         surfP;
        AdvancedSurfaceParams advP;
        VoidDetectionParams   voidP;
        {
            std::lock_guard<std::mutex> lk(m_paramsMutex);
            surfP = m_surfParams;
            advP = m_advParams;
            voidP = m_voidParams;
        }

        // Step 1: 内部掩码
        auto interior = VoidDetector::CreateInteriorMask(m_volBuf, surfP.isoValue);
        if (m_cancelFlag.load()) return false;

        // Step 2: 候选空洞
        auto candidates = VoidDetector::ExtractCandidates(m_volBuf, interior, voidP);
        if (m_cancelFlag.load()) return false;

        // Step 3: 连通域分析
        GapAnalysisResult result;
        result.voids = VoidDetector::LabelAndAnalyze(
            m_volBuf, candidates, voidP, result.labelVolume);
        result.succeeded = true;

        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_result = std::move(result);
        return true;
    }

    void RequestCancel() { m_cancelFlag.store(true); }
    void ResetCancel() { m_cancelFlag.store(false); }

    // ================================================================
    // 【后处理】— 主线程消费，生成 vtkPolyData 供 Strategy 使用
    // ================================================================

    // 获取空洞统计列表（供 UI 显示）
    std::vector<VoidRegion> GetVoidRegions() const {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        return m_result.voids;
    }

    // 生成空洞 Mesh（vtkPolyData），供 IsoSurfaceStrategy::SetInputData 消费
    vtkSmartPointer<vtkPolyData> BuildVoidMesh() const {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        if (!m_result.succeeded || m_result.labelVolume.empty()) return nullptr;

        auto importer = vtkSmartPointer<vtkImageImport>::New();
        importer->SetDataScalarTypeToInt();
        importer->SetNumberOfScalarComponents(1);
        importer->SetWholeExtent(0, m_volBuf.dims[0] - 1,
            0, m_volBuf.dims[1] - 1,
            0, m_volBuf.dims[2] - 1);
        importer->SetDataExtentToWholeExtent();
        importer->SetDataSpacing(m_volBuf.spacing[0], m_volBuf.spacing[1], m_volBuf.spacing[2]);
        importer->SetDataOrigin(m_volBuf.origin[0], m_volBuf.origin[1], m_volBuf.origin[2]);
        importer->SetImportVoidPointer(
            const_cast<int*>(m_result.labelVolume.data()));
        importer->Update();

        // 与项目统一使用 vtkFlyingEdges3D（替代 vtkMarchingCubes）
        auto fe = vtkSmartPointer<vtkFlyingEdges3D>::New();
        fe->SetInputConnection(importer->GetOutputPort());
        fe->SetValue(0, 0.5);
        fe->ComputeNormalsOff();
        fe->Update();
        return fe->GetOutput();
    }

    bool HasResult() const {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        return m_result.succeeded;
    }

private:
    std::shared_ptr<AbstractDataManager> m_dataMgr;

    mutable std::mutex  m_paramsMutex;
    SurfaceParams         m_surfParams;
    AdvancedSurfaceParams m_advParams;
    VoidDetectionParams   m_voidParams;

    mutable std::mutex  m_resultMutex;
    GapAnalysisResult   m_result;

    std::atomic<bool>   m_cancelFlag{ false };
    VolumeBuffer        m_volBuf;

    // 桥接：从 vtkImageData 填充 VolumeBuffer
    bool BuildVolumeBuffer(vtkSmartPointer<vtkImageData> img) {
        if (!img) return false;
        int dims[3]; img->GetDimensions(dims);
        m_volBuf.dims = { dims[0], dims[1], dims[2] };
        double sp[3];    img->GetSpacing(sp);
        m_volBuf.spacing = { sp[0], sp[1], sp[2] };
        double og[3];    img->GetOrigin(og);
        m_volBuf.origin = { og[0], og[1], og[2] };

        size_t total = (size_t)dims[0] * dims[1] * dims[2];
        m_volBuf.voxels.resize(total);
        for (size_t i = 0; i < total; ++i) {
            int x = (int)(i % dims[0]);
            int y = (int)((i / dims[0]) % dims[1]);
            int z = (int)(i / ((size_t)dims[0] * dims[1]));
            m_volBuf.voxels[i] = img->GetScalarComponentAsFloat(x, y, z, 0);
        }
        auto [mn, mx] = std::minmax_element(m_volBuf.voxels.begin(), m_volBuf.voxels.end());
        m_volBuf.minVal = *mn;
        m_volBuf.maxVal = *mx;
        return true;
    }
};