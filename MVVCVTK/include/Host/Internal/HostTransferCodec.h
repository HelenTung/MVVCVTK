#pragma once

#include "App/AppTypes.h"
#include "Host/Types/HostValueTypes.h"

#include <optional>

// Host 私有边界转换；不属于 SDK 头文件集。
class HostTransferCodec final {
public:
    static std::optional<VolumeTransferFunction> BuildVolumeTransferFunction(
        const HostVolumeTransferFunction& value);
    static HostVolumeTransferFunction GetHostVolumeTransfer(
        const VolumeTransferFunction& value);

private:
    static bool GetUnitValid(double value);
};
