#pragma once
// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/include/GapAnalysisTypes.h
// GapAnalysisTypes.h - 间隙/空洞分析模块纯数据结构与插件接口
// =====================================================================

#include <vector>
#include <array>
#include <cstddef>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>

// ── worker 执行轴；不表示显示模式或 overlay 是否开启 ────────────────
enum class GapAnalysisState {
    Idle,       // 尚未发起，或 worker 在进入算法前消费到停止请求。
    Running,    // StartAsync 已发布快照，后台 worker 可能正在分阶段计算。
    Succeeded,  // worker 已提交成功结果，主线程可以读取并挂载显示。
    Failed      // 输入校验、后台异常，或 worker 在阶段间消费到取消请求；显示 tick 不挂载 overlay。
};

// ── 表面参数（外部手动传入 isoValue，不在此自动估算）───────────────
struct SurfaceParams {
    float isoValue = 0.0f;  // 输入图像标量域中的等值面阈值（ISO-50 由外部计算后传入）。
    float background = 0.0f;  // 输入图像标量域中的背景灰度上限。
    float material = 0.0f;  // 输入图像标量域中的材料灰度下限。
};

// ── 显示请求中的等值面阈值来源 ────────────────────────────────────
// 这是 GapAnalysis feature 自己的执行语义，不携带 host 窗口、按键或上位机协议细节。
enum class GapAnalysisIsoValueMode {
    // 在当前输入标量范围按 min + (max - min) * ratio 解析；feature 不额外钳制 ratio。
    DataRangeRatio,
    // 直接采用输入图像标量域中的 absoluteIsoValue。
    AbsoluteValue
};

struct GapAnalysisSurfaceRequest {
    GapAnalysisIsoValueMode isoMode = GapAnalysisIsoValueMode::DataRangeRatio;
    double dataRangeRatio = 0.0;
    double absoluteIsoValue = 0.0;
};

// ── 高级法向精化参数 ──────────────────────────────────────────────────
struct AdvancedSurfaceParams {
    bool  isEnabled = true;
    float normalSearchDistance = 2.0f;
    bool  isMillimeter = false;
    float searchStep = 0.5f;
    float maxVertexShift = 2.0f;
    float gradientThreshold = 0.0f;
    int   normalSmoothIterations = 0;
};

// ── 空洞检测参数 ──────────────────────────────────────────────────────
struct VoidDetectionParams {
    float  grayMin = 0.0f;
    float  grayMax = 0.0f;
    double minVolumeMM3 = 0.00;
    float  angleThresholdDeg = 40.0f;
    int    tensorWindowSize = 1;
    int erosionIterations = 2;
};

// ── 空洞区域统计结果 ──────────────────────────────────────────────────
struct VoidRegion {
    int                  id = 0;
    size_t               voxelCount = 0;
    double               volumeMM3 = 0.0;
    double               equivalentDiameterMM = 0.0;
    std::array<double, 3> centroidMM = { 0, 0, 0 }; // 输入图像 physical 坐标，布局为 [x, y, z]，单位 mm。
    std::array<int, 6>    bbox = { 0, 0, 0, 0, 0, 0 }; // 闭区间 voxel index：[minX,maxX,minY,maxY,minZ,maxZ]。

    std::array<int, 3> seedVoxel = { 0, 0, 0 }; // 连通区域种子 voxel index，布局为 [x, y, z]。

    double minGray = 0.0;
    double maxGray = 0.0;
    double meanGray = 0.0;
    double stdDevGray = 0.0;
    double contrastDeviation = 0.0; //对比偏差

    double radius = 0.0;
    double gapMM = 0.0;
    double compactness = 0.0;

    double surfaceAreaMM2 = 0.0;
    double sphericity = 0.0;
    std::array<double, 3> pcaAxes = { 0, 0, 0 }; // 长、中、短轴长度 (特征值)
    double elongation = 0.0; // 伸长率
    double flatness = 0.0;   // 扁平率

    //投影尺寸 [mm]
    float xProjection = 0.0f;
    float yProjection = 0.0f;
    float zProjection = 0.0f;

    //主分量分析 (PCA) 偏差
    double pcaDeviation1 = 0.0;
    double pcaMaxDeviationRatio = 0.0;

    //投影区域 [mm²]
    double projectedAreaYZMM2 = 0.0;
    double projectedAreaXZMM2 = 0.0;
    double projectedAreaXYMM2 = 0.0;

};

// ── 完整分析结果（GapAnalysisService 填充，主线程消费）──────────────
struct GapAnalysisResult {
    std::vector<VoidRegion> voids;
    std::vector<int>        labelVolume;  // 与体素一一对应，0 = 非空洞
    // 标签体继承输入快照的 dimensions、spacing 与 origin；worker 构建一次，主线程只读并挂载。
    vtkSmartPointer<vtkImageData> labelImage;
    bool                    isSucceeded = false; // 只表示分析 payload 有效，不代表 display/overlay 已显示。
};
