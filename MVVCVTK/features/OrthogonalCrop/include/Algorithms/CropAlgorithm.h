#pragma once

#include "OrthogonalCropTypes.h"
#include "Algorithms/CropGeometry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// Canonical double predicates are authoritative; rgbaValues is their derived GPU encoding.
struct CropPredicateTable final {
    std::uint32_t schemaVersion = 1;
    std::vector<CropGeometry> geometry;
    std::vector<float> rgbaValues;
    std::size_t operationCount = 0;
};

struct CropTableResult final {
    bool isSucceeded = false;
    CropFailure failureReason = CropFailure::None;
    std::uint64_t failureOperationIndex = 0;
    std::string message;
    std::shared_ptr<const CropPredicateTable> predicateTable;
};

// worker-only 候选；只有 Host 的 DataTransaction 成功后才产生公开 CropBuildResult。
struct CropMaterializationCandidate final {
    CropDocumentId documentId = 0;
    CropNodeId nodeId = 0;
    CropRequestId requestId = 0;
    std::string buildParameters;
    bool isSucceeded = false;
    bool isCancelled = false;
    CropFailure failureReason = CropFailure::None;
    std::uint64_t failureOperationIndex = 0;
    std::vector<CropOpItem> operations;
    DataRevisionRef sourceRevision;
    std::size_t nodeCount = 0;
    std::string message;
    std::shared_ptr<const IDataPayload> outputPayload;
    std::shared_ptr<const RoiGeometryPayload> recipePayload;
    std::shared_ptr<const VtkPreparedDataView> preparedView;
    vtkSmartPointer<vtkImageData> imageData;
    vtkSmartPointer<vtkImageData> maskImage;
    vtkSmartPointer<vtkPolyData> polyData;
    double meshErrorBound = 0;
    double meshAreaErrorBound = 0;
    std::size_t meshTriangleCount = 0;
};

class CropAlgorithm final {
public:
    CropAlgorithm() = delete;

    static constexpr std::size_t GetTexelCount()
    {
        return 5;
    }

    static CropMatrixDouble16Array GetIdentityMatrix();
    static CropMatrixDouble16Array GetBoxMatrix(
        const CropBoundsDouble6Array& inputModelBounds);

    static CropTableResult BuildPredicateTable(
        const std::vector<CropOpItem>& operations,
        std::size_t nodeCount);

    static bool GetTableValid(const CropPredicateTable& table, std::size_t nodeCount);

    static bool GetPointKept(
        const CropPredicateTable& predicateTable,
        std::size_t nodeCount,
        const CropPointFloat3Array& inputModelPoint);

    static bool GetInputValid(const CropInputSnapshot& input);
    static bool GetInputSame(
        const CropInputSnapshot& left,
        const CropInputSnapshot& right);

    static CropMaterializationCandidate GetResult(
        vtkImageData* image,
        vtkImageData* validityMask,
        const CropBuildParams& params,
        const CropShaderPayload& payload,
        std::size_t fallbackAvailableRamBytes = 0,
        const std::function<bool()>& getStopRequested = {},
        const ImageGrid3DPayload* sourcePayload = nullptr);

    static CropMaterializationCandidate GetResult(
        vtkPolyData* polyData,
        const CropBuildParams& params,
        const CropShaderPayload& payload,
        const std::function<bool()>& getStopRequested = {},
        std::shared_ptr<const SurfaceMeshPayload> sourcePayload = {});
};
