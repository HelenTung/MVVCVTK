#include "Model/PartIdentityFactory.h"

#include "Model/PartCatalog.h"

#include <cstdint>
#include <random>
#include <utility>

namespace {

constexpr int identityAttemptLimit = 16;

PartObjectId GetRandomIdentity()
{
    std::random_device source;
    const auto getWord = [&source]() {
        const std::uint64_t high = static_cast<std::uint32_t>(source());
        const std::uint64_t low = static_cast<std::uint32_t>(source());
        return (high << 32) | low;
    };
    return { getWord(), getWord() };
}

} // namespace

PartIdentityFactory::PartIdentityFactory()
    : m_getIdentity(GetRandomIdentity)
{
}

PartIdentityFactory::PartIdentityFactory(
    PartIdentitySource getIdentity)
    : m_getIdentity(std::move(getIdentity))
{
}

std::optional<PartSetId> PartIdentityFactory::CreatePartSetId(
    const PartSetIdUsed& getIsUsed)
{
    if (!m_getIdentity) return std::nullopt;
    try {
        for (int attempt = 0; attempt < identityAttemptLimit; ++attempt) {
            const auto identity = m_getIdentity();
            const PartSetId candidate{ identity.high, identity.low };
            if (GetPartSetIdValid(candidate)
                && (!getIsUsed || !getIsUsed(candidate))) {
                return candidate;
            }
        }
    }
    catch (...) {
    }
    return std::nullopt;
}

std::optional<PartObjectId> PartIdentityFactory::CreatePartObjectId(
    const PartObjectIdUsed& getIsUsed)
{
    if (!m_getIdentity) return std::nullopt;
    try {
        for (int attempt = 0; attempt < identityAttemptLimit; ++attempt) {
            const auto candidate = m_getIdentity();
            if (GetPartObjectIdValid(candidate)
                && (!getIsUsed || !getIsUsed(candidate))) {
                return candidate;
            }
        }
    }
    catch (...) {
    }
    return std::nullopt;
}
