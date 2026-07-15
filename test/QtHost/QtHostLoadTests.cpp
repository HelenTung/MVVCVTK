#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <array>
#include <utility>

int GetLoadFailCount()
{
    int failureCount{0};
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        bool hasCallback{false};
        InitialVolumeLoadConfig request;
        const bool isAccepted = session.LoadVolume(
            request,
            [&hasCallback](bool) { hasCallback = true; });
        failureCount += GetCaseResult(
            isAccepted && !hasCallback,
            "Load disabled no-op") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        InitialVolumeLoadConfig request;
        request.isInitialLoadEnabled = true;
        request.geometry.emplace(
            std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
            std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
        failureCount += GetCaseResult(
            !session.LoadVolume(request),
            "Load missing path rejection") ? 0 : 1;
    }
    {
        VtkAppHostSession session(VtkAppHostSession::Config{});
        HostVolumeBufferRequest request;
        request.voxels.assign(7, 1.0f);
        request.dimensions = { 2, 2, 2 };
        request.geometry.emplace(
            std::array<float, 3>{ 1.0f, 1.0f, 1.0f },
            std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
        bool hasCallback{false};
        const bool isAccepted = session.ReloadVolume(
            std::move(request),
            [&hasCallback](bool) { hasCallback = true; });
        failureCount += GetCaseResult(
            !isAccepted && !hasCallback,
            "Reload voxel-count rejection") ? 0 : 1;
    }
    return failureCount;
}
