#include "Routing/CropRouter.h"

#include "Algorithms/CropAlgorithm.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef GetMessage
#undef GetMessage
#endif
#endif

#include <cstddef>
#include <utility>

namespace {
std::size_t GetRamBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX memoryStatus = {};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus) != 0) {
        return static_cast<std::size_t>(memoryStatus.ullAvailPhys);
    }
#endif
    return 0;
}
}

std::optional<std::packaged_task<CropBuildResult()>> CropRouter::BuildResultTask(
    CropInputSnapshot input,
    CropBuildParams params,
    CropShaderPayload payload) const
{
    if (!CropAlgorithm::GetInputValid(input)
        || params.dataSource != input.dataSource
        || params.inputVersion != input.inputVersion
        || params.operations.size() != params.nodeCount
        || params.nodeCount == 0
        || payload.revision == 0
        || payload.sourceStamp.version != input.inputVersion
        || payload.nodeCount != params.nodeCount
        || !payload.predicateTable
        || payload.predicateTable->operationCount < payload.nodeCount) {
        return std::nullopt;
    }
    if (params.availableRamBytes == 0) {
        params.availableRamBytes = GetRamBytes();
    }

    return std::packaged_task<CropBuildResult()>(
        [input = std::move(input), params = std::move(params),
            payload = std::move(payload)]() mutable {
            if (params.dataSource == OrthogonalCropDataSource::ImageData) {
                return CropAlgorithm::GetResult(
                    input.imageData,
                    input.validityMask,
                    params,
                    payload);
            }
            return CropAlgorithm::GetResult(input.polyData, params, payload);
        });
}
