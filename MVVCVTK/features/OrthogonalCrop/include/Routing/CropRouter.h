#pragma once

#include "OrthogonalCropTypes.h"

#include <future>
#include <optional>

// Router 不保存输入或交互状态，只在 owner thread 构造一次性导出任务。
class CropRouter final {
public:
    std::optional<std::packaged_task<CropExportResult()>> BuildExportTask(
        CropInputSnapshot input,
        CropExportRequest request,
        CropShaderPayload payload) const;
};
