#pragma once
#include "App/Services/AppPorts.h"
#include <functional>
#include <memory>
#include <optional>
class AbstractViewContext;
class RenderUpdatePort;

// 单次 Host 跨组件事务；AppViewPort 继续负责其局部字段事务。
class HostViewTransaction final {
public:
    HostViewTransaction(std::shared_ptr<AppViewPort> view,
        std::shared_ptr<RenderUpdatePort> update,
        std::shared_ptr<AbstractViewContext> context,
        std::function<bool()> stopView);
    bool SetView(const AppViewUpdate& update, std::optional<bool> isAxesVisible) const;
    bool ResetView() const;
private:
    std::shared_ptr<AppViewPort> m_view;
    std::shared_ptr<RenderUpdatePort> m_update;
    std::shared_ptr<AbstractViewContext> m_context;
    std::function<bool()> m_stopView;
};
