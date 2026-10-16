#pragma once

#include "Host/TrustedDataPort.h"
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace FeatureInternal {
// 数据图是生命周期真源；账本只保留本生产者尚未释放的退役所有权。
class ResultScopes final {
    using ResultKey = std::set<std::tuple<std::string, std::string, DataTypeId>>;
    struct Scope final {
        DataLifetimeRetirement retirement;
        std::set<std::string> bindings;
        ResultKey resultKey;
        bool hasBoundResult = false;
        bool isSuperseded = false;
    };
    struct Guard final {
        bool& flag;
        explicit Guard(bool& value) : flag(value) { flag = true; }
        ~Guard() { flag = false; }
    };
    std::vector<Scope> m_scopes;
    bool m_busy = false;

    static bool GetOwns(const Scope& scope, const DataRevisionRef& ref) {
        const auto& refs = scope.retirement.expectedRevisions;
        return std::find(refs.begin(), refs.end(), ref) != refs.end();
    }
    // 与数据图的持久结果使用相同的替换身份；不在这里复制依赖或资源判断。
    static ResultKey GetResultKey(const DataTransaction& transaction) {
        ResultKey key;
        for (const auto& draft : transaction.outputs) {
            if (draft.inputs.empty() || !draft.provenance
                || draft.provenance->producerId.empty() || draft.provenance->operationId.empty()
                || draft.provenance->producerId == "org.mvvcvtk.host") return {};
            key.emplace(draft.provenance->producerId, draft.provenance->operationId, draft.type);
        }
        return key;
    }
    static bool GetBound(const Scope& scope, TrustedDataPort& data, const DataGraphSnapshot& graph) {
        for (const auto& name : scope.bindings) {
            const auto binding = data.GetDataBinding(graph, name);
            if (binding && binding->target && GetOwns(scope, *binding->target)) return true;
        }
        return false;
    }
    // 仅跳过仍有可读外部后代的候选；实际退役仍由数据图在锁内重新校验。
    static bool GetHasLiveDerived(const Scope& scope, TrustedDataPort& data, const DataGraphSnapshot& graph) {
        if (!graph.view) return false;
        for (const auto& ref : scope.retirement.expectedRevisions)
            for (const auto& derived : graph.view->GetDerivedData(ref))
                if (!GetOwns(scope, derived) && data.GetData(graph, derived)) return true;
        return false;
    }
    void CollectReleased(TrustedDataPort& data) noexcept {
        // 从新到旧退役，使已替换的派生结果先解除对更早结果的保护。
        for (auto index = m_scopes.size(); index != 0;) {
            auto& scope = m_scopes[--index];
            try {
                auto state = data.GetDataLifetime(scope.retirement.scopeId);
                if (state.status == DataLifetimeStatus::Published) {
                    const auto graph = data.GetDataGraph();
                    if (GetBound(scope, data, graph)) continue;
                    if (scope.hasBoundResult) scope.isSuperseded = true;
                    if (scope.isSuperseded) {
                        if (GetHasLiveDerived(scope, data, graph)) continue;
                        DataTransaction retirement;
                        retirement.retireScopes.push_back(scope.retirement);
                        if (data.SetDataCommit(std::move(retirement)).status != DataCommitStatus::Succeeded)
                            continue;
                        state.status = DataLifetimeStatus::Releasing;
                    }
                }
                if (state.status == DataLifetimeStatus::Releasing)
                    state = data.SetDataRelease(scope.retirement.scopeId);
                if (state.status == DataLifetimeStatus::Released)
                    m_scopes.erase(m_scopes.begin() + index);
            } catch (...) {
                // 发布已成功；清理分配失败或暂时受阻时保留所有权，后续提交/Clear 重试。
            }
        }
    }
public:
    std::size_t GetScopeCount() const noexcept { return m_scopes.size(); }

    // 关闭显示或释放私有快照后可再次收敛；暂时受保护的结果继续由创建者持有。
    void Collect(TrustedDataPort& data) noexcept {
        if (m_busy || m_scopes.empty()) return;
        try {
            Guard guard(m_busy);
            auto batch = data.StartDataChanges();
            if (batch) CollectReleased(data);
        } catch (...) {}
    }

    DataCommitResult Commit(TrustedDataPort& data, DataTransaction transaction, const DataSnapshot& policySource = {}) {
        if (m_busy) return {};
        bool scoped = false, explicitRetirement = false;
        const auto graph = data.GetDataGraph();
        const auto readPolicy = [&](const DataSnapshot& source) {
            if (source && GetDataEntityIdValid(source->lifetimeScope)) {
                scoped = scoped || source->lifetimePolicy == DataLifetimePolicy::OwnerRetirement;
                explicitRetirement = explicitRetirement || source->lifetimePolicy == DataLifetimePolicy::ExplicitRetirement;
            }
        };
        // Part 的寿命继承由主输入决定；其他调用者沿用全部直接输入的既有规则。
        if (policySource) readPolicy(policySource);
        else for (const auto& draft : transaction.outputs) for (const auto& input : draft.inputs)
            readPolicy(data.GetData(graph, input.source));
        if (!scoped && explicitRetirement && !transaction.outputs.empty()) {
            const auto scope = data.CreateDataEntityId();
            for (auto& draft : transaction.outputs) {
                draft.lifetimeScope = scope;
                draft.lifetimePolicy = DataLifetimePolicy::ExplicitRetirement;
            }
        }
        if (!scoped && m_scopes.empty()) return data.SetDataCommit(std::move(transaction));
        Guard guard(m_busy);
        auto next = m_scopes;
        const auto resultKey = GetResultKey(transaction);
        if (scoped) {
            Scope scope;
            scope.retirement = {data.CreateDataEntityId(), DataLifetimeStatus::Published, {}, true};
            scope.resultKey = resultKey;
            for (auto& draft : transaction.outputs) {
                draft.lifetimeScope = scope.retirement.scopeId;
                draft.lifetimePolicy = DataLifetimePolicy::OwnerRetirement;
                scope.retirement.expectedRevisions.push_back({draft.entityId, draft.expectedGeneration + 1});
            }
            next.push_back(std::move(scope));
        }
        for (const auto& binding : transaction.bindings) if (binding.target)
            for (auto& scope : next) if (GetOwns(scope, *binding.target))
                scope.bindings.insert(binding.binding);
        // 通知释放前先采用完整账本，避免观察者重入关闭时丢失结果所有权。
        auto batch = data.StartDataChanges();
        if (!batch) return {};
        const auto pendingBindings = transaction.bindings;
        DataCommitResult committed;
        try {
            committed = data.SetDataCommit(std::move(transaction));
        } catch (...) {
            // 私有端口包装可能在正式提交后抛出；仅在新 scope 或全部绑定确认已提交时接管候选。
            bool isCommitted = scoped
                && data.GetDataLifetime(next.back().retirement.scopeId).status != DataLifetimeStatus::Unknown;
            const auto current = data.GetDataGraph();
            if (!scoped && !pendingBindings.empty()) {
                isCommitted = std::all_of(pendingBindings.begin(), pendingBindings.end(), [&](const auto& update) {
                    const auto binding = data.GetDataBinding(current, update.binding);
                    return binding && binding->revision == update.expectedRevision + 1 && binding->target == update.target;
                });
            }
            if (isCommitted) {
                m_scopes.swap(next);
                for (auto& scope : m_scopes)
                    if (GetBound(scope, data, current)) scope.hasBoundResult = true;
            }
            throw;
        }
        if (committed.status == DataCommitStatus::Succeeded || committed.status == DataCommitStatus::SucceededHistorical) {
            if (committed.status == DataCommitStatus::Succeeded) {
                if (!resultKey.empty())
                    for (auto index = std::size_t{0}; index < next.size() - (scoped ? 1 : 0); ++index)
                        if (next[index].resultKey == resultKey) next[index].isSuperseded = true;
                for (const auto& binding : committed.bindings) if (binding.target)
                    for (auto& scope : next) if (GetOwns(scope, *binding.target)) scope.hasBoundResult = true;
            }
            m_scopes.swap(next);
            CollectReleased(data);
        }
        return committed;
    }
    bool Clear(TrustedDataPort& data) {
        if (m_busy) return false;
        if (m_scopes.empty()) return true;
        Guard guard(m_busy);
        auto batch = data.StartDataChanges();
        if (!batch) return false;
        DataTransaction transaction;
        std::set<std::string> names;
        std::set<DataRevisionRef> refs;
        const auto graph = data.GetDataGraph();
        for (const auto& scope : m_scopes) {
            const auto state = data.GetDataLifetime(scope.retirement.scopeId);
            if (state.status == DataLifetimeStatus::Published) transaction.retireScopes.push_back(scope.retirement);
            else if (state.status != DataLifetimeStatus::Releasing && state.status != DataLifetimeStatus::Released) return false;
            names.insert(scope.bindings.begin(), scope.bindings.end());
            refs.insert(scope.retirement.expectedRevisions.begin(), scope.retirement.expectedRevisions.end());
        }
        // 只解除本生产者写入且仍指向自有结果的绑定，不改变其他消费者的绑定。
        for (const auto& name : names) if (const auto binding = data.GetDataBinding(graph, name); binding && binding->target && refs.count(*binding->target))
            transaction.bindings.push_back({name, binding->revision, true, binding->target, {}});
        if ((!transaction.retireScopes.empty() || !transaction.bindings.empty())
            && data.SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded) return false;
        for (auto scope = m_scopes.begin(); scope != m_scopes.end();)
            if (data.SetDataRelease(scope->retirement.scopeId).status == DataLifetimeStatus::Released) scope = m_scopes.erase(scope);
            else ++scope;
        return m_scopes.empty();
    }
};
}
