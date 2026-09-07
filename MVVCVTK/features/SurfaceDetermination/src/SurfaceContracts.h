#pragma once

#include "Host/SurfaceDeterminationHostTypes.h"
#include "Host/TrustedDataPort.h"

namespace SurfaceContract {
SurfaceTaskPurpose GetPurpose(const SurfaceDeterminationStartParams& params);
bool GetInputValid(const SurfaceDeterminationStartParams& params);
bool GetPointValid(const SurfacePointRecord& point, SurfaceDeterminationMethod method);
std::string GetBindingName(std::string_view scope);
bool GetSourceCurrent(const TrustedDataReadPort& data, const DataGraphSnapshot& graph,
    DataRevisionRef source, const std::optional<DataBinding>& binding = {});
std::string BuildParameters(const SurfaceDeterminationStartParams& requested,
    const SurfaceDeterminationStartParams& resolved, const std::string& frame,
    std::size_t workingBytes);
bool GetParameters(const std::string& text, SurfaceDeterminationStartParams& requested,
    SurfaceDeterminationStartParams& resolved, std::string& frame,
    std::size_t& workingBytes);
}
