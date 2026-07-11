#include "AppStateTests.h"

#include "App/AppState.h"

#include <iostream>
#include <memory>
#include <vector>

namespace {

class StateEventSink final : public IStateEventSink {
public:
    void SendFlags(UpdateFlags flags) override
    {
        m_events.push_back(flags);
    }

    const std::vector<UpdateFlags>& GetEvents() const
    {
        return m_events;
    }

private:
    std::vector<UpdateFlags> m_events;
};

}

int AppStateSuite::GetFailCount() const
{
    int failureCount = 0;
    const auto sink = std::make_shared<StateEventSink>();
    SharedInteractionState state(sink);

    auto material = state.GetMaterial();
    material.opacity = 0.42;
    state.SetMaterial(material);
    state.SetMaterial(material);
    if (sink->GetEvents().size() != 1
        || sink->GetEvents().front() != UpdateFlags::Material) {
        std::cerr << "App state must broadcast one material diff.\n";
        ++failureCount;
    }

    state.SetSpacing(0.5, 0.75, 1.25);
    state.SetSpacing(0.5, 0.75, 1.25);
    if (sink->GetEvents().size() != 2
        || sink->GetEvents().back() != UpdateFlags::Spacing) {
        std::cerr << "App state must broadcast one spacing diff.\n";
        ++failureCount;
    }
    return failureCount;
}
