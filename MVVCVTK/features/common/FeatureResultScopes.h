#pragma once

#include "Host/TrustedDataPort.h"
#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace FeatureInternal {
// 数据图是生命周期真源；账本只保留本生产者尚未释放的退役所有权。
class ResultScopes final {
    using ResultKey = std::set<std::tuple<std::string, std::string, DataTypeId>>;
    using BindingClock = std::pair<std::string, DataBindingRevision>;
    struct Scope final {
        DataLifetimeRetirement retirement;
        std::map<BindingClock, bool> bindings;
        ResultKey resultKey;
        bool hasBoundResult = false;
        bool isSuperseded = false;
        bool isPublicationPending = false;
    };
    struct Guard final {
        bool& flag;
        explicit Guard(bool& value) : flag(value) { flag = true; }
        ~Guard() { flag = false; }
    };
    struct CommitRecovery final : DataCommitReceipt {
        struct Binding final {
            std::string name;
            std::optional<DataRevisionRef> target;
            DataBindingRevision expectedRevision = 0;
            DataBindingRevision actualRevision = 0;
        };
        ResultKey resultKey;
        DataEntityId newScope;
        std::vector<DataRevisionRef> outputs;
        std::vector<Binding> bindings;
        bool hasReceipt = false;
        bool wereOutputsPublished = false;
        bool wereBindingsApplied = false;
        CommitRecovery(const DataTransaction& transaction,
                       const ResultKey& key, DataEntityId scope)
            : resultKey(key), newScope(scope) {
            for (const auto& draft : transaction.outputs) {
                const DataRevisionRef ref{draft.entityId, draft.expectedGeneration + 1};
                outputs.push_back(ref);
            }
            for (const auto& update : transaction.bindings)
                bindings.push_back({update.binding, update.target, update.expectedRevision, 0});
        }
        // 仅接收本次 SetDataCommit 在正式交换后同步交付的真实结果；不分配、不查询或重入。
        // 预期时钟只负责预分配槽位，不能靠后来相同的值/事件证明本次调用生效。
        void SetDataCommitted(const DataCommitResult& committed) noexcept override {
            if (hasReceipt || committed.status == DataCommitStatus::Rejected) return;
            hasReceipt = true;
            wereOutputsPublished = !outputs.empty() && committed.published.size() == outputs.size();
            // 集合顺序不作为身份；按已准备的实体/绑定键匹配，仍不分配任何接收存储。
            if (wereOutputsPublished) for (std::size_t index = 0; index < outputs.size(); ++index) {
                bool found = false;
                for (std::size_t actualIndex = 0; actualIndex < committed.published.size(); ++actualIndex) {
                    const auto& actual = committed.published[actualIndex];
                    if (actual && actual->self == outputs[index]
                        && (!GetDataEntityIdValid(newScope) || actual->lifetimeScope == newScope)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    wereOutputsPublished = false;
                    break;
                }
            }
            // 历史输出发布不应用绑定；不能把 Published 或当前同 target 当激活历史。
            if (committed.status != DataCommitStatus::Succeeded || !committed.isActivated
                || committed.bindings.size() != bindings.size()) return;
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                auto& expected = bindings[index];
                if (expected.expectedRevision == std::numeric_limits<DataBindingRevision>::max()) return;
                bool found = false;
                for (std::size_t actualIndex = 0; actualIndex < committed.bindings.size(); ++actualIndex) {
                    const auto& actual = committed.bindings[actualIndex];
                    if (actual.name != expected.name) continue;
                    if (actual.target != expected.target || actual.revision != expected.expectedRevision + 1) return;
                    expected.actualRevision = actual.revision;
                    found = true;
                    break;
                }
                if (!found) return;
            }
            wereBindingsApplied = true;
        }
    };
    std::vector<Scope> m_scopes;
    std::optional<CommitRecovery> m_commitRecovery;
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
    static bool GetBound(Scope& scope, TrustedDataPort& data, const DataGraphSnapshot& graph) {
        bool bound = false;
        for (auto clock = scope.bindings.begin(); clock != scope.bindings.end();) {
            const auto binding = data.GetDataBinding(graph, clock->first.first);
            if (!binding || binding->revision != clock->first.second) clock = scope.bindings.erase(clock);
            else {
                bound = bound || (clock->second && binding->target && GetOwns(scope, *binding->target));
                ++clock;
            }
        }
        return bound;
    }
    static void AcceptBindings(std::vector<Scope>& scopes, const std::vector<DataBinding>& bindings) noexcept {
        for (const auto& binding : bindings) if (binding.target)
            for (auto& scope : scopes) if (GetOwns(scope, *binding.target)) {
                scope.hasBoundResult = true;
                for (auto& clock : scope.bindings)
                    if (clock.first.first == binding.name && clock.first.second == binding.revision) clock.second = true;
            }
    }
    static void AcceptRecoveredBindings(std::vector<Scope>& scopes, const CommitRecovery& recovery) noexcept {
        if (!recovery.hasReceipt) return;
        if (recovery.wereOutputsPublished) for (auto& scope : scopes)
            if (scope.retirement.scopeId == recovery.newScope) scope.isPublicationPending = false;
        if (!recovery.wereBindingsApplied) return;
        if (recovery.wereOutputsPublished && !recovery.resultKey.empty())
            for (auto& scope : scopes)
                if (scope.retirement.scopeId != recovery.newScope && scope.resultKey == recovery.resultKey)
                    scope.isSuperseded = true;
        for (const auto& binding : recovery.bindings) if (binding.target && binding.actualRevision)
            for (auto& scope : scopes) if (GetOwns(scope, *binding.target)) {
                scope.hasBoundResult = true;
                for (auto& clock : scope.bindings)
                    if (clock.first.first == binding.name && clock.first.second == binding.actualRevision) clock.second = true;
            }
    }
    static void DiscardUnconfirmedBindings(std::vector<Scope>& scopes) noexcept {
        for (auto& scope : scopes)
            for (auto clock = scope.bindings.begin(); clock != scope.bindings.end();)
                if (!clock->second) clock = scope.bindings.erase(clock);
                else ++clock;
    }
    // 仅跳过仍有可读外部后代的候选；实际退役仍由数据图在锁内重新校验。
    static bool GetHasLiveDerived(const Scope& scope, TrustedDataPort& data, const DataGraphSnapshot& graph) {
        if (!graph.view) return false;
        for (const auto& ref : scope.retirement.expectedRevisions)
            for (const auto& derived : graph.view->GetDerivedData(ref))
                if (!GetOwns(scope, derived) && data.GetData(graph, derived)) return true;
        return false;
    }
    static void SetSuperseded(std::vector<Scope>& scopes, std::size_t before, const ResultKey& key) noexcept {
        if (!key.empty())
            for (std::size_t index = 0; index < before; ++index)
                if (scopes[index].resultKey == key) scopes[index].isSuperseded = true;
    }
    bool ResolvePublications(TrustedDataPort& data) noexcept {
        for (std::size_t index = 0; index < m_scopes.size();) {
            auto& scope = m_scopes[index];
            if (!scope.isPublicationPending) { ++index; continue; }
            try {
                const auto state = data.GetDataLifetime(scope.retirement.scopeId);
                if (state.status == DataLifetimeStatus::Unknown) {
                    // 只有未确认候选可由 Unknown 排除；它从未成为需要退役的正式 scope。
                    m_scopes.erase(m_scopes.begin() + index);
                    continue;
                }
                // 没有本次真实回执时只确认候选退役责任，不从 Published 推断绑定或替换历史。
                scope.isPublicationPending = false;
            } catch (...) {
                // 原提交结果尚不可查；保守持有候选，后续 Collect/Clear 再确认。
            }
            ++index;
        }
        return std::none_of(m_scopes.begin(), m_scopes.end(), [](const Scope& scope) { return scope.isPublicationPending; });
    }
    bool ResolveCommit(TrustedDataPort& data) noexcept {
        if (m_commitRecovery) AcceptRecoveredBindings(m_scopes, *m_commitRecovery);
        if (!ResolvePublications(data)) return false;
        if (!m_commitRecovery) return true;
        try {
            const auto graph = data.GetDataGraph();
            if (!graph.view) return false;
            for (auto& scope : m_scopes)
                if (GetBound(scope, data, graph)) scope.hasBoundResult = true;
            DiscardUnconfirmedBindings(m_scopes);
            m_commitRecovery.reset();
            return true;
        } catch (...) {
            // 保留真实回执和候选责任；图/当前绑定不可查时先阻止后续写入，恢复后重试。
            return false;
        }
    }
    void CollectReleased(TrustedDataPort& data) noexcept {
        if (!ResolveCommit(data)) return;
        // 从新到旧退役，使已替换的派生结果先解除对更早结果的保护。
        for (auto index = m_scopes.size(); index != 0;) {
            auto& scope = m_scopes[--index];
            if (scope.isPublicationPending) continue;
            try {
                auto state = data.GetDataLifetime(scope.retirement.scopeId);
                if (state.status == DataLifetimeStatus::Published) {
                    const auto graph = data.GetDataGraph();
                    if (!graph.view) continue; // 无法取得正式图时保留已有绑定时钟，不能按缺失绑定收敛。
                    if (GetBound(scope, data, graph)) { scope.hasBoundResult = true; continue; }
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
        if (m_busy || (m_scopes.empty() && !m_commitRecovery)) return;
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
        if (!scoped && m_scopes.empty() && !m_commitRecovery) return data.SetDataCommit(std::move(transaction));
        Guard guard(m_busy);
        if (!ResolveCommit(data)) return {};
        auto next = m_scopes;
        const auto resultKey = GetResultKey(transaction);
        if (scoped) {
            Scope scope;
            scope.retirement = {data.CreateDataEntityId(), DataLifetimeStatus::Published, {}, true};
            scope.resultKey = resultKey;
            scope.isPublicationPending = true;
            for (auto& draft : transaction.outputs) {
                draft.lifetimeScope = scope.retirement.scopeId;
                draft.lifetimePolicy = DataLifetimePolicy::OwnerRetirement;
                scope.retirement.expectedRevisions.push_back({draft.entityId, draft.expectedGeneration + 1});
            }
            next.push_back(std::move(scope));
        }
        CommitRecovery recovery(transaction, resultKey,
            scoped ? next.back().retirement.scopeId : DataEntityId{});
        // 预测修订只预分配候选槽，不能证明是谁写入；真实回执确认前不算绑定或清理权限。
        // 同时保留先前已确认的时钟，失败的新候选不能覆盖合法旧所有权。
        for (const auto& binding : recovery.bindings) if (binding.target
            && binding.expectedRevision != std::numeric_limits<DataBindingRevision>::max())
            for (auto& scope : next) if (GetOwns(scope, *binding.target))
                scope.bindings.emplace(BindingClock{binding.name, binding.expectedRevision + 1}, false);
        // 通知释放前先采用完整账本，避免观察者重入关闭时丢失结果所有权。
        auto batch = data.StartDataChanges();
        if (!batch) return {};
        // 所有恢复记录都在真实提交前分配，只保存修订/绑定时钟，不复制 payload 或结果缓存。
        m_commitRecovery.emplace(std::move(recovery));
        DataCommitResult committed;
        try {
            committed = data.SetDataCommit(std::move(transaction), &*m_commitRecovery);
        } catch (...) {
            const auto original = std::current_exception();
            // swap 不分配；必须先接管，再调用任何可能继续抛出的恢复查询。
            // 先接管退役责任，绑定候选仍未确认；晚到的外部同修订写入不能变成本次回执。
            m_scopes.swap(next);
            (void)ResolveCommit(data);
            std::rethrow_exception(original);
        }
        m_commitRecovery.reset();
        if (committed.status == DataCommitStatus::Succeeded || committed.status == DataCommitStatus::SucceededHistorical) {
            if (scoped) next.back().isPublicationPending = false;
            if (committed.status == DataCommitStatus::Succeeded && committed.isActivated) {
                SetSuperseded(next, next.size() - (scoped ? 1 : 0), resultKey);
                AcceptBindings(next, committed.bindings);
            }
            DiscardUnconfirmedBindings(next);
            m_scopes.swap(next);
            CollectReleased(data);
        }
        return committed;
    }
    bool Clear(TrustedDataPort& data) {
        if (m_busy) return false;
        if (m_scopes.empty() && !m_commitRecovery) return true;
        Guard guard(m_busy);
        auto batch = data.StartDataChanges();
        if (!batch) return false;
        if (!ResolveCommit(data)) return false;
        if (m_scopes.empty()) return true;
        DataTransaction transaction;
        std::set<BindingClock> bindings;
        std::set<DataRevisionRef> refs;
        const auto graph = data.GetDataGraph();
        for (const auto& scope : m_scopes) {
            const auto state = data.GetDataLifetime(scope.retirement.scopeId);
            if (state.status == DataLifetimeStatus::Published) transaction.retireScopes.push_back(scope.retirement);
            else if (state.status != DataLifetimeStatus::Releasing && state.status != DataLifetimeStatus::Released) return false;
            for (const auto& clock : scope.bindings) if (clock.second) bindings.insert(clock.first);
            refs.insert(scope.retirement.expectedRevisions.begin(), scope.retirement.expectedRevisions.end());
        }
        // 只解除本生产者写入且仍指向自有结果的绑定，不改变其他消费者的绑定。
        for (const auto& clock : bindings) if (const auto binding = data.GetDataBinding(graph, clock.first);
            binding && binding->revision == clock.second && binding->target && refs.count(*binding->target))
            transaction.bindings.push_back({clock.first, binding->revision, true, binding->target, {}});
        if ((!transaction.retireScopes.empty() || !transaction.bindings.empty())
            && data.SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded) return false;
        for (auto scope = m_scopes.begin(); scope != m_scopes.end();)
            if (data.SetDataRelease(scope->retirement.scopeId).status == DataLifetimeStatus::Released) scope = m_scopes.erase(scope);
            else ++scope;
        return m_scopes.empty();
    }
};
}
