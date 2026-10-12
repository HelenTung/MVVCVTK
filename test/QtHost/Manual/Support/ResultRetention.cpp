#include "ResultRetention.h"
#include "JsonInput.h"
#include <algorithm>
namespace Manual {
namespace {
std::string GetKeepBinding(DataRevisionRef ref)
{
    return "manual.retained." + GetRefText(ref).toStdString();
}
}
bool ResultRetention::AttachHost(const HostFeatureContext& context)
{
    if (m_data || !context.data) return false;
    m_data = context.data;
    m_releasing=std::make_shared<ReleaseQueue>();
    m_observer = m_data->AttachDataChange([queue=m_releasing](const DataChangeSet& change) {
        const std::lock_guard<std::mutex> lock(queue->mutex);
        for (const auto& id : change.retiredScopes)
            if (std::find(queue->scopes.begin(),queue->scopes.end(),id)==queue->scopes.end()) queue->scopes.push_back(id);
    });
    if (!m_observer) { m_data.reset(); return false; }
    return true;
}
bool ResultRetention::DetachHost()
{
    if (m_data && m_observer && !m_data->DetachDataChange(m_observer)) return false;
    m_observer=0; m_releasing.reset(); m_data.reset(); return true;
}
bool ResultRetention::OnHostTick()
{
    if (!m_data) return false;
    const auto data=m_data; const auto queue=m_releasing;
    std::vector<DataEntityId> pending;
    { const std::lock_guard<std::mutex> lock(queue->mutex); pending.swap(queue->scopes); }
    for (auto id=pending.begin(); id!=pending.end();)
        if (data->SetDataRelease(*id).status==DataLifetimeStatus::Released) id=pending.erase(id); else ++id;
    { const std::lock_guard<std::mutex> lock(queue->mutex);
        for (const auto& id : pending)
            if (std::find(queue->scopes.begin(),queue->scopes.end(),id)==queue->scopes.end()) queue->scopes.push_back(id);
    }
    return true;
}
bool ResultRetention::GetRetained(DataRevisionRef ref) const
{
    if (!m_data) return false;
    const auto binding=m_data->GetDataBinding(m_data->GetDataGraph(),GetKeepBinding(ref));
    return binding && binding->target==ref;
}
DataCommitResult ResultRetention::SetRetained(DataRevisionRef ref, bool retained)
{
    if (!m_data) { DataCommitResult result; result.message="会话不可用"; return result; }
    const auto graph=m_data->GetDataGraph();
    if (!m_data->GetData(graph,ref)) { DataCommitResult result; result.message="所选结果已卸载，请重新选择"; return result; }
    const auto name=GetKeepBinding(ref);
    const auto binding=m_data->GetDataBinding(graph,name);
    DataTransaction transaction;
    transaction.bindings.push_back({name,binding?binding->revision:0,true,
        binding?binding->target:std::optional<DataRevisionRef>{},retained?std::optional<DataRevisionRef>{ref}:std::nullopt});
    return m_data->SetDataCommit(std::move(transaction));
}
DataCommitResult ResultRetention::UnloadResult(DataRevisionRef ref)
{
    if (!m_data) return {};
    const auto value=m_data->GetData(m_data->GetDataGraph(),ref);
    if (!value || !value->provenance || value->provenance->producerId=="org.mvvcvtk.host"
        || value->lifetimePolicy!=DataLifetimePolicy::ExplicitRetirement
        || !GetDataEntityIdValid(value->lifetimeScope)) {
        DataCommitResult result; result.message="此结果由所属业务管理，请先清除或退出该业务"; return result;
    }
    const auto scope=m_data->GetDataLifetime(value->lifetimeScope);
    DataTransaction transaction;
    transaction.retireScopes.push_back({scope.scopeId,scope.status,scope.ownedRevisions,true});
    // 显式保留、当前显示和下游业务绑定仍由图事务阻塞；卸载不夺取其他消费者的资源。
    return m_data->SetDataCommit(std::move(transaction));
}
}
