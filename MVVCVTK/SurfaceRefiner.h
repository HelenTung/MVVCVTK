#pragma once
// =====================================================================
// SurfaceRefiner.h — 沿表面法向精化顶点位置（纯算法）
// =====================================================================

#include "VolumeBuffer.h"
#include "GapAnalysisTypes.h"
#include <vtkSmartPointer.h>
#include <vtkPolyData.h>
#include <vtkPointData.h>
#include <vtkSmoothPolyDataFilter.h>
#include <vtkSMPTools.h>
#include <cmath>
#include <vector>

class SurfaceRefiner {
public:
    // ── 主入口：沿法向搜索优化顶点边界位置 ─────────────────────────
    static vtkSmartPointer<vtkPolyData> RefineSurfaceAlongNormals(
        const VolumeBuffer& vol,
        vtkSmartPointer<vtkPolyData> surface,
        const SurfaceParams& surfParams,
        const AdvancedSurfaceParams& adv);

private:
    // ── 核心内核：在采样序列中寻找最优边界偏移（梯度最大穿越点）───
    static bool FindBestOffsetAlongNormal(
        const std::vector<float>& vals,
        const std::vector<double>& ts,
        float                      iso,
        float                      gradThresh,
        double& outBestT);
};

// ─────────────────────────────────────────────────────────────────────

inline bool SurfaceRefiner::FindBestOffsetAlongNormal(
    const std::vector<float>& vals,
    const std::vector<double>& ts,
    float                      iso,
    float                      gradThresh,
    double& outBestT)
{
    int    bestIdx = -1;
    double bestGrad = static_cast<double>(gradThresh);
    const size_t N = ts.size();

    for (size_t i = 1; i + 1 < N; ++i) {
        const float v0 = vals[i - 1] - iso;
        const float v1 = vals[i] - iso;
        const float v2 = vals[i + 1] - iso;
        if ((v0 * v1 > 0.f) && (v1 * v2 > 0.f)) continue; // 无穿越

        const double dt = ts[i + 1] - ts[i - 1];
        if (std::abs(dt) < 1e-6) continue;

        const double grad = std::abs(
            static_cast<double>(vals[i + 1] - vals[i - 1]) / dt);
        if (grad > bestGrad) { bestGrad = grad; bestIdx = static_cast<int>(i); }
    }

    if (bestIdx >= 0) { outBestT = ts[bestIdx]; return true; }
    return false;
}

inline vtkSmartPointer<vtkPolyData> SurfaceRefiner::RefineSurfaceAlongNormals(
    const VolumeBuffer& vol,
    vtkSmartPointer<vtkPolyData> surface,
    const SurfaceParams& surfParams,
    const AdvancedSurfaceParams& adv)
{
    if (!adv.enabled || !surface) return surface;

    vtkPoints* points = surface->GetPoints();
    vtkDataArray* nArray = surface->GetPointData()->GetNormals();
    if (!points || !nArray) return surface;

    const vtkIdType numPts = points->GetNumberOfPoints();
    if (numPts == 0) return surface;

    auto unitToWorld = [&](float v) -> double {
        if (adv.useMillimeter) return static_cast<double>(v);
        const double avgSpacing =
            (vol.spacing[0] + vol.spacing[1] + vol.spacing[2]) / 3.0;
        return static_cast<double>(v) * avgSpacing;
        };

    const double searchDistWorld = unitToWorld(adv.normalSearchDistance);
    const double searchStepWorld = std::max(1e-3, unitToWorld(adv.searchStep));
    const double maxShiftWorld = unitToWorld(adv.maxVertexShift);
    const float  iso = surfParams.isoValue;
    const float  gradThresh = adv.gradientThreshold;
    const size_t numSamples =
        static_cast<size_t>(searchDistWorld * 2.0 / searchStepWorld) + 5;

    // ── 用 vtkSMPTools::For 替代 #pragma omp parallel ────────────────
    // vtkSMPTools 与项目已有的 vtkSMPTools::Initialize 共享线程池
    vtkSMPTools::For(0, numPts,
        [&](vtkIdType begin, vtkIdType end)
        {
            // 每个 SMP 任务独立的临时容器（线程局部，避免竞争）
            std::vector<double> ts;
            std::vector<float>  svals;
            ts.reserve(numSamples);
            svals.reserve(numSamples);

            for (vtkIdType pid = begin; pid < end; ++pid) {
                double p[3], n[3];
                points->GetPoint(pid, p);
                nArray->GetTuple(pid, n);

                const double nLen =
                    std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nLen < 1e-6) continue;
                n[0] /= nLen; n[1] /= nLen; n[2] /= nLen;

                ts.clear();
                svals.clear();
                for (double t = -searchDistWorld;
                    t <= searchDistWorld + 1e-7;
                    t += searchStepWorld)
                {
                    ts.push_back(t);
                    svals.push_back(vol.sampleTrilinearWorld(
                        p[0] + t * n[0], p[1] + t * n[1], p[2] + t * n[2]));
                }

                double tBest = 0.0;
                if (FindBestOffsetAlongNormal(svals, ts, iso, gradThresh, tBest)) {
                    if (std::abs(tBest) > maxShiftWorld)
                        tBest = tBest > 0 ? maxShiftWorld : -maxShiftWorld;
                    const double newP[3] = {
                        p[0] + tBest * n[0], p[1] + tBest * n[1], p[2] + tBest * n[2]
                    };
                    points->SetPoint(pid, newP);
                }
            }
        }
    );

    points->Modified();

    if (adv.normalSmoothIterations > 0) {
        auto smoother = vtkSmartPointer<vtkSmoothPolyDataFilter>::New();
        smoother->SetInputData(surface);
        smoother->SetNumberOfIterations(adv.normalSmoothIterations);
        smoother->SetRelaxationFactor(0.1);
        smoother->Update();
        surface = smoother->GetOutput();
    }
    return surface;
}