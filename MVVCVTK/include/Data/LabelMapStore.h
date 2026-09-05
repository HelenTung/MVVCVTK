#pragma once

#include "Host/HostFeature.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class AbstractDataManager;

// Session 私有真源；公开 Host 只取得值副本，Feature 只取得绑定自身 ID 的窄端口。
class LabelMapStore final {
public:
    LabelMapStore(
        std::weak_ptr<AbstractDataManager> source,
        std::thread::id ownerThread);
    ~LabelMapStore();

    LabelMapStore(const LabelMapStore&) = delete;
    LabelMapStore& operator=(const LabelMapStore&) = delete;
    LabelMapStore(LabelMapStore&&) = delete;
    LabelMapStore& operator=(LabelMapStore&&) = delete;

    std::vector<LabelMapDescriptor> GetDescriptors() const;
    std::optional<LabelMapDescriptor> GetDescriptor(
        std::string_view id) const;
    LabelMapReadResult GetReadResult(
        const LabelMapReadRequest& request) const;
    LabelMapReadChunkResult GetReadChunk(
        const LabelMapReadRequest& request,
        std::size_t voxelOffset) const;
    TrustedLabelMapSnapshot GetSnapshot(std::string_view id) const;

    TrustedLabelMapStageResult Stage(
        std::string_view ownerFeatureId,
        TrustedLabelMapCandidate candidate);
    TrustedLabelMapCommitResult Commit(
        std::string_view ownerFeatureId,
        LabelMapStageToken token);
    bool Discard(
        std::string_view ownerFeatureId,
        LabelMapStageToken token) noexcept;
    TrustedLabelMapRemoveResult Remove(
        std::string_view ownerFeatureId,
        std::string_view id,
        std::optional<LabelMapVersion> expectedVersion);

    void RemoveOwner(std::string_view ownerFeatureId) noexcept;
    void Clear() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
