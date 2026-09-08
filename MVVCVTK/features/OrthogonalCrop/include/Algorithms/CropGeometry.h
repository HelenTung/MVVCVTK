#pragma once

#include "OrthogonalCropTypes.h"

#include <optional>

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
    static bool GetOperationsSame(const CropOpItem& a, const CropOpItem& b) noexcept;

private:
    CropOpItem m_operation;
    CropMatrixDouble16Array m_boxInverse{};
    CropVectorDouble3Array m_boxRowNorm{};
};
