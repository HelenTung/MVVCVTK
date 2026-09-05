#pragma once

#include "Data/DataGraphTypes.h"

#include <array>
#include <cstdint>
#include <optional>

// 仓内可信姿态能力；只进入 source staging，不属于 SDK 安装闭包。
enum class ModelTransformStatus {
    None,
    Committed,
    Cancelled,
    Invalidated
};

struct ModelTransformSnapshot final {
    std::uint64_t sessionGeneration = 0;
    DataRevisionRef dataRevision;
    DataBindingRevision bindingRevision = 0;
    std::uint64_t transformRevision = 0;
    std::array<double, 16> modelToWorld = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    std::array<double, 16> worldToModel = modelToWorld;
    std::uint64_t editToken = 0;
    bool hasPending = false;
    std::uint64_t completedToken = 0;
    ModelTransformStatus completion = ModelTransformStatus::None;
};

class FeatureModelTransformPort {
public:
    virtual ~FeatureModelTransformPort() = default;
    // 全部调用限活动 Session 的 owner thread；拒绝不保存 callback。
    virtual std::optional<ModelTransformSnapshot> GetTransformState() const = 0;
    virtual std::optional<std::uint64_t> StartTransform(
        const ModelTransformSnapshot& expected) = 0;
    // sequence 严格递增；Commit 包含最后位置，封闭后不再接纳 Preview。
    virtual bool SetTransformPreview(std::uint64_t token,
        std::uint64_t sequence, const std::array<double, 16>& matrix) = 0;
    virtual bool SetTransformCommit(std::uint64_t token,
        std::uint64_t sequence, const std::array<double, 16>& matrix) = 0;
    virtual bool StopTransform(std::uint64_t token) = 0;
};
