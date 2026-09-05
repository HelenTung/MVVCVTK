#pragma once

#include "Data/DataGraphTypes.h"

#include <memory>

// Host 私有组合根；公开 Feature 只取得 TrustedDataPort，不取得 Store identity。
class DataGraphStore final {
public:
    DataGraphStore();
    ~DataGraphStore();

    DataGraphStore(const DataGraphStore&) = delete;
    DataGraphStore& operator=(const DataGraphStore&) = delete;
    DataGraphStore(DataGraphStore&&) = delete;
    DataGraphStore& operator=(DataGraphStore&&) = delete;

    DataGraphSnapshot GetDataGraph() const;
    DataSnapshot GetData(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const;
    DataQueryResult GetDataQuery(
        const DataGraphSnapshot& graph,
        const DataQuery& query) const;
    std::optional<DataBinding> GetDataBinding(
        const DataGraphSnapshot& graph,
        std::string_view name) const;
    ProjectDataSnapshot GetProjectData() const;

    DataEntityId CreateDataEntityId();
    bool SetDataType(DataTypeDescriptor descriptor);
    DataCommitResult SetDataCommit(DataTransaction transaction);

    DataObserverId AttachDataChange(DataChangeCallback callback);
    bool DetachDataChange(DataObserverId observerId);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
