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
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace {
std::shared_ptr<const RoiGeometryPayload> CreateRecipePayload(
    const std::vector<CropOpItem>& operations)
{
    std::vector<RoiPrimitive> primitives;
    primitives.reserve(operations.size());
    for (const auto& operation : operations) {
        RoiPrimitive primitive;
        primitive.operation = operation.removalMode
            == CropRemovalMode::KeepInside
            ? "keep-inside" : "remove-inside";
        if (operation.geometryType == CropShape::Box) {
            primitive.shape = RoiShape::Box;
            primitive.localToSource = operation.boxToInputModelMatrix;
        }
        else if (operation.geometryType == CropShape::Plane) {
            primitive.shape = RoiShape::Plane;
            primitive.origin = operation.planeCenterInInputModel;
            primitive.normal = operation.planeNormalInInputModel;
        }
        else if (operation.geometryType == CropShape::Sphere || operation.geometryType == CropShape::Cylinder) {
            primitive.shape = operation.geometryType == CropShape::Sphere ? RoiShape::Sphere : RoiShape::Cylinder;
            primitive.center = operation.centerInInputModel;
            primitive.axis = operation.axisInInputModel;
            primitive.radius = operation.radius; primitive.height = operation.height;
        } else return {};
        primitive.recipeVersion = operation.recipeVersion;
        primitive.boundaryPolicyVersion = operation.boundaryPolicyVersion;
        primitives.push_back(std::move(primitive));
    }
    auto payload = std::make_shared<const RoiGeometryPayload>(
        std::move(primitives));
    return payload->GetValid() ? payload : nullptr;
}

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
            CropMaterializationCandidate result;
            if (input.image) {
                const auto* source = dynamic_cast<const ImageGrid3DPayload*>(input.data->payload.get());
                if (!source) return CropMaterializationCandidate{};
                result = CropAlgorithm::GetResult(input.image->image, input.image->validityMask,
                    params, payload, 0, getStopRequested, source);
            } else {
                result = CropAlgorithm::GetResult(input.mesh ? input.mesh->mesh.GetPointer() : nullptr,
                    params, payload, getStopRequested,std::dynamic_pointer_cast<const SurfaceMeshPayload>(input.data->payload));
                if (result.isSucceeded) {
                    const auto* source=dynamic_cast<const SurfaceMeshPayload*>(input.data->payload.get());
                    result.preparedView = source?VtkPreparedDataView::BuildDataView(result.polyData,source->GetCoordinateFrame()):nullptr;
                    if (result.preparedView) result.outputPayload = result.preparedView->payload;
                }
            }
            result.documentId=params.documentId;result.nodeId=params.nodeId;result.requestId=params.requestId;
            if (!result.isSucceeded) return result;
            std::ostringstream parameters;parameters.imbue(std::locale::classic());
            parameters<<std::setprecision(std::numeric_limits<double>::max_digits10)
                <<"{\"schemaVersion\":1,\"geometryVersion\":1,\"boundaryPolicyVersion\":1,\"meshBackendVersion\":1,\"meshTolerance\":"<<params.meshTolerance
                <<",\"maxCells\":"<<params.maxCells<<",\"maxDepth\":"<<params.maxDepth
                <<",\"availableRamBytes\":"<<params.availableRamBytes
                <<",\"meshErrorBound\":"<<result.meshErrorBound<<",\"meshAreaErrorBound\":"<<result.meshAreaErrorBound<<"}";
            result.buildParameters=parameters.str();
            result.recipePayload = CreateRecipePayload(result.operations);
            if (!result.preparedView) result.preparedView = VtkPreparedDataView::BuildDataView(result.outputPayload, input.image);
            if (!result.recipePayload || !result.preparedView) {
                result.isSucceeded = false;
                result.failureReason = CropFailure::BadInput;
                result.message = "Crop worker could not prepare the formal payload and trusted view.";
                return result;
            }
            if (result.preparedView->image) {
                result.imageData = result.preparedView->image->image;
                result.maskImage = result.preparedView->image->validityMask;
            } else result.polyData = result.preparedView->mesh->mesh;
            if (getStopRequested && getStopRequested()) {
                result.isSucceeded = false;
                result.isCancelled = true;
                result.failureReason = CropFailure::Cancelled;
                result.message = "Crop build was cancelled during worker preparation.";
                result.outputPayload.reset();result.recipePayload.reset();result.preparedView.reset();
                result.imageData=nullptr;result.maskImage=nullptr;result.polyData=nullptr;
            }
            return result;
        });
}
