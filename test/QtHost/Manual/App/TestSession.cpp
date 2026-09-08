// 测试用途：为手动和自动化测试持有单个宿主会话及功能对象，验证统一停止和资源释放。
#include "TestSession.h"
namespace Manual {
bool TestSession::BuildSession(HostSessionConfig config)
{
    m_session = std::make_shared<VtkAppHostSession>(std::move(config));
    return m_session->BuildSession();
}
bool TestSession::AttachFeature(const std::shared_ptr<HostFeature>& feature)
{
    if (!m_session || !m_session->AttachFeature(feature)) return false;
    m_features.push_back(feature);
    return true;
}
bool TestSession::Stop()
{
    if (!m_session) return true;
    if (!m_session->Stop() || !m_session->GetIsStopped()) return false;
    m_features.clear();
    m_session.reset();
    return true;
}
}
