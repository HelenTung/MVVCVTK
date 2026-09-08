#pragma once

#include "OrthogonalCropTypes.h"

#include <map>
#include <set>

// Feature 私有业务真源。Stage 预先分配 map 节点/child 索引，提交只做 merge/swap/erase。
class CropHistory final {
public:
    struct Stage final {
        CropFailure failureReason = CropFailure::None;
        CropDocumentId documentId = 0;
        std::uint64_t expectedRevision = 0;
        CropNodeId head = 0;
        std::vector<CropOpItem> operations;
        CropPruneImpact impact;
    private:
        friend class CropHistory;
        std::map<CropNodeId, CropNodeSnapshot> additions;
        std::map<CropNodeId, std::vector<CropNodeId>> childAdditions;
        std::map<CropNodeId, std::vector<CropNodeId>> childChanges;
        std::set<CropNodeId> deletions;
    };

    static CropHistory Create(DataRevisionRef source, CropDocumentId documentId = 0);
    static CropNodeId CreateNodeId() noexcept;
    CropHistorySnapshot GetSnapshot(CropNodeId after = 0, std::size_t limit = 1000) const;
    std::optional<CropNodeSnapshot> GetNode(CropNodeId nodeId) const;
    std::vector<CropNodeId> GetChildren(CropNodeId nodeId) const;
    std::vector<CropOpItem> GetPath(CropNodeId nodeId) const;
    CropDocumentId GetDocumentId() const noexcept { return m_documentId; }
    CropNodeId GetRootId() const noexcept { return m_root; }
    CropNodeId GetAppliedHead() const noexcept { return m_appliedHead; }
    CropNodeId GetRequestedHead() const noexcept { return m_requestedHead; }
    std::uint64_t GetRevision() const noexcept { return m_revision; }
    std::size_t GetNodeCount() const noexcept { return m_nodes.size(); }
    const std::vector<CropResultRecord>& GetResults() const noexcept { return m_results; }

    Stage BuildAppend(CropNodeId parent, CropOpItem operation, CropNodeId reservedId = 0) const;
    Stage BuildReplace(CropNodeId original, CropOpItem operation, CropNodeId reservedId = 0) const;
    Stage BuildSelection(CropNodeId nodeId) const;
    Stage BuildPrune(const CropPruneRequest& request, bool isQueuedCommand = false) const;
    CropPruneImpact GetPruneImpact(const CropPruneRequest& request) const;
    bool GetStageReady(const Stage& stage) const noexcept;
    void SetCommit(Stage&& stage) noexcept;
    // 只由已接纳命令队列设置；允许引用该队列独占预留、尚未正式插入的 ID。
    void SetRequestedHead(CropNodeId nodeId) noexcept { m_requestedHead = nodeId; }
    bool GetResultsValid(const std::vector<CropResultRecord>& results) const noexcept;
    void SetResults(std::vector<CropResultRecord>&& results) noexcept;
    CropDocumentArchive GetArchive() const;
    static std::optional<CropHistory> CreateFromArchive(const CropDocumentArchive& archive,
        CropFailure& failure);

private:
    Stage BuildStage(CropNodeId head) const;
    std::vector<CropNodeId> GetPathIds(CropNodeId nodeId) const;
    bool SetSubtree(CropNodeId nodeId, std::set<CropNodeId>& nodes) const;

    CropDocumentId m_documentId = 0;
    CropNodeId m_root = 0;
    DataRevisionRef m_source;
    std::uint64_t m_revision = 0;
    CropNodeId m_requestedHead = 0;
    CropNodeId m_appliedHead = 0;
    std::map<CropNodeId, CropNodeSnapshot> m_nodes;
    std::map<CropNodeId, std::vector<CropNodeId>> m_children;
    std::vector<CropResultRecord> m_results;
};
