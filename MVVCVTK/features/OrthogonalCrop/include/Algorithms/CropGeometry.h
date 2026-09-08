#pragma once

#include "OrthogonalCropTypes.h"

#include <optional>

enum class CropPointClassification { Kept, Removed, BoundaryBand, PrecisionNotMet };
struct CropFloatBounds final {
    CropPointClassification classification = CropPointClassification::PrecisionNotMet;
    std::array<std::array<double,2>,3> predicates{};
    std::size_t predicateCount = 0;
    std::size_t operationCount = 0;
};

// 规范双精度几何。所有派生表、物化与网格距离都从同一个 operation 构建。
class CropGeometry final {
public:
    static std::optional<CropGeometry> Build(CropOpItem operation);
    const CropOpItem& GetOperation() const noexcept { return m_operation; }
    const CropMatrixDouble16Array& GetBoxInverse() const noexcept { return m_boxInverse; }
    bool GetInside(const CropVectorDouble3Array& point) const noexcept;
    bool GetKept(const CropVectorDouble3Array& point) const noexcept;
    // 负值为内部；Box 为归一化半空间的 max，仍是1-Lipschitz保守函数。
    double GetSignedDistance(const CropVectorDouble3Array& point) const noexcept;
    // inputError is an absolute bound on upstream position conversion/interpolation error.
    // Bounds enclose canonical double signs and float evaluation, including FTZ and dot reordering.
    CropFloatBounds GetFloatBounds(const CropVectorDouble3Array& point,
        const CropVectorDouble3Array& inputError = {}) const noexcept;
    static std::optional<CropVectorDouble3Array> GetAffineFloatError(
        const CropMatrixDouble16Array& matrix, const CropVectorDouble3Array& point,
        const CropVectorDouble3Array& inputError = {}) noexcept;
    static bool GetOperationsSame(const CropOpItem& a, const CropOpItem& b) noexcept;

private:
    CropGeometry() = default;
    CropOpItem m_operation;
    CropMatrixDouble16Array m_boxInverse{};
    CropVectorDouble3Array m_boxRowNorm{};
};
