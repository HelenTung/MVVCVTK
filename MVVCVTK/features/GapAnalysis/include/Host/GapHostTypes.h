#pragma once

#include "Data/DataGraphTypes.h"

#include <cstddef>
#include <array>
#include <optional>
#include <vector>
#include <string>

// worker 执行轴；Host 只读取已发布状态，不借此控制 overlay 显示。
enum class GapAnalysisState {
    Idle,
    Running,
    Succeeded,
    Failed,
    Stale
};

enum class GapResultStatus {
    Succeeded,
    SucceededWithDisplayFailure,
    Failed,
    SourceChanged
};

// Gap Feature 的等值面阈值来源，不携带 Host 窗口或实现对象。
enum class GapIsoMode {
    DataRangeRatio,
    AbsoluteValue
};

struct GapSurfaceConfig {
    GapIsoMode isoMode = GapIsoMode::DataRangeRatio;
    // DataRangeRatio 下按 min + (max-min)*ratio 解析；当前不钳制范围。
    double dataRangeRatio = 0.0;
    // AbsoluteValue 下直接使用输入标量域中的阈值。
    double absoluteIsoValue = 0.0;
    // DefX 材料定义的背景与材料均值；Feature 只做请求映射。
    float backgroundMean = 0.0f;
    float materialMean = 0.0f;
};

struct GapVoidParams {
    // 显式控制 DefX 过滤；关闭时 minVolumeMM3 不参与筛选。
    bool isFilterEnabled = false;
    // DefX 最小缺陷体积，单位 mm^3。
    double minVolumeMM3 = 0.00;
};

struct GapStatistics final {
    std::size_t objectVoxelCount = 0;
    std::size_t voidVoxelCount = 0;
    double objectVolumeMM3 = 0.0;
    double voidVolumeMM3 = 0.0;
    double porosityRatio = 0.0;
};

enum class GapColorMode { Constant, Gradient, Rainbow, InverseRainbow, HueLoop };
enum class GapRangeMode { Result, SelectedInterval };
enum class GapDisplayStyle { Constant, Inclined, InverseInclined };
struct GapColorSegment final {
    // 缺省端点分别表示负／正无穷；无穷端段只支持恒定色，邻段必须连续。
    std::optional<double> lower, upper;
    GapColorMode mode=GapColorMode::Gradient;
    std::array<double,3> lowColor{0.294,0.294,0.84},highColor{0.84,0.294,0.294};
};

// 色带只投影已发布的缺陷体积；不改变标签、筛选、网格和统计。
struct GapDisplayParams final {
    GapColorMode mode = GapColorMode::Rainbow;
    std::array<double, 3> constantColor{0.70, 0.70, 0.70};
    std::array<double, 3> lowColor{0.294, 0.294, 0.84};
    std::array<double, 3> highColor{0.84, 0.294, 0.294};
    std::array<double, 3> belowColor{0.64, 0.29, 0.78};
    std::array<double, 3> aboveColor{0.84, 0.29, 0.65};
    GapRangeMode rangeMode = GapRangeMode::Result;
    std::array<double, 2> range{0, 1};
    std::vector<GapColorSegment> segments;
    GapDisplayStyle style=GapDisplayStyle::Constant;
    // 按本 Feature 当前体积范围渐变的相对叠加强度，仍乘以各视图原有透明度。
    std::array<double,2> opacityRange{0.15,1.0};
};

struct GapHostState final {
    GapAnalysisState analysisState = GapAnalysisState::Idle;
    GapStatistics statistics;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef labelMap;
    DataRevisionRef voidTable;
    DataRevisionRef voidMesh;
    DataRevisionRef statisticsData;
    DataRevisionRef resultSet;
    bool isViewActive = false;
    bool isExitPending = false;
    bool isOverlayVisible = false;
    GapDisplayParams display;
};

struct GapHostResult final {
    GapResultStatus status = GapResultStatus::Failed;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef labelMap;
    DataRevisionRef voidTable;
    DataRevisionRef voidMesh;
    DataRevisionRef statisticsData;
    DataRevisionRef resultSet;
    GapStatistics statistics;
    std::string message;
};
