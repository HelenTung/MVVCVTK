#pragma once

#include "Host/PartSegmentationHostTypes.h"

#include <functional>
#include <optional>

using PartIdentitySource = std::function<PartObjectId()>;
using PartSetIdUsed = std::function<bool(const PartSetId&)>;
using PartObjectIdUsed = std::function<bool(const PartObjectId&)>;

class PartIdentityFactory final {
public:
    PartIdentityFactory();
    explicit PartIdentityFactory(PartIdentitySource getIdentity);

    std::optional<PartSetId> CreatePartSetId(
        const PartSetIdUsed& getIsUsed = nullptr);
    std::optional<PartObjectId> CreatePartObjectId(
        const PartObjectIdUsed& getIsUsed = nullptr);

private:
    PartIdentitySource m_getIdentity;
};
