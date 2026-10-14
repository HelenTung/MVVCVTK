#pragma once

#include "Data/DataGraphStore.h"
#include "Data/RoiTypes.h"

class AbstractDataManager;

// Host 私有薄服务：唯一正式状态位于传入的 DataGraphStore。
class RoiService final {
public:
    // Session 退出时回收依赖临时来源的 Host ROI；独立 Feature 卸载不调用此清理。
    static bool ReleaseTransientScopes(AbstractDataManager& data,
        std::vector<DataEntityId>& releasingScopes);
    static RoiArchiveReadResult GetArchiveReader(const DataGraphSnapshot&,const std::vector<RoiArchiveSelection>&);
    RoiArchiveRestoreResult LoadArchiveReader(const RoiArchiveReader&,const std::vector<RoiSourceMapping>&);
    static RoiArchiveBundleResult GetArchiveBundle(const DataGraphSnapshot&,const std::vector<RoiArchiveSelection>&);
    RoiArchiveRestoreResult LoadArchiveBundle(const RoiArchiveBundle&,const std::vector<RoiSourceMapping>&);
    explicit RoiService(DataGraphStore& store) noexcept : m_store(store) {}
    RoiResult SetRoi(const RoiRequest& request);
    static RoiOperationResult GetOperation(const DataGraphSnapshot& graph,const RoiOperationRequest& request);
    RoiOperationResult SetOperation(const RoiOperationRequest& request);
    RoiOperationResult SetOperationCommit(const RoiOperationRequest& request,RoiOperationResult candidate);
    static std::vector<RoiDescriptor> GetDescriptors(
        const DataGraphSnapshot& graph, bool includeArchived = false);
    static std::optional<RoiDescriptor> GetDescriptor(
        const DataGraphSnapshot& graph, const DataRevisionRef& ref);
    static RoiArchiveResult GetArchive(const DataGraphSnapshot& graph,
        const DataRevisionRef& ref, const std::string& sourceKey);
    RoiResult LoadArchive(const RoiArchive& archive, const std::string& sourceKey,
        const DataRevisionRef& sourceRef, DataBindingRevision expectedCatalogRevision);
private:
    static bool GetInputTransient(const DataSnapshot& input) noexcept;
    RoiResult SetRoiCommit(const RoiRequest& request, std::vector<DataRevisionDraft> masks);
    DataGraphStore& m_store;
};
