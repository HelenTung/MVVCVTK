#pragma once

#include "AppService.h"
#include "Host/HostSessionTypes.h"
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
        const std::string& id,
        bool isRoleUsed,
        HostRenderViewRole role) const
    {
        if (!id.empty()) {
            for (const auto& view : m_views) {
                if (view.config.id == id) {
                    return &view;
                }
            }
            return nullptr;
        }
        if (isRoleUsed) {
            for (const auto& view : m_views) {
                if (view.config.role == role) {
                    return &view;
                }
            }
        }
        return nullptr;
    }

    std::vector<const HostRenderViewRuntime*> GetViewsByIdsAndRoles(
        const std::vector<std::string>& ids,
        const std::vector<HostRenderViewRole>& roles) const
    {
        std::vector<const HostRenderViewRuntime*> selectedViews;
        for (const auto& view : m_views) {
            const bool isIdSelected = std::find(ids.begin(), ids.end(), view.config.id) != ids.end();
            const bool isRoleSelected = std::find(roles.begin(), roles.end(), view.config.role) != roles.end();
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
