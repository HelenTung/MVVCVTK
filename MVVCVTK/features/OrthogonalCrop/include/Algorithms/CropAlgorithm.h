#pragma once

#include "OrthogonalCropTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// 一条 history 对应一份不可变 float32 table；nodeCount 由 payload 选择有效前缀。
struct CropPredicateTable final {
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
    bool isSucceeded = false;
    CropFailure failureReason = CropFailure::None;
    std::uint64_t failureOperationIndex = 0;
    std::vector<CropOpItem> operations;
    DataRevisionRef sourceRevision;
    std::size_t nodeCount = 0;
    std::string message;
    vtkSmartPointer<vtkImageData> imageData;
    vtkSmartPointer<vtkImageData> maskImage;
    vtkSmartPointer<vtkPolyData> polyData;
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
        std::size_t fallbackAvailableRamBytes = 0);

    static CropMaterializationCandidate GetResult(
        vtkPolyData* polyData,
        const CropBuildParams& params,
        const CropShaderPayload& payload);
};
