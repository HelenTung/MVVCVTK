#pragma once

#include "AppPorts.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "ViewContext.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Router 测试只实现类型化目录，不继承、不伪装生产组合根。
class HostRouteStub final
    : public IHostViewDirectory
    , public std::enable_shared_from_this<HostRouteStub> {
public:
    bool CreateView(
        std::string id,
        const HostRenderViewRole role,
        std::shared_ptr<ViewContextStub> context,
        const HostInputMode inputMode = HostInputMode::NativeInteractor)
    {
        if (id.empty() || !context) return false;

        View view;
        view.id = std::move(id);
        view.role = role;
        view.inputMode = inputMode;
        view.state = std::make_shared<AppPortState>();
        view.data = std::make_shared<DataPortStub>(view.state);
        view.view = std::make_shared<ViewPortStub>(view.state);
        view.session = std::make_shared<SessionPortStub>(view.state);
        view.update = std::make_shared<UpdatePortStub>(view.state);
        view.context = std::move(context);
        m_views.push_back(std::move(view));
        return true;
    }

    std::weak_ptr<IHostViewDirectory> GetViewDirectory()
    {
        return shared_from_this();
    }

    std::optional<HostDataRoute> GetDataRoute(
        const HostViewTarget& target) const override
    {
        const auto* view = GetDataView(target);
        if (!view) return std::nullopt;
        return HostDataRoute{
            view->id,
            view->role,
            GetHostMode(view->state->GetVizMode()),
            view->data
        };
    }

    std::optional<HostViewRoute> GetViewRoute(
        const HostViewTarget& target) const override
    {
        const auto* view = GetView(target);
        if (!view || !view->state->GetIsAvailable()) {
            return std::nullopt;
        }
        const std::weak_ptr<AppPortState> state = view->state;
        return HostViewRoute{
            view->id,
            view->role,
            view->view,
            view->update,
            view->context,
            [state]() {
                const auto current = state.lock();
                return current && current->StopView();
            }
        };
    }

    std::weak_ptr<AppSessionPort> GetSessionPort() const override
    {
        const auto* view = GetDataView(HostViewTarget{});
        return view ? std::weak_ptr<AppSessionPort>(view->session)
            : std::weak_ptr<AppSessionPort>{};
    }

    std::vector<HostInputRoute> GetInputRoutes(
        const HostViewTargets& targets) const override
    {
        std::vector<HostInputRoute> selected;
        for (const auto& view : m_views) {
            const bool isIdSelected = std::find(
                targets.viewIds.begin(),
                targets.viewIds.end(),
                view.id) != targets.viewIds.end();
            const bool isRoleSelected = std::find(
                targets.viewRoles.begin(),
                targets.viewRoles.end(),
                view.role) != targets.viewRoles.end();
            if (isIdSelected || isRoleSelected) {
                selected.push_back({
                    view.id, view.role, view.inputMode, view.context });
            }
        }
        return selected;
    }

    bool StopView(const std::string_view viewId) override
    {
        const auto found = std::find_if(
            m_views.begin(), m_views.end(),
            [viewId](const View& view) {
                return view.id.size() == viewId.size()
                    && std::equal(
                        view.id.begin(), view.id.end(), viewId.begin());
            });
        return found != m_views.end()
            && found->state
            && found->state->StopView();
    }

    bool ClearViewContext(const std::string_view viewId)
    {
        const auto found = std::find_if(
            m_views.begin(), m_views.end(),
            [viewId](const View& view) {
                return view.id.size() == viewId.size()
                    && std::equal(
                        view.id.begin(), view.id.end(), viewId.begin());
            });
        if (found == m_views.end()) return false;
        found->context.reset();
        return true;
    }

    std::shared_ptr<AppPortState> GetState(const std::string& id) const
    {
        const auto found = std::find_if(
            m_views.begin(), m_views.end(),
            [&id](const View& view) { return view.id == id; });
        return found == m_views.end() ? nullptr : found->state;
    }

private:
    struct View final {
        std::string id;
        HostRenderViewRole role = HostRenderViewRole::Auxiliary;
        HostInputMode inputMode = HostInputMode::NativeInteractor;
        std::shared_ptr<AppPortState> state;
        std::shared_ptr<AppDataPort> data;
        std::shared_ptr<AppViewPort> view;
        std::shared_ptr<AppSessionPort> session;
        std::shared_ptr<RenderUpdatePort> update;
        std::shared_ptr<AbstractViewContext> context;
    };

    const View* GetView(const HostViewTarget& target) const
    {
        if (!target.viewId.empty()) {
            const auto found = std::find_if(
                m_views.begin(), m_views.end(),
                [&target](const View& view) {
                    return view.id == target.viewId;
                });
            return found == m_views.end() ? nullptr : &*found;
        }
        if (target.isViewRoleUsed) {
            const auto found = std::find_if(
                m_views.begin(), m_views.end(),
                [&target](const View& view) {
                    return view.role == target.viewRole;
                });
            return found == m_views.end() ? nullptr : &*found;
        }
        return nullptr;
    }

    const View* GetDataView(const HostViewTarget& target) const
    {
        if (!target.viewId.empty() || target.isViewRoleUsed) {
            return GetView(target);
        }
        const auto primary = std::find_if(
            m_views.begin(), m_views.end(),
            [](const View& view) {
                return view.role == HostRenderViewRole::Primary3D;
            });
        return primary != m_views.end()
            ? &*primary
            : (m_views.empty() ? nullptr : &m_views.front());
    }

    static HostRenderMode GetHostMode(const VizMode mode)
    {
        switch (mode) {
        case VizMode::Volume: return HostRenderMode::Volume;
        case VizMode::IsoSurface: return HostRenderMode::IsoSurface;
        case VizMode::SliceTop_down: return HostRenderMode::SliceTopDown;
        case VizMode::SliceFront_back: return HostRenderMode::SliceFrontBack;
        case VizMode::SliceLeft_right: return HostRenderMode::SliceLeftRight;
        case VizMode::CompositeVolume:
            return HostRenderMode::CompositeVolume;
        case VizMode::CompositeIsoSurface:
            return HostRenderMode::CompositeIsoSurface;
        }
        return HostRenderMode::Volume;
    }

    std::vector<View> m_views;
};
