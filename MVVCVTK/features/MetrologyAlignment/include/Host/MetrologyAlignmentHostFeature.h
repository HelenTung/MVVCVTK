#pragma once

#include "Host/HostFeature.h"
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Host/MetrologyReport.h"
#include <memory>
#include <optional>

// Attach、请求、查询、Detach 和已挂接对象析构均由 owner thread 驱动。
// 宿主须先成功 Detach 再释放对象；返回 false 表示取消仍在收口，需要重试。

class MetrologyAlignmentHostFeature final
    : public HostFeature,
      public std::enable_shared_from_this<MetrologyAlignmentHostFeature> {
  public:
    explicit MetrologyAlignmentHostFeature(AlignmentConfig config = {});
    ~MetrologyAlignmentHostFeature() noexcept override;
    MetrologyAlignmentHostFeature(const MetrologyAlignmentHostFeature &) = delete;
    MetrologyAlignmentHostFeature &operator=(const MetrologyAlignmentHostFeature &) = delete;
    MetrologyAlignmentHostFeature(MetrologyAlignmentHostFeature &&) = delete;
    MetrologyAlignmentHostFeature &operator=(MetrologyAlignmentHostFeature &&) = delete;
    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    bool AttachHost(const HostFeatureContext &context) override;
    // 尚有 worker 时请求取消并返回 false；owner tick 收口后可以重试。
    bool DetachHost() override;
    bool OnHostTick() override;
    AlignmentAdmission SendRequest(AlignmentRequest request, AlignmentCallback onComplete = {});
    std::optional<AlignmentDeviationSnapshot> GetDeviation(const DataRevisionRef &result) const;
    std::optional<AlignmentDeviationSelection> GetDeviationSample(const DataRevisionRef &result, std::uint64_t vertex) const;
    std::optional<InspectionSnapshot> GetInspection(const DataRevisionRef &result) const;
    std::optional<VoxelVolumeSnapshot> GetVoxelVolume(const DataRevisionRef &result) const;
    // 在 owner thread 的同一个图快照上捕获；返回后可在后台只读使用，状态为捕获时状态。
    MetrologyReportResult GetReport(const MetrologyReportRequest &request) const;
    AlignmentState GetState() const;
    AlignmentState GetScopeState(const std::string &scope) const;
    std::optional<AlignmentSnapshot> GetResult(const DataRevisionRef &result) const;
    std::optional<AlignmentArchive> GetArchive(const DataRevisionRef &result) const;
    std::optional<AlignmentReferenceSnapshot> GetReference(const DataRevisionRef &mesh) const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
