#include "Host/Internal/HostTransferCodec.h"

#include <cmath>

bool HostTransferCodec::GetUnitValid(const double value)
{
    return std::isfinite(value)
        && value >= 0.0
        && value <= 1.0;
}

std::optional<VolumeTransferFunction>
HostTransferCodec::BuildVolumeTransferFunction(
    const HostVolumeTransferFunction& value)
{
    if (value.colorNodes.size() < 2
        || value.opacityNodes.size() < 2) {
        return std::nullopt;
    }

    VolumeTransferFunction result;
    result.colorNodes.reserve(value.colorNodes.size());
    for (const auto& node : value.colorNodes) {
        if (!std::isfinite(node.scalar)
            || !GetUnitValid(node.r)
            || !GetUnitValid(node.g)
            || !GetUnitValid(node.b)
            || (!result.colorNodes.empty()
                && node.scalar
                    <= result.colorNodes.back().scalar)) {
            return std::nullopt;
        }
        result.colorNodes.push_back({
            node.scalar, node.r, node.g, node.b
        });
    }

    result.opacityNodes.reserve(value.opacityNodes.size());
    for (const auto& node : value.opacityNodes) {
        if (!std::isfinite(node.scalar)
            || !GetUnitValid(node.opacity)
            || (!result.opacityNodes.empty()
                && node.scalar
                    <= result.opacityNodes.back().scalar)) {
            return std::nullopt;
        }
        result.opacityNodes.push_back({
            node.scalar, node.opacity
        });
    }
    return result;
}

HostVolumeTransferFunction
HostTransferCodec::GetHostVolumeTransfer(
    const VolumeTransferFunction& value)
{
    HostVolumeTransferFunction result;
    result.colorNodes.reserve(value.colorNodes.size());
    for (const auto& node : value.colorNodes) {
        result.colorNodes.push_back({
            node.scalar, node.r, node.g, node.b
        });
    }
    result.opacityNodes.reserve(value.opacityNodes.size());
    for (const auto& node : value.opacityNodes) {
        result.opacityNodes.push_back({
            node.scalar, node.opacity
        });
    }
    return result;
}
