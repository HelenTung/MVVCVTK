#pragma once

#include "Host/SurfaceDeterminationHostTypes.h"

#include <memory>
#include <mutex>
#include <utility>

inline const DataTypeId surfaceGenerationType{
    "org.mvvcvtk.surface-determination.generation", 1 };
inline const DataFacetId surfaceGenerationFacet{ "surface-measurement-generation" };

class SurfaceGenerationPayload final : public IDataPayload {
public:
    explicit SurfaceGenerationPayload(
        std::shared_ptr<const SurfaceGenerationSnapshot> generation)
        : m_generation(std::move(generation)) {}
    DataTypeId GetDataType() const override { return surfaceGenerationType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const SurfaceGenerationPayload>(*this);
    }
    const std::shared_ptr<const SurfaceGenerationSnapshot>& GetGeneration() const noexcept
    {
        return m_generation;
    }
private:
    std::shared_ptr<const SurfaceGenerationSnapshot> m_generation;
};

class SurfaceGenerationStore final {
public:
    void SetGeneration(
        DataSnapshot generation);
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetGeneration() const;
    void ClearGeneration() noexcept;

private:
    mutable std::mutex m_mutex;
    // 仅保留 DataGraph 正式修订的只读视图，Clear 不删除图中的历史。
    DataSnapshot m_generation;
};
