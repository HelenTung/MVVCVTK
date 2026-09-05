#pragma once

#include "Host/SurfaceDeterminationHostTypes.h"
#include "Host/TrustedDataPort.h"

#include <memory>
#include <mutex>
#include <utility>

inline const DataTypeId surfaceGenerationType{
    "org.mvvcvtk.surface-determination.generation", 1 };
inline const DataFacetId surfaceGenerationFacet{ "surface-measurement-generation" };
inline constexpr std::string_view surfaceResultBinding = "analysis.surface-determination.active";

class SurfaceGenerationPayload final : public IDataPayload {
public:
    // 算法输出的固有质量统计随正式结果保留；不保存任务/窗口/显示运行态。
    struct Statistics final {
        std::uint64_t acceptedPointCount = 0;
        std::uint64_t lowContrastPointCount = 0;
        std::uint64_t rejectedPointCount = 0;
        std::uint64_t truncatedPointCount = 0;
        std::uint32_t nonManifoldObjectCount = 0;
    };
    explicit SurfaceGenerationPayload(
        std::shared_ptr<const SurfaceGenerationSnapshot> generation,
        Statistics statistics)
        : m_generation(std::move(generation)), m_statistics(statistics) {}
    DataTypeId GetDataType() const override { return surfaceGenerationType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const SurfaceGenerationPayload>(*this);
    }
    const std::shared_ptr<const SurfaceGenerationSnapshot>& GetGeneration() const noexcept
    {
        return m_generation;
    }
    const Statistics& GetStatistics() const noexcept { return m_statistics; }
private:
    std::shared_ptr<const SurfaceGenerationSnapshot> m_generation;
    Statistics m_statistics;
};

class SurfaceGenerationStore final {
public:
    void SetDataPort(std::weak_ptr<TrustedDataReadPort> data);
    std::shared_ptr<const SurfaceGenerationSnapshot> GetCurrentGeneration() const;
    void SetGeneration(
        DataSnapshot generation);
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetGeneration() const;
    void ClearGeneration() noexcept;

private:
    mutable std::mutex m_mutex;
    // 仅保留 DataGraph 正式修订的只读视图，Clear 不删除图中的历史。
    DataSnapshot m_generation;
    std::weak_ptr<TrustedDataReadPort> m_data;
};
