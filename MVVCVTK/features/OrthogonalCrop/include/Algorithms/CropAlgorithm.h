#pragma once

#include "OrthogonalCropTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

    static CropExportResult GetResult(
        vtkImageData* image,
        vtkImageData* validityMask,
        const CropExportRequest& request,
        const CropShaderPayload& payload,
        std::size_t fallbackAvailableRamBytes = 0);

    static CropExportResult GetResult(
        vtkPolyData* polyData,
        const CropExportRequest& request,
        const CropShaderPayload& payload);
};
