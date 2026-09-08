#include "SurfaceProfileSolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
constexpr double epsilon = 1e-12;

double GetMedian(std::vector<double> &values)
{
    if (values.empty())
        return 0.0;
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return values.size() % 2 ? *middle : 0.5 * (*middle + *std::max_element(values.begin(), middle));
}
double GetNoise(const std::vector<double> &values, const double median, std::vector<double> &scratch)
{
    scratch.clear();
    for (auto value : values)
        scratch.push_back(std::abs(value - median));
    return 1.4826 * GetMedian(scratch);
}
void SetSmoothed(SurfaceProfileWorkspace &p, const double sigma)
{
    p.values.resize(p.raw.size());
    if (sigma <= epsilon)
    {
        std::copy(p.raw.begin(), p.raw.end(), p.values.begin());
        return;
    }
    const auto radius = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(p.raw.size() - 1), std::ceil(3 * sigma / p.step)));
    p.weights.resize(radius + 1);
    for (std::size_t i = 0; i <= radius; ++i)
        p.weights[i] = std::exp(-0.5 * std::pow(i * p.step / sigma, 2));
    for (std::size_t i = 0; i < p.raw.size(); ++i)
    {
        double sum = 0, weight = 0;
        const auto begin = i > radius ? i - radius : 0, end = std::min(p.raw.size() - 1, i + radius);
        for (auto j = begin; j <= end; ++j)
        {
            const auto w = p.weights[i > j ? i - j : j - i];
            sum += w * p.raw[j];
            weight += w;
        }
        p.values[i] = sum / weight;
    }
}
void SetCandidates(SurfaceProfileWorkspace &p)
{
    p.candidates.clear();
    p.derivatives.assign(p.values.size(), 0.0);
    double maximum = 0;
    for (std::size_t i = 1; i + 1 < p.values.size(); ++i)
    {
        p.derivatives[i] = (p.values[i + 1] - p.values[i - 1]) / (2 * p.step);
        maximum = std::max(maximum, std::abs(p.derivatives[i]));
    }
    if (maximum <= epsilon)
        return;
    // 平台状同高峰只记一次；候选保持物理位置顺序，避免浮点强度并列改变身份。
    for (std::size_t i = 2; i + 2 < p.values.size(); ++i)
    {
        const auto value = std::abs(p.derivatives[i]);
        if (value < maximum * 0.35 || value < std::abs(p.derivatives[i - 1]) ||
            value <= std::abs(p.derivatives[i + 1]))
            continue;
        std::size_t left = i, right = i;
        while (left > 0 && std::abs(p.derivatives[left]) > value * 0.5)
            --left;
        while (right + 1 < p.values.size() && std::abs(p.derivatives[right]) > value * 0.5)
            ++right;
        const double before = std::abs(p.derivatives[i - 1]), after = std::abs(p.derivatives[i + 1]);
        const double denominator = before - 2 * value + after;
        const double shift = std::abs(denominator) > epsilon
                                 ? std::clamp(0.5 * (before - after) / denominator, -0.5, 0.5)
                                 : 0.0;
        p.candidates.push_back(
            {p.offsets[i] + shift * p.step, p.derivatives[i], (right - left) * p.step, false});
    }
}

struct ModelFit final
{
    bool valid = false;
    double first = 0, second = 0, width = 0, baseline = 0, slope = 0, amplitude = 0;
    double residual = std::numeric_limits<double>::infinity();
};
ModelFit BuildModel(const SurfaceProfileWorkspace &p, const double first, const double second,
                    const double width, const bool paired)
{
    ModelFit fit;
    fit.first = first;
    fit.second = second;
    fit.width = width;
    if (!(width > 0) || (paired && !(second > first)))
        return fit;
    double system[3][4]{};
    const double scale = std::max(std::abs(p.offsets.front()), std::abs(p.offsets.back()));
    for (std::size_t i = 0; i < p.values.size(); ++i)
    {
        const auto t = p.offsets[i];
        const double transition = 0.5 * (1 + std::tanh((t - first) / width)) -
                                  (paired ? 0.5 * (1 + std::tanh((t - second) / width)) : 0.0);
        const std::array<double, 3> basis{1.0, t / scale, transition};
        for (unsigned r = 0; r < 3; ++r)
        {
            for (unsigned c = 0; c < 3; ++c)
                system[r][c] += basis[r] * basis[c];
            system[r][3] += basis[r] * p.values[i];
        }
    }
    for (unsigned c = 0; c < 3; ++c)
    {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 3; ++r)
            if (std::abs(system[r][c]) > std::abs(system[pivot][c]))
                pivot = r;
        if (std::abs(system[pivot][c]) < epsilon * p.values.size())
            return fit;
        for (unsigned k = 0; k < 4; ++k)
            std::swap(system[c][k], system[pivot][k]);
        const auto divisor = system[c][c];
        for (unsigned k = c; k < 4; ++k)
            system[c][k] /= divisor;
        for (unsigned r = 0; r < 3; ++r)
            if (r != c)
            {
                const auto factor = system[r][c];
                for (unsigned k = c; k < 4; ++k)
                    system[r][k] -= factor * system[c][k];
            }
    }
    fit.baseline = system[0][3];
    fit.slope = system[1][3];
    fit.amplitude = system[2][3];
    double error = 0;
    for (std::size_t i = 0; i < p.values.size(); ++i)
    {
        const auto t = p.offsets[i];
        const double transition = 0.5 * (1 + std::tanh((t - first) / width)) -
                                  (paired ? 0.5 * (1 + std::tanh((t - second) / width)) : 0.0);
        const double predicted = fit.baseline + fit.slope * t / scale + fit.amplitude * transition;
        error += std::pow(p.values[i] - predicted, 2);
    }
    fit.residual = std::sqrt(error / p.values.size());
    fit.valid = std::isfinite(fit.residual) && std::isfinite(fit.amplitude) &&
                std::abs(fit.amplitude) > epsilon && std::abs(fit.slope) <= 2 * std::abs(fit.amplitude);
    return fit;
}
ModelFit GetBestModel(const SurfaceProfileWorkspace &p, const SurfaceLocalParams &params, const double first,
                      const double second, const double width, const bool paired)
{
    auto best =
        BuildModel(p, first, second,
                   std::clamp(width, params.minimumEdgeWidthModel, params.maximumEdgeWidthModel), paired);
    double shift =
        std::max(p.step, std::min(params.maximumOffsetModel, params.profileHalfLengthModel) * 0.25);
    double widthStep = std::max(p.step, width * 0.5);
    for (unsigned level = 0; level < 10; ++level)
    {
        for (unsigned sweep = 0; sweep < 4; ++sweep)
        {
            bool improved = false;
            for (unsigned axis = 0; axis < (paired ? 3U : 2U); ++axis)
            {
                const auto current = best;
                for (int sign : {-1, 1})
                {
                    auto a = current.first, b = current.second, w = current.width;
                    if (axis == 0)
                        a += sign * shift;
                    else if (paired && axis == 1)
                        b += sign * shift;
                    else
                        w += sign * widthStep;
                    if (w < params.minimumEdgeWidthModel || w > params.maximumEdgeWidthModel ||
                        std::abs(a) > params.profileHalfLengthModel - p.step ||
                        (paired && (b <= a || std::abs(b) > params.profileHalfLengthModel - p.step)))
                        continue;
                    auto candidate = BuildModel(p, a, b, w, paired);
                    if (candidate.valid && (!best.valid || candidate.residual < best.residual))
                    {
                        best = candidate;
                        improved = true;
                    }
                }
            }
            if (!improved)
                break;
        }
        shift *= 0.5;
        widthStep *= 0.5;
    }
    return best;
}
} // namespace

void SurfaceProfileWorkspace::Reserve(const std::size_t count)
{
    for (auto *buffer : {&offsets, &raw, &values, &derivatives, &plateauA, &plateauB, &scratch, &weights})
        buffer->reserve(count);
    support.reserve(count);
    labels.reserve(count);
    candidates.reserve(count);
}

SurfaceProfileFit SurfaceProfileSolver::BuildFit(SurfaceProfileWorkspace &p, const SurfaceLocalParams &params)
{
    SurfaceProfileFit fit;
    fit.threshold = params.initialIsoValue;
    p.candidates.clear();
    p.values = p.raw;
    for (auto status : p.support)
    {
        if (status == SurfaceSampleStatus::Clipped)
            fit.flags |= SurfacePointFlags::ProfileClipped;
        else if (status == SurfaceSampleStatus::OtherMaterial)
            fit.flags |= SurfacePointFlags::MaterialJunction;
        else if (status == SurfaceSampleStatus::InvalidSupport)
            fit.flags |= SurfacePointFlags::InvalidSupport;
    }
    if (p.raw.size() < 9 || p.raw.size() > 4097 || p.offsets.size() != p.raw.size() ||
        p.support.size() != p.raw.size() || (!p.labels.empty() && p.labels.size() != p.raw.size()) ||
        !std::isfinite(p.step) || p.step <= 0 || !std::isfinite(params.profileSmoothingSigmaModel) ||
        params.profileSmoothingSigmaModel < 0 ||
        !std::all_of(p.raw.begin(), p.raw.end(), [](double v) { return std::isfinite(v); }))
        fit.flags |= SurfacePointFlags::FitRejected;
    if (fit.flags == SurfacePointFlags::None)
        for (std::size_t i = 0; i < p.offsets.size(); ++i)
            if (!std::isfinite(p.offsets[i]) ||
                (i && std::abs(p.offsets[i] - p.offsets[i - 1] - p.step) > p.step * 1e-8))
                fit.flags |= SurfacePointFlags::FitRejected;
    if (fit.flags != SurfacePointFlags::None)
        return fit;
    SetSmoothed(p, params.profileSmoothingSigmaModel);
    SetCandidates(p);
    const auto plateauCount = std::max<std::size_t>(2, p.raw.size() / 5);
    p.plateauA.assign(p.raw.begin(), p.raw.begin() + plateauCount);
    p.plateauB.assign(p.raw.end() - plateauCount, p.raw.end());
    fit.sideA = GetMedian(p.plateauA);
    fit.sideB = GetMedian(p.plateauB);
    const auto noiseA = GetNoise(p.plateauA, fit.sideA, p.scratch),
               noiseB = GetNoise(p.plateauB, fit.sideB, p.scratch);
    fit.noise = std::hypot(noiseA, noiseB) / std::sqrt(2.0);
    fit.contrast = std::abs(fit.sideA - fit.sideB);
    const bool paired = params.method == SurfaceDeterminationMethod::PairedEdgeModelFit;
    if (params.grayPair && !paired &&
        (fit.sideA < params.grayPair->sideA[0] || fit.sideA > params.grayPair->sideA[1] ||
         fit.sideB < params.grayPair->sideB[0] || fit.sideB > params.grayPair->sideB[1]))
        fit.flags |= SurfacePointFlags::DirectionMismatch;
    if (params.materials && !p.labels.empty() &&
        (p.labels.front() != params.materials->materialA ||
         (!paired && p.labels.back() != params.materials->materialB)))
        fit.flags |= SurfacePointFlags::DirectionMismatch;
    const double scale = std::max({1.0, std::abs(fit.sideA), std::abs(fit.sideB)});
    if (!paired && fit.contrast < std::max(params.minimumContrast, epsilon * scale))
        fit.flags |= SurfacePointFlags::LowContrast;
    if (!paired && fit.noise > fit.contrast * params.maximumPlateauNoiseRatio)
        fit.flags |= SurfacePointFlags::PlateauUnstable;
    if (!paired && p.candidates.size() > 1)
    {
        fit.flags |= SurfacePointFlags::MultipleCrossings | SurfacePointFlags::Unresolved;
        fit.crossingCount = static_cast<std::uint32_t>(p.candidates.size());
    }
    if (fit.flags != SurfacePointFlags::None)
        return fit;
    const double sign = fit.sideB < fit.sideA ? -1.0 : 1.0;
    fit.threshold = fit.sideA + params.localFraction * (fit.sideB - fit.sideA);

    if (params.method == SurfaceDeterminationMethod::LocalAdaptiveIso50 ||
        params.method == SurfaceDeterminationMethod::LocalRelativeIso)
    {
        p.candidates.clear();
        std::size_t selected = p.values.size();
        double distance = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i + 1 < p.values.size(); ++i)
        {
            const double left = p.values[i] - fit.threshold, right = p.values[i + 1] - fit.threshold;
            if (!((left >= 0 && right < 0) || (left <= 0 && right > 0)))
                continue;
            const double gradient = (p.values[i + 1] - p.values[i]) / p.step;
            if (std::abs(gradient) <= epsilon)
                continue;
            const double offset = p.offsets[i] - left / gradient;
            ++fit.crossingCount;
            p.candidates.push_back({offset, gradient, 0.0, false});
            if (gradient * sign > 0 && std::abs(offset) < distance)
            {
                distance = std::abs(offset);
                fit.offset = offset;
                selected = i;
            }
        }
        if (selected == p.values.size())
            fit.flags |= SurfacePointFlags::FitRejected;
        else
        {
            for (auto &candidate : p.candidates)
                candidate.isSelected = candidate.offsetModel == fit.offset;
            fit.gradient = std::abs(p.values[selected + 1] - p.values[selected]) / p.step;
            if (selected > 0 && selected + 2 < p.values.size())
            {
                const double slope = p.values[selected + 1] - p.values[selected];
                fit.residual = std::hypot(p.values[selected - 1] - (p.values[selected] - slope),
                                          p.values[selected + 2] - (p.values[selected + 1] + slope)) /
                               std::sqrt(2.0);
            }
        }
        if (fit.crossingCount > 1)
            fit.flags |= SurfacePointFlags::MultipleCrossings | SurfacePointFlags::Unresolved;
    }
    else
    {
        std::size_t selectedCount = 0, selectedIndex = 0;
        for (std::size_t i = 0; i < p.candidates.size(); ++i)
            if (std::abs(p.candidates[i].offsetModel) <= params.maximumOffsetModel + p.step)
            {
                ++selectedCount;
                selectedIndex = i;
            }
        fit.crossingCount = static_cast<std::uint32_t>(selectedCount);
        if (paired)
        {
            // 双边缘有一个共同模型；其他竞争峰或同向峰不能被静默忽略。
            if (p.candidates.size() != 2 || p.candidates[0].gradient * p.candidates[1].gradient >= 0)
            {
                fit.flags |= SurfacePointFlags::MultipleCrossings | SurfacePointFlags::Unresolved;
                return fit;
            }
            const auto model =
                GetBestModel(p, params, p.candidates[0].offsetModel, p.candidates[1].offsetModel,
                             (p.candidates[0].widthModel + p.candidates[1].widthModel) / 3.5255, true);
            if (!model.valid)
            {
                fit.flags |= SurfacePointFlags::FitRejected;
                return fit;
            }
            const auto selectedEdge = std::abs(model.first) <= std::abs(model.second) ? 0U : 1U;
            p.candidates[selectedEdge].isSelected = true;
            fit.offset = selectedEdge == 0 ? model.first : model.second;
            fit.width = model.width;
            fit.separation = model.second - model.first;
            fit.contrast = std::abs(model.amplitude);
            fit.residual = model.residual;
            fit.gradient = fit.contrast / (2 * model.width);
            const double profileScale = std::max(std::abs(p.offsets.front()), std::abs(p.offsets.back()));
            const double baseline = model.baseline + model.slope * fit.offset / profileScale;
            fit.threshold = baseline + model.amplitude * 0.5;
            fit.crossingCount = 2;
            const double signedAmplitude = model.amplitude * (selectedEdge == 0 ? 1.0 : -1.0);
            if (params.expectedDerivativeSign != 0 && signedAmplitude * params.expectedDerivativeSign <= 0)
                fit.flags |= SurfacePointFlags::DirectionMismatch;
            if (params.grayPair)
            {
                const double before = baseline + (selectedEdge == 0 ? 0 : model.amplitude);
                const double after = baseline + (selectedEdge == 0 ? model.amplitude : 0);
                if (before < params.grayPair->sideA[0] || before > params.grayPair->sideA[1] ||
                    after < params.grayPair->sideB[0] || after > params.grayPair->sideB[1])
                    fit.flags |= SurfacePointFlags::DirectionMismatch;
            }
            if (params.materials && !p.labels.empty())
            {
                std::size_t transition = p.labels.size();
                double distance = std::numeric_limits<double>::max();
                for (std::size_t i = 0; i + 1 < p.labels.size(); ++i)
                    if (p.labels[i] != p.labels[i + 1])
                    {
                        const double next = std::abs(.5 * (p.offsets[i] + p.offsets[i + 1]) - fit.offset);
                        if (next < distance)
                        {
                            distance = next;
                            transition = i;
                        }
                    }
                if (transition == p.labels.size() || p.labels[transition] != params.materials->materialA ||
                    p.labels[transition + 1] != params.materials->materialB)
                    fit.flags |= SurfacePointFlags::DirectionMismatch;
            }
            if (fit.separation < std::max(params.minimumEdgeSeparationModel, 2 * model.width))
                fit.flags |= SurfacePointFlags::Unresolved;
        }
        else if (selectedCount != 1)
            fit.flags |= SurfacePointFlags::MultipleCrossings | SurfacePointFlags::Unresolved;
        else
        {
            auto &edge = p.candidates[selectedIndex];
            edge.isSelected = true;
            if (edge.gradient * sign <= 0)
                fit.flags |= SurfacePointFlags::DirectionMismatch;
            fit.offset = edge.offsetModel;
            fit.gradient = std::abs(edge.gradient);
            fit.width = edge.widthModel / 1.76275;
            if (params.method == SurfaceDeterminationMethod::EdgeModelFit)
            {
                const auto model = GetBestModel(p, params, edge.offsetModel, 0, fit.width, false);
                if (!model.valid)
                    fit.flags |= SurfacePointFlags::FitRejected;
                else
                {
                    fit.offset = model.first;
                    fit.width = model.width;
                    fit.residual = model.residual;
                    fit.contrast = std::abs(model.amplitude);
                    fit.gradient = fit.contrast / (2 * model.width);
                    fit.threshold = model.baseline +
                                    model.slope * model.first /
                                        std::max(std::abs(p.offsets.front()), std::abs(p.offsets.back())) +
                                    model.amplitude * 0.5;
                    if (model.amplitude * sign <= 0)
                        fit.flags |= SurfacePointFlags::DirectionMismatch;
                }
            }
        }
    }
    if (fit.noise > fit.contrast * params.maximumPlateauNoiseRatio)
        fit.flags |= SurfacePointFlags::PlateauUnstable;
    if (fit.contrast < std::max(params.minimumContrast, epsilon * scale))
        fit.flags |= SurfacePointFlags::LowContrast;
    if (params.minimumCnr > 0 && fit.contrast < params.minimumCnr * std::sqrt(2.0) * fit.noise)
        fit.flags |= SurfacePointFlags::LowContrast;
    fit.normalizedResidual = fit.residual / std::max(fit.contrast, epsilon * scale);
    if (!std::isfinite(fit.offset) || !std::isfinite(fit.gradient) || fit.gradient <= epsilon ||
        fit.normalizedResidual > params.maximumNormalizedResidual)
        fit.flags |= SurfacePointFlags::FitRejected;
    if (fit.width > 0 &&
        (fit.width < params.minimumEdgeWidthModel || fit.width > params.maximumEdgeWidthModel))
        fit.flags |= SurfacePointFlags::Unresolved;
    if (std::abs(fit.offset) > params.maximumOffsetModel + epsilon)
        fit.flags |= SurfacePointFlags::ExcessiveOffset;
    return fit;
}
