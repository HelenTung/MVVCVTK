#include "SurfaceGenerationStore.h"
#include "SurfaceContracts.h"

#include <utility>
#include <algorithm>

void SurfaceGenerationStore::SetDataPort(std::weak_ptr<TrustedDataReadPort> data)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_data = std::move(data);
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceGenerationStore::GetCurrentGeneration(const std::string_view scope) const
{
    std::shared_ptr<TrustedDataReadPort> data;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        data = m_data.lock();
    }
    if (!data) return {};
    const auto graph = data->GetDataGraph();
    const auto binding = data->GetDataBinding(graph, SurfaceContract::GetBindingName(scope));
    const auto current = binding && binding->target
        ? data->GetData(graph, *binding->target) : DataSnapshot{};
    const auto* payload = current
        ? dynamic_cast<const SurfaceGenerationPayload*>(current->payload.get()) : nullptr;
    const auto generation = payload ? payload->GetGeneration() : nullptr;
    return generation && generation->dataRevision == current->self &&
                   generation->purpose == SurfaceTaskPurpose::Determine && generation->resultScope == scope &&
                   SurfaceContract::GetSourceCurrent(*data, graph, generation->sourceRevision,
                                                     generation->sourceBinding) &&
                   SurfaceContract::GetInputsCurrent(*data, graph, generation->inputs)
               ? generation
               : nullptr;
}

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

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceGenerationStore::GetGeneration(const DataRevisionRef revision) const
{
    std::shared_ptr<TrustedDataReadPort> data;
    { const std::lock_guard<std::mutex> lock(m_mutex); data = m_data.lock(); }
    const auto snapshot = data ? data->GetData(data->GetDataGraph(), revision) : DataSnapshot{};
    const auto* payload = snapshot ? dynamic_cast<const SurfaceGenerationPayload*>(snapshot->payload.get()) : nullptr;
    const auto generation = payload ? payload->GetGeneration() : nullptr;
    return generation && generation->dataRevision == revision
        && generation->purpose == SurfaceTaskPurpose::Determine ? generation : nullptr;
}
