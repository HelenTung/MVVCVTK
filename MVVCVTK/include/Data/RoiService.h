#pragma once

#include "Data/DataGraphStore.h"
#include "Data/RoiTypes.h"

// Host 私有薄服务：唯一正式状态位于传入的 DataGraphStore。
class RoiService final {
public:
    explicit RoiService(DataGraphStore& store) noexcept : m_store(store) {}
    RoiResult SetRoi(const RoiRequest& request);
    static std::vector<RoiDescriptor> GetDescriptors(
        const DataGraphSnapshot& graph, bool includeArchived = false);
    static std::optional<RoiDescriptor> GetDescriptor(
        const DataGraphSnapshot& graph, const DataRevisionRef& ref);
    static RoiArchiveResult GetArchive(const DataGraphSnapshot& graph,
        const DataRevisionRef& ref, const std::string& sourceKey, std::size_t maxBytes);
    RoiResult LoadArchive(const RoiArchive& archive, const std::string& sourceKey,
        const DataRevisionRef& sourceRef, DataBindingRevision expectedCatalogRevision, std::size_t maxBytes);
private:
    RoiResult SetRoiCommit(const RoiRequest& request, std::vector<DataRevisionDraft> masks);
    DataGraphStore& m_store;
};
