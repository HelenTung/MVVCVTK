#pragma once
// =====================================================================
// GapAnalysisTypes.h — 间隙/空洞分析模块纯数据结构
//
// 无 VTK / OpenCV / 线程依赖，可被任意层包含。
// =====================================================================

#include <vector>
#include <array>
#include <cstddef>

// ── 表面参数（手动传入，不在此计算）─────────────────────────────────
struct SurfaceParams {
    float isoValue = 0.0f;  // 等值面阈值
    float background = 0.0f;  // 背景灰度上限（用于空洞候选筛选）
    float material = 0.0f;  // 材料灰度下限
};

// ── 高级法向精化参数 ──────────────────────────────────────────────────
struct AdvancedSurfaceParams {
    bool  enabled = false;
    float normalSearchDistance = 0.0f;   // 搜索距离（单位由 useMillimeter 决定）
    bool  useMillimeter = false;  // true: mm；false: voxel
    float searchStep = 0.5f;   // 采样步长
    float maxVertexShift = 2.0f;   // 顶点最大位移
    float gradientThreshold = 0.0f;   // 最小梯度阈值（0 = 不限制）
    int   normalSmoothIterations = 0;      // 几何平滑迭代次数（0 = 不平滑）
};

// ── 空洞检测参��� ──────────────────────────────────────────────────────
struct VoidDetectionParams {
    float  grayMin = 0.0f;
    float  grayMax = 0.0f;
    double minVolumeMM3 = 0.01;
    float  angleThresholdDeg = 30.0f;  // 结构张量角度阈值（场驱动合并）
    int    tensorWindowSize = 1;       // 张量窗口半径 (1 = 3x3x3)
};

// ── 空洞区域统计结果 ──────────────────────────────────────────────────
struct VoidRegion {
    int                  id = 0;
    size_t               voxelCount = 0;
    double               volumeMM3 = 0.0;
    double               equivalentDiameterMM = 0.0;
    std::array<double, 3> centroidMM = { 0, 0, 0 };
    std::array<int, 6>    bbox = { 0, 0, 0, 0, 0, 0 };
};

// ── 完整分析结果（由 GapAnalysisService 填充，供上层消费）───────────
struct GapAnalysisResult {
    std::vector<VoidRegion> voids;       // 空洞统计列表
    std::vector<int>        labelVolume; // 标签体（与体素一一对应，0=非空洞）
    bool                    succeeded = false;
};