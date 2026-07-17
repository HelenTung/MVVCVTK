#pragma once

#include "AppService.h"
#include "Host/Types/HostSessionTypes.h"
#include "StdRenderContext.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct HostRenderViewRuntime {
    HostRenderViewConfig config;
    std::shared_ptr<VizService> service;
    std::shared_ptr<StdRenderContext> context;
};

class HostRenderViewSet {
public:
    bool CreateView(
        std::string id,
        HostRenderViewRole role,
        std::shared_ptr<StdRenderContext> context)
    {
        HostRenderViewRuntime view;
        view.config.id = std::move(id);
        view.config.role = role;
        view.service = std::make_shared<VizService>();
        view.context = std::move(context);
        m_views.push_back(std::move(view));
        return true;
    }

    const HostRenderViewRuntime* GetPrimaryView() const
    {
        for (const auto& view : m_views) {
            if (view.config.role == HostRenderViewRole::Primary3D) {
                return &view;
            }
        }
        return m_views.empty() ? nullptr : &m_views.front();
    }

    const HostRenderViewRuntime* GetViewBySelector(
        const HostViewTarget& target) const
    {
        if (!target.viewId.empty()) {
            for (const auto& view : m_views) {
                if (view.config.id == target.viewId) {
                    return &view;
                }
            }
            return nullptr;
        }
        if (target.isViewRoleUsed) {
            for (const auto& view : m_views) {
                if (view.config.role == target.viewRole) {
                    return &view;
                }
            }
        }
        return nullptr;
    }

    std::vector<const HostRenderViewRuntime*> GetViewsByTargets(
        const HostViewTargets& targets) const
    {
        std::vector<const HostRenderViewRuntime*> selectedViews;
        for (const auto& view : m_views) {
            const bool isIdSelected = std::find(targets.viewIds.begin(), targets.viewIds.end(), view.config.id) != targets.viewIds.end();
            const bool isRoleSelected = std::find(targets.viewRoles.begin(), targets.viewRoles.end(), view.config.role) != targets.viewRoles.end();
            if (isIdSelected || isRoleSelected) {
                selectedViews.push_back(&view);
            }
        }
        return selectedViews;
    }

    bool GetRoleIsSliceView(HostRenderViewRole role) const
    {
        return role == HostRenderViewRole::TopDownSlice
            || role == HostRenderViewRole::FrontBackSlice
            || role == HostRenderViewRole::LeftRightSlice;
    }

private:
    std::vector<HostRenderViewRuntime> m_views;
};
