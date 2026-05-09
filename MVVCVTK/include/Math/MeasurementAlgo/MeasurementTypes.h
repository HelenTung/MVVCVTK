#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class MeasurementType {
    None,
    Length,
    Angle
};

enum class MeasurementStatus {
    Idle,
    InProgress,
    Succeeded,
    Invalid
};

struct MeasurementResult {
    uint64_t id = 0;
    MeasurementType type = MeasurementType::None;
    MeasurementStatus status = MeasurementStatus::Idle;
    double value = 0.0;
    std::string unit;
    bool visible = true;
    bool isHistorical = false;
    std::vector<std::array<double, 3>> worldPoints; // 当前测量结果对应的世界坐标点击点
    std::vector<std::array<double, 3>> modelPoints; // 当前测量结果对应的模型坐标点击点
};

struct MeasurementSessionState {
    MeasurementResult result;
    std::array<double, 3> previewWorldPoint = { 0.0, 0.0, 0.0 }; // 当前测量会话尚未确认的预览世界端点
    std::array<double, 3> previewModelPoint = { 0.0, 0.0, 0.0 }; // 当前测量会话尚未确认的预览模型端点
    bool hasPreviewPoint = false;
};
