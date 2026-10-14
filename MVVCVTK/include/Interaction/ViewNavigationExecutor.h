#pragma once
#include "Interaction/IInteractionHandler.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/NavigationAccess.h"
#include "Interaction/NavigationBindings.h"

class vtkRenderer;
// 每 View 的执行和手势状态；相机状态始终读取实际 renderer。
class ViewNavigationExecutor final {
public:
    ViewNavigationExecutor(InteractionPorts,vtkRenderer*,std::function<bool()>,std::function<bool()>);
    ~ViewNavigationExecutor();
    std::shared_ptr<ViewNavigation> GetNavigation() const;
    NavigationStatus SetNavigation(std::shared_ptr<ViewNavigation>);
    void ResetReference();
    void SetMode(VizMode);
    void SetPointerPosition(std::optional<std::array<double,2>>,double displayScale=1);
    InteractionResult Send(const InteractionEvent&,const NavigationBindings&);
    NavigationStatus Trigger(std::string_view,const NavigationInput&);
    InteractionCaptureKey GetCaptureKey() const noexcept;
    InteractionEventKind GetReleaseKind() const noexcept;
    bool GetIsActive() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
class ViewNavigationHandler final : public IInteractionHandler {
public:
    ViewNavigationHandler(std::shared_ptr<ViewNavigationExecutor>,const NavigationBindings&);
    InteractionResult Send(const InteractionEvent&) override;
    InteractionDispatch Route(const InteractionEvent&) override;
private:
    std::shared_ptr<ViewNavigationExecutor> m_executor;
    const NavigationBindings& m_bindings;
};
