#pragma once

#include "Algorithms/CropAlgorithm.h"

#include <future>
#include <optional>

// Router 不保存输入或交互状态，只在 owner thread 构造一次性导出任务。
class CropRouter final {
public:
    std::optional<std::packaged_task<CropMaterializationCandidate()>> BuildResultTask(
        CropInputSnapshot input,
        CropBuildParams params,
        CropShaderPayload payload) const;
};
