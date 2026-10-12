#pragma once
#include "Data/DataGraphTypes.h"
#include <optional>
#include <set>
#include <utility>

namespace FeatureInternal {
using InputReads = std::vector<std::shared_ptr<const DataResourceLease>>;
// 在途候选还没有图依赖边；Reader 保证其精确输入仍可用于后续条件发布。
inline std::optional<InputReads> StartInputReads(const DataGraphSnapshot& graph,
    const std::vector<DataRevisionRef>& refs, const std::string& owner)
{
    if (!graph.view) return {};
    InputReads readers; std::set<DataEntityId> scopes;
    for (const auto& ref : refs) {
        const auto value=graph.view->GetData(ref);
        if (!value) return {};
        if (!GetDataEntityIdValid(value->lifetimeScope) || !scopes.insert(value->lifetimeScope).second) continue;
        const auto lifetime=value->lifetime.lock();
        auto reader=lifetime?lifetime->StartResourceUse(ref,owner):nullptr;
        if (!reader) return {};
        readers.push_back(std::move(reader));
    }
    return readers;
}
inline std::optional<InputReads> StartInputReads(const DataGraphSnapshot& graph,
    const std::vector<DataExpectation>& expectations, const std::string& owner)
{
    std::vector<DataRevisionRef> refs;
    for (const auto& expected : expectations)
        if (expected.kind==DataExpectationKind::EntityHead)
            refs.push_back({expected.entityId,expected.expectedGeneration});
        else if (expected.expectedTarget) refs.push_back(*expected.expectedTarget);
    return StartInputReads(graph,refs,owner);
}
}
