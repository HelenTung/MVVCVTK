#pragma once

#include "Interaction/CropHistory.h"
#include <deque>
#include <map>

// 一个文档的已接纳命令。只保存不可变请求/预留身份，正式节点仍仅由 CropHistory 拥有。
class CropHistoryQueue final {
public:
    static bool GetRequestsSame(const CropEditRequest& a,const CropEditRequest& b);
    void ForgetOutcome(CropRequestId requestId) noexcept;
    CropEditAdmission StartRequest(CropHistory& history, CropEditRequest request);
    std::optional<CropEditOutcome> GetOutcome(CropRequestId requestId) const;
    std::optional<CropNodeSnapshot> GetNode(const CropHistory& history,CropNodeId nodeId) const;
    CropHistory::Stage BuildNext(const CropHistory& history) const;
    const CropEditRequest* GetNext() const noexcept;
    bool GetIsEmpty() const noexcept { return m_pending.empty(); }
    std::size_t GetPendingCount() const noexcept { return m_pending.size(); }
    // GPU 全目标预检完成后只 move/erase/pop，不分配、不执行外部回调。
    void SetComplete(CropHistory& history,CropHistory::Stage&& stage) noexcept;
    void SetFailed(CropHistory& history,CropFailure failure,CropPruneImpact impact = {}) noexcept;
    void SetCancelled(CropHistory& history) noexcept;
private:
    struct Entry final {
        CropEditRequest request;
        CropEditOutcome outcome;
        std::optional<CropNodeSnapshot> reserved;
        std::uint64_t admissionOrder = 0;
        bool hadPendingDependency = false;
    };
    void SetRequestedHead(CropHistory& history) const noexcept;
    void ClearExpired();
    std::map<CropRequestId,Entry> m_entries;
    std::deque<CropRequestId> m_pending;
    CropRequestId m_expiredThrough = 0;
    std::uint64_t m_nextOrder = 1;
};
