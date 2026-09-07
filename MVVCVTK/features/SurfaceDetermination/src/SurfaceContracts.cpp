#include "SurfaceContracts.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace SurfaceContract {
namespace {
bool GetScopeValid(const std::string& scope)
{
    if (scope.size() > 128) return false;
    for (std::size_t i = 0; i < scope.size();) {
        const auto first = static_cast<unsigned char>(scope[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        const unsigned count = first >= 0xC2 && first <= 0xDF ? 1
            : first >= 0xE0 && first <= 0xEF ? 2
            : first >= 0xF0 && first <= 0xF4 ? 3 : 0;
        if (!count || scope.size() - i < count) return false;
        std::uint32_t code = first & (0x7F >> (count + 1));
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(scope[i++]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800)
            || (count == 3 && code < 0x10000) || code > 0x10FFFF
            || (code >= 0xD800 && code <= 0xDFFF)) return false;
    }
    return true;
}

template<class T> void WriteOptional(std::ostream& out, const std::optional<T>& value)
{
    out << bool(value) << ' ';
    if (value) out << *value << ' ';
}
template<class T> bool ReadOptional(std::istream& in, std::optional<T>& value)
{
    int present = 0;
    if (!(in >> present) || (present != 0 && present != 1)) return false;
    value.reset();
    if (present) { T item{}; if (!(in >> item)) return false; value = item; }
    return true;
}
template<std::size_t N> void WriteArray(std::ostream& out,
    const std::optional<std::array<double, N>>& value)
{
    out << bool(value) << ' ';
    if (value) for (const auto item : *value) out << item << ' ';
}
template<std::size_t N> bool ReadArray(std::istream& in,
    std::optional<std::array<double, N>>& value)
{
    int present = 0;
    if (!(in >> present) || (present != 0 && present != 1)) return false;
    value.reset();
    if (present) {
        std::array<double, N> items{};
        for (auto& item : items) if (!(in >> item) || !std::isfinite(item)) return false;
        value = items;
    }
    return true;
}
void WriteParams(std::ostream& out, const SurfaceDeterminationStartParams& p)
{
    out << unsigned(p.method) << ' ' << unsigned(p.componentSelection) << ' '
        << unsigned(GetPurpose(p)) << ' ' << unsigned(p.sourcePolicy) << ' '
        << std::quoted(p.resultScope) << ' ' << std::quoted(p.modelUnit) << ' ';
    // 源引用在图 inputs 和快照中保存；配方文本不重复序列化实体 ID。
    WriteOptional(out, p.initialIsoValue);
    WriteArray(out, p.seedModelPoint); WriteArray(out, p.roiModelBounds);
    WriteOptional(out, p.profileHalfLengthModel); WriteOptional(out, p.profileSampleStepModel);
    WriteOptional(out, p.maximumOffsetModel); WriteOptional(out, p.profileSmoothingSigmaModel);
    out << p.minimumObjectVoxels << ' ' << p.minimumContrast << '\n';
}
bool ReadParams(std::istream& in, SurfaceDeterminationStartParams& p)
{
    unsigned method, selection, purpose, policy;
    if (!(in >> method >> selection >> purpose >> policy
        >> std::quoted(p.resultScope) >> std::quoted(p.modelUnit))
        || method > 3 || selection > 2 || purpose > 2 || policy > 1) return false;
    p.method = static_cast<SurfaceDeterminationMethod>(method);
    p.componentSelection = static_cast<SurfaceComponentSelection>(selection);
    p.purpose = static_cast<SurfaceTaskPurpose>(purpose);
    p.sourcePolicy = static_cast<DataPublishPolicy>(policy);
    if (!ReadOptional(in, p.initialIsoValue) || !ReadArray(in, p.seedModelPoint)
        || !ReadArray(in, p.roiModelBounds) || !ReadOptional(in, p.profileHalfLengthModel)
        || !ReadOptional(in, p.profileSampleStepModel) || !ReadOptional(in, p.maximumOffsetModel)
        || !ReadOptional(in, p.profileSmoothingSigmaModel)
        || !(in >> p.minimumObjectVoxels >> p.minimumContrast)) return false;
    const auto positive = [](const std::optional<double>& x) { return !x || (std::isfinite(*x) && *x > 0.0); };
    if (!GetInputValid(p) || !p.minimumObjectVoxels || !std::isfinite(p.minimumContrast) || p.minimumContrast < 0.0
        || (p.initialIsoValue && !std::isfinite(*p.initialIsoValue))
        || !positive(p.profileHalfLengthModel) || !positive(p.profileSampleStepModel)
        || !positive(p.maximumOffsetModel) || !positive(p.profileSmoothingSigmaModel)
        || (p.componentSelection == SurfaceComponentSelection::Seeded && !p.seedModelPoint)) return false;
    if (p.roiModelBounds) for (unsigned axis=0;axis<3;++axis)
        if ((*p.roiModelBounds)[axis*2] >= (*p.roiModelBounds)[axis*2+1]) return false;
    return true;
}
}

SurfaceTaskPurpose GetPurpose(const SurfaceDeterminationStartParams& params)
{
    return params.purpose.value_or(params.method == SurfaceDeterminationMethod::AutomaticIso50
        ? SurfaceTaskPurpose::Estimate : params.method == SurfaceDeterminationMethod::GlobalIsoPreview
        ? SurfaceTaskPurpose::Preview : SurfaceTaskPurpose::Determine);
}

bool GetInputValid(const SurfaceDeterminationStartParams& p)
{
    const auto purpose = GetPurpose(p);
    if (purpose != SurfaceTaskPurpose::Estimate && purpose != SurfaceTaskPurpose::Preview
        && purpose != SurfaceTaskPurpose::Determine) return false;
    if (static_cast<unsigned>(p.method) > 3 || static_cast<unsigned>(p.componentSelection) > 2
        || static_cast<unsigned>(p.sourcePolicy) > 1 || !GetScopeValid(p.resultScope)
        || (p.sourceVolume && !GetDataRevisionRefValid(*p.sourceVolume))
        || (p.modelUnit != "" && p.modelUnit != "mm" && p.modelUnit != "cm"
            && p.modelUnit != "m" && p.modelUnit != "um")) return false;
    if ((purpose == SurfaceTaskPurpose::Estimate) != (p.method == SurfaceDeterminationMethod::AutomaticIso50)
        || (purpose == SurfaceTaskPurpose::Determine && p.method == SurfaceDeterminationMethod::GlobalIsoPreview)) return false;
    return true;
}

bool GetPointValid(const SurfacePointRecord& point, const SurfaceDeterminationMethod method)
{
    if (method == SurfaceDeterminationMethod::GlobalIsoPreview
        || method == SurfaceDeterminationMethod::AutomaticIso50
        || point.flags != SurfacePointFlags::None) return false;
    double normal2 = 0.0;
    for (const auto x : point.positionModel) if (!std::isfinite(x)) return false;
    for (const auto x : point.normalModel) normal2 += double(x) * x;
    return std::isfinite(normal2) && normal2 > 1e-24
        && std::isfinite(point.localThreshold) && std::isfinite(point.contrast)
        && std::isfinite(point.gradientMagnitude) && std::isfinite(point.offsetFromSeed)
        && std::isfinite(point.fitResidual) && point.fitResidual >= 0.0F
        && std::isfinite(point.validSupportRatio) && point.validSupportRatio > 0.0F
        && point.validSupportRatio <= 1.0F && std::isfinite(point.estimatedLocalizationSigma)
        && point.estimatedLocalizationSigma >= 0.0F;
}

std::string GetBindingName(const std::string_view scope)
{
    if (scope.empty()) return "analysis.surface-determination.active";
    if (scope.size() > 128 || !GetScopeValid(std::string(scope))) return {};
    std::string name = "analysis.surface-determination.scope.";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char value : scope) { name += hex[value >> 4]; name += hex[value & 15]; }
    return name + ".active";
}

bool GetSourceCurrent(const TrustedDataReadPort& data, const DataGraphSnapshot& graph,
    const DataRevisionRef source, const std::optional<DataBinding>& binding)
{
    if (!graph.view || !data.GetData(graph, source)) return false;
    DataQuery query; query.entityId = source.entityId;
    const auto result = data.GetDataQuery(graph, query);
    DataGeneration head = 0;
    for (const auto& item : result.data) if (item) head = std::max(head, item->self.generation);
    if (head != source.generation) return false;
    if (binding) {
        const auto current = data.GetDataBinding(graph, binding->name);
        if (!current || current->revision != binding->revision || current->target != binding->target) return false;
    }
    return true;
}

std::string BuildParameters(const SurfaceDeterminationStartParams& requested,
    const SurfaceDeterminationStartParams& resolved, const std::string& frame, const std::size_t workingBytes)
{
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "surface-parameters 1 " << std::quoted(frame) << ' ' << workingBytes << '\n';
    WriteParams(out, requested); WriteParams(out, resolved);
    return out.str();
}

bool GetParameters(const std::string& text, SurfaceDeterminationStartParams& requested,
    SurfaceDeterminationStartParams& resolved, std::string& frame, std::size_t& workingBytes)
{
    if (text.size() > 8192) return false;
    std::istringstream in(text); in.imbue(std::locale::classic());
    std::string tag, nextFrame; unsigned version = 0; std::size_t nextBytes = 0;
    SurfaceDeterminationStartParams nextRequested, nextResolved;
    if (!(in >> tag >> version >> std::quoted(nextFrame) >> nextBytes)
        || tag != "surface-parameters" || version != 1 || nextFrame.empty() || !nextBytes
        || !ReadParams(in, nextRequested) || !ReadParams(in, nextResolved)) return false;
    in >> std::ws; if (!in.eof()) return false;
    requested = std::move(nextRequested); resolved = std::move(nextResolved);
    frame = std::move(nextFrame); workingBytes = nextBytes;
    return true;
}
}
