#pragma once

#include "Host/TrustedDataPort.h"
#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace FeatureInternal {
// 私有生产者账本：普通结果保持持久化；依赖临时输入的结果由创建者负责退休。
class ResultScopes final {
    struct Scope final {
        DataLifetimeRetirement retirement;
        std::set<std::string> bindings;
    };
    struct Guard final {
        bool& flag;
        explicit Guard(bool& value):flag(value){flag=true;}
        ~Guard(){flag=false;}
    };
    std::vector<Scope> m_scopes;
    bool m_busy=false;
public:
    DataCommitResult Commit(TrustedDataPort& data,DataTransaction transaction) {
        if(m_busy)return {};
        bool scoped=false;const auto graph=data.GetDataGraph();
        for(const auto& draft:transaction.outputs)for(const auto& input:draft.inputs) {
            const auto source=data.GetData(graph,input.source);
            scoped=scoped||(source&&GetDataEntityIdValid(source->lifetimeScope));
        }
        if(!scoped&&m_scopes.empty())return data.SetDataCommit(std::move(transaction));
        Guard guard(m_busy);
        auto next=m_scopes;
        if(scoped) {
            if(next.size()>=1024)return {};
            Scope scope;scope.retirement={data.CreateDataEntityId(),DataLifetimeStatus::Published,{},true};
            for(auto& draft:transaction.outputs) {
                draft.lifetimeScope=scope.retirement.scopeId;
                scope.retirement.expectedRevisions.push_back({draft.entityId,draft.expectedGeneration+1});
            }
            next.push_back(std::move(scope));
        }
        for(const auto& binding:transaction.bindings)if(binding.target)
            for(auto& scope:next)if(std::find(scope.retirement.expectedRevisions.begin(),scope.retirement.expectedRevisions.end(),*binding.target)!=scope.retirement.expectedRevisions.end())
                scope.bindings.insert(binding.binding);
        // 通知释放前先采用完整账本，避免观察者重入关闭时丢失结果所有权。
        auto batch=data.StartDataChanges();if(!batch)return {};
        auto committed=data.SetDataCommit(std::move(transaction));
        if(committed.status==DataCommitStatus::Succeeded||committed.status==DataCommitStatus::SucceededHistorical)m_scopes.swap(next);
        return committed;
    }
    bool Clear(TrustedDataPort& data) {
        if(m_busy)return false;
        if(m_scopes.empty())return true;
        Guard guard(m_busy);auto batch=data.StartDataChanges();if(!batch)return false;
        DataTransaction transaction;std::set<std::string> names;std::set<DataRevisionRef> refs;
        const auto graph=data.GetDataGraph();
        for(const auto& scope:m_scopes) {
            const auto state=data.GetDataLifetime(scope.retirement.scopeId);
            if(state.status==DataLifetimeStatus::Published)transaction.retireScopes.push_back(scope.retirement);
            else if(state.status!=DataLifetimeStatus::Releasing&&state.status!=DataLifetimeStatus::Released)return false;
            names.insert(scope.bindings.begin(),scope.bindings.end());
            refs.insert(scope.retirement.expectedRevisions.begin(),scope.retirement.expectedRevisions.end());
        }
        // 只解除本生产者写入且仍指向自有结果的绑定，不改变其他消费者的绑定。
        for(const auto& name:names)if(const auto binding=data.GetDataBinding(graph,name);binding&&binding->target&&refs.count(*binding->target))
            transaction.bindings.push_back({name,binding->revision,true,binding->target,{}});
        if((!transaction.retireScopes.empty()||!transaction.bindings.empty())
            &&data.SetDataCommit(std::move(transaction)).status!=DataCommitStatus::Succeeded)return false;
        for(auto scope=m_scopes.begin();scope!=m_scopes.end();)
            if(data.SetDataRelease(scope->retirement.scopeId).status==DataLifetimeStatus::Released)scope=m_scopes.erase(scope);else ++scope;
        return m_scopes.empty();
    }
};
}
