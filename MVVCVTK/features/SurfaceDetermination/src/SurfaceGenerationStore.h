#pragma once

#include "Host/SurfaceDeterminationHostTypes.h"

#include <memory>
#include <mutex>

class SurfaceGenerationStore final {
public:
    void SetGeneration(
        std::shared_ptr<const SurfaceGenerationSnapshot> generation);
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetGeneration() const;
    void ClearGeneration() noexcept;

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const SurfaceGenerationSnapshot> m_generation;
};
