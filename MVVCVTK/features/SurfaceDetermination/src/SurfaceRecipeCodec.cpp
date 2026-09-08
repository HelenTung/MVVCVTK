#include "SurfaceRecipe.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace
{
template <class T> void WriteValue(std::ostream &stream, const std::optional<T> &value)
{
    stream << bool(value) << ' ';
    if (value)
        stream << *value << ' ';
}
template <class T> bool ReadValue(std::istream &stream, std::optional<T> &value)
{
    int present = 0;
    if (!(stream >> present) || (present != 0 && present != 1))
        return false;
    value.reset();
    if (present)
    {
        T next{};
        if (!(stream >> next))
            return false;
        value = next;
    }
    return true;
}
template <std::size_t N>
void WriteArray(std::ostream &stream, const std::optional<std::array<double, N>> &value)
{
    stream << bool(value) << ' ';
    if (value)
        for (const auto item : *value)
            stream << item << ' ';
}
template <std::size_t N> bool ReadArray(std::istream &stream, std::optional<std::array<double, N>> &value)
{
    int present = 0;
    if (!(stream >> present) || (present != 0 && present != 1))
        return false;
    value.reset();
    if (present)
    {
        std::array<double, N> next{};
        for (auto &item : next)
            if (!(stream >> item))
                return false;
        value = next;
    }
    return true;
}
bool GetBoundsValid(const std::array<double, 6> &bounds)
{
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(bounds[axis * 2]) || !std::isfinite(bounds[axis * 2 + 1]) ||
            bounds[axis * 2] >= bounds[axis * 2 + 1])
            return false;
    return true;
}
bool GetOverlap(const std::array<double, 6> &a, const std::array<double, 6> &b)
{
    for (unsigned axis = 0; axis < 3; ++axis)
        if (a[axis * 2] >= b[axis * 2 + 1] || b[axis * 2] >= a[axis * 2 + 1])
            return false;
    return true;
}
bool GetPositive(const std::optional<double> &value, const bool hasZero = false)
{
    return !value || (std::isfinite(*value) && (hasZero ? *value >= 0 : *value > 0));
}
void WriteRecipe(std::ostream &out, const SurfaceRecipe &p)
{
    out << unsigned(p.method) << ' ' << unsigned(p.componentSelection) << ' ';
    WriteValue(out, p.initialIsoValue);
    WriteArray(out, p.seedModelPoint);
    WriteValue(out, p.profileHalfLengthModel);
    WriteValue(out, p.profileSampleStepModel);
    WriteValue(out, p.maximumOffsetModel);
    WriteValue(out, p.profileSmoothingSigmaModel);
    out << p.minimumObjectVoxels << ' ' << p.minimumContrast << ' ' << p.seedFraction << ' '
        << p.localFraction << ' ' << bool(p.grayPair) << ' ';
    if (p.grayPair)
        for (auto range : {p.grayPair->sideA, p.grayPair->sideB})
            for (auto value : range)
                out << value << ' ';
    out << p.minimumCnr << ' ' << p.maximumPlateauNoiseRatio << ' ' << p.maximumNormalizedResidual << ' ';
    WriteValue(out, p.minimumEdgeWidthModel);
    WriteValue(out, p.maximumEdgeWidthModel);
    WriteValue(out, p.minimumEdgeSeparationModel);
    out << p.sharpCornerAngleDeg << ' ' << p.maximumNormalTurnDeg << ' ' << p.materialPairs.size() << '\n';
    for (const auto &pair : p.materialPairs)
        out << pair.materialA << ' ' << pair.materialB << '\n';
    out << p.regionOverrides.size() << '\n';
    for (const auto &rule : p.regionOverrides)
    {
        out << std::quoted(rule.id) << ' ' << rule.priority << ' ';
        for (auto value : rule.boundsModel)
            out << value << ' ';
        out << bool(rule.method) << ' ';
        if (rule.method)
            out << unsigned(*rule.method) << ' ';
        WriteValue(out, rule.localFraction);
        WriteValue(out, rule.profileHalfLengthModel);
        WriteValue(out, rule.profileSampleStepModel);
        WriteValue(out, rule.maximumOffsetModel);
        WriteValue(out, rule.profileSmoothingSigmaModel);
        WriteValue(out, rule.minimumContrast);
        WriteValue(out, rule.minimumCnr);
        out << '\n';
    }
}
bool ReadRecipe(std::istream &in, SurfaceRecipe &p)
{
    unsigned method = 0, selection = 0;
    int gray = 0;
    if (!(in >> method >> selection) || method > 6 || selection > 2)
        return false;
    p.method = static_cast<SurfaceDeterminationMethod>(method);
    p.componentSelection = static_cast<SurfaceComponentSelection>(selection);
    if (!ReadValue(in, p.initialIsoValue) || !ReadArray(in, p.seedModelPoint) ||
        !ReadValue(in, p.profileHalfLengthModel) ||
        !ReadValue(in, p.profileSampleStepModel) || !ReadValue(in, p.maximumOffsetModel) ||
        !ReadValue(in, p.profileSmoothingSigmaModel) ||
        !(in >> p.minimumObjectVoxels >> p.minimumContrast >> p.seedFraction >> p.localFraction >> gray) ||
        (gray != 0 && gray != 1))
        return false;
    if (gray)
    {
        SurfaceGrayPair pair;
        if (!(in >> pair.sideA[0] >> pair.sideA[1] >> pair.sideB[0] >> pair.sideB[1]))
            return false;
        p.grayPair = pair;
    }
    if (!(in >> p.minimumCnr >> p.maximumPlateauNoiseRatio >> p.maximumNormalizedResidual) ||
        !ReadValue(in, p.minimumEdgeWidthModel) || !ReadValue(in, p.maximumEdgeWidthModel) ||
        !ReadValue(in, p.minimumEdgeSeparationModel) ||
        !(in >> p.sharpCornerAngleDeg >> p.maximumNormalTurnDeg))
        return false;
    std::size_t count = 0;
    if (!(in >> count) || count > 32)
        return false;
    p.materialPairs.resize(count);
    for (auto &pair : p.materialPairs)
    {
        std::uint64_t a = 0, b = 0;
        if (!(in >> a >> b) || a > UINT32_MAX || b > UINT32_MAX)
            return false;
        pair = {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)};
    }
    if (!(in >> count) || count > 64)
        return false;
    p.regionOverrides.resize(count);
    for (auto &rule : p.regionOverrides)
    {
        if (!(in >> std::quoted(rule.id) >> rule.priority))
            return false;
        for (auto &value : rule.boundsModel)
            if (!(in >> value))
                return false;
        int present = 0;
        if (!(in >> present) || (present != 0 && present != 1))
            return false;
        if (present)
        {
            if (!(in >> method) || method > 6)
                return false;
            rule.method = static_cast<SurfaceDeterminationMethod>(method);
        }
        if (!ReadValue(in, rule.localFraction) || !ReadValue(in, rule.profileHalfLengthModel) ||
            !ReadValue(in, rule.profileSampleStepModel) || !ReadValue(in, rule.maximumOffsetModel) ||
            !ReadValue(in, rule.profileSmoothingSigmaModel) || !ReadValue(in, rule.minimumContrast) ||
            !ReadValue(in, rule.minimumCnr))
            return false;
    }
    return true;
}
} // namespace

std::string SurfaceRecipeCodec::GetError(const SurfaceRecipe &p)
{
    if (unsigned(p.method) > 6 || unsigned(p.componentSelection) > 2)
        return "Unsupported method or component selection.";
    if (!p.minimumObjectVoxels || !std::isfinite(p.minimumContrast) || p.minimumContrast < 0 ||
        !std::isfinite(p.minimumCnr) || p.minimumCnr < 0)
        return "Invalid count, contrast or CNR.";
    if (!std::isfinite(p.seedFraction) || p.seedFraction <= 0 || p.seedFraction >= 1 ||
        !std::isfinite(p.localFraction) || p.localFraction <= 0 || p.localFraction >= 1)
        return "Relative fractions must be inside (0,1).";
    if (p.method == SurfaceDeterminationMethod::LocalAdaptiveIso50 && p.localFraction != 0.5)
        return "ISO50 requires a local fraction of 0.5.";
    if ((p.initialIsoValue && !std::isfinite(*p.initialIsoValue)) || !GetPositive(p.profileHalfLengthModel) ||
        !GetPositive(p.profileSampleStepModel) || !GetPositive(p.maximumOffsetModel, true) ||
        !GetPositive(p.profileSmoothingSigmaModel, true) || !GetPositive(p.minimumEdgeWidthModel) ||
        !GetPositive(p.maximumEdgeWidthModel) || !GetPositive(p.minimumEdgeSeparationModel))
        return "Invalid physical profile scale.";
    if ((p.profileHalfLengthModel && p.maximumOffsetModel &&
         *p.maximumOffsetModel > *p.profileHalfLengthModel) ||
        (p.profileHalfLengthModel && p.profileSampleStepModel &&
         2 * *p.profileHalfLengthModel / *p.profileSampleStepModel > 4096) ||
        (p.minimumEdgeWidthModel && p.maximumEdgeWidthModel &&
         *p.minimumEdgeWidthModel > *p.maximumEdgeWidthModel))
        return "Inconsistent profile bounds.";
    if (!std::isfinite(p.maximumPlateauNoiseRatio) || p.maximumPlateauNoiseRatio <= 0 ||
        !std::isfinite(p.maximumNormalizedResidual) || p.maximumNormalizedResidual <= 0 ||
        !std::isfinite(p.sharpCornerAngleDeg) || p.sharpCornerAngleDeg <= 0 || p.sharpCornerAngleDeg > 180 ||
        !std::isfinite(p.maximumNormalTurnDeg) || p.maximumNormalTurnDeg <= 0 || p.maximumNormalTurnDeg > 180)
        return "Invalid quality or normal-turn limit.";
    if (p.componentSelection == SurfaceComponentSelection::Seeded && !p.seedModelPoint)
        return "Seeded selection needs a model point.";
    if (p.seedModelPoint && !std::all_of(p.seedModelPoint->begin(), p.seedModelPoint->end(),
                                         [](double x) { return std::isfinite(x); }))
        return "Non-finite seed point.";
    if (p.grayPair)
    {
        const auto a = p.grayPair->sideA, b = p.grayPair->sideB;
        if (!std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(b[0]) || !std::isfinite(b[1]) ||
            a[0] > a[1] || b[0] > b[1] || !(a[1] < b[0] || b[1] < a[0]))
            return "Gray intervals must be finite and disjoint.";
    }
    if (p.materialPairs.size() > 32 || p.regionOverrides.size() > 64)
        return "Recipe collection limit exceeded.";
    if (!p.materialPairs.empty() && p.componentSelection != SurfaceComponentSelection::All)
        return "Material interfaces require All components.";
    std::set<std::pair<std::uint32_t, std::uint32_t>> pairs;
    for (const auto &pair : p.materialPairs)
        if (pair.materialA == pair.materialB ||
            !pairs.emplace(std::min(pair.materialA, pair.materialB), std::max(pair.materialA, pair.materialB))
                 .second)
            return "Material interfaces must be unique unordered pairs with a separately declared direction.";
    std::set<std::string> ids;
    for (std::size_t i = 0; i < p.regionOverrides.size(); ++i)
    {
        const auto &rule = p.regionOverrides[i];
        if (rule.id.empty() || rule.id.size() > 128 || rule.id.find('\0') != std::string::npos ||
            !ids.insert(rule.id).second || !GetBoundsValid(rule.boundsModel))
            return "Invalid or duplicate override identity/bounds.";
        for (std::size_t j = 0; j < i; ++j)
            if (p.regionOverrides[j].priority == rule.priority &&
                GetOverlap(rule.boundsModel, p.regionOverrides[j].boundsModel))
                return "Equal-priority regions overlap.";
        auto local = p;
        local.regionOverrides.clear();
        if (rule.method)
            local.method = *rule.method;
        if (rule.localFraction)
            local.localFraction = *rule.localFraction;
        if (rule.profileHalfLengthModel)
            local.profileHalfLengthModel = rule.profileHalfLengthModel;
        if (rule.profileSampleStepModel)
            local.profileSampleStepModel = rule.profileSampleStepModel;
        if (rule.maximumOffsetModel)
            local.maximumOffsetModel = rule.maximumOffsetModel;
        if (rule.profileSmoothingSigmaModel)
            local.profileSmoothingSigmaModel = rule.profileSmoothingSigmaModel;
        if (rule.minimumContrast)
            local.minimumContrast = *rule.minimumContrast;
        if (rule.minimumCnr)
            local.minimumCnr = *rule.minimumCnr;
        if (local.method == SurfaceDeterminationMethod::AutomaticIso50 ||
            local.method == SurfaceDeterminationMethod::GlobalIsoPreview)
            return "Region overrides select localization methods only.";
        if (const auto error = GetError(local); !error.empty())
            return error;
    }
    return {};
}

std::string SurfaceRecipeCodec::BuildText(const SurfaceRecipe &recipe)
{
    if (!GetError(recipe).empty())
        return {};
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << "surface-recipe 2\n";
    WriteRecipe(out, recipe);
    return out.str();
}

SurfaceRecipeReadResult SurfaceRecipeCodec::GetRecipe(const std::string_view text)
{
    if (text.size() > 1024U * 1024U)
        return {{}, "Recipe text limit exceeded."};
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string tag;
    unsigned version = 0;
    SurfaceRecipe recipe;
    if (!(in >> tag >> version) || tag != "surface-recipe" || version != 2)
        return {{}, "Unsupported recipe schema."};
    if (!ReadRecipe(in, recipe))
        return {{}, "Malformed recipe."};
    in >> std::ws;
    if (!in.eof())
        return {{}, "Trailing recipe data."};
    if (auto error = GetError(recipe); !error.empty())
        return {{}, std::move(error)};
    return {std::move(recipe), {}};
}
