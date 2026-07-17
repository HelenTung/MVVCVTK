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
struct GapSurfaceParams {
    float isoValue = 0.0f;  // 输入标量域阈值；当前 worker 用它区分低于 ISO 的内部候选。
    float background = 0.0f;  // 预留的背景灰度上限；当前 worker 不读取该字段。
    float material = 0.0f;  // 预留的材料灰度下限；当前 worker 不读取该字段。
};

// ── 显示请求中的等值面阈值来源 ────────────────────────────────────
// 这是 GapAnalysis feature 自己的执行语义，不携带 host 窗口、按键或上位机协议细节。
enum class GapIsoMode {
    // 在当前输入标量范围按 min + (max - min) * ratio 解析；feature 不额外钳制 ratio。
    DataRangeRatio,
    // 直接采用输入图像标量域中的 absoluteIsoValue。
    AbsoluteValue
};

struct GapSurfaceRequest {
    // 阈值来源选择器；DataRangeRatio 只读取 ratio，AbsoluteValue 只读取 absoluteIsoValue。
    GapIsoMode isoMode = GapIsoMode::DataRangeRatio;
    // DataRangeRatio 下按 min + (max-min)*ratio 解析；当前不钳制范围，默认 0 对应输入最小值。
    double dataRangeRatio = 0.0;
    // AbsoluteValue 下直接使用的输入标量域阈值；其它模式忽略。
    double absoluteIsoValue = 0.0;
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

// ── 空洞检测参数 ──────────────────────────────────────────────────────
struct GapVoidParams {
    // 预留的候选灰度下限；当前 BuildCandidates 不读取，不能视作已生效筛选条件。
    float  grayMin = 0.0f;
    // 候选灰度上限，处于输入标量域；内部 mask 中 value <= grayMax 的 voxel 才进入原始候选。
    float  grayMax = 0.0f;
    // 连通区域最小保留体积，单位 mm^3；volume >= threshold 时保留，默认 0 不过滤。
    double minVolumeMM3 = 0.00;
    // 预留的方向夹角阈值，单位 degree；当前检测器不读取。
    float  angleThresholdDeg = 40.0f;
    // 预留的结构张量邻域半径/窗口参数，单位 voxel；当前检测器不读取。
    int    tensorWindowSize = 1;
    // 六邻域腐蚀轮数；小于等于 0 时跳过，腐蚀后从幸存种子回长到原始候选连通域。
    int erosionIterations = 2;
};

// ── 空洞区域统计结果 ──────────────────────────────────────────────────
struct VoidRegion {
    // 被保留区域的正标签 ID；从 1 连续编号，并与 labelVolume/labelImage 的正值对应。
    int                  id = 0;
    // 当前六邻域连通候选包含的 voxel 数。
    size_t               voxelCount = 0;
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
    bool                    isSucceeded = false; // 只表示分析 payload 有效，不代表 display/overlay 已显示。
};
