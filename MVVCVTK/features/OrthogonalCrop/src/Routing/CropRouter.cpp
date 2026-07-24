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

std::optional<std::packaged_task<CropExportResult()>> CropRouter::BuildExportTask(
    CropInputSnapshot input,
    CropExportRequest request,
    CropShaderPayload payload) const
{
    if (!CropAlgorithm::GetInputValid(input)
        || request.dataSource != input.dataSource
        || request.inputVersion != input.inputVersion
        || request.operations.size() != request.nodeCount
        || request.nodeCount == 0
        || payload.revision == 0
        || payload.sourceStamp.version != input.inputVersion
        || payload.nodeCount != request.nodeCount
        || !payload.predicateTable
        || payload.predicateTable->operationCount < payload.nodeCount) {
        return std::nullopt;
    }
    if (request.availableRamBytes == 0) {
        request.availableRamBytes = GetRamBytes();
    }

    return std::packaged_task<CropExportResult()>(
        [input = std::move(input), request = std::move(request),
            payload = std::move(payload)]() mutable {
            if (request.dataSource == OrthogonalCropDataSource::ImageData) {
                return CropAlgorithm::GetResult(
                    input.imageData,
                    input.validityMask,
                    request,
                    payload);
            }
            return CropAlgorithm::GetResult(input.polyData, request, payload);
        });
}
