#pragma once

#include "SurfaceDeterminationAlgorithm.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct SurfaceRequestProgress final {
    SurfaceDeterminationStage stage = SurfaceDeterminationStage::Preparing;
    double progress01 = 0.0;
};

struct SurfaceJobComplete final {
    std::uint64_t requestId = 0;
    SurfaceAlgorithmResult result;
};

class SurfaceDeterminationService final {
public:
    SurfaceDeterminationService();
    ~SurfaceDeterminationService() noexcept;

    SurfaceDeterminationService(
        const SurfaceDeterminationService&) = delete;
    SurfaceDeterminationService& operator=(
        const SurfaceDeterminationService&) = delete;

    SurfaceAdmissionStatus Start(
        TrustedImageSnapshot source,
        SurfaceDeterminationStartParams params,
        std::size_t maxWorkingBytes,
        std::uint64_t requestId);
    bool StopRequest(std::uint64_t requestId) noexcept;
    std::optional<SurfaceJobComplete> GetComplete();
    std::optional<SurfaceRequestProgress> GetProgress(
        std::uint64_t requestId) const noexcept;
    bool GetIsBusy() const;
    bool Stop(std::chrono::steady_clock::time_point deadline) noexcept;

private:
    struct Job final {
        TrustedImageSnapshot source;
        SurfaceDeterminationStartParams params;
        std::size_t maxWorkingBytes = 0;
        std::uint64_t requestId = 0;
        std::shared_ptr<std::atomic<bool>> isCancelled;
    };

    static SurfaceJobComplete BuildCancelled(const Job& job);
    void WorkerLoop() noexcept;
    void SetProgress(
        std::uint64_t requestId,
        SurfaceDeterminationStage stage,
        double progress) noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_workReady;
    std::condition_variable m_workerExited;
    std::optional<Job> m_pendingJob;
    std::vector<SurfaceJobComplete> m_complete;
    std::thread m_worker;
    std::shared_ptr<std::atomic<bool>> m_activeCancel;
    std::uint64_t m_activeRequestId = 0;
    std::uint64_t m_latestRequestId = 0;
    std::atomic<std::uint64_t> m_progressRequestId{ 0 };
    std::atomic<std::uint32_t> m_progressPermille{ 0 };
    std::atomic<std::uint8_t> m_progressStage{
        static_cast<std::uint8_t>(SurfaceDeterminationStage::Preparing) };
    bool m_isStopping = false;
    bool m_hasExited = false;
};
