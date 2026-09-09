#pragma once

#include "Algorithms/CropAlgorithm.h"

#include <future>
#include <optional>

// Router 不保存输入或交互状态，只在 owner thread 构造一次性导出任务。
class CropRouter final {
public:
    static std::shared_ptr<const RoiGeometryPayload> CreateRecipePayload(const std::vector<CropOpItem>& operations,const DataRevisionRef& source);
    static bool GetRecipeSame(const RoiGeometryPayload& recipe,const std::vector<CropOpItem>& operations);
    std::packaged_task<CropMaterializationCandidate()> BuildRestoreTask(
        CropInputSnapshot input,CropBuildParams params,DataSnapshot output,
        std::shared_ptr<const DataResourceLease> reader,CropResultRecord record,std::shared_ptr<const RoiGeometryPayload> recipe,
        std::function<bool()> getStopRequested) const;
    std::optional<std::packaged_task<CropMaterializationCandidate()>> BuildRoiTask(
        CropInputSnapshot input, CropBuildParams params, RoiReadSnapshot roi, std::function<bool()> getStopRequested) const;
    std::optional<std::packaged_task<CropMaterializationCandidate()>> BuildResultTask(
        CropInputSnapshot input,
        CropBuildParams params,
        CropShaderPayload payload,
        std::function<bool()> getStopRequested = {}) const;
};
