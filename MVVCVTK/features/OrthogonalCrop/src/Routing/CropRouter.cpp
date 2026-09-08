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

std::optional<std::packaged_task<CropMaterializationCandidate()>>
CropRouter::BuildResultTask(
    CropInputSnapshot input,
    CropBuildParams params,
    CropShaderPayload payload,
    std::function<bool()> getStopRequested) const
{
    if (!CropAlgorithm::GetInputValid(input)
        || !input.data
        || params.sourceRevision != input.data->self
        || params.operations.size() != params.nodeCount
        || params.nodeCount == 0
        || payload.revision == 0
        || payload.sourceStamp.dataRevision != input.data->self
        || payload.nodeCount != params.nodeCount
        || !payload.predicateTable
        || payload.predicateTable->operationCount < payload.nodeCount) {
        return std::nullopt;
    }
    if (params.availableRamBytes == 0) {
        params.availableRamBytes = GetRamBytes();
    }

    return std::packaged_task<CropMaterializationCandidate()>(
        [input = std::move(input), params = std::move(params),
            payload = std::move(payload),
            getStopRequested = std::move(getStopRequested)]() mutable {
            if (input.image) {
                return CropAlgorithm::GetResult(
                    input.image->image,
                    input.image->validityMask,
                    params,
                    payload, 0, getStopRequested);
            }
            return CropAlgorithm::GetResult(
                input.mesh ? input.mesh->mesh.GetPointer() : nullptr,
                params,
                payload, getStopRequested);
        });
}

std::optional<std::packaged_task<CropMaterializationCandidate()>> CropRouter::BuildRoiTask(
    CropInputSnapshot input, RoiReadSnapshot roi, std::function<bool()> getStopRequested) const
{
    if (!CropAlgorithm::GetInputValid(input) || !input.data || !roi || roi->GetSource()!=input.data->self) return {};
    if (input.mesh && roi->GetClipPlanes().error!=RoiError::None) return {};
    const auto budget=GetRamBytes();
    return std::packaged_task<CropMaterializationCandidate()>(
        [input=std::move(input),roi=std::move(roi),budget,getStopRequested=std::move(getStopRequested)] {
            return CropAlgorithm::GetRoiResult(input,roi,budget,getStopRequested);
        });
}
