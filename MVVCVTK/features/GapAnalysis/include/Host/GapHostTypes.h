#pragma once

#include "Data/DataGraphTypes.h"

#include <cstddef>
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
