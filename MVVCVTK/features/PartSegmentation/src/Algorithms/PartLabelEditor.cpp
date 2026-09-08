#include "Algorithms/PartLabelEditor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

constexpr std::size_t maxParts = 4096;
constexpr std::size_t cancelBatch = 4096;
constexpr auto noIndex = std::numeric_limits<std::size_t>::max();

class EditFailure final : public std::runtime_error {
public:
    EditFailure(PartFailureReason value, const char* text)
        : std::runtime_error(text), reason(value) {}
    PartFailureReason reason;
};

void SetFailure(PartFailureReason reason, const char* text)
{
    throw EditFailure(reason, text);
}

double GetScalar(const PartScalarView& values, std::size_t index)
{
    if (!values.data || index >= values.valueCount) {
        SetFailure(PartFailureReason::InvalidGeometry, "Invalid scalar buffer.");
    }
#define PART_EDIT_SCALAR(kind, type) case PartScalarType::kind: \
    return static_cast<double>(static_cast<const type*>(values.data)[index])
    switch (values.scalarType) {
    PART_EDIT_SCALAR(Int8, std::int8_t);
    PART_EDIT_SCALAR(UInt8, std::uint8_t);
    PART_EDIT_SCALAR(Int16, std::int16_t);
    PART_EDIT_SCALAR(UInt16, std::uint16_t);
    PART_EDIT_SCALAR(Int32, std::int32_t);
    PART_EDIT_SCALAR(UInt32, std::uint32_t);
    PART_EDIT_SCALAR(Int64, std::int64_t);
    PART_EDIT_SCALAR(UInt64, std::uint64_t);
    PART_EDIT_SCALAR(Float32, float);
    PART_EDIT_SCALAR(Float64, double);
    }
#undef PART_EDIT_SCALAR
    SetFailure(PartFailureReason::UnsupportedScalar, "Unsupported edit scalar.");
    return 0.0;
}

bool GetMaskPoint(const LabelMap3DPayload& mask, std::size_t index)
{
    return std::visit([index](const auto& values) {
        return values && index < values->size() && (*values)[index] != 0;
    }, mask.GetValues());
}

template<class Scalar>
bool GetGrayInRange(Scalar value, double minimum, double maximum)
{
    if constexpr (std::is_integral_v<Scalar>) {
        // 避免 uint64/int64 转 double 后舍入，从而把阈值外整数纳入区域。
        const double upperExclusive = std::ldexp(1.0, std::numeric_limits<Scalar>::digits);
        const double lowerInclusive = std::is_signed_v<Scalar> ? -upperExclusive : 0.0;
        minimum = std::ceil(minimum);
        maximum = std::floor(maximum);
        if (minimum > maximum || minimum >= upperExclusive || maximum < lowerInclusive) return false;
        const auto lower = minimum <= lowerInclusive ? std::numeric_limits<Scalar>::lowest()
            : static_cast<Scalar>(minimum);
        const auto upper = maximum >= upperExclusive ? std::numeric_limits<Scalar>::max()
            : static_cast<Scalar>(maximum);
        return value >= lower && value <= upper;
    }
    else return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool GetGrayInRange(const PartScalarView& view, std::size_t index, double minimum, double maximum)
{
    if (!view.data || index >= view.valueCount) return false;
#define PART_EDIT_RANGE(kind, type) case PartScalarType::kind: \
    return GetGrayInRange(static_cast<const type*>(view.data)[index], minimum, maximum)
    switch (view.scalarType) {
    PART_EDIT_RANGE(Int8, std::int8_t);
    PART_EDIT_RANGE(UInt8, std::uint8_t);
    PART_EDIT_RANGE(Int16, std::int16_t);
    PART_EDIT_RANGE(UInt16, std::uint16_t);
    PART_EDIT_RANGE(Int32, std::int32_t);
    PART_EDIT_RANGE(UInt32, std::uint32_t);
    PART_EDIT_RANGE(Int64, std::int64_t);
    PART_EDIT_RANGE(UInt64, std::uint64_t);
    PART_EDIT_RANGE(Float32, float);
    PART_EDIT_RANGE(Float64, double);
    }
#undef PART_EDIT_RANGE
    return false;
}

PartBindingRef GetBinding(const PartCatalog& catalog, PartObjectId object)
{
    return { { catalog.partSetId, object }, catalog.resultRevision };
}

class EditBuilder final {
public:
    EditBuilder(const PartEditInput& input, PartIdentityFactory& identities,
        const std::function<bool()>& stop)
        : m_input(input), m_ids(identities), m_stop(stop) {}

    PartEditBuildResult Build()
    {
        // 1. 校验完整输入与预算后，才创建独占的可写标签候选。
        SetInput();
        m_labels = std::make_shared<std::vector<PartLabelId>>(*m_input.previous.labels);
        m_changed.resize(m_old->partsByLabel.size(), false);
        // 2. 每个工具只改变候选；不执行初始阈值分割，也不触碰原始 scalar。
        std::visit([this](const auto& operation) { SetOperation(operation); },
            m_input.request.operation);
        if (!m_changedExtent) {
            SetFailure(PartFailureReason::NoChange, "Edit changes no voxel ownership.");
        }
        // 3. 统一紧凑编号、重算指标及目录，再验证标签与身份的一致性。
        auto catalog = BuildCatalog();
        if (!GetPartCatalogCountsValid(*catalog, m_counts, m_stop)) {
            CheckStop(0);
            SetFailure(PartFailureReason::InternalError, "Edited catalog is inconsistent.");
        }
        return { PartFailureReason::None, "Label edit candidate is ready.",
            m_requiredBytes, m_labels, std::move(catalog) };
    }

    std::size_t GetRequiredBytes() const noexcept { return m_requiredBytes; }

private:
    void CheckStop(std::size_t index) const
    {
        if (index % cancelBatch == 0 && m_stop && m_stop()) {
            SetFailure(PartFailureReason::Cancelled, "Label editing was cancelled.");
        }
    }

    static bool GetInside(const std::array<int, 3>& p, const std::array<int, 6>& e)
    {
        for (std::size_t a = 0; a < 3; ++a) {
            if (p[a] < e[a * 2] || p[a] > e[a * 2 + 1]) return false;
        }
        return true;
    }

    std::array<int, 3> GetIndex(std::size_t offset) const
    {
        return { static_cast<int>(offset % m_stride[1]) + m_geometry.extent[0],
            static_cast<int>((offset / m_stride[1]) % m_geometry.dimensions[1])
                + m_geometry.extent[2],
            static_cast<int>(offset / m_stride[2]) + m_geometry.extent[4] };
    }

    std::size_t GetOffset(const std::array<int, 3>& index) const
    {
        if (!GetInside(index, m_geometry.extent)) {
            SetFailure(PartFailureReason::InvalidEdit, "Edit index is outside the source grid.");
        }
        std::size_t result = 0;
        for (std::size_t a = 0; a < 3; ++a) {
            result += static_cast<std::size_t>(
                static_cast<std::int64_t>(index[a]) - m_geometry.extent[a * 2]) * m_stride[a];
        }
        return result;
    }

    template<class Callback>
    void SendNeighbors(std::size_t offset, const Callback& callback) const
    {
        const auto index = GetIndex(offset);
        for (std::size_t a = 0; a < 3; ++a) {
            if (index[a] > m_geometry.extent[a * 2]) callback(offset - m_stride[a], a);
            if (index[a] < m_geometry.extent[a * 2 + 1]) callback(offset + m_stride[a], a);
        }
    }

    std::array<double, 3> GetPhysical(const std::array<int, 3>& index) const
    {
        auto point = m_geometry.origin;
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t a = 0; a < 3; ++a) {
                point[r] += m_geometry.direction[r * 3 + a]
                    * m_geometry.spacing[a] * index[a];
            }
        }
        return point;
    }

    std::array<int, 6> GetPartsExtent(const std::vector<PartLabelId>& parts) const
    {
        std::optional<std::array<int,6>> result;
        for (const auto label : parts) {
            if (!label || label >= m_old->partsByLabel.size()) SetFailure(PartFailureReason::InvalidEdit, "Part label is invalid.");
            const auto& extent = m_old->partsByLabel[label].metrics.voxelExtent;
            for (std::size_t a = 0; a < 3; ++a)
                if (extent[a*2] > extent[a*2+1] || extent[a*2] < m_geometry.extent[a*2]
                    || extent[a*2+1] > m_geometry.extent[a*2+1])
                    SetFailure(PartFailureReason::InvalidGeometry, "Part bounds are outside the source grid.");
            if (!result) result = extent;
            else for (std::size_t a = 0; a < 3; ++a) {
                (*result)[a*2] = std::min((*result)[a*2], extent[a*2]);
                (*result)[a*2+1] = std::max((*result)[a*2+1], extent[a*2+1]);
            }
        }
        if (!result) SetFailure(PartFailureReason::InvalidEdit, "No source part bounds.");
        return *result;
    }

    template<class Callback>
    void SendExtentVoxels(const std::array<int,6>& extent, const Callback& callback) const
    {
        std::size_t visited = 0;
        for (std::int64_t z = extent[4]; z <= extent[5]; ++z)
            for (std::int64_t y = extent[2]; y <= extent[3]; ++y) {
                auto offset = GetOffset({extent[0], static_cast<int>(y), static_cast<int>(z)});
                for (std::int64_t x = extent[0]; x <= extent[1]; ++x, ++offset) {
                    CheckStop(visited++); callback(offset);
                }
            }
    }

    std::size_t GetSplitOffset(std::size_t sourceOffset) const
    {
        const auto p = GetIndex(sourceOffset);
        if (!GetInside(p, m_splitExtent)) return noIndex;
        std::size_t result = 0;
        for (std::size_t a = 0; a < 3; ++a)
            result += static_cast<std::size_t>(static_cast<std::int64_t>(p[a])-m_splitExtent[a*2])*m_splitStride[a];
        return result;
    }

    std::size_t GetSplitSource(std::size_t offset) const
    {
        return GetOffset({static_cast<int>(offset%m_splitDimensions[0])+m_splitExtent[0],
            static_cast<int>((offset/m_splitStride[1])%m_splitDimensions[1])+m_splitExtent[2],
            static_cast<int>(offset/m_splitStride[2])+m_splitExtent[4]});
    }

    template<class Callback>
    void SendSplitNeighbors(std::size_t offset, const Callback& callback) const
    {
        for (std::size_t a = 0; a < 3; ++a) {
            const auto coordinate = (offset/m_splitStride[a])%m_splitDimensions[a];
            if (coordinate) callback(offset-m_splitStride[a], a);
            if (coordinate+1 < m_splitDimensions[a]) callback(offset+m_splitStride[a], a);
        }
    }

    PartLabelId GetLabel(const PartBindingRef& binding) const
    {
        if (binding.object.partSetId != m_old->partSetId
            || binding.resultRevision != m_old->resultRevision) {
            SetFailure(PartFailureReason::RevisionConflict, "Part binding is stale.");
        }
        const auto found = m_old->labelByObject.find(binding.object.objectId);
        if (found == m_old->labelByObject.end() || found->second == 0
            || found->second >= m_old->partsByLabel.size()
            || m_old->partsByLabel[found->second].objectId != binding.object.objectId) {
            SetFailure(PartFailureReason::InvalidEdit, "Part binding was not found.");
        }
        return found->second;
    }

    void SetMaskGeometry(const std::shared_ptr<const LabelMap3DPayload>& mask) const
    {
        if (!mask) return;
        const auto& g = mask->GetGeometry();
        if (!mask->GetValid() || g.extent != m_geometry.extent
            || g.dimensions != m_geometry.dimensions || g.spacing != m_geometry.spacing
            || g.origin != m_geometry.origin || g.direction != m_geometry.direction
            || g.coordinateFrame != m_geometry.coordinateFrame) {
            SetFailure(PartFailureReason::InvalidGeometry, "Edit mask grid does not match labels.");
        }
    }

    void SetInput()
    {
        CheckStop(0);
        m_old = m_input.previous.catalog;
        if (!m_old || !m_input.previous.labels || m_old->partsByLabel.empty()
            || m_old->partsByLabel.size() > maxParts + 1
            || m_old->catalogRevision != m_input.request.expectedCatalogRevision
            || m_old->resultRevision == std::numeric_limits<std::uint64_t>::max()
            || m_old->catalogRevision == std::numeric_limits<std::uint64_t>::max()) {
            SetFailure(PartFailureReason::RevisionConflict, "Edit base catalog is invalid or stale.");
        }
        const auto& v = m_input.volume;
        m_geometry = { v.extent, v.dimensions, v.spacing, v.origin, v.direction,
            m_input.coordinateFrame };
        const auto count = GetGridVoxelCount(m_geometry);
        if (!count || !GetGridGeometryValid(m_geometry)
            || m_input.previous.labels->size() != *count
            || v.values.valueCount != *count || !v.values.data
            || (v.validity && (v.validity->valueCount != *count || !v.validity->data))) {
            SetFailure(PartFailureReason::InvalidGeometry, "Edit grid or buffer is invalid.");
        }
        for (std::size_t a = 0; a < 3; ++a) {
            for (std::size_t b = 0; b < 3; ++b) {
                double dot = 0;
                for (std::size_t r = 0; r < 3; ++r) {
                    dot += v.direction[r * 3 + a] * v.direction[r * 3 + b];
                }
                if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 1e-6) {
                    SetFailure(PartFailureReason::InvalidGeometry, "Edit direction must be orthonormal.");
                }
                if (a == b) m_edgeLength[a] = std::sqrt(dot) * v.spacing[a];
            }
        }
        m_count = *count;
        m_stride = {1, static_cast<std::size_t>(v.dimensions[0]),
            static_cast<std::size_t>(v.dimensions[0]) * static_cast<std::size_t>(v.dimensions[1])};
        if (const auto* split = std::get_if<PartSplitEdit>(&m_input.request.operation)) {
            m_splitExtent = GetPartsExtent({GetLabel(split->target)});
            m_splitCount = 1;
            for (std::size_t a = 0; a < 3; ++a) {
                m_splitStride[a] = m_splitCount;
                m_splitDimensions[a] = static_cast<std::size_t>(static_cast<std::int64_t>(m_splitExtent[a*2+1])-m_splitExtent[a*2]+1);
                if (m_splitCount > m_count / m_splitDimensions[a]) SetFailure(PartFailureReason::InvalidGeometry, "Part bounds overflow.");
                m_splitCount *= m_splitDimensions[a];
            }
        }
        // 目录、指标、映射、请求派生的小容器及分配器余量。
        constexpr std::size_t catalogReserve = (maxParts * 2 + 1) * 2048;
        m_requiredBytes = catalogReserve;
        const auto reserve = [&](std::size_t count, std::size_t bytes) {
            if (count > (std::numeric_limits<std::size_t>::max()-m_requiredBytes)/bytes) {
                m_requiredBytes = std::numeric_limits<std::size_t>::max();
                SetFailure(PartFailureReason::BudgetExceeded, "Edit size overflows.");
            }
            m_requiredBytes += count*bytes;
        };
        reserve(m_count, sizeof(PartLabelId));
        std::visit([&](const auto& operation) {
            using Op = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<Op, PartSplitEdit>)
                reserve(m_splitCount, sizeof(std::uint32_t)+2*sizeof(std::uint8_t)+sizeof(double)+2*sizeof(std::size_t));
            else if constexpr (std::is_same_v<Op, PartFillEdit> || std::is_same_v<Op, PartGrowEdit> || std::is_same_v<Op, PartIslandEdit>)
                reserve(m_count/64 + (m_count%64 != 0), sizeof(std::uint64_t));
        }, m_input.request.operation);
        const auto requestBytes = GetPartEditBytes(m_input.request);
        if (!requestBytes || *requestBytes > (std::numeric_limits<std::size_t>::max() - m_requiredBytes) / 3U) {
            SetFailure(PartFailureReason::BudgetExceeded, "Edit request storage exceeds the bounded limit.");
        }
        m_requiredBytes += *requestBytes * 3U;
        if (m_requiredBytes > m_input.maxWorkingBytes) {
            SetFailure(PartFailureReason::BudgetExceeded, "Edit workspace budget exceeded.");
        }
        m_fixedBytes = m_requiredBytes;
        m_queueLimit = std::min(m_count, (m_input.maxWorkingBytes-m_fixedBytes)/(3*sizeof(std::size_t)));
        if (!GetPartCatalogValid(*m_old, *m_input.previous.labels, m_stop, &m_counts)) {
            CheckStop(0);
            SetFailure(PartFailureReason::InvalidEdit, "Edit input labels and catalog disagree.");
        }
        m_stride = { 1, static_cast<std::size_t>(v.dimensions[0]),
            static_cast<std::size_t>(v.dimensions[0]) * static_cast<std::size_t>(v.dimensions[1]) };
        m_extent = m_input.request.scope.extent.value_or(v.extent);
        for (std::size_t a = 0; a < 3; ++a) {
            if (m_extent[a * 2] > m_extent[a * 2 + 1]
                || m_extent[a * 2] < v.extent[a * 2]
                || m_extent[a * 2 + 1] > v.extent[a * 2 + 1]) {
                SetFailure(PartFailureReason::InvalidEdit, "Edit scope extent is invalid.");
            }
        }
        if (m_input.request.scope.roiMask.has_value() != static_cast<bool>(m_input.roiMask)
            || m_input.request.scope.protectionMask.has_value() != static_cast<bool>(m_input.protectionMask)) {
            SetFailure(PartFailureReason::InvalidEdit, "Edit mask was not resolved.");
        }
        SetMaskGeometry(m_input.roiMask);
        SetMaskGeometry(m_input.protectionMask);
        m_locked.resize(m_old->partsByLabel.size(), false);
        for (std::size_t i = 0; i < m_input.request.scope.protectedParts.size(); ++i) {
            CheckStop(i); m_locked[GetLabel(m_input.request.scope.protectedParts[i])] = true;
        }
    }

    bool GetEditable(std::size_t i) const
    {
        const auto label = (*m_labels)[i];
        if (m_locked[label] || !GetInside(GetIndex(i), m_extent)
            || (m_input.protectionMask && GetMaskPoint(*m_input.protectionMask, i))
            || (m_input.roiMask && !GetMaskPoint(*m_input.roiMask, i))) return false;
        const double validity = m_input.volume.validity ? GetScalar(*m_input.volume.validity, i) : 1.0;
        return std::isfinite(validity) && validity != 0.0;
    }

    PartLabelId GetWritableLabel(const PartBindingRef& binding) const
    {
        const auto label = GetLabel(binding);
        if (m_locked[label]) SetFailure(PartFailureReason::ConstraintConflict, "Protected part cannot gain or lose voxels.");
        return label;
    }

    std::vector<bool> GetAllowed(PartLabelId target,
        const std::vector<PartBindingRef>& overwrite, bool background) const
    {
        std::vector<bool> allowed(m_old->partsByLabel.size(), false);
        allowed[0] = background;
        allowed[target] = true;
        for (std::size_t i = 0; i < overwrite.size(); ++i) { CheckStop(i); allowed[GetLabel(overwrite[i])] = true; }
        return allowed;
    }

    void SetVoxel(std::size_t i, PartLabelId target)
    {
        const auto old = (*m_labels)[i];
        if (old == target) return;
        if (target >= m_counts.size()) m_counts.resize(static_cast<std::size_t>(target)+1, 0);
        if (!m_counts[old]) SetFailure(PartFailureReason::InternalError, "Edit label count underflows.");
        --m_counts[old]; ++m_counts[target];
        const auto point = GetIndex(i);
        if (!m_changedExtent) m_changedExtent = std::array<int,6>{point[0],point[0],point[1],point[1],point[2],point[2]};
        else for (std::size_t a = 0; a < 3; ++a) {
            (*m_changedExtent)[a*2] = std::min((*m_changedExtent)[a*2], point[a]);
            (*m_changedExtent)[a*2+1] = std::max((*m_changedExtent)[a*2+1], point[a]);
        }
        if (old < m_changed.size()) m_changed[old] = true;
        if (target < m_changed.size()) m_changed[target] = true;
        (*m_labels)[i] = target;
    }

    static double GetDot(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        return std::inner_product(a.begin(), a.end(), b.begin(), 0.0);
    }

    void SetOperation(const PartBrushEdit& op)
    {
        const auto target = GetWritableLabel(op.target);
        if (op.sourcePoints.empty() || !std::isfinite(op.radiusMM) || op.radiusMM <= 0
            || (op.isErase && !op.overwriteParts.empty())) {
            SetFailure(PartFailureReason::InvalidEdit, "Invalid brush path or radius.");
        }
        for (std::size_t i = 0; i < op.sourcePoints.size(); ++i) {
            CheckStop(i);
            const auto& p = op.sourcePoints[i];
            if (!std::all_of(p.begin(), p.end(), [](double x) { return std::isfinite(x); })) {
                SetFailure(PartFailureReason::InvalidEdit, "Brush points must be finite.");
            }
        }
        std::array<double, 3> normal{};
        if (op.slice) {
            normal = op.slice->normal;
            const double length = std::sqrt(GetDot(normal, normal));
            if (!std::isfinite(length) || length <= 0
                || !std::isfinite(op.slice->thicknessMM) || op.slice->thicknessMM <= 0
                || !std::all_of(op.slice->origin.begin(), op.slice->origin.end(),
                    [](double x) { return std::isfinite(x); })) {
                SetFailure(PartFailureReason::InvalidEdit, "Invalid brush slice plane.");
            }
            for (auto& x : normal) x /= length;
        }
        const auto allowed = GetAllowed(target, op.overwriteParts, op.isBackgroundAllowed);
        for (std::size_t s = 0; s < op.sourcePoints.size(); ++s) {
            CheckStop(s);
            auto start = op.sourcePoints[s == 0 ? 0 : s - 1], end = op.sourcePoints[s];
            if (op.slice) for (auto* p : {&start, &end}) {
                std::array<double,3> delta{};
                for (std::size_t a = 0; a < 3; ++a) delta[a] = (*p)[a]-op.slice->origin[a];
                const auto distance = GetDot(delta, normal);
                for (std::size_t a = 0; a < 3; ++a) (*p)[a] -= distance*normal[a];
            }
            auto extent = m_extent;
            bool intersects = true;
            const double radius = op.radiusMM + (op.slice ? op.slice->thicknessMM/2 : 0);
            for (std::size_t a = 0; a < 3; ++a) {
                double first = 0, last = 0;
                for (std::size_t r = 0; r < 3; ++r) {
                    first += m_geometry.direction[r*3+a]*(start[r]-m_geometry.origin[r]);
                    last += m_geometry.direction[r*3+a]*(end[r]-m_geometry.origin[r]);
                }
                first /= m_geometry.spacing[a]; last /= m_geometry.spacing[a];
                const auto lower = std::floor(std::min(first,last)-radius/m_geometry.spacing[a]);
                const auto upper = std::ceil(std::max(first,last)+radius/m_geometry.spacing[a]);
                if (!std::isfinite(lower) || !std::isfinite(upper)) SetFailure(PartFailureReason::InvalidEdit, "Brush bounds overflow.");
                if (lower > extent[a*2+1] || upper < extent[a*2]) { intersects = false; break; }
                extent[a*2] = static_cast<int>(std::max(lower, static_cast<double>(extent[a*2])));
                extent[a*2+1] = static_cast<int>(std::min(upper, static_cast<double>(extent[a*2+1])));
            }
            if (!intersects) continue;
            std::array<double,3> line{};
            for (std::size_t a = 0; a < 3; ++a) line[a] = end[a]-start[a];
            const double length2 = GetDot(line,line);
            SendExtentVoxels(extent, [&](std::size_t i) {
                if (!GetEditable(i) || !allowed[(*m_labels)[i]] || (op.isErase && (*m_labels)[i] != target)) return;
                const auto point = GetPhysical(GetIndex(i));
                std::array<double,3> delta{};
                for (std::size_t a = 0; a < 3; ++a) delta[a] = point[a]-start[a];
                if (op.slice) {
                    const double dn = GetDot(delta,normal);
                    if (std::abs(dn) > op.slice->thicknessMM/2) return;
                    for (std::size_t a = 0; a < 3; ++a) delta[a] -= dn*normal[a];
                }
                const double t = length2 == 0 ? 0 : std::clamp(GetDot(delta, line) / length2, 0.0, 1.0);
                for (std::size_t a = 0; a < 3; ++a) delta[a] -= t * line[a];
                const double distance = std::sqrt(GetDot(delta, delta));
                if (!std::isfinite(distance)) {
                    SetFailure(PartFailureReason::InvalidEdit, "Brush distance overflows.");
                }
                if (distance <= op.radiusMM) {
                    SetVoxel(i, op.isErase ? 0 : target);
                }
            });
        }
    }

    template<class Predicate>
    std::vector<std::size_t> BuildRegion(const std::vector<std::size_t>& seeds,
        const Predicate& predicate, std::vector<std::uint64_t>& visited)
    {
        std::vector<std::size_t> queue;
        const auto wasVisited = [&](std::size_t i) { return (visited[i/64] & (std::uint64_t{1} << (i%64))) != 0; };
        const auto append = [&](std::size_t i) {
            if (queue.size() == queue.capacity()) {
                if (queue.size() >= m_queueLimit) SetFailure(PartFailureReason::BudgetExceeded, "Edit region exceeds the queue budget.");
                const auto capacity = std::min(m_queueLimit, std::max<std::size_t>(64, queue.capacity() > m_queueLimit/2 ? m_queueLimit : queue.capacity()*2));
                m_requiredBytes = std::max(m_requiredBytes, m_fixedBytes + capacity*3*sizeof(std::size_t));
                queue.reserve(capacity);
            }
            visited[i/64] |= std::uint64_t{1} << (i%64); queue.push_back(i);
        };
        for (const auto seed : seeds) {
            if (!GetEditable(seed) || !predicate(seed)) {
                SetFailure(PartFailureReason::ConstraintConflict, "Seed is not in the editable region.");
            }
            if (!wasVisited(seed)) append(seed);
        }
        for (std::size_t head = 0; head < queue.size(); ++head) {
            CheckStop(head);
            SendNeighbors(queue[head], [&](std::size_t next, std::size_t) {
                if (!wasVisited(next) && GetEditable(next) && predicate(next)) append(next);
            });
        }
        return queue;
    }

    void SetOperation(const PartFillEdit& op)
    {
        const auto target = GetWritableLabel(op.target);
        const auto seed = GetOffset(op.seed);
        const auto source = (*m_labels)[seed];
        std::vector<std::uint64_t> visited(m_count/64 + (m_count%64 != 0), 0);
        auto region = BuildRegion({ seed }, [&](std::size_t i) {
            return (*m_labels)[i] == source;
        }, visited);
        for (std::size_t n = 0; n < region.size(); ++n) { CheckStop(n); SetVoxel(region[n], target); }
    }

    void SetOperation(const PartGrowEdit& op)
    {
        const auto target = GetWritableLabel(op.target);
        if (op.seeds.empty() || !std::isfinite(op.minimum)
            || !std::isfinite(op.maximum) || op.minimum > op.maximum) {
            SetFailure(PartFailureReason::InvalidEdit, "Invalid growth seeds or gray interval.");
        }
        const auto allowed = GetAllowed(target, op.overwriteParts, op.isBackgroundAllowed);
        std::vector<std::size_t> seeds;
        seeds.reserve(op.seeds.size());
        for (std::size_t i = 0; i < op.seeds.size(); ++i) { CheckStop(i); seeds.push_back(GetOffset(op.seeds[i])); }
        std::vector<std::uint64_t> visited(m_count/64 + (m_count%64 != 0), 0);
        auto region = BuildRegion(seeds, [&](std::size_t i) {
            return allowed[(*m_labels)[i]]
                && GetGrayInRange(m_input.volume.values, i, op.minimum, op.maximum);
        }, visited);
        for (std::size_t n = 0; n < region.size(); ++n) { CheckStop(n); SetVoxel(region[n], target); }
    }

    void SetOperation(const PartIslandEdit& op)
    {
        const auto target = GetLabel(op.target);
        if (op.minIslandVoxels == 0) SetFailure(PartFailureReason::InvalidEdit, "Island minimum is zero.");
        std::vector<std::uint64_t> visited(m_count/64 + (m_count%64 != 0), 0);
        SendExtentVoxels(GetPartsExtent({target}), [&](std::size_t seed) {
            if ((visited[seed/64] & (std::uint64_t{1} << (seed%64))) || !GetEditable(seed) || (*m_labels)[seed] != target) return;
            auto region = BuildRegion({ seed }, [&](std::size_t i) {
                return (*m_labels)[i] == target;
            }, visited);
            bool isTruncated = false;
            for (std::size_t n = 0; n < region.size(); ++n) {
                CheckStop(n);
                SendNeighbors(region[n], [&](std::size_t next, std::size_t) {
                    // 同标签的连通性被作用域/保护截断时，不能清除局部碎片。
                    if ((*m_labels)[next] == target && !GetEditable(next)) isTruncated = true;
                });
            }
            if (!isTruncated && region.size() < op.minIslandVoxels) {
                for (std::size_t n = 0; n < region.size(); ++n) { CheckStop(n); SetVoxel(region[n], 0); }
            }
        });
    }

    void SetWholeParts(const std::vector<PartLabelId>& parts) const
    {
        std::vector<bool> selected(m_old->partsByLabel.size(), false);
        std::vector<std::uint64_t> counts(selected.size(), 0);
        for (const auto part : parts) selected[part] = true;
        SendExtentVoxels(GetPartsExtent(parts), [&](std::size_t i) {
            if (selected[(*m_labels)[i]]) ++counts[(*m_labels)[i]];
            if (selected[(*m_labels)[i]] && !GetEditable(i)) {
                SetFailure(PartFailureReason::ConstraintConflict,
                    "Split/merge requires every source voxel to be editable and inside the scope.");
            }
        });
        for (const auto label : parts) if (counts[label] != m_counts[label])
            SetFailure(PartFailureReason::InvalidGeometry, "Part bounds do not contain all source voxels.");
    }

    void SetOperation(const PartMergeEdit& op)
    {
        for (std::size_t i = 0; i < op.parts.size(); ++i) { CheckStop(i); m_sources.push_back(GetLabel(op.parts[i])); }
        std::sort(m_sources.begin(), m_sources.end());
        if (m_sources.size() < 2
            || std::adjacent_find(m_sources.begin(), m_sources.end()) != m_sources.end()) {
            SetFailure(PartFailureReason::InvalidEdit, "Merge needs distinct source parts.");
        }
        SetWholeParts(m_sources);
        const auto label = static_cast<PartLabelId>(m_old->partsByLabel.size());
        m_newCount = 1;
        SendExtentVoxels(GetPartsExtent(m_sources), [&](std::size_t i) {
            if (std::binary_search(m_sources.begin(), m_sources.end(), (*m_labels)[i])) SetVoxel(i, label);
        });
    }

    void SetOperation(const PartSplitEdit& op)
    {
        const auto parent = GetLabel(op.target);
        m_sources = { parent };
        SetWholeParts(m_sources);
        std::vector<bool> targets(maxParts + 1, false);
        std::vector<std::uint32_t> owner(m_splitCount, 0);
        std::vector<std::uint8_t> fixed(m_splitCount, 0), barriers(m_splitCount, 0);
        std::vector<double> distance(m_splitCount, std::numeric_limits<double>::infinity());
        for (std::size_t seedIndex = 0; seedIndex < op.seeds.size(); ++seedIndex) {
            CheckStop(seedIndex);
            const auto& seed = op.seeds[seedIndex];
            const auto source = GetOffset(seed.imageIndex);
            const auto i = GetSplitOffset(source);
            if (seed.target == 0 || seed.target > maxParts) {
                SetFailure(PartFailureReason::InvalidEdit, "Split target is outside 1..4096.");
            }
            if (i == noIndex || (*m_labels)[source] != parent || !GetEditable(source)
                || (owner[i] != 0 && owner[i] != seed.target)) {
                SetFailure(PartFailureReason::ConstraintConflict, "Split seeds conflict or lie outside the parent.");
            }
            targets[seed.target] = true;
            m_newCount = std::max(m_newCount, static_cast<std::size_t>(seed.target));
            owner[i] = seed.target;
            distance[i] = 0;
            fixed[i] = 1;
        }
        if (m_newCount < 2
            || !std::all_of(targets.begin() + 1, targets.begin() + m_newCount + 1,
                [](bool value) { return value; })) {
            SetFailure(PartFailureReason::InvalidEdit, "Split targets must cover contiguous 1..N, N>=2.");
        }
        if (m_old->partsByLabel.size() - 2 + m_newCount > maxParts) {
            SetFailure(PartFailureReason::BudgetExceeded, "Split result exceeds the 4096 part limit.");
        }
        for (std::size_t edgeIndex = 0; edgeIndex < op.barriers.size(); ++edgeIndex) {
            CheckStop(edgeIndex);
            const auto& edge = op.barriers[edgeIndex];
            const auto i = GetSplitOffset(GetOffset(edge.imageIndex));
            if (edge.axis > 2 || edge.imageIndex[edge.axis] >= m_geometry.extent[edge.axis * 2 + 1]) {
                SetFailure(PartFailureReason::InvalidEdit, "Split barrier is not a grid adjacency edge.");
            }
            if (i != noIndex) barriers[i] |= static_cast<std::uint8_t>(1U << edge.axis);
        }
        // 索引堆每体素最多一个节点，decrease-key 保证队列内存不随松弛次数增长。
        std::vector<std::size_t> heap;
        heap.reserve(m_splitCount);
        std::vector<std::size_t> position(m_splitCount, noIndex);
        const auto less = [&](std::size_t a, std::size_t b) {
            if (distance[a] != distance[b]) return distance[a] < distance[b];
            if (owner[a] != owner[b]) return owner[a] < owner[b];
            return a < b;
        };
        const auto exchange = [&](std::size_t a, std::size_t b) {
            std::swap(heap[a], heap[b]); position[heap[a]] = a; position[heap[b]] = b;
        };
        const auto raise = [&](std::size_t node) {
            if (position[node] == noIndex) { position[node] = heap.size(); heap.push_back(node); }
            auto p = position[node];
            while (p > 0 && less(heap[p], heap[(p - 1) / 2])) {
                const auto parentPos = (p - 1) / 2; exchange(p, parentPos); p = parentPos;
            }
        };
        for (std::size_t i = 0; i < m_splitCount; ++i) { CheckStop(i); if (fixed[i]) raise(i); }
        std::size_t processed = 0;
        while (!heap.empty()) {
            CheckStop(processed++);
            const auto current = heap.front();
            exchange(0, heap.size() - 1); heap.pop_back(); position[current] = noIndex;
            std::size_t p = 0;
            while (p < heap.size() / 2) {
                auto child = p * 2 + 1;
                if (child + 1 < heap.size() && less(heap[child + 1], heap[child])) ++child;
                if (!less(heap[child], heap[p])) break;
                exchange(child, p); p = child;
            }
            SendSplitNeighbors(current, [&](std::size_t next, std::size_t axis) {
                if ((*m_labels)[GetSplitSource(next)] != parent || fixed[next]
                    || (barriers[std::min(current, next)] & (1U << axis))) return;
                const double candidate = distance[current] + m_edgeLength[axis];
                if (!std::isfinite(candidate)) SetFailure(PartFailureReason::InvalidGeometry, "Split distance overflows.");
                if (candidate < distance[next]
                    || (candidate == distance[next] && owner[current] < owner[next])) {
                    distance[next] = candidate; owner[next] = owner[current]; raise(next);
                }
            });
        }
        const auto first = static_cast<PartLabelId>(m_old->partsByLabel.size() - 1);
        for (std::size_t i = 0; i < m_splitCount; ++i) {
            CheckStop(i);
            const auto source = GetSplitSource(i);
            if ((*m_labels)[source] != parent) continue;
            if (owner[i] == 0) SetFailure(PartFailureReason::UnassignedVoxels, "A parent component has no reachable seed.");
            SetVoxel(source, first + owner[i]);
        }
    }

    void SetOperation(const PartHistoryEdit&)
    {
        SetFailure(PartFailureReason::InvalidEdit, "History must use the restore path.");
    }

    std::shared_ptr<const PartCatalog> BuildCatalog()
    {
        const auto oldSize = m_old->partsByLabel.size();
        std::vector<PartLabelId> mapping(oldSize + m_newCount, 0);
        m_counts.resize(mapping.size(), 0);
        PartLabelId count = 0;
        for (std::size_t label = 1; label < mapping.size(); ++label) {
            if (m_counts[label] != 0) mapping[label] = ++count;
        }
        if (count > maxParts) SetFailure(PartFailureReason::BudgetExceeded, "Edited part count exceeds 4096.");
        std::vector<PartMetrics> metrics(mapping.size());
        for (std::size_t label = 1; label < mapping.size(); ++label) {
            if (!mapping[label]) continue;
            if (label < oldSize && !m_changed[label]) { metrics[label] = m_old->partsByLabel[label].metrics; continue; }
            auto extent = *m_changedExtent;
            if (label < oldSize) for (std::size_t a = 0; a < 3; ++a) {
                extent[a*2] = std::min(extent[a*2], m_old->partsByLabel[label].metrics.voxelExtent[a*2]);
                extent[a*2+1] = std::max(extent[a*2+1], m_old->partsByLabel[label].metrics.voxelExtent[a*2+1]);
            }
            const auto updated = ClassicalPartSegmenter::BuildPartMetrics(m_input.volume, *m_labels,
                static_cast<PartLabelId>(label), extent, m_stop);
            if (!updated || updated->voxelCount != m_counts[label]) {
                CheckStop(0); SetFailure(PartFailureReason::InvalidGeometry, "Edited part metrics do not cover its voxel ownership.");
            }
            metrics[label] = *updated;
        }
        bool remapped = false;
        for (std::size_t label = 1; label < mapping.size(); ++label) if (m_counts[label] && mapping[label] != label) remapped = true;
        if (remapped) for (std::size_t i = 0; i < m_count; ++i) { CheckStop(i); (*m_labels)[i] = mapping[(*m_labels)[i]]; }
        std::vector<std::uint64_t> counts(static_cast<std::size_t>(count)+1, 0); counts[0] = m_counts[0];
        for (std::size_t label = 1; label < mapping.size(); ++label) if (mapping[label]) counts[mapping[label]] = m_counts[label];
        m_counts = std::move(counts);
        auto catalog = std::make_shared<PartCatalog>();
        catalog->partSetId = m_old->partSetId;
        catalog->resultRevision = m_old->resultRevision + 1;
        catalog->catalogRevision = m_old->catalogRevision + 1;
        catalog->partsByLabel.resize(static_cast<std::size_t>(count) + 1);
        for (std::size_t label = 1; label < mapping.size(); ++label) {
            CheckStop(label);
            if (mapping[label] == 0) continue;
            auto& entry = catalog->partsByLabel[mapping[label]];
            if (label < oldSize) {
                entry = m_old->partsByLabel[label];
                if (m_changed[label]) { entry.userState.isReviewed = false; entry.metrics.confidence.reset(); entry.isEdited = true; }
            }
            else {
                const auto id = m_ids.CreatePartObjectId([&](const PartObjectId& value) {
                    return m_old->labelByObject.count(value) || catalog->labelByObject.count(value);
                });
                if (!id) SetFailure(PartFailureReason::InternalError, "Unable to create part identity.");
                entry.objectId = *id;
                entry.presentation.color = GetPartStableColor(*id);
                entry.isEdited = true;
            }
            const auto confidence = entry.metrics.confidence;
            entry.labelId = mapping[label];
            entry.metrics = metrics[label];
            entry.metrics.confidence = confidence;
            catalog->labelByObject.emplace(entry.objectId, entry.labelId);
            if (label < oldSize) {
                catalog->relationsFromPrevious.push_back({ GetBinding(*catalog, entry.objectId),
                    GetBinding(*m_old, entry.objectId), PartRelationKind::ContinuedFrom, m_changed[label] ? 0.0 : 1.0 });
            }
            else {
                for (const auto source : m_sources) {
                    const auto& previous = m_old->partsByLabel[source];
                    const double score = static_cast<double>(std::min(entry.metrics.voxelCount, previous.metrics.voxelCount))
                        / static_cast<double>(std::max(entry.metrics.voxelCount, previous.metrics.voxelCount));
                    catalog->relationsFromPrevious.push_back({ GetBinding(*catalog, entry.objectId),
                        GetBinding(*m_old, previous.objectId), m_sources.size() == 1
                            ? PartRelationKind::SplitFrom : PartRelationKind::MergedFrom, score });
                }
            }
        }
        for (std::size_t label = 1; label < oldSize; ++label) {
            if (mapping[label] == 0) catalog->retiredFromPrevious.push_back(GetBinding(*m_old, m_old->partsByLabel[label].objectId));
        }
        return catalog;
    }

    const PartEditInput& m_input;
    PartIdentityFactory& m_ids;
    const std::function<bool()>& m_stop;
    std::shared_ptr<const PartCatalog> m_old;
    GridGeometry3D m_geometry;
    std::array<int, 6> m_extent{};
    std::array<std::size_t, 3> m_stride{};
    std::array<double, 3> m_edgeLength{};
    std::size_t m_count = 0, m_requiredBytes = 0, m_newCount = 0;
    std::size_t m_fixedBytes = 0, m_queueLimit = 0, m_splitCount = 0;
    std::array<int,6> m_splitExtent{};
    std::array<std::size_t,3> m_splitDimensions{}, m_splitStride{};
    std::optional<std::array<int,6>> m_changedExtent;
    std::shared_ptr<std::vector<PartLabelId>> m_labels;
    std::vector<std::uint64_t> m_counts;
    std::vector<bool> m_locked, m_changed;
    std::vector<PartLabelId> m_sources;
};

} // namespace

std::optional<std::size_t> GetPartEditBytes(const PartEditRequest& request)
{
    constexpr std::size_t itemLimit = 1024U * 1024U;
    constexpr std::size_t byteLimit = 32U * 1024U * 1024U;
    std::size_t bytes = sizeof(PartEditRequest);
    bool isValid = true;
    const auto add = [&](const auto& values) {
        using Item = typename std::decay_t<decltype(values)>::value_type;
        if (values.size() > itemLimit || values.capacity() > (byteLimit - bytes) / sizeof(Item)) {
            isValid = false;
        }
        else bytes += values.capacity() * sizeof(Item);
    };
    add(request.scope.protectedParts);
    std::visit([&](const auto& op) {
        using Op = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<Op, PartBrushEdit>) { add(op.sourcePoints); add(op.overwriteParts); }
        else if constexpr (std::is_same_v<Op, PartGrowEdit>) { add(op.seeds); add(op.overwriteParts); }
        else if constexpr (std::is_same_v<Op, PartSplitEdit>) { add(op.seeds); add(op.barriers); }
        else if constexpr (std::is_same_v<Op, PartMergeEdit>) add(op.parts);
    }, request.operation);
    return isValid ? std::optional<std::size_t>{ bytes } : std::nullopt;
}

PartEditBuildResult PartLabelEditor::BuildLabels(const PartEditInput& input,
    PartIdentityFactory& identities, const std::function<bool()>& getStopRequested)
{
    EditBuilder builder(input, identities, getStopRequested);
    try { return builder.Build(); }
    catch (const EditFailure& error) { return { error.reason, error.what(), builder.GetRequiredBytes(), {}, {} }; }
    catch (const std::bad_alloc&) { return { PartFailureReason::BudgetExceeded, "Edit allocation failed.", builder.GetRequiredBytes(), {}, {} }; }
    catch (...) { return { PartFailureReason::InternalError, "Edit failed unexpectedly.", builder.GetRequiredBytes(), {}, {} }; }
}

PartEditBuildResult PartLabelEditor::BuildRestore(const PartHistorySnapshot& current,
    const PartHistorySnapshot& restored, std::size_t maxWorkingBytes,
    const std::function<bool()>& getStopRequested)
{
    try {
        if (!current.catalog || !current.labels || !restored.catalog || !restored.labels
            || current.catalog->partSetId != restored.catalog->partSetId
            || current.catalog->resultRevision == std::numeric_limits<std::uint64_t>::max()
            || current.catalog->catalogRevision == std::numeric_limits<std::uint64_t>::max()
            || restored.catalog->resultRevision > current.catalog->resultRevision
            || restored.labels->size() != current.labels->size()) {
            return { PartFailureReason::RevisionConflict, "History does not match the current part set." };
        }
        std::size_t bytes = 0;
        if (!GetPartCatalogStorageBytes(*restored.catalog, bytes)
            || bytes > maxWorkingBytes / 4U) {
            return { PartFailureReason::BudgetExceeded, "History catalog budget exceeded." };
        }
        std::vector<std::uint64_t> counts;
        if (!GetPartCatalogValid(*restored.catalog, *restored.labels, getStopRequested, &counts)) {
            return { getStopRequested && getStopRequested() ? PartFailureReason::Cancelled
                : PartFailureReason::InvalidEdit, "History snapshot is invalid." };
        }
        auto catalog = std::make_shared<PartCatalog>(*restored.catalog);
        catalog->resultRevision = current.catalog->resultRevision + 1;
        catalog->catalogRevision = current.catalog->catalogRevision + 1;
        catalog->relationsFromPrevious.clear();
        catalog->retiredFromPrevious.clear();
        for (std::size_t i = 1; i < catalog->partsByLabel.size(); ++i) {
            const auto id = catalog->partsByLabel[i].objectId;
            catalog->relationsFromPrevious.push_back({ GetBinding(*catalog, id),
                GetBinding(*restored.catalog, id), PartRelationKind::ContinuedFrom, 1.0 });
        }
        for (std::size_t i = 1; i < current.catalog->partsByLabel.size(); ++i) {
            const auto id = current.catalog->partsByLabel[i].objectId;
            if (!catalog->labelByObject.count(id)) catalog->retiredFromPrevious.push_back(GetBinding(*current.catalog, id));
        }
        if (!GetPartCatalogCountsValid(*catalog, counts, getStopRequested)) {
            return { getStopRequested && getStopRequested() ? PartFailureReason::Cancelled
                : PartFailureReason::InvalidEdit, "Restored catalog is invalid." };
        }
        return { PartFailureReason::None, "History candidate is ready.", bytes * 4U,
            restored.labels, std::move(catalog) };
    }
    catch (const std::bad_alloc&) { return { PartFailureReason::BudgetExceeded, "History allocation failed." }; }
    catch (...) { return { PartFailureReason::InternalError, "History restore failed." }; }
}
