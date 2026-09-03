#pragma once

#include "Algorithms/ClassicalPartSegmenter.h"
#include "Data/TrustedImageState.h"
#include "Host/PartSegmentationHostTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct PartLabelCandidate final {
    std::uint64_t requestId = 0;
    DataVersion sourceVersion = 0;
    PartResultStatus status = PartResultStatus::Failed;
    PartFailureReason failureReason = PartFailureReason::InternalError;
    std::string message;
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{};
    std::array<double, 9> direction{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::shared_ptr<std::vector<std::uint32_t>> labels;
    std::vector<PartRecord> parts;
    std::size_t requiredBytes = 0;
    PartAlgorithmMetrics metrics;
};

class PartSegmentationService final {
public:
    PartSegmentationService();
    ~PartSegmentationService() noexcept;

    PartSegmentationService(const PartSegmentationService&) = delete;
    PartSegmentationService& operator=(
        const PartSegmentationService&) = delete;

    PartAdmissionStatus Start(
        TrustedImageSnapshot source,
        PartSegmentationStartParams params,
        std::size_t maxWorkingBytes,
        std::uint64_t requestId);
    void StopRequest() noexcept;
    std::optional<PartLabelCandidate> GetComplete();
    std::optional<double> GetProgress(
        std::uint64_t requestId) const noexcept;
    bool GetIsBusy() const;
    bool Stop(std::chrono::steady_clock::time_point deadline) noexcept;

private:
    struct Job final {
        TrustedImageSnapshot source;
        PartSegmentationStartParams params;
        std::size_t maxWorkingBytes = 0;
        std::uint64_t requestId = 0;
    };

    void WorkerLoop() noexcept;
    PartLabelCandidate BuildCandidate(const Job& job) noexcept;
    void SetProgress(
        std::uint64_t requestId,
        double progress) noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_workReady;
    std::condition_variable m_workerExited;
    std::optional<Job> m_job;
    std::optional<PartLabelCandidate> m_complete;
    std::thread m_worker;
    std::atomic<bool> m_cancelRequested{ false };
    std::atomic<std::uint64_t> m_progressRequestId{ 0 };
    std::atomic<std::uint32_t> m_progressPermille{ 0 };
    bool m_isBusy = false;
    bool m_isStopping = false;
    bool m_hasExited = false;
};
