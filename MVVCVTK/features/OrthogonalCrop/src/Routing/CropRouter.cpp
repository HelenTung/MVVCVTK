#include "Routing/CropRouter.h"

#include "Algorithms/CropAlgorithm.h"
#include "Interaction/CropHistory.h"
#include <new>
#include <stdexcept>

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

bool CropRouter::GetRecipeSame(const RoiGeometryPayload& recipe,const std::vector<CropOpItem>& operations) {
    const auto expected=CreateRecipePayload(operations);if(!expected||!recipe.GetValid())return false;
    const auto& first=recipe.GetPrimitives();const auto& second=expected->GetPrimitives();if(first.size()!=second.size())return false;
    for(std::size_t index=0;index<first.size();++index) {
        const auto& a=first[index];const auto& b=second[index];
        if(a.shape!=b.shape||a.operation!=b.operation||a.localToSource!=b.localToSource||a.origin!=b.origin||a.normal!=b.normal
            ||a.points!=b.points||a.mask!=b.mask||a.center!=b.center||a.axis!=b.axis||a.radius!=b.radius||a.height!=b.height
            ||a.recipeVersion!=b.recipeVersion||a.boundaryPolicyVersion!=b.boundaryPolicyVersion)return false;
    }
    return true;
}
namespace {
struct RestoreCancelled final {};
template<class T> std::vector<T> CopyRestoreArray(const std::vector<T>& source,const std::function<bool()>& stop) {
    std::vector<T> result;result.reserve(source.size());
    constexpr std::size_t chunk=1024*1024/sizeof(T);
    for(std::size_t offset=0;offset<source.size();) {
        if(stop&&stop())throw RestoreCancelled{};
        const auto end=offset+std::min(chunk,source.size()-offset);
        result.insert(result.end(),source.begin()+offset,source.begin()+end);offset=end;
    }
    return result;
}
bool AddRestoreBytes(std::size_t count,std::size_t size,std::size_t& bytes) {
    if(count>(std::numeric_limits<std::size_t>::max()-bytes)/size)return false;
    bytes+=count*size;return true;
}
}
std::packaged_task<CropMaterializationCandidate()> CropRouter::BuildRestoreTask(
    CropInputSnapshot input,CropBuildParams params,DataSnapshot output,
    std::shared_ptr<const DataResourceLease> reader,CropResultRecord record,std::function<bool()> stop) const {
    return std::packaged_task<CropMaterializationCandidate()>(
        [input=std::move(input),params=std::move(params),output=std::move(output),reader=std::move(reader),record,stop=std::move(stop)]() mutable {
        // Move scoped reads out of the packaged callable so its retained future
        // cannot keep the original result alive after the worker finishes.
        const auto original=std::move(output);const auto use=std::move(reader);
        CropMaterializationCandidate result;result.documentId=params.documentId;result.nodeId=params.nodeId;result.requestId=params.requestId;
        result.sourceRevision=params.sourceRevision;result.operations=params.operations;result.nodeCount=params.operations.size();
        try {
            if(stop&&stop())throw RestoreCancelled{};
            if(!original||!input.data||result.operations.empty()||!params.availableRamBytes){result.failureReason=CropFailure::BadInput;return result;}
            constexpr std::size_t margin=16*1024*1024;
            if(const auto image=std::dynamic_pointer_cast<const ImageGrid3DPayload>(original->payload)) {
                const auto source=std::dynamic_pointer_cast<const ImageGrid3DPayload>(input.data->payload);
                if(!source||!source->GetValid()||!image->GetValid()||!image->GetValidityMask()
                    ||!CropHistory::GetGeometrySame(source->GetGeometry(),image->GetGeometry())
                    ||source->GetValues()!=image->GetValues()||source->GetValueType()!=image->GetValueType()
                    ||source->GetComponentCount()!=image->GetComponentCount()){result.failureReason=CropFailure::BadInput;return result;}
                const auto count=image->GetValidityMask()->size();
                if(params.availableRamBytes<margin||count>(params.availableRamBytes-margin)/2){result.failureReason=CropFailure::ResourceLimit;return result;}
                result.outputPayload=source->CreateMaskSnapshot(CopyRestoreArray(*image->GetValidityMask(),stop));
            } else if(const auto mesh=std::dynamic_pointer_cast<const SurfaceMeshPayload>(original->payload)) {
                const auto source=std::dynamic_pointer_cast<const SurfaceMeshPayload>(input.data->payload);
                if(!source||!source->GetValid()||!mesh->GetValid()||source->GetCoordinateFrame()!=mesh->GetCoordinateFrame()) {
                    result.failureReason=CropFailure::BadInput;return result;
                }
                std::size_t bytes=0;bool valid=AddRestoreBytes(mesh->GetVertices().size(),sizeof(double),bytes)
                    &&AddRestoreBytes(mesh->GetTriangles().size(),sizeof(std::uint64_t),bytes);
                for(const auto* attributes:{&mesh->GetPointAttributes(),&mesh->GetCellAttributes()})for(const auto& attribute:*attributes)
                    valid=valid&&AddRestoreBytes(attribute.values.size(),sizeof(double),bytes)&&AddRestoreBytes(attribute.name.size()+1,1,bytes);
                if(!valid||params.availableRamBytes<margin||bytes>(params.availableRamBytes-margin)/5){result.failureReason=CropFailure::ResourceLimit;return result;}
                auto vertices=CopyRestoreArray(mesh->GetVertices(),stop);auto triangles=CopyRestoreArray(mesh->GetTriangles(),stop);
                const auto copyAttributes=[&](const std::vector<MeshAttribute>& input) {
                    std::vector<MeshAttribute> attributes;attributes.reserve(input.size());
                    for(const auto& item:input)attributes.push_back({item.name,item.componentCount,CopyRestoreArray(item.values,stop),item.activeRoles});
                    return attributes;
                };
                auto points=copyAttributes(mesh->GetPointAttributes());auto cells=copyAttributes(mesh->GetCellAttributes());
                result.outputPayload=std::make_shared<const SurfaceMeshPayload>(std::move(vertices),std::move(triangles),std::move(points),mesh->GetCoordinateFrame(),std::move(cells));
            } else {result.failureReason=CropFailure::BadInput;return result;}
            result.recipePayload=CreateRecipePayload(result.operations);
            if(stop&&stop())throw RestoreCancelled{};
            result.preparedView=VtkPreparedDataView::BuildDataView(result.outputPayload,input.image);
            if(!result.recipePayload||!result.preparedView){result.failureReason=CropFailure::BadInput;return result;}
            if(stop&&stop())throw RestoreCancelled{};
            result.meshErrorBound=record.meshErrorBound;result.meshAreaErrorBound=record.meshAreaErrorBound;result.meshTriangleCount=record.meshTriangleCount;
            result.buildParameters=original->provenance?original->provenance->canonicalParameters:std::string{};
            if(result.preparedView->image){result.imageData=result.preparedView->image->image;result.maskImage=result.preparedView->image->validityMask;}
            else result.polyData=result.preparedView->mesh->mesh;
            result.isSucceeded=true;return result;
        } catch(const RestoreCancelled&) {result.isCancelled=true;result.failureReason=CropFailure::Cancelled;}
        catch(const std::bad_alloc&) {result.failureReason=CropFailure::LowRam;}
        catch(...) {result.failureReason=CropFailure::WorkerFailed;}
        result.outputPayload.reset();result.recipePayload.reset();result.preparedView.reset();result.imageData=nullptr;result.maskImage=nullptr;result.polyData=nullptr;
        return result;
    });
}
