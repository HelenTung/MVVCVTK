#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Host/FeatureModelTransformPort.h"
#include <chrono>

// 当前 Host 模型的姿态桥接；不拥有体素/网格，不更改计量结果或 active binding。
class AlignmentModelPose final {
public:
  struct Completion {
    AlignmentCallback callback;
    AlignmentResult result;
  };
  void Attach(std::shared_ptr<FeatureModelTransformPort> port) {
    m_port = std::move(port);
  }
  AlignmentAdmission Begin(const DataGraphSnapshot &graph,
                           const DataRevisionRef &result, std::uint64_t id,
                           AlignmentCallback &callback,
                           const AlignmentConfig &config);
  bool Cancel(std::uint64_t requestId = 0,
              AlignmentStatus reason = AlignmentStatus::Cancelled);
  std::optional<Completion> Tick(const DataGraphSnapshot &graph);
  AlignmentModelPoseState GetState(const DataGraphSnapshot &graph) const;
  bool GetPending() const { return m_pending.has_value(); }
  void Clear() {
    m_port.reset();
    m_state = {};
  }

private:
  struct Pending {
    std::uint64_t id = 0, token = 0;
    ModelTransformSnapshot before;
    AlignmentCallback callback;
    DataRevisionRef transform;
    std::optional<AlignmentStatus> cancellation;
    std::chrono::steady_clock::time_point deadline;
  };
  bool GetCurrent(const DataGraphSnapshot &graph) const;
  std::shared_ptr<FeatureModelTransformPort> m_port;
  std::optional<Pending> m_pending;
  AlignmentModelPoseState m_state;
};
