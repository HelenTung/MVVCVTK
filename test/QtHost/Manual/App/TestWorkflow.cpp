// 测试用途：协调功能测试中的计算占用、输入选择和交互结束，避免互斥操作并发。
#include "TestWorkflow.h"
namespace Manual {
bool TestWorkflow::StartOperation(std::uint64_t id, TestPolicy policy, bool exitTools, const QString& module)
{
    if (m_isClosing) return false;
    const bool isRead = policy == TestPolicy::Read || policy == TestPolicy::View || policy == TestPolicy::Stop;
    if (m_busyId && !isRead) return false;
    if (exitTools) for (const auto& exit : m_exits)
        if ((exit.first != module || policy == TestPolicy::Input) && !exit.second()) return false;
    if (policy == TestPolicy::Compute || policy == TestPolicy::Input) m_busyId = id;
    return true;
}
void TestWorkflow::SetComplete(std::uint64_t id)
{
    if (m_busyId == id) m_busyId = 0;
}
}
