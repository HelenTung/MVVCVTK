#include "SurfaceGenerationStore.h"

#include <utility>

void SurfaceGenerationStore::SetGeneration(
    DataSnapshot generation)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_generation = std::move(generation);
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceGenerationStore::GetGeneration() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto* payload = m_generation
        ? dynamic_cast<const SurfaceGenerationPayload*>(m_generation->payload.get()) : nullptr;
    return payload ? payload->GetGeneration() : nullptr;
}

void SurfaceGenerationStore::ClearGeneration() noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_generation.reset();
}
