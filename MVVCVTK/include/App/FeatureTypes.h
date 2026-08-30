#pragma once

#include <string>

// 通用交互来源键；主体只比较完整键，不解释 Feature 的 owner/channel 文本。
struct InteractionSource final {
    std::string ownerId;
    std::string channelId;

    bool operator==(const InteractionSource& other) const noexcept
    {
        return ownerId == other.ownerId
            && channelId == other.channelId;
    }
};

// 通用 Feature 来源键；App 只比较 id，不解释具体模块名称。
struct FeatureSource final {
    std::string id;

    bool operator==(const FeatureSource& other) const noexcept
    {
        return id == other.id;
    }
};
