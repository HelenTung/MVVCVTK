#pragma once

#include "App/AppTypes.h"
#include "Platform/TaskStopToken.h"
#include "Render/Contracts/RenderEffect.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

class AbstractDataManager;

struct RenderInputUse final {
    std::shared_ptr<DataLifetimeAccess> lifetime;
    std::shared_ptr<const DataResourceLease> resource;
    bool GetIsPublished() const { return !lifetime || lifetime->GetIsPublished(); }
};

struct VolumeLodKey;
struct VolumeLodProduct;
struct IsoSurfaceKey;
struct IsoSurfaceProduct;

enum class RenderProductKind {
    VolumeLod,
    IsoSurface
};

enum class RenderProductStatus {
    Idle,
    Preparing,
    Ready,
    Active,
    Failed,
    Cancelled
};

enum class RenderProductFailure {
    None,
    InvalidInput,
    ResourceRejected,
    TaskRejected,
    BuildFailed,
    StaleInput,
    CommitFailed,
    Cancelled,
    Stopping
};

enum class RenderInteractionPhase {
    Interactive,
    Settling,
    Still
};

struct RenderTransitionStats final {
    std::uint64_t requestRevision = 0;
    std::uint64_t activeRevision = 0;
    std::uint64_t cpuPrepareUs = 0;
    std::uint64_t gpuReleaseUs = 0;
    std::uint64_t gpuUploadUs = 0;
    std::uint64_t firstRenderUs = 0;
    std::uint64_t candidateBytes = 0;
    std::uint64_t activeBytes = 0;
    std::uint64_t cacheBytes = 0;
    std::array<int, 3> resolvedDimensions{};
    std::array<unsigned short, 3> partitions{ 1, 1, 1 };
    bool isCacheHit = false;
    bool isPreview = false;
};

struct RenderTransitionState final {
    RenderProductStatus status = RenderProductStatus::Idle;
    RenderProductFailure failureReason = RenderProductFailure::None;
    RenderInputStamp inputStamp;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    VolumeQuality appliedQuality = VolumeQuality::Auto;
    RenderTransitionStats stats;
    std::string message;
};

class RenderTaskToken final {
public:
    RenderTaskToken() = default;
    bool GetIsStopped() const noexcept;
    bool SetActualBytes(std::uint64_t actualBytes) const;
    // 产品读者在取消/退役后仍存活时继续计费；不取得业务数据或渲染对象所有权。
    bool SetProductOwner(const std::shared_ptr<const void>& product,
        std::uint64_t actualBytes) const;

private:
    class Impl;
    explicit RenderTaskToken(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> m_impl;
    friend class RenderResourceCoordinator;
};

using RenderTaskWork = std::function<void(RenderTaskToken)>;
using RenderLaneWork = std::packaged_task<bool(TaskStopToken)>;
using RenderLaneStart = std::function<bool(RenderLaneWork)>;

enum class RenderTaskAdmission {
    Accepted,
    Replaced,
    ResourceRejected,
    Stopping,
    Unavailable
};

struct RenderTaskRequest final {
    std::uint64_t requestRevision = 0;
    std::uint64_t estimatedBytes = 0;
    RenderTaskWork work;
};

class RenderTaskChannel final {
public:
    ~RenderTaskChannel() noexcept;

    RenderTaskChannel(const RenderTaskChannel&) = delete;
    RenderTaskChannel& operator=(const RenderTaskChannel&) = delete;

    RenderTaskAdmission StartTask(RenderTaskRequest request);
    RenderTransitionState GetState() const;
    bool SetActiveBytes(
        std::uint64_t requestRevision,
        std::uint64_t activeBytes,
        const void* productIdentity = nullptr);
    bool SetCachedActive(
        std::uint64_t requestRevision,
        std::uint64_t activeBytes,
        const void* productIdentity = nullptr);
    bool CompleteActiveBytes(std::uint64_t requestRevision);
    bool RestoreActiveBytes(std::uint64_t requestRevision);
    bool SetReadyFailed(
        std::uint64_t requestRevision,
        RenderProductFailure failureReason,
        std::string message);
    bool Stop();

private:
    class Impl;
    explicit RenderTaskChannel(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> m_impl;
    friend class RenderResourceCoordinator;
};

struct RenderResourceState final {
    std::uint64_t activeBytes = 0;
    std::uint64_t runningBytes = 0;
    std::uint64_t pendingBytes = 0;
    std::uint64_t cacheBytes = 0;
    std::uint64_t cpuBudgetBytes = 0;
};

struct RenderGpuResourceState final {
    std::uint64_t budgetBytes = 0;
    std::uint64_t reservedBytes = 0;
    std::size_t reservationCount = 0;
};

class RenderResourceCoordinator final {
public:
    explicit RenderResourceCoordinator(RenderLaneStart onTaskStart,
        std::weak_ptr<AbstractDataManager> data = {});
    // 请求、后台工作、完成邮箱和派生产品共同持有同一输入租约。
    std::optional<RenderInputUse> StartDataUse(const RenderInputStamp& input) const;
    ~RenderResourceCoordinator() noexcept;

    RenderResourceCoordinator(
        const RenderResourceCoordinator&) = delete;
    RenderResourceCoordinator& operator=(
        const RenderResourceCoordinator&) = delete;

    std::shared_ptr<RenderTaskChannel> CreateTaskChannel(
        RenderProductKind productKind);
    bool SendTasks();
    bool StartStop();
    bool Stop(std::chrono::steady_clock::time_point deadline);
    RenderResourceState GetResourceState() const;
    bool SetCpuBudgetBytes(std::uint64_t budgetBytes);

    std::shared_ptr<const VolumeLodProduct> GetVolumeProduct(
        const VolumeLodKey& key);
    bool SetVolumeProduct(
        const VolumeLodKey& key,
        std::shared_ptr<const VolumeLodProduct> product);
    std::shared_ptr<const IsoSurfaceProduct> GetIsoSurfaceProduct(
        const IsoSurfaceKey& key);
    bool SetIsoSurfaceProduct(
        const IsoSurfaceKey& key,
        std::shared_ptr<const IsoSurfaceProduct> product);

    bool SetGpuContextBudget(
        const void* contextIdentity,
        std::uint64_t budgetBytes);
    bool SetGpuReservation(
        const void* contextIdentity,
        const void* ownerIdentity,
        std::uint64_t reservedBytes);
    bool ClearGpuReservation(
        const void* contextIdentity,
        const void* ownerIdentity);
    bool ClearGpuContext(const void* contextIdentity);
    RenderGpuResourceState GetGpuResourceState(
        const void* contextIdentity) const;
    std::uint64_t AdvanceTopologyRevision();
    std::uint64_t GetTopologyRevision() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    friend class RenderTaskChannel;
};

struct RenderStrategyServices final {
    bool isHostDriven = false;
    std::shared_ptr<RenderResourceCoordinator> resources;
};
