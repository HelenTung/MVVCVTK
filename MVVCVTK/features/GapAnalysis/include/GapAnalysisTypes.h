#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/GapAnalysisTypes.h
// GapAnalysisTypes.h - 间隙/空洞分析模块纯数据结构与插件接口
// =====================================================================

#include "Host/GapHostTypes.h"

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// ── 表面参数（外部手动传入 isoValue，不在此自动估算）───────────────
struct GapSurfaceParams {
    float isoValue = 0.0f;  // 输入标量域阈值；当前 worker 将它映射为私有内核表面阈值。
    float background = 0.0f;  // DefX 材料定义的背景均值。
    float material = 0.0f;  // DefX 材料定义的材料均值。
};

// ── 空洞区域结果 ──────────────────────────────────────────────────────
struct VoidRegion {
    // 以下字段逐项复制私有算法内核的区域输出；Feature 不重新连通、重编号或派生形状量。
    std::int32_t id = 0;
    std::int64_t voxelCount = 0;
    float volumeMM3 = 0.0f;
    float equivalentDiameterMM = 0.0f;
    float radiusMM = 0.0f;
    float diameterMM = 0.0f;
    std::array<double, 3> centerMM = { 0.0, 0.0, 0.0 };
    std::array<std::int32_t, 3> centroidMM = { 0, 0, 0 };
    std::array<std::int32_t, 6> bbox = { 0, 0, 0, 0, 0, 0 };
    std::array<std::int32_t, 3> seedVoxel = { 0, 0, 0 };
    float minGray = 0.0f;
    float maxGray = 0.0f;
    float meanGray = 0.0f;
    float stdDevGray = 0.0f;
    float grayDeviation = 0.0f;
    // 相邻缺陷外接球表面的最小距离，单位 mm。
    float gapMM = 0.0f;
    float compactness = 0.0f;
    float surfaceAreaMM2 = 0.0f;
    float sphericity = 0.0f;
    // 三个主轴标准差，而非旧实现的协方差特征值。
    std::array<float, 3> pcaDeviation = { 0.0f, 0.0f, 0.0f };
    float pcaMaxDeviationRatio = 0.0f;
    float pcaMinDeviationRatio = 0.0f;
    float projectedAreaXMM2 = 0.0f;
    float projectedAreaYMM2 = 0.0f;
    float projectedAreaZMM2 = 0.0f;
    // 供应商定义为三个方向的 voxel 索引差，不擅自换算为物理长度。
    std::array<float, 3> projectedSize = { 0.0f, 0.0f, 0.0f };
    float defectProbability = 0.0f;
};

// ── 完整分析结果（GapAnalysisService 填充，主线程消费）──────────────
struct GapAnalysisResult {
    // 私有内核完成筛选后的原始区域集合。
    std::vector<VoidRegion> voids;
    // 私有内核原始标签 ID 直接写入此 VTK_INT 图；不二值化、重编号或长期保留第二份整卷 vector。
    // 标签体完整继承输入快照的 extent、spacing、origin 与 direction。
    vtkSmartPointer<vtkImageData> labelImage;
    // 只由私有内核 header/region 投影，禁止扫描 label 后重算算法统计。
    GapStatistics statistics;
    bool                    isSucceeded = false; // 只表示分析 payload 有效，不代表 display/overlay 已显示。
};
