// 测试用途：为手动和自动化测试持有单个宿主会话及功能对象，验证统一停止和资源释放。
#pragma once
#include "Host/VtkAppHostSession.h"
#include "Host/HostFeature.h"
#include <memory>
#include <vector>
namespace Manual {
class TestSession final {
public:
    bool BuildSession(HostSessionConfig config);
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    bool Stop();
    std::shared_ptr<VtkAppHostSession> GetSession() const { return m_session; }
private:
    std::shared_ptr<VtkAppHostSession> m_session;
    std::vector<std::shared_ptr<HostFeature>> m_features;
};
}
