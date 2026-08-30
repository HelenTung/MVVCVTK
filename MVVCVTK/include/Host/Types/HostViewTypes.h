#pragma once

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
