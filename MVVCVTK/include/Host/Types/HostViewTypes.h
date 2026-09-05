#pragma once

#include "Data/DataGraphTypes.h"

#include <cstdint>
#include <string>
#include <vector>

enum class HostRenderViewRole {
    Primary3D,
    Composite3D,
    TopDownSlice,
    FrontBackSlice,
    LeftRightSlice,
    Auxiliary
};

// 单目标保持 id 优先且 id 未命中时不回退 role。
struct HostViewTarget final {
    std::string viewId;
    bool isViewRoleUsed = false;
    HostRenderViewRole viewRole = HostRenderViewRole::Auxiliary;
};

// 多目标按 topology 顺序返回 ids/roles 的去重并集；空集合不表示全选。
struct HostViewTargets final {
    std::vector<std::string> viewIds;
    std::vector<HostRenderViewRole> viewRoles;
};

// 一次挂载内的执行尝试。attachmentId 由 Session 分配，不能用 Feature 名称替代。
struct FeatureOperationId final {
    std::string featureId;
    std::uint64_t attachmentId = 0;
    std::uint64_t requestId = 0;
};

enum class HostViewSyncPolicy : std::uint8_t { SamePrimary, ExplicitInputs };

inline bool operator==(const FeatureOperationId& left,
    const FeatureOperationId& right) noexcept
{
    return left.featureId == right.featureId
        && left.attachmentId == right.attachmentId && left.requestId == right.requestId;
}

enum class FeatureRunStatus : std::uint8_t {
    Idle, Preparing, Running, Ready, Succeeded, Failed, Cancelled, Stopping
};

struct FeatureOperationState final {
    FeatureOperationId operation;
    // 仅覆盖该执行尝试的实际状态，独立于 DataGraph 和 sceneEpoch。
    std::uint64_t stateRevision = 0;
    FeatureRunStatus status = FeatureRunStatus::Idle;
    std::vector<DataInputRef> inputs;
    std::vector<DataRevisionRef> outputs;
    double progress = 0.0;
};

struct HostDisplayRef final {
    std::string viewId;
    std::string featureId;
    std::string localId;
    DataRevisionRef data;
    FeatureOperationId operation;
};

inline bool operator==(const HostDisplayRef& left, const HostDisplayRef& right) noexcept
{
    return left.viewId == right.viewId && left.featureId == right.featureId
        && left.localId == right.localId && left.data == right.data
        && left.operation == right.operation;
}

struct HostSemanticTarget final {
    HostDisplayRef display;
    std::string objectId;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t resultRevision = 0;
};
