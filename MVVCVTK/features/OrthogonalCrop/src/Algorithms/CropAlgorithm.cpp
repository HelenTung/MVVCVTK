#include "Algorithms/CropAlgorithm.h"
#include "Algorithms/CropMeshAlgorithm.h"

#include "Data/DataPayloads.h"

#include <vtkClipPolyData.h>
#include <vtkCleanPolyData.h>
#include <vtkCellArray.h>
#include <vtkIdList.h>
#include <vtkPoints.h>
#include <vtkTriangleFilter.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkSMPTools.h>
#include <vtkType.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
constexpr std::size_t kTexelSize = 4;
constexpr float kBoxTolerance = 1.0e-6f;
constexpr double kMatrixTolerance = 1.0e-12;
constexpr std::size_t kRamMargin = 16ULL * 1024ULL * 1024ULL;

bool GetFinite(const double value)
{
    return vtkMath::IsFinite(value);
}

bool GetBoundsValid(const CropBoundsDouble6Array& bounds)
{
    return std::all_of(bounds.begin(), bounds.end(), GetFinite)
        && bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

bool GetMaskValid(
    vtkImageData* image,
    vtkImageData* validityMask)
{
    if (!validityMask) {
        return true;
    }
    if (!image
        || validityMask->GetScalarType()
            != VTK_UNSIGNED_CHAR
        || validityMask->GetNumberOfScalarComponents() != 1
        || !validityMask->GetScalarPointer()) {
        return false;
    }

    int imageExtent[6] = {};
    int maskExtent[6] = {};
    image->GetExtent(imageExtent);
    validityMask->GetExtent(maskExtent);
    for (int index = 0; index < 6; ++index) {
        if (imageExtent[index] != maskExtent[index]) {
            return false;
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (image->GetOrigin()[axis]
                != validityMask->GetOrigin()[axis]
            || image->GetSpacing()[axis]
                != validityMask->GetSpacing()[axis]) {
            return false;
        }
    }
    const auto* imageDirection =
        image->GetDirectionMatrix();
    const auto* maskDirection =
        validityMask->GetDirectionMatrix();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (imageDirection->GetElement(row, column)
                != maskDirection->GetElement(row, column)) {
                return false;
            }
        }
    }
    return true;
}

CropTableResult BuildTableFailure(
    const CropOpItem* operation,
    const char* message)
{
    CropTableResult result;
    result.failureReason = CropFailure::BadInput;
    result.failureOperationIndex = operation ? operation->operationIndex : 0;
    result.message = message;
    return result;
}

bool BuildGeometryRows(const CropGeometry& geometry, float* values)
{
    const auto& operation=geometry.GetOperation();
    const auto set=[&](int index,double value) {
        if (!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())return false;
        values[index]=static_cast<float>(value);
        return value==0 || values[index]!=0;
    };
    if(operation.geometryType==CropShape::Box) {
        const auto& inverse=geometry.GetBoxInverse();
        for(int i=0;i<16;++i)if(!set(i,inverse[i]))return false;
    } else if(operation.geometryType==CropShape::Plane) {
        for(int i=0;i<3;++i)
            if(!set(i,operation.planeCenterInInputModel[i])||!set(4+i,operation.planeNormalInInputModel[i]))return false;
    } else {
        for(int i=0;i<3;++i)if(!set(i,operation.centerInInputModel[i]))return false;
        if(!set(3,operation.radius)||!std::isfinite(values[3]*values[3])||values[3]*values[3]<=0)return false;
        if(operation.geometryType==CropShape::Cylinder) {
            for(int i=0;i<3;++i)if(!set(4+i,operation.axisInInputModel[i]))return false;
            if(!set(7,operation.height*0.5))return false;
        }
    }
    return true;
}

CropMaterializationCandidate BuildResultFailure(
    const CropBuildParams& params,
    const CropFailure failureReason,
    const std::string& message,
    const std::uint64_t failureOperationIndex = 0)
{
    CropMaterializationCandidate result;
    result.failureReason = failureReason;
    result.failureOperationIndex = failureOperationIndex;
    result.sourceRevision = params.sourceRevision;
    result.nodeCount = std::min(params.nodeCount, params.operations.size());
    result.operations.assign(
        params.operations.begin(),
        params.operations.begin() + result.nodeCount);
    result.message = message;
    return result;
}

CropMaterializationCandidate BuildResultBase(const CropBuildParams& params)
{
    CropMaterializationCandidate result;
    result.sourceRevision = params.sourceRevision;
    result.nodeCount = params.nodeCount;
    result.operations.reserve(params.operations.size());
    for (const auto& operation:params.operations) result.operations.push_back(CropGeometry::Build(operation)->GetOperation());
    return result;
}

// predicate table 已经是 history 的不可变编译产物；Plan 只在一次物化开始时
// 验证其结构，体素热循环直接执行，避免每点重复检查表长、tag 和 nodeCount。
class CropPredicatePlan final {
public:
    CropPredicatePlan(
        const CropPredicateTable& predicateTable,
        const std::size_t nodeCount)
        : m_values(predicateTable.rgbaValues.data())
        , m_nodeCount(nodeCount)
    {
        m_isValid=predicateTable.schemaVersion==1 && predicateTable.geometry.size()==predicateTable.operationCount
            && predicateTable.operationCount<=std::numeric_limits<std::size_t>::max()/kItemSize
            && predicateTable.rgbaValues.size()==predicateTable.operationCount*kItemSize
            && nodeCount<=predicateTable.operationCount;
        for(std::size_t index=0;m_isValid&&index<nodeCount;++index) {
            const auto& geometry=predicateTable.geometry[index];
            std::array<float,kItemSize> expected{};
            expected[0]=static_cast<float>(geometry.GetOperation().geometryType);
            expected[1]=geometry.GetOperation().removalMode==CropRemovalMode::KeepInside?0.0f:1.0f;
            m_isValid=BuildGeometryRows(geometry,expected.data()+4)
                &&std::equal(expected.begin(),expected.end(),predicateTable.rgbaValues.begin()+index*kItemSize);
        }
    }

    bool GetValid() const
    {
        return m_isValid;
    }

    bool GetPointKept(
        const CropPointFloat3Array& inputModelPoint) const
    {
        if (!m_isValid
            || !std::all_of(
                inputModelPoint.begin(),
                inputModelPoint.end(),
                [](const float value) {
                    return vtkMath::IsFinite(
                        static_cast<double>(value));
                })) {
            return false;
        }
        return GetPointKeptUnchecked(
            inputModelPoint);
    }

    bool GetPointKeptUnchecked(
        const CropPointFloat3Array& inputModelPoint) const
    {
        for (std::size_t index = 0;
            index < m_nodeCount;
            ++index) {
            const auto* values =
                m_values + index * kItemSize;
            bool isInside = false;
            if (values[0] == 0.0f) {
                std::array<float, 3> boxPoint = {};
                for (int row = 0; row < 3; ++row) {
                    const auto* matrixRow =
                        values + kTexelSize + row * 4;
                    boxPoint[row] =
                        matrixRow[0]
                            * inputModelPoint[0]
                        + matrixRow[1]
                            * inputModelPoint[1]
                        + matrixRow[2]
                            * inputModelPoint[2]
                        + matrixRow[3];
                }
                isInside =
                    std::abs(boxPoint[0])
                        <= 1.0f + kBoxTolerance
                    && std::abs(boxPoint[1])
                        <= 1.0f + kBoxTolerance
                    && std::abs(boxPoint[2])
                        <= 1.0f + kBoxTolerance;
            }
            else if(values[0]==1.0f) {
                const auto* center =
                    values + kTexelSize;
                const auto* normal =
                    values + kTexelSize * 2;
                const float signedDistance =
                    (inputModelPoint[0] - center[0])
                        * normal[0]
                    + (inputModelPoint[1] - center[1])
                        * normal[1]
                    + (inputModelPoint[2] - center[2])
                        * normal[2];
                isInside = signedDistance > 0.0f;
            } else {
                std::array<float,3> d{inputModelPoint[0]-values[4],inputModelPoint[1]-values[5],inputModelPoint[2]-values[6]};
                bool cap=true;
                if(values[0]==2.0f) {
                    const float t=d[0]*values[8]+d[1]*values[9]+d[2]*values[10];
                    cap=std::abs(t)<=values[11];
                    for(int axis=0;axis<3;++axis)d[axis]-=t*values[8+axis];
                }
                isInside=cap && d[0]*d[0]+d[1]*d[1]+d[2]*d[2]<=values[7]*values[7];
            }

            const bool isKept =
                values[1] == 0.0f
                ? isInside : !isInside;
            if (!isKept) {
                return false;
            }
        }
        return true;
    }

private:
    static constexpr std::size_t kItemSize =
        CropAlgorithm::GetTexelCount()
        * kTexelSize;

    const float* m_values = nullptr;
    std::size_t m_nodeCount = 0;
    bool m_isValid = false;
};

bool GetPayloadValid(
    const CropBuildParams& params,
    const CropShaderPayload& payload)
{
    const bool hasPlan = payload.predicateTable
        && CropPredicatePlan(
            *payload.predicateTable,
            payload.nodeCount).GetValid();
    if (!hasPlan || params.operations.size()!=payload.nodeCount) return false;
    for(std::size_t index=0;index<params.operations.size();++index) {
        const auto canonical=CropGeometry::Build(params.operations[index]);
        if(!canonical||!CropGeometry::GetOperationsSame(canonical->GetOperation(),payload.predicateTable->geometry[index].GetOperation()))return false;
        std::array<float,20> expected{};
        expected[0]=static_cast<float>(canonical->GetOperation().geometryType);
        expected[1]=canonical->GetOperation().removalMode==CropRemovalMode::KeepInside?0.0f:1.0f;
        if(!BuildGeometryRows(*canonical,expected.data()+4)
            ||!std::equal(expected.begin(),expected.end(),payload.predicateTable->rgbaValues.begin()+index*20))return false;
    }
    return GetDataRevisionRefValid(params.sourceRevision)
        && params.operations.size() == params.nodeCount
        && params.nodeCount != 0
        && payload.revision != 0
        && payload.sourceStamp.dataRevision == params.sourceRevision
        && payload.nodeCount == params.nodeCount
        && hasPlan;
}

bool GetRamValid(
    vtkImageData* image,
    const CropBuildParams& params,
    const CropShaderPayload& payload,
    const std::size_t fallbackAvailableRamBytes,
    const ImageGrid3DPayload* sourcePayload)
{
    const std::size_t availableRamBytes = params.availableRamBytes != 0
        ? params.availableRamBytes
        : fallbackAvailableRamBytes;
    if (availableRamBytes == 0) {
        return true;
    }

    const vtkIdType pointCount = sourcePayload
        ? static_cast<vtkIdType>(GetGridVoxelCount(sourcePayload->GetGeometry()).value_or(0))
        : image?image->GetNumberOfPoints():0;
    if (pointCount < 0) {
        return false;
    }
    // One formal result mask plus its isolated trusted VTK bridge copy. Root scalars are shared.
    if (static_cast<std::uint64_t>(pointCount) > std::numeric_limits<std::size_t>::max() / 2) return false;
    const std::size_t maskBytes = static_cast<std::size_t>(pointCount) * 2;
    int dimensions[3] = {};
    if(sourcePayload)std::copy(sourcePayload->GetGeometry().dimensions.begin(),sourcePayload->GetGeometry().dimensions.end(),dimensions);
    else image->GetDimensions(dimensions);
    const std::size_t sliceBytes = static_cast<std::size_t>(std::max(0, dimensions[2])) * sizeof(std::size_t);
    const std::size_t tableBytes = payload.predicateTable
        ? payload.predicateTable->rgbaValues.size() * sizeof(float)
            + payload.predicateTable->geometry.size()*sizeof(CropGeometry)
        : 0;
    if (maskBytes
            > std::numeric_limits<std::size_t>::max()
                - tableBytes
        || maskBytes + tableBytes
            > std::numeric_limits<std::size_t>::max() - kRamMargin) {
        return false;
    }
    const auto fixedBytes = maskBytes + tableBytes + kRamMargin;
    return sliceBytes <= std::numeric_limits<std::size_t>::max() - fixedBytes
        && fixedBytes + sliceBytes <= availableRamBytes;
}


}

CropMatrixDouble16Array CropAlgorithm::GetIdentityMatrix()
{
    return {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
}

CropMatrixDouble16Array CropAlgorithm::GetBoxMatrix(
    const CropBoundsDouble6Array& inputModelBounds)
{
    const double centerX = (inputModelBounds[0] + inputModelBounds[1]) * 0.5;
    const double centerY = (inputModelBounds[2] + inputModelBounds[3]) * 0.5;
    const double centerZ = (inputModelBounds[4] + inputModelBounds[5]) * 0.5;
    const double halfX = (inputModelBounds[1] - inputModelBounds[0]) * 0.5;
    const double halfY = (inputModelBounds[3] - inputModelBounds[2]) * 0.5;
    const double halfZ = (inputModelBounds[5] - inputModelBounds[4]) * 0.5;
    return {
        halfX, 0.0, 0.0, centerX,
        0.0, halfY, 0.0, centerY,
        0.0, 0.0, halfZ, centerZ,
        0.0, 0.0, 0.0, 1.0
    };
}

CropTableResult CropAlgorithm::BuildPredicateTable(
    const std::vector<CropOpItem>& operations,
    const std::size_t nodeCount)
{
    if (nodeCount > operations.size()) {
        return BuildTableFailure(nullptr, "Crop nodeCount exceeds operation count.");
    }
    constexpr std::size_t itemSize =
        GetTexelCount() * kTexelSize;
    if (operations.size()
        > std::numeric_limits<std::size_t>::max()
            / itemSize) {
        return BuildTableFailure(
            nullptr,
            "Crop predicate table size overflows.");
    }

    auto predicateTable = std::make_shared<CropPredicateTable>();
    predicateTable->operationCount = operations.size();
    predicateTable->geometry.reserve(operations.size());
    predicateTable->rgbaValues.assign(
        operations.size() * itemSize,
        0.0f);

    std::unordered_set<std::uint64_t> operationIndices;
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (operation.operationIndex == 0
            || !operationIndices.insert(operation.operationIndex).second) {
            return BuildTableFailure(
                &operation,
                "Crop operation index must be non-zero and unique.");
        }

        auto* itemValues = predicateTable->rgbaValues.data()
            + index * GetTexelCount() * kTexelSize;
        const auto geometry=CropGeometry::Build(operation);
        if(!geometry)return BuildTableFailure(&operation,"Crop geometry or recipe version is invalid.");
        predicateTable->geometry.push_back(*geometry);
        itemValues[0]=static_cast<float>(operation.geometryType);
        if(!BuildGeometryRows(*geometry,itemValues+kTexelSize)) {
            auto failure=BuildTableFailure(&operation,"Crop geometry cannot be represented by the preview table.");
            failure.failureReason=CropFailure::PrecisionNotMet;return failure;
        }

        switch (operation.removalMode) {
        case CropRemovalMode::KeepInside:
            itemValues[1] = 0.0f;
            break;
        case CropRemovalMode::RemoveInside:
            itemValues[1] = 1.0f;
            break;
        default:
            return BuildTableFailure(
                &operation,
                "Crop removal mode is unsupported.");
        }
    }

    CropTableResult result;
    result.isSucceeded = true;
    result.predicateTable = std::move(predicateTable);
    return result;
}

bool CropAlgorithm::GetTableValid(const CropPredicateTable& table,std::size_t nodeCount)
{
    return CropPredicatePlan(table,nodeCount).GetValid();
}

bool CropAlgorithm::GetPointKept(
    const CropPredicateTable& predicateTable,
    const std::size_t nodeCount,
    const CropPointFloat3Array& inputModelPoint)
{
    return CropPredicatePlan(
        predicateTable,
        nodeCount).GetPointKept(
            inputModelPoint);
}

bool CropAlgorithm::GetInputValid(const CropInputSnapshot& input)
{
    if (!input.graph.view || !input.data
        || !GetDataRevisionRefValid(input.data->self)
        || !std::all_of(input.inputModelBounds.begin(),input.inputModelBounds.end(),GetFinite)
        || input.inputModelBounds[0]>input.inputModelBounds[1]
        || input.inputModelBounds[2]>input.inputModelBounds[3]
        || input.inputModelBounds[4]>input.inputModelBounds[5]
        || (input.binding
            && (!input.binding->target
                || *input.binding->target != input.data->self))) {
        return false;
    }
    const auto facets = input.graph.view->GetDataFacets(input.data->type);
    const auto hasFacet = [&facets](const DataFacetId& facet) {
        return std::find(facets.begin(), facets.end(), facet)
            != facets.end();
    };
    const bool isImage = hasFacet(DataFacets::scalarGrid3D);
    const bool isMesh = hasFacet(DataFacets::surfaceMesh);
    if (isImage == isMesh) return false;
    if (isImage) {
        return input.image && input.image->data
            && input.image->data->self == input.data->self
            && !input.mesh
            && GetMaskValid(
                input.image->image, input.image->validityMask);
    }
    return input.mesh && input.mesh->data
        && input.mesh->data->self == input.data->self
        && input.mesh->mesh && !input.image;
}

bool CropAlgorithm::GetInputSame(
    const CropInputSnapshot& left,
    const CropInputSnapshot& right)
{
    return left.data && right.data
        && left.data->self == right.data->self
        && left.inputModelBounds == right.inputModelBounds
        && static_cast<bool>(left.image) == static_cast<bool>(right.image)
        && static_cast<bool>(left.mesh) == static_cast<bool>(right.mesh);
}

CropMaterializationCandidate CropAlgorithm::GetResult(
    vtkImageData* image,
    vtkImageData* validityMask,
    const CropBuildParams& params,
    const CropShaderPayload& payload,
    const std::size_t fallbackAvailableRamBytes,
    const std::function<bool()>& getStopRequested,
    const ImageGrid3DPayload* sourcePayload)
{
    std::atomic<bool> isCancelled{ false };
    const auto getStopped = [&]() noexcept {
        if (isCancelled.load(std::memory_order_relaxed)) return true;
        try {
            if (!getStopRequested || !getStopRequested()) return false;
        }
        catch (...) {
            // SMP 工作体不传播未知回调异常；失败作为取消并丢弃完整候选。
        }
        isCancelled.store(true, std::memory_order_relaxed);
        return true;
    };
    const auto getCancelled = [&] {
        auto result = BuildResultFailure(params, CropFailure::Cancelled,
            "The crop build was cancelled before publication.");
        result.isCancelled = true;
        return result;
    };
    if (getStopped()) return getCancelled();
    if (!image || !image->GetPointData() || !image->GetPointData()->GetScalars()) {
        return BuildResultFailure(
            params,
            CropFailure::NoImage,
            "Crop result build requires image scalars.");
    }
    if ((sourcePayload?!sourcePayload->GetValid():!GetMaskValid(image,validityMask))
        || !GetPayloadValid(params, payload)) {
        return BuildResultFailure(
            params,
            CropFailure::BadInput,
            "Crop image build parameters are invalid.");
    }

    int extent[6] = {};
    if(sourcePayload)std::copy(sourcePayload->GetGeometry().extent.begin(),sourcePayload->GetGeometry().extent.end(),extent);
    else image->GetExtent(extent);
    const vtkIdType xCount =
        static_cast<vtkIdType>(extent[1])
        - static_cast<vtkIdType>(extent[0]) + 1;
    const vtkIdType yCount =
        static_cast<vtkIdType>(extent[3])
        - static_cast<vtkIdType>(extent[2]) + 1;
    const vtkIdType zCount =
        static_cast<vtkIdType>(extent[5])
        - static_cast<vtkIdType>(extent[4]) + 1;
    if (xCount <= 0
        || yCount <= 0
        || zCount <= 0
        || (!sourcePayload&&image->GetNumberOfPoints()<=0)) {
        return BuildResultFailure(
            params,
            CropFailure::EmptyResult,
            "Crop image build has an empty extent.");
    }

    std::array<double, 12> indexToModel = {};
    const auto* indexMatrix =
        image->GetIndexToPhysicalMatrix();
    if (!sourcePayload&&!indexMatrix) {
        return BuildResultFailure(
            params,
            CropFailure::BadInput,
            "Crop image index transform is unavailable.");
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0;
            column < 4;
            ++column) {
            const double value=sourcePayload
                ? column==3?sourcePayload->GetGeometry().origin[row]
                    :sourcePayload->GetGeometry().direction[row*3+column]*sourcePayload->GetGeometry().spacing[column]
                :indexMatrix->GetElement(row,column);
            if (!vtkMath::IsFinite(value)) {
                return BuildResultFailure(
                    params,
                    CropFailure::BadInput,
                    "Crop image index transform must be finite.");
            }
            indexToModel[
                static_cast<std::size_t>(
                    row * 4 + column)] =
                value;
        }
    }
    const double linear[9]={indexToModel[0],indexToModel[1],indexToModel[2],
        indexToModel[4],indexToModel[5],indexToModel[6],indexToModel[8],indexToModel[9],indexToModel[10]};
    const double determinant=vtkMatrix3x3::Determinant(linear);
    if(!std::isfinite(determinant)||determinant==0 || (!sourcePayload&&(indexMatrix->GetElement(3,0)!=0
        ||indexMatrix->GetElement(3,1)!=0||indexMatrix->GetElement(3,2)!=0||indexMatrix->GetElement(3,3)!=1)))
        return BuildResultFailure(params,CropFailure::BadInput,"Crop image lattice must be an invertible affine transform.");
    for (int cornerIndex = 0;
        cornerIndex < 8;
        ++cornerIndex) {
        const int i = extent[
            (cornerIndex & 1) != 0 ? 1 : 0];
        const int j = extent[
            (cornerIndex & 2) != 0 ? 3 : 2];
        const int k = extent[
            (cornerIndex & 4) != 0 ? 5 : 4];
        double point[3] = {};
        for(int row=0;row<3;++row)point[row]=indexToModel[row*4]*i+indexToModel[row*4+1]*j
            +indexToModel[row*4+2]*k+indexToModel[row*4+3];
        for (const double value : point) {
            if (!vtkMath::IsFinite(value)
                || std::abs(value)
                    > static_cast<double>(
                        std::numeric_limits<float>::max())) {
                return BuildResultFailure(
                    params,
                    CropFailure::BadInput,
                    "Crop image coordinates exceed predicate precision.");
            }
        }
    }

    if (!GetRamValid(
            image,
            params,
            payload,
            fallbackAvailableRamBytes,sourcePayload)) {
        return BuildResultFailure(
            params,
            CropFailure::LowRam,
            "Crop image build exceeds available RAM.");
    }

    if (getStopped()) return getCancelled();
    auto outputImage = vtkSmartPointer<vtkImageData>::New();
    // scalar 真源不可变；新快照只创建 VTK 外壳并共享 scalar storage，
    // 真实裁切域由独立 mask 表达，避免复制整卷 float 数据。
    outputImage->ShallowCopy(image);
    if(sourcePayload) {
        const auto& geometry=sourcePayload->GetGeometry();
        outputImage->SetExtent(geometry.extent[0],geometry.extent[1],geometry.extent[2],geometry.extent[3],geometry.extent[4],geometry.extent[5]);outputImage->SetOrigin(geometry.origin.data());
        outputImage->SetSpacing(geometry.spacing.data());outputImage->SetDirectionMatrix(geometry.direction.data());
    }
    const auto pointCount=sourcePayload?GetGridVoxelCount(sourcePayload->GetGeometry()).value_or(0)
        :static_cast<std::size_t>(image->GetNumberOfPoints());
    std::vector<std::uint8_t> maskValues(pointCount);

    const auto* inputMask=sourcePayload
        ? sourcePayload->GetValidityMask()?sourcePayload->GetValidityMask()->data():nullptr
        :validityMask?static_cast<const unsigned char*>(validityMask->GetScalarPointer(extent[0],extent[2],extent[4])):nullptr;
    auto* outputMask = maskValues.data();
    if ((!sourcePayload && validityMask && !inputMask)
        || !outputMask) {
        return BuildResultFailure(
            params,
            CropFailure::MaskFailed,
            "Crop image mask storage is unavailable.");
    }

    vtkIdType inputInc[3] = { 1, 0, 0 };
    vtkIdType outputInc[3] = { 1, 0, 0 };
    if(sourcePayload){inputInc[1]=xCount;inputInc[2]=xCount*yCount;}
    else if(validityMask)validityMask->GetIncrements(inputInc);
    outputInc[1] = xCount;
    outputInc[2] = xCount * yCount;

    const auto& canonical=payload.predicateTable->geometry;
    std::vector<std::size_t> keptBySlice(
        static_cast<std::size_t>(zCount),
        0);
    vtkSMPTools::For(
        vtkIdType{ 0 },
        zCount,
        [&](const vtkIdType first,
            const vtkIdType last) {
            for (vtkIdType zOffset = first;
                zOffset < last;
                ++zOffset) {
                const double indexK =
                    static_cast<double>(extent[4])
                    + static_cast<double>(zOffset);
                const auto* inputSlice = inputMask
                    ? inputMask
                        + zOffset * inputInc[2]
                    : nullptr;
                auto* outputSlice =
                    outputMask
                    + zOffset * outputInc[2];
                std::size_t sliceCount = 0;
                for (vtkIdType yOffset = 0;
                    yOffset < yCount;
                    ++yOffset) {
                    const double indexJ =
                        static_cast<double>(extent[2])
                        + static_cast<double>(yOffset);
                    const auto* inputRow = inputSlice
                        ? inputSlice
                            + yOffset * inputInc[1]
                        : nullptr;
                    auto* outputRow =
                        outputSlice
                        + yOffset * outputInc[1];
                    for (vtkIdType xOffset = 0;
                        xOffset < xCount;
                        ++xOffset) {
                        if (xOffset % 4096 == 0 && getStopped()) return;
                        const double indexI =
                            static_cast<double>(extent[0])
                            + static_cast<double>(xOffset);
                        CropVectorDouble3Array inputModelPoint = {};
                        for (int row = 0;
                            row < 3;
                            ++row) {
                            const auto* matrixRow =
                                indexToModel.data()
                                + row * 4;
                            inputModelPoint[row] = matrixRow[0] * indexI
                                + matrixRow[1] * indexJ + matrixRow[2] * indexK + matrixRow[3];
                        }
                        const bool hasBaseline =
                            !inputRow
                            || inputRow[
                                xOffset * inputInc[0]]
                                != 0;
                        const bool isKept = hasBaseline && std::all_of(canonical.begin(),canonical.begin()+payload.nodeCount,
                            [&](const CropGeometry& predicate){return predicate.GetKept(inputModelPoint);});
                        outputRow[
                            xOffset * outputInc[0]] =
                            isKept ? 255 : 0;
                        sliceCount +=
                            isKept ? 1 : 0;
                    }
                }
                keptBySlice[
                    static_cast<std::size_t>(
                        zOffset)] =
                    sliceCount;
            }
        });
    if (getStopped()) return getCancelled();
    const std::size_t keptCount =
        std::accumulate(
            keptBySlice.begin(),
            keptBySlice.end(),
            std::size_t{ 0 });

    if (keptCount == 0) {
        return BuildResultFailure(
            params,
            CropFailure::EmptyResult,
            "Crop image build removed every voxel.");
    }

    auto result = BuildResultBase(params);
    result.isSucceeded = true;
    result.imageData = std::move(outputImage);
    if (sourcePayload) {
        auto output = sourcePayload->CreateMaskSnapshot(std::move(maskValues));
        if (!output || !output->GetValid())
            return BuildResultFailure(params, CropFailure::BadInput, "Crop formal image payload is invalid.");
        result.outputPayload = std::move(output);
    } else {
        auto maskImage = vtkSmartPointer<vtkImageData>::New();
        maskImage->CopyStructure(image);
        maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        if (!maskImage->GetScalarPointer())
            return BuildResultFailure(params, CropFailure::MaskFailed, "Crop mask view allocation failed.");
        std::memcpy(maskImage->GetScalarPointer(), maskValues.data(), maskValues.size());
        result.maskImage = std::move(maskImage);
    }
    return result;
}

CropMaterializationCandidate CropAlgorithm::GetResult(
    vtkPolyData* polyData,
    const CropBuildParams& params,
    const CropShaderPayload& payload,
    const std::function<bool()>& getStopRequested,
    std::shared_ptr<const SurfaceMeshPayload> sourcePayload)
{
    const auto getCancelled = [&] {
        auto result = BuildResultFailure(params, CropFailure::Cancelled,
            "The crop build was cancelled before publication.");
        result.isCancelled = true;
        return result;
    };
    if (getStopRequested && getStopRequested()) return getCancelled();
    if (!polyData) {
        return BuildResultFailure(
            params,
            CropFailure::NoPolyData,
            "Crop result build requires PolyData input.");
    }
    if (!GetPayloadValid(params, payload)) {
        return BuildResultFailure(
            params,
            CropFailure::BadInput,
            "Crop PolyData build parameters are invalid.");
    }

    return CropMeshAlgorithm::GetResult(polyData,params,payload.predicateTable->geometry,getStopRequested,std::move(sourcePayload));
}

CropMaterializationCandidate CropAlgorithm::GetRoiResult(
    const CropInputSnapshot& input, RoiReadSnapshot roi,
    std::size_t availableRamBytes, const std::function<bool()>& getStopRequested)
{
    CropMaterializationCandidate result;
    result.failureReason=CropFailure::BadInput;
    if (!input.data || !GetInputValid(input) || !roi || roi->GetSource()!=input.data->self) return result;
    result.sourceRevision=input.data->self;
    result.roi=std::move(roi);
    result.inputRoi=result.roi->GetRevision();
    const auto cancelled=[&] {
        if (getStopRequested && getStopRequested()) {
            result.isCancelled=true; result.failureReason=CropFailure::Cancelled;
            result.message="ROI crop cancelled."; return true;
        }
        return false;
    };
    try {
        if (cancelled()) return result;
        if (input.image) {
            const auto* image=dynamic_cast<const ImageGrid3DPayload*>(input.data->payload.get());
            if (!image || !image->GetValid()) return result;
            const auto count=*GetGridVoxelCount(image->GetGeometry());
            const auto reserve=roiCopyLimit+16ULL*1024*1024;
            if (availableRamBytes<reserve || count>(availableRamBytes-reserve)/3) {
                result.failureReason=CropFailure::LowRam; return result;
            }
            std::vector<std::uint8_t> values(count);
            RoiMaskRequest request;
            for (int a=0;a<3;++a) request.region.size[a]=static_cast<std::size_t>(image->GetGeometry().dimensions[a]);
            request.getCancelled=getStopRequested;
            std::size_t selected=0;
            for (;;) {
                const auto chunk=result.roi->GetMaskChunk(request);
                if (chunk.error!=RoiError::None) {
                    result.isCancelled=chunk.error==RoiError::Cancelled;
                    result.failureReason=chunk.error==RoiError::Cancelled ? CropFailure::Cancelled
                        : chunk.error==RoiError::TooLarge ? CropFailure::LowRam:CropFailure::MaskFailed;
                    return result;
                }
                for (std::size_t i=0;i<chunk.values.size();++i) {
                    const auto index=request.voxelOffset+i;
                    const bool valid=(!image->GetValidityMask() || (*image->GetValidityMask())[index]!=0) && chunk.values[i]!=0;
                    values[index]=valid ? 255:0; selected+=valid;
                }
                if (chunk.isComplete) break;
                request.voxelOffset=chunk.nextOffset;
            }
            if (!selected) { result.failureReason=CropFailure::EmptyResult; return result; }
            if (cancelled()) return result;
            result.outputPayload=image->CreateMaskSnapshot(std::move(values));
        } else if (input.mesh && input.mesh->mesh) {
            const auto planes=result.roi->GetClipPlanes();
            if (planes.error!=RoiError::None) { result.failureReason=CropFailure::BadBuildMode; return result; }
            const auto* mesh=dynamic_cast<const SurfaceMeshPayload*>(input.data->payload.get());
            if (!mesh || !mesh->GetValid()) return result;
            const auto triangles=mesh->GetTriangles().size()/3;
            const auto perTriangle=(planes.planes.size()+3)*256;
            const auto reserve=16ULL*1024*1024;
            if (availableRamBytes<reserve || triangles>(availableRamBytes-reserve)/perTriangle) {
                result.failureReason=CropFailure::LowRam; return result;
            }
            vtkNew<vtkPoints> points; points->SetDataTypeToDouble();
            vtkNew<vtkCellArray> cells;
            const auto& vertices=mesh->GetVertices();
            const auto& topology=mesh->GetTriangles();
            for (std::size_t i=0;i<triangles;++i) {
                if ((i&127)==0 && cancelled()) return result;
                std::array<std::array<double,3>,3> triangle;
                for (std::size_t j=0;j<3;++j) {
                    const auto index=static_cast<std::size_t>(topology[i*3+j])*3;
                    triangle[j]={vertices[index],vertices[index+1],vertices[index+2]};
                }
                const auto polygon=result.roi->GetClippedTriangle(triangle);
                if (polygon.error!=RoiError::None) { result.failureReason=CropFailure::ClipFailed; return result; }
                if (polygon.points.size()<3) continue;
                cells->InsertNextCell(static_cast<int>(polygon.points.size()));
                for (const auto& point:polygon.points) cells->InsertCellPoint(points->InsertNextPoint(point.data()));
            }
            vtkNew<vtkPolyData> clipped; clipped->SetPoints(points); clipped->SetPolys(cells);
            vtkNew<vtkCleanPolyData> clean; clean->SetInputData(clipped); clean->ToleranceIsAbsoluteOn();
            const auto bounds=result.roi->GetBounds();
            const double scale=std::max({bounds[1]-bounds[0],bounds[3]-bounds[2],bounds[5]-bounds[4],1e-12});
            clean->SetAbsoluteTolerance(scale*1e-12); clean->ConvertLinesToPointsOff(); clean->ConvertPolysToLinesOff();
            vtkNew<vtkTriangleFilter> filter; filter->SetInputConnection(clean->GetOutputPort());
            filter->PassLinesOff(); filter->PassVertsOff(); filter->Update();
            if (cancelled()) return result;
            if (!filter->GetOutput()->GetNumberOfPolys()) { result.failureReason=CropFailure::EmptyResult; return result; }
            result.polyData=vtkSmartPointer<vtkPolyData>::New(); result.polyData->ShallowCopy(filter->GetOutput());
            result.preparedView=VtkPreparedDataView::BuildDataView(result.polyData,mesh->GetCoordinateFrame());
            if (result.preparedView) result.outputPayload=result.preparedView->payload;
        } else return result;
        result.recipePayload=std::make_shared<const RoiGeometryPayload>(result.roi->GetDefinition());
        if (!result.preparedView) result.preparedView=VtkPreparedDataView::BuildDataView(result.outputPayload,input.image);
        if (!result.outputPayload || !result.preparedView) return result;
        if (result.preparedView->image) {
            result.imageData=result.preparedView->image->image; result.maskImage=result.preparedView->image->validityMask;
        } else result.polyData=result.preparedView->mesh->mesh;
        if (cancelled()) return result;
        result.buildParameters="{\"schemaVersion\":2,\"inputRoi\":true}";
        result.isSucceeded=true; result.failureReason=CropFailure::None; result.message="ROI crop candidate ready.";
    } catch (const std::bad_alloc&) { result.failureReason=CropFailure::LowRam; }
    catch (...) { result.failureReason=CropFailure::WorkerFailed; }
    return result;
}
