#pragma once

#include "Data/DataPayloads.h"

#include <functional>

// 不进入 SDK surface。生产者交出唯一可写 owner 后，只能取得冻结快照。
class LabelMapBuilder final {
public:
    template<class Value>
    static std::shared_ptr<const LabelMap3DPayload> Build(
        GridGeometry3D geometry,
        std::unique_ptr<std::vector<Value>> labels,
        const std::function<bool()>& getStopRequested = nullptr,
        std::string id = {},
        std::string displayName = {})
    {
        const auto count = GetGridVoxelCount(geometry);
        if (!count || !GetGridGeometryValid(geometry) || !labels
            || labels->size() != *count || labels->empty()) return {};
        Value minimum = labels->front(), maximum = minimum;
        for (std::size_t index = 0; index < labels->size(); ++index) {
            if (index % 4096 == 0 && getStopRequested && getStopRequested()) return {};
            minimum = std::min(minimum, (*labels)[index]);
            maximum = std::max(maximum, (*labels)[index]);
        }
        auto payload = std::make_shared<LabelMap3DPayload>(
            std::move(geometry), LabelMapValues{}, std::vector<LabelDefinition>{},
            std::move(id), std::move(displayName));
        // unique_ptr 转换只转移 owner，不复制数组，也不保留生产者的可写 shared alias。
        payload->m_labels = std::shared_ptr<const std::vector<Value>>(std::move(labels));
        payload->m_scalarRange = { static_cast<double>(minimum), static_cast<double>(maximum) };
        return payload->GetValid() ? payload : nullptr;
    }

    static std::shared_ptr<const LabelMap3DPayload> ShareFrozen(
        const std::shared_ptr<const LabelMap3DPayload>& source,
        std::string id, std::string displayName)
    {
        if (!source || !source->GetValid()) return {};
        // 已冻结存储按快照共享；不同图节点仍保留各自的标签标识与显示名。
        auto payload = std::make_shared<LabelMap3DPayload>(*source);
        payload->m_id = std::move(id);
        payload->m_displayName = std::move(displayName);
        return payload->GetValid() ? payload : nullptr;
    }
};
