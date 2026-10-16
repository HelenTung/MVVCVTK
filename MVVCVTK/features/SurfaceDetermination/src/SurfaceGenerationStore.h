#pragma once

#include "Host/SurfaceDeterminationHostTypes.h"
#include "Host/TrustedDataPort.h"
#include "FeatureSupport/SpatialMeshSource.h"

#include <memory>
#include <mutex>
#include <utility>

inline const DataTypeId surfaceGenerationType{
    "org.mvvcvtk.surface-determination.generation", 1 };
inline const DataFacetId surfaceGenerationFacet{ "surface-measurement-generation" };
inline constexpr std::string_view surfaceResultBinding = "analysis.surface-determination.active";
struct SurfacePreparedGeometry;

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
        Statistics statistics,std::shared_ptr<const SurfacePreparedGeometry> storage={},
        std::shared_ptr<const FeatureInternal::SpatialMeshSource> display={},std::string displayError={})
        : m_generation(std::move(generation)), m_storage(std::move(storage)),m_statistics(statistics),m_display(std::move(display)),m_displayError(std::move(displayError)) {}
    DataTypeId GetDataType() const override { return surfaceGenerationType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const SurfaceGenerationPayload>(*this);
    }
    const std::shared_ptr<const SurfaceGenerationSnapshot>& GetGeneration() const noexcept
    {
        return m_generation;
    }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override
    {
        if (!m_generation) return {};
        return {m_generation,m_generation->points,m_generation->triangleIndices,m_generation->objects,
            m_generation->triangleValidity,m_generation->interfaces,m_generation->selection,m_storage,m_display};
    }
    const std::shared_ptr<const FeatureInternal::SpatialMeshSource>& GetDisplaySource()const noexcept{return m_display;}
    const std::string& GetDisplayError()const noexcept{return m_displayError;}
    const Statistics& GetStatistics() const noexcept { return m_statistics; }
    const std::shared_ptr<const SurfacePreparedGeometry>& GetStorage()const noexcept{return m_storage;}
private:
    std::shared_ptr<const SurfaceGenerationSnapshot> m_generation;
    std::shared_ptr<const SurfacePreparedGeometry> m_storage;
    Statistics m_statistics;
    std::shared_ptr<const FeatureInternal::SpatialMeshSource> m_display;
    std::string m_displayError;
};

class SurfaceGenerationStore final {
public:
    void SetDataPort(std::weak_ptr<TrustedDataReadPort> data);
    std::shared_ptr<const SurfaceGenerationSnapshot> GetCurrentGeneration(std::string_view scope = {}) const;
    std::shared_ptr<const SurfaceGenerationSnapshot> GetGeneration(DataRevisionRef revision) const;
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
