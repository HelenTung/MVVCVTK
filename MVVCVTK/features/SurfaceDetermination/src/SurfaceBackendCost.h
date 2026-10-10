#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// 仅用于私有实验调度；成本估计不是资源上界，也不参与测量字段计算。
struct SurfaceBackendProbe final {
    std::size_t cpuPoints = 0, gpuPoints = 0, remainingPoints = 0;
    std::uint64_t volumeBytes = 0, uploadedBytes = 0;
    double cpuMs = 0, gpuMs = 0, uploadMs = 0, startupMs = 0;
};
struct SurfaceBackendChoice final {
    bool keepGpu = false, hasEstimate = false;
    double cpuTotalMs = 0, gpuTotalMs = 0;
};
inline SurfaceBackendChoice GetSurfaceBackendChoice(const SurfaceBackendProbe &probe)
{
    SurfaceBackendChoice choice;
    if (!probe.cpuPoints || !probe.gpuPoints || !probe.remainingPoints || !probe.uploadedBytes ||
        !std::isfinite(probe.cpuMs) || !std::isfinite(probe.gpuMs) || !std::isfinite(probe.uploadMs) ||
        !std::isfinite(probe.startupMs) || probe.cpuMs<=0 || probe.uploadMs<=0 ||
        probe.gpuMs<probe.uploadMs || probe.startupMs<0) return choice;
    const double cpuRemaining=probe.cpuMs/probe.cpuPoints*probe.remainingPoints;
    const double gpuCompute=(probe.gpuMs-probe.uploadMs)/probe.gpuPoints*probe.remainingPoints;
    // 使用整卷剩余字节估计未来传输；不额外扫描点，不把首窗上传均摊到每个点。
    const double bytesRemaining=std::max(0.,double(probe.volumeBytes)-double(probe.uploadedBytes));
    const double gpuRemaining=gpuCompute+probe.uploadMs/double(probe.uploadedBytes)*bytesRemaining;
    const double spent=probe.cpuMs+probe.startupMs+probe.gpuMs;
    choice.cpuTotalMs=spent+cpuRemaining;choice.gpuTotalMs=spent+gpuRemaining;
    if (!std::isfinite(choice.cpuTotalMs) || !std::isfinite(choice.gpuTotalMs)) return {};
    choice.hasEstimate=true;
    // 仅在预计余段有至少20%裕量时继续GPU，避免将小于调度噪声的差异当作收益。
    choice.keepGpu=gpuRemaining<.8*cpuRemaining;
    return choice;
}
