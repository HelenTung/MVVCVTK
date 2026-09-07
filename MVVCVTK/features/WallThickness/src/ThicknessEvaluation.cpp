#include "ThicknessAlgorithm.h"
#include "ThicknessMath.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace ThicknessAlgorithm
{
Candidate BuildEvaluation(const Field &field, const ThicknessEvaluation &evaluation,
                          const ThicknessParams &params, const ThicknessConfig &config,
                          const std::shared_ptr<std::atomic<bool>> &cancelled,
                          std::chrono::steady_clock::time_point deadline) noexcept
{
    Candidate result;
    try
    {
        if (!field.samples || !field.neighbors ||
            field.samples->size() != field.neighbors->size() || !GetEvaluationValid(evaluation) ||
            !GetParamsValid(params) || !GetConfigValid(config))
        {
            result.status = ThicknessStatus::InvalidInput;
            result.message = "Invalid evaluation input.";
            return result;
        }
        const auto &samples = *field.samples;
        if (samples.size() > config.maxSamples ||
            samples.size() > config.maxWorkingBytes / (sizeof(ThicknessSample) + 128U))
        {
            result.status = ThicknessStatus::BudgetExceeded;
            return result;
        }
        const auto check = [&]()
        {
            if (cancelled && cancelled->load())
                return ThicknessStatus::Cancelled;
            if (std::chrono::steady_clock::now() >= deadline)
                return ThicknessStatus::DeadlineExceeded;
            return ThicknessStatus::Succeeded;
        };
        auto &stats = result.statistics;
        stats.histogramAreas.assign(evaluation.histogramBins, 0);
        std::vector<std::size_t> sorted;
        sorted.reserve(samples.size());
        double weighted = 0;
        bool hasSearchLimit = false;
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            if ((i & 255U) == 0 && (result.status = check()) != ThicknessStatus::Succeeded)
                return result;
            const auto &sample = samples[i];
            const auto reason = static_cast<std::size_t>(sample.validity);
            if (reason >= stats.reasonCounts.size() || !std::isfinite(sample.area) ||
                sample.area <= 0 || !ThicknessMath::Finite(sample.source))
            {
                result.status = ThicknessStatus::InvalidInput;
                return result;
            }
            ++stats.reasonCounts[reason];
            stats.reasonAreas[reason] += sample.area;
            if (sample.validity == ThicknessValidity::OutsideEvaluation)
                continue;
            ++stats.sampleCount;
            stats.evaluatedArea += sample.area;
            hasSearchLimit =
                hasSearchLimit || sample.validity == ThicknessValidity::SearchLimitReached;
            if (sample.validity != ThicknessValidity::Valid)
                continue;
            if (!std::isfinite(sample.thickness) || sample.thickness <= 0)
            {
                result.status = ThicknessStatus::InvalidInput;
                return result;
            }
            ++stats.validCount;
            stats.validArea += sample.area;
            weighted += sample.thickness * sample.area;
            if (!stats.minimum || sample.thickness < *stats.minimum)
            {
                stats.minimum = sample.thickness;
                stats.minimumSample = i;
            }
            if (!stats.maximum || sample.thickness > *stats.maximum)
            {
                stats.maximum = sample.thickness;
                stats.maximumSample = i;
            }
            sorted.push_back(i);
            const auto low = evaluation.histogramRange[0], high = evaluation.histogramRange[1];
            if (sample.thickness < low)
                stats.histogramBelowArea += sample.area;
            else if (sample.thickness > high)
                stats.histogramAboveArea += sample.area;
            else
            {
                const double normalized = (sample.thickness - low) / (high - low);
                const auto bin =
                    std::min(evaluation.histogramBins - 1,
                             static_cast<std::size_t>(normalized * evaluation.histogramBins));
                stats.histogramAreas[bin] += sample.area;
            }
        }
        if (hasSearchLimit && evaluation.upper > params.maxDistance)
        {
            result.status = ThicknessStatus::InvalidInput;
            result.message = "Evaluation exceeds search support; start a new analysis.";
            return result;
        }
        if (!std::isfinite(stats.evaluatedArea) || !std::isfinite(stats.validArea) ||
            !std::isfinite(weighted))
        {
            result.status = ThicknessStatus::InvalidInput;
            result.message = "Statistics overflow.";
            return result;
        }
        for (double area : stats.reasonAreas)
            if (!std::isfinite(area))
            {
                result.status = ThicknessStatus::InvalidInput;
                return result;
            }
        for (double area : stats.histogramAreas)
            if (!std::isfinite(area))
            {
                result.status = ThicknessStatus::InvalidInput;
                return result;
            }
        stats.coverage = stats.evaluatedArea > 0 ? stats.validArea / stats.evaluatedArea : 0;
        if (stats.validArea > 0)
            stats.mean = weighted / stats.validArea;
        std::sort(sorted.begin(), sorted.end(),
                  [&](auto a, auto b)
                  {
                      return samples[a].thickness < samples[b].thickness ||
                             (samples[a].thickness == samples[b].thickness && a < b);
                  });
        double cumulative = 0;
        const std::array<double, 3> fractions{0.05, 0.5, 0.95};
        for (auto id : sorted)
        {
            cumulative += samples[id].area;
            for (std::size_t q = 0; q < 3; ++q)
                if (!stats.quantiles[q] && cumulative >= fractions[q] * stats.validArea)
                    stats.quantiles[q] = samples[id].thickness;
        }
        const auto classification = [&](std::size_t i)
        {
            const auto &sample = samples[i];
            return sample.validity != ThicknessValidity::Valid ? 0
                   : sample.thickness < evaluation.lower       ? -1
                   : sample.thickness > evaluation.upper       ? 1
                                                               : 0;
        };
        std::vector<std::uint8_t> visited(samples.size(), 0);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            if ((result.status = check()) != ThicknessStatus::Succeeded)
                return result;
            const int kind = classification(i);
            if (visited[i] || kind == 0)
                continue;
            ThicknessRegion region;
            region.id = result.regions.size() + 1;
            region.isBelowLower = kind < 0;
            region.minimum = region.maximum = samples[i].thickness;
            for (std::size_t a = 0; a < 3; ++a)
                region.sampleBounds[a * 2] = region.sampleBounds[a * 2 + 1] = samples[i].source[a];
            region.sampleIds.push_back(i);
            visited[i] = 1;
            for (std::size_t next = 0; next < region.sampleIds.size(); ++next)
            {
                if ((next & 255U) == 0 && (result.status = check()) != ThicknessStatus::Succeeded)
                    return result;
                const auto id = region.sampleIds[next];
                const auto &sample = samples[id];
                region.area += sample.area;
                region.minimum = std::min(region.minimum, sample.thickness);
                region.maximum = std::max(region.maximum, sample.thickness);
                for (std::size_t a = 0; a < 3; ++a)
                {
                    region.sampleBounds[a * 2] =
                        std::min(region.sampleBounds[a * 2], sample.source[a]);
                    region.sampleBounds[a * 2 + 1] =
                        std::max(region.sampleBounds[a * 2 + 1], sample.source[a]);
                }
                for (auto neighbor : (*field.neighbors)[id])
                {
                    if (neighbor == noNeighbor)
                        continue;
                    if (neighbor >= samples.size())
                    {
                        result.status = ThicknessStatus::InvalidInput;
                        return result;
                    }
                    if (!visited[neighbor] && classification(neighbor) == kind)
                    {
                        visited[neighbor] = 1;
                        region.sampleIds.push_back(neighbor);
                    }
                }
            }
            region.isDisplayed = region.area >= evaluation.minRegionArea;
            result.regions.push_back(std::move(region));
        }
        result.status =
            stats.validCount ? ThicknessStatus::Succeeded : ThicknessStatus::NoValidSamples;
        result.message =
            stats.validCount
                ? "Sampled ray thickness evaluated; areas count both surface sides."
                : "No reliable thickness samples; no compliance decision is available.";
        return result;
    }
    catch (const std::bad_alloc &)
    {
        result.status = ThicknessStatus::BudgetExceeded;
    }
    catch (...)
    {
        result.status = ThicknessStatus::InternalError;
    }
    result.field = {};
    return result;
}
} // namespace ThicknessAlgorithm
