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
#include <vector>

// ── 表面参数（外部手动传入 isoValue，不在此自动估算）───────────────
struct GapSurfaceParams {
    float isoValue = 0.0f;  // 输入标量域阈值；当前 worker 用它区分低于 ISO 的内部候选。
    float background = 0.0f;  // 预留的背景灰度上限；当前 worker 不读取该字段。
    float material = 0.0f;  // 预留的材料灰度下限；当前 worker 不读取该字段。
};

// ── 高级法向精化参数 ──────────────────────────────────────────────────
struct GapAdvancedParams {
    // 法向精化总开关；当前 worker 尚未接入精化算法，因此本结构全部字段不影响分析结果。
    bool  isEnabled = true;
    // 计划的法向双侧搜索距离；isMillimeter=true 时为 mm，否则为平均 voxel spacing 的倍数。
    float normalSearchDistance = 2.0f;
    // 距离、步长和最大位移的单位选择：true 为 mm，false 为平均 voxel spacing 倍数。
    bool  isMillimeter = false;
    // 计划的法向采样步长，单位由 isMillimeter 选择；必须由未来精化入口校验为正值。
    float searchStep = 0.5f;
    // 计划允许的单顶点最大法向位移，单位由 isMillimeter 选择。
    float maxVertexShift = 2.0f;
    // 计划接受等值穿越点的最小梯度阈值，处于输入标量/physical distance 域。
    float gradientThreshold = 0.0f;
    // 计划在法向位移后执行的平滑迭代次数；大于 0 才启用平滑。
    int   normalSmoothIterations = 0;
};

// ── 空洞区域统计结果 ──────────────────────────────────────────────────
struct VoidRegion {
    // 被保留区域的正标签 ID；从 1 连续编号，并与 labelVolume/labelImage 的正值对应。
    int                  id = 0;
    // 当前六邻域连通候选包含的 voxel 数。
    std::size_t          voxelCount = 0;
    // voxelCount * spacingX * spacingY * spacingZ，单位 mm^3。
    double               volumeMM3 = 0.0;
    // 与 volumeMM3 等体积球的直径，单位 mm。
    double               equivalentDiameterMM = 0.0;
    std::array<double, 3> centroidMM = { 0, 0, 0 }; // VolumeBuffer 轴对齐 physical 坐标，布局 [x,y,z]，单位 mm。
    std::array<int, 6>    bbox = { 0, 0, 0, 0, 0, 0 }; // 闭区间 voxel index：[minX,maxX,minY,maxY,minZ,maxZ]。

    std::array<int, 3> seedVoxel = { 0, 0, 0 }; // 连通区域种子 voxel index，布局为 [x, y, z]。

    // 区域内输入标量的最小值、最大值、总体均值和总体标准差。
    double minGray = 0.0;
    double maxGray = 0.0;
    double meanGray = 0.0;
    double stdDevGray = 0.0;
    double contrastDeviation = 0.0; // 预留对比偏差；当前检测器不赋值，保持默认 0。

    // 等效球半径，即 equivalentDiameterMM / 2，单位 mm。
    double radius = 0.0;
    // 特征厚度近似 2 * volumeMM3 / surfaceAreaMM2，单位 mm。
    double gapMM = 0.0;
    // 无量纲紧致度 36*pi*V^2/S^3；仅在表面积有效时计算。
    double compactness = 0.0;

    // 基于 13 方向边界穿越和平均 voxel 截面积的近似表面积，单位 mm^2。
    double surfaceAreaMM2 = 0.0;
    // compactness 的立方根，无量纲。
    double sphericity = 0.0;
    // physical 坐标协方差矩阵的降序特征值 [lambdaMax, lambdaMid, lambdaMin]，单位 mm^2；并非轴长。
    std::array<double, 3> pcaAxes = { 0, 0, 0 };
    double elongation = 0.0; // sqrt(lambdaMid/lambdaMax)，无量纲；lambdaMax 无效时为 0。
    double flatness = 0.0;   // sqrt(lambdaMin/lambdaMid)，无量纲；lambdaMid 无效时为 0。

    // 轴对齐闭区间 bbox 的三轴 physical 尺寸，均包含端点 voxel，单位 mm。
    float xProjection = 0.0f;
    float yProjection = 0.0f;
    float zProjection = 0.0f;

    // PCA 无量纲比值：第一主分量占比，以及最大/最小特征值比。
    double pcaDeviation1 = 0.0;
    // lambdaMin <= 1e-9 时保持 0，避免把退化区域报告为无穷比值。
    double pcaMaxDeviationRatio = 0.0;

    // 区域 voxel 在三个坐标平面的去重占据面积，单位 mm^2。
    double projectedAreaYZMM2 = 0.0;
    double projectedAreaXZMM2 = 0.0;
    double projectedAreaXYMM2 = 0.0;

};

// ── 完整分析结果（GapAnalysisService 填充，主线程消费）──────────────
struct GapAnalysisResult {
    // 通过 minVolumeMM3 筛选的区域统计；id 与正标签值一一对应。
    std::vector<VoidRegion> voids;
    // 与输入体素一一对应的 x-fast 扁平标签：[x + y*dimX + z*dimX*dimY]，0 表示非保留区域。
    std::vector<int>        labelVolume;
    // 标签体继承输入快照的 dimensions、spacing 与 origin；worker 构建一次，主线程只读并挂载。
    vtkSmartPointer<vtkImageData> labelImage;
    // 与 voids、labelVolume 和 labelImage 在同一 worker 提交段发布的聚合统计。
    GapStatistics statistics;
    bool                    isSucceeded = false; // 只表示分析 payload 有效，不代表 display/overlay 已显示。
};
