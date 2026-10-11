#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>

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

struct SurfaceBackendCalibration final {
    double computeMsPerPoint=0,uploadMsPerByte=0,startupMs=0;
};
inline std::optional<SurfaceBackendCalibration> GetSurfaceBackendCalibration(const SurfaceBackendProbe &probe)
{
    if(!GetSurfaceBackendChoice(probe).hasEstimate)return {};
    return SurfaceBackendCalibration{(probe.gpuMs-probe.uploadMs)/probe.gpuPoints,
        probe.uploadMs/double(probe.uploadedBytes),probe.startupMs};
}
inline SurfaceBackendChoice GetSurfaceBackendAdmission(const SurfaceBackendCalibration &calibration,
    std::size_t cpuPoints,double cpuMs,std::size_t remainingPoints,std::uint64_t volumeBytes)
{
    SurfaceBackendChoice choice;
    if(!cpuPoints || !remainingPoints || !volumeBytes || !std::isfinite(cpuMs) || cpuMs<=0 ||
        !std::isfinite(calibration.computeMsPerPoint) || calibration.computeMsPerPoint<0 ||
        !std::isfinite(calibration.uploadMsPerByte) || calibration.uploadMsPerByte<=0 ||
        !std::isfinite(calibration.startupMs) || calibration.startupMs<0)return choice;
    const auto cpuRemaining=cpuMs/cpuPoints*remainingPoints;
    const auto gpuRemaining=calibration.startupMs+calibration.computeMsPerPoint*remainingPoints+
        calibration.uploadMsPerByte*double(volumeBytes);
    choice.cpuTotalMs=cpuMs+cpuRemaining;choice.gpuTotalMs=cpuMs+gpuRemaining;
    if(!std::isfinite(choice.cpuTotalMs) || !std::isfinite(choice.gpuTotalMs))return {};
    choice.hasEstimate=true;
    // 准入只允许明确较快的 CPU 提前结束探测；GPU 仍要执行真实 Start 与新 pilot。
    choice.keepGpu=!(cpuRemaining<.8*gpuRemaining);
    return choice;
}

// 值键不保存任何图像/ROI/VTK对象；最多四条、两分钟有效、命中后立即消费。
class SurfaceBackendCalibrationCache final {
public:
    using Key=std::array<std::uint64_t,10>;
    using Clock=std::chrono::steady_clock;
    std::optional<SurfaceBackendCalibration> Take(const Key &key,Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for(auto &entry:m_entries){
            if(entry.valid && (now<entry.created || now-entry.created>=std::chrono::minutes(2)))entry.valid=false;
            if(entry.valid && entry.key==key){entry.valid=false;return entry.calibration;}
        }
        return {};
    }
    void Store(const Key &key,const SurfaceBackendCalibration &calibration,Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for(auto &entry:m_entries)if(entry.valid && entry.key==key){entry={key,calibration,now,true};return;}
        auto &entry=m_entries[m_next];entry={key,calibration,now,true};m_next=(m_next+1)%m_entries.size();
    }
    static SurfaceBackendCalibrationCache &Get()
    {
        static SurfaceBackendCalibrationCache cache;
        return cache;
    }
private:
    struct Entry final {Key key{};SurfaceBackendCalibration calibration;Clock::time_point created{};bool valid=false;};
    std::array<Entry,4> m_entries;
    std::size_t m_next=0;
    std::mutex m_mutex;
};
static_assert(sizeof(SurfaceBackendCalibrationCache)<=1024,"Surface cost cache must stay within one KiB");
