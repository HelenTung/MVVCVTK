#include "Algorithms/CropAlgorithm.h"

#include <vtkClipPolyData.h>
#include <vtkImageData.h>
#include <vtkImplicitFunction.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkSMPTools.h>
#include <vtkType.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

bool BuildBoxRows(const CropOpItem& operation, float* values)
{
    if (!values || !std::all_of(
            operation.boxToInputModelMatrix.begin(),
            operation.boxToInputModelMatrix.end(),
            GetFinite)) {
        return false;
    }

    const auto& matrixData = operation.boxToInputModelMatrix;
    if (std::abs(matrixData[12]) > kMatrixTolerance
        || std::abs(matrixData[13]) > kMatrixTolerance
        || std::abs(matrixData[14]) > kMatrixTolerance
        || std::abs(matrixData[15] - 1.0) > kMatrixTolerance) {
        return false;
    }

    vtkNew<vtkMatrix4x4> boxToInput;
    boxToInput->DeepCopy(matrixData.data());
    if (std::abs(boxToInput->Determinant()) <= kMatrixTolerance) {
        return false;
    }

    vtkNew<vtkMatrix4x4> inputToBox;
    vtkMatrix4x4::Invert(boxToInput, inputToBox);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const double value = inputToBox->GetElement(row, column);
            if (!GetFinite(value)
                || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
                return false;
            }
            values[row * 4 + column] = static_cast<float>(value);
        }
    }
    return true;
}

bool BuildPlaneRows(const CropOpItem& operation, float* values)
{
    if (!values) {
        return false;
    }
    const auto& center = operation.planeCenterInInputModel;
    const auto& normal = operation.planeNormalInInputModel;
    for (int axis = 0; axis < 3; ++axis) {
        if (!GetFinite(center[axis])
            || !GetFinite(normal[axis])
            || std::abs(center[axis])
                > static_cast<double>(
                    std::numeric_limits<float>::max())) {
            return false;
        }
    }

    double unitNormal[3] = {
        normal[0], normal[1], normal[2]
    };
    const double length =
        vtkMath::Normalize(unitNormal);
    if (!GetFinite(length) || length <= kMatrixTolerance) {
        return false;
    }

    for (int axis = 0; axis < 3; ++axis) {
        values[axis] = static_cast<float>(center[axis]);
        values[4 + axis] =
            static_cast<float>(unitNormal[axis]);
    }
    return true;
}

CropExportResult BuildExportFailure(
    const CropExportRequest& request,
    const CropFailure failureReason,
    const std::string& message,
    const std::uint64_t failureOperationIndex = 0)
{
    CropExportResult result;
    result.resolvedDataSource = request.dataSource;
    result.failureReason = failureReason;
    result.failureOperationIndex = failureOperationIndex;
    result.inputVersion = request.inputVersion;
    result.nodeCount = std::min(request.nodeCount, request.operations.size());
    result.operations.assign(
        request.operations.begin(),
        request.operations.begin() + result.nodeCount);
    result.message = message;
    return result;
}

CropExportResult BuildExportBase(const CropExportRequest& request)
{
    CropExportResult result;
    result.resolvedDataSource = request.dataSource;
    result.inputVersion = request.inputVersion;
    result.nodeCount = request.nodeCount;
    result.operations = request.operations;
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
        const auto& rgbaValues =
            predicateTable.rgbaValues;
        m_isValid =
            predicateTable.operationCount
                <= std::numeric_limits<std::size_t>::max()
                    / kItemSize
            && rgbaValues.size()
                == predicateTable.operationCount
                    * kItemSize
            && nodeCount
                <= predicateTable.operationCount;
        for (std::size_t index = 0;
            m_isValid && index < nodeCount;
            ++index) {
            const auto* values =
                m_values + index * kItemSize;
            m_isValid =
                (values[0] == 0.0f
                    || values[0] == 1.0f)
                && (values[1] == 0.0f
                    || values[1] == 1.0f)
                && std::all_of(
                    values,
                    values + kItemSize,
                    [](const float value) {
                        return vtkMath::IsFinite(
                            static_cast<double>(value));
                    });
            if (!m_isValid) {
                break;
            }
            if (values[0] == 0.0f) {
                const auto* matrix =
                    values + kTexelSize;
                m_isValid =
                    std::abs(matrix[12])
                        <= kBoxTolerance
                    && std::abs(matrix[13])
                        <= kBoxTolerance
                    && std::abs(matrix[14])
                        <= kBoxTolerance
                    && std::abs(matrix[15] - 1.0f)
                        <= kBoxTolerance;
                double matrixData[16] = {};
                for (int valueIndex = 0;
                    m_isValid && valueIndex < 16;
                    ++valueIndex) {
                    matrixData[valueIndex] =
                        static_cast<double>(
                            matrix[valueIndex]);
                }
                m_isValid = m_isValid
                    && vtkMatrix4x4::Determinant(
                        matrixData) != 0.0;
            }
            else {
                double unitNormal[3] = {
                    static_cast<double>(
                        values[kTexelSize * 2]),
                    static_cast<double>(
                        values[kTexelSize * 2 + 1]),
                    static_cast<double>(
                        values[kTexelSize * 2 + 2])
                };
                const double length =
                    vtkMath::Normalize(unitNormal);
                m_isValid = vtkMath::IsFinite(length)
                    && length > kMatrixTolerance;
            }
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
            else {
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
    const CropExportRequest& request,
    const CropShaderPayload& payload)
{
    const bool hasPlan = payload.predicateTable
        && CropPredicatePlan(
            *payload.predicateTable,
            payload.nodeCount).GetValid();
    return request.inputVersion != 0
        && request.operations.size() == request.nodeCount
        && request.nodeCount != 0
        && payload.revision != 0
        && payload.sourceStamp.version == request.inputVersion
        && payload.nodeCount == request.nodeCount
        && hasPlan;
}

bool GetRamValid(
    vtkImageData* image,
    const CropExportRequest& request,
    const CropShaderPayload& payload,
    const std::size_t fallbackAvailableRamBytes)
{
    const std::size_t availableRamBytes = request.availableRamBytes != 0
        ? request.availableRamBytes
        : fallbackAvailableRamBytes;
    if (availableRamBytes == 0) {
        return true;
    }

    const vtkIdType pointCount = image
        ? image->GetNumberOfPoints() : 0;
    if (pointCount < 0) {
        return false;
    }
    const std::size_t maskBytes =
        static_cast<std::size_t>(pointCount);
    const std::size_t tableBytes = payload.predicateTable
        ? payload.predicateTable->rgbaValues.size() * sizeof(float)
        : 0;
    if (maskBytes
            > std::numeric_limits<std::size_t>::max()
                - tableBytes
        || maskBytes + tableBytes
            > std::numeric_limits<std::size_t>::max() - kRamMargin) {
        return false;
    }
    return maskBytes + tableBytes
        + kRamMargin <= availableRamBytes;
}

class CropImplicit final : public vtkImplicitFunction {
public:
    static CropImplicit* New();
    vtkTypeMacro(CropImplicit, vtkImplicitFunction);

    bool SetTable(
        std::shared_ptr<const CropPredicateTable> predicateTable,
        const std::size_t nodeCount)
    {
        if (!predicateTable || predicateTable->operationCount < nodeCount) {
            return false;
        }
        m_predicateTable = std::move(predicateTable);
        m_nodeCount = nodeCount;
        Modified();
        return true;
    }

    double EvaluateFunction(double point[3]) override
    {
        const CropPointFloat3Array inputModelPoint = {
            static_cast<float>(point[0]),
            static_cast<float>(point[1]),
            static_cast<float>(point[2])
        };
        return m_predicateTable
            && CropAlgorithm::GetPointKept(
                *m_predicateTable,
                m_nodeCount,
                inputModelPoint)
            ? 1.0
            : -1.0;
    }

    void EvaluateGradient(double[3], double gradient[3]) override
    {
        gradient[0] = 0.0;
        gradient[1] = 0.0;
        gradient[2] = 0.0;
    }

private:
    std::shared_ptr<const CropPredicateTable> m_predicateTable;
    std::size_t m_nodeCount = 0;
};

vtkStandardNewMacro(CropImplicit);
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
        switch (operation.geometryType) {
        case CropShape::Box:
            itemValues[0] = 0.0f;
            if (!BuildBoxRows(operation, itemValues + kTexelSize)) {
                return BuildTableFailure(
                    &operation,
                    "Crop box matrix must be finite, affine, and invertible.");
            }
            break;
        case CropShape::Plane:
            itemValues[0] = 1.0f;
            if (!BuildPlaneRows(operation, itemValues + kTexelSize)) {
                return BuildTableFailure(
                    &operation,
                    "Crop plane center and normal must be finite and non-zero.");
            }
            break;
        default:
            return BuildTableFailure(&operation, "Crop shape is unsupported.");
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
    if (input.inputVersion == 0 || !GetBoundsValid(input.inputModelBounds)) {
        return false;
    }
    switch (input.dataSource) {
    case OrthogonalCropDataSource::ImageData:
        return input.imageData != nullptr
            && input.polyData == nullptr
            && GetMaskValid(
                input.imageData, input.validityMask);
    case OrthogonalCropDataSource::PolyData:
        return input.polyData != nullptr
            && input.imageData == nullptr
            && input.validityMask == nullptr;
    default:
        return false;
    }
}

bool CropAlgorithm::GetInputSame(
    const CropInputSnapshot& left,
    const CropInputSnapshot& right)
{
    return left.dataSource == right.dataSource
        && left.inputVersion == right.inputVersion
        && left.inputModelBounds == right.inputModelBounds
        && left.imageData.GetPointer() == right.imageData.GetPointer()
        && left.validityMask.GetPointer()
            == right.validityMask.GetPointer()
        && left.polyData.GetPointer() == right.polyData.GetPointer();
}

CropExportResult CropAlgorithm::GetResult(
    vtkImageData* image,
    vtkImageData* validityMask,
    const CropExportRequest& request,
    const CropShaderPayload& payload,
    const std::size_t fallbackAvailableRamBytes)
{
    if (!image || !image->GetPointData() || !image->GetPointData()->GetScalars()) {
        return BuildExportFailure(
            request,
            CropFailure::NoImage,
            "Crop export requires image scalars.");
    }
    if (!GetMaskValid(image, validityMask)
        || request.dataSource
            != OrthogonalCropDataSource::ImageData
        || !GetPayloadValid(request, payload)) {
        return BuildExportFailure(
            request,
            CropFailure::BadInput,
            "Crop image export request is invalid.");
    }

    int extent[6] = {};
    image->GetExtent(extent);
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
        || image->GetNumberOfPoints() <= 0) {
        return BuildExportFailure(
            request,
            CropFailure::EmptyResult,
            "Crop image export has an empty extent.");
    }

    std::array<double, 12> indexToModel = {};
    const auto* indexMatrix =
        image->GetIndexToPhysicalMatrix();
    if (!indexMatrix) {
        return BuildExportFailure(
            request,
            CropFailure::BadInput,
            "Crop image index transform is unavailable.");
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0;
            column < 4;
            ++column) {
            const double value =
                indexMatrix->GetElement(row, column);
            if (!vtkMath::IsFinite(value)) {
                return BuildExportFailure(
                    request,
                    CropFailure::BadInput,
                    "Crop image index transform must be finite.");
            }
            indexToModel[
                static_cast<std::size_t>(
                    row * 4 + column)] =
                value;
        }
    }
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
        image->TransformIndexToPhysicalPoint(
            i, j, k, point);
        for (const double value : point) {
            if (!vtkMath::IsFinite(value)
                || std::abs(value)
                    > static_cast<double>(
                        std::numeric_limits<float>::max())) {
                return BuildExportFailure(
                    request,
                    CropFailure::BadInput,
                    "Crop image coordinates exceed predicate precision.");
            }
        }
    }

    if (!GetRamValid(
            image,
            request,
            payload,
            fallbackAvailableRamBytes)) {
        return BuildExportFailure(
            request,
            CropFailure::LowRam,
            "Crop image export exceeds available RAM.");
    }

    auto outputImage = vtkSmartPointer<vtkImageData>::New();
    // scalar 真源不可变；新快照只创建 VTK 外壳并共享 scalar storage，
    // 真实裁切域由独立 mask 表达，避免复制整卷 float 数据。
    outputImage->ShallowCopy(image);
    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    maskImage->CopyStructure(image);
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    const auto* inputMask = validityMask
        ? static_cast<const unsigned char*>(
            validityMask->GetScalarPointer(
                extent[0], extent[2], extent[4]))
        : nullptr;
    auto* outputMask =
        static_cast<unsigned char*>(
            maskImage->GetScalarPointer(
                extent[0], extent[2], extent[4]));
    if ((validityMask && !inputMask)
        || !outputMask) {
        return BuildExportFailure(
            request,
            CropFailure::MaskFailed,
            "Crop image mask storage is unavailable.");
    }

    vtkIdType inputInc[3] = { 1, 0, 0 };
    vtkIdType outputInc[3] = { 1, 0, 0 };
    if (validityMask) {
        validityMask->GetIncrements(inputInc);
    }
    maskImage->GetIncrements(outputInc);

    const CropPredicatePlan predicatePlan(
        *payload.predicateTable,
        payload.nodeCount);
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
                        const double indexI =
                            static_cast<double>(extent[0])
                            + static_cast<double>(xOffset);
                        CropPointFloat3Array inputModelPoint = {};
                        for (int row = 0;
                            row < 3;
                            ++row) {
                            const auto* matrixRow =
                                indexToModel.data()
                                + row * 4;
                            inputModelPoint[row] =
                                static_cast<float>(
                                    matrixRow[0] * indexI
                                    + matrixRow[1] * indexJ
                                    + matrixRow[2] * indexK
                                    + matrixRow[3]);
                        }
                        const bool hasBaseline =
                            !inputRow
                            || inputRow[
                                xOffset * inputInc[0]]
                                != 0;
                        const bool isKept =
                            hasBaseline
                            && predicatePlan
                                .GetPointKeptUnchecked(
                                inputModelPoint);
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
    const std::size_t keptCount =
        std::accumulate(
            keptBySlice.begin(),
            keptBySlice.end(),
            std::size_t{ 0 });

    if (keptCount == 0) {
        return BuildExportFailure(
            request,
            CropFailure::EmptyResult,
            "Crop image export removed every voxel.");
    }

    auto result = BuildExportBase(request);
    result.isSucceeded = true;
    result.imageData = std::move(outputImage);
    result.maskImage = std::move(maskImage);
    return result;
}

CropExportResult CropAlgorithm::GetResult(
    vtkPolyData* polyData,
    const CropExportRequest& request,
    const CropShaderPayload& payload)
{
    if (!polyData) {
        return BuildExportFailure(
            request,
            CropFailure::NoPolyData,
            "Crop export requires PolyData input.");
    }
    if (request.dataSource != OrthogonalCropDataSource::PolyData
        || !GetPayloadValid(request, payload)) {
        return BuildExportFailure(
            request,
            CropFailure::BadInput,
            "Crop PolyData export request is invalid.");
    }

    vtkNew<CropImplicit> cropFunction;
    if (!cropFunction->SetTable(payload.predicateTable, payload.nodeCount)) {
        return BuildExportFailure(
            request,
            CropFailure::BadInput,
            "Crop predicate payload is invalid.");
    }
    vtkNew<vtkClipPolyData> clip;
    clip->SetInputData(polyData);
    clip->SetClipFunction(cropFunction);
    clip->SetValue(0.0);
    clip->InsideOutOff();
    clip->GenerateClippedOutputOff();
    clip->Update();

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->DeepCopy(clip->GetOutput());
    if (output->GetNumberOfPoints() == 0 || output->GetNumberOfCells() == 0) {
        return BuildExportFailure(
            request,
            CropFailure::EmptyResult,
            "Crop PolyData export removed every cell.");
    }

    auto result = BuildExportBase(request);
    result.isSucceeded = true;
    result.polyData = std::move(output);
    return result;
}
