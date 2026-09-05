#pragma once

#include "Data/DataGraphTypes.h"
#include "Data/ImageReadTypes.h"

#include <string_view>
#include <utility>

// 一个冻结图上的普通读取投影；不存储标签、版本计数或待提交状态。
class LabelMapReader final {
public:
    explicit LabelMapReader(DataGraphSnapshot graph) : m_graph(std::move(graph)) {}
    std::vector<LabelMapDescriptor> GetDescriptors() const;
    std::optional<LabelMapDescriptor> GetDescriptor(std::string_view id) const;
    LabelMapReadResult GetReadResult(const LabelMapReadRequest& request) const;
    LabelMapReadChunkResult GetReadChunk(const LabelMapReadRequest& request, std::size_t voxelOffset) const;
private:
    DataGraphSnapshot m_graph;
};
