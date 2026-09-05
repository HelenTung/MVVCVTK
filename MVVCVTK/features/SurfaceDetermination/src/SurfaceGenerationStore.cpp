#include "SurfaceGenerationStore.h"

#include <utility>

void SurfaceGenerationStore::SetGeneration(
    std::shared_ptr<const SurfaceGenerationSnapshot> generation)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_generation = std::move(generation);
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceGenerationStore::GetGeneration() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_generation;
}

void SurfaceGenerationStore::ClearGeneration() noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_generation.reset();
}
