#include "AlignmentModelPose.h"
#include "AlignmentData.h"
#include "AlignmentMath.h"
#include "FeatureSupport/WorkLimit.h"

bool AlignmentModelPose::GetCurrent(const DataGraphSnapshot &graph) const {
  const auto result = AlignmentData::GetResult(graph, m_state.alignmentResult);
  return result && AlignmentData::GetExpectationsCurrent(
                       graph, result->record.expectations);
}
AlignmentAdmission AlignmentModelPose::Begin(const DataGraphSnapshot &graph,
                                             const DataRevisionRef &ref,
                                             std::uint64_t id,
                                             AlignmentCallback &callback,
                                             const AlignmentConfig &config) {
  if (!m_port)
    return {AlignmentAdmissionStatus::Unavailable, 0};
  if (m_pending)
    return {AlignmentAdmissionStatus::Busy, 0};
  const auto before = m_port->GetTransformState();
  if (!before)
    return {AlignmentAdmissionStatus::Unavailable, 0};
  if (before->editToken || before->hasPending)
    return {AlignmentAdmissionStatus::Busy, 0};
  const auto result = AlignmentData::GetResult(graph, ref);
  if (!result ||
      !AlignmentData::GetExpectationsCurrent(graph,
                                             result->record.expectations) ||
      result->record.diagnostics.status != AlignmentStatus::FullyDetermined ||
      !result->record.diagnostics.isQualityPassed)
    return {};
  const auto &input = result->record.input;
  if (before->dataRevision != input.source &&
      before->dataRevision != input.mesh)
    return {};
  const auto data = graph.view->GetData(before->dataRevision);
  const auto *image =
      data ? dynamic_cast<const ImageGrid3DPayload *>(data->payload.get())
           : nullptr;
  const auto *mesh =
      data ? dynamic_cast<const SurfaceMeshPayload *>(data->payload.get())
           : nullptr;
  if (image ? image->GetGeometry().coordinateFrame != input.coordinateFrame ||
                  input.unit == AlignmentUnit::Meter
            : !mesh || mesh->GetCoordinateFrame() != input.coordinateFrame)
    return {};
  const auto transformNode = graph.view->GetData(result->record.transform);
  const auto transform =
      transformNode ? std::dynamic_pointer_cast<const Transform3DPayload>(
                          transformNode->payload)
                    : nullptr;
  if (!transform || !AlignmentMath::Rigid(transform->GetSourceToTarget()))
    return {};
  AlignmentModelPoseState next;
  next.alignmentResult = ref;
  next.displayedData = before->dataRevision;
  next.modelToWorld = transform->GetSourceToTarget();
  next.status = AlignmentStatus::Unavailable;
  next.isPending = true;
  Pending pending;
  pending.id = id;
  pending.before = *before;
  pending.transform = result->record.transform;

  const auto token = m_port->StartTransform(*before);
  if (!token)
    return {AlignmentAdmissionStatus::Busy, 0};
  pending.token = *token;
  pending.callback = std::move(callback);
  m_pending = std::move(pending);
  m_state = std::move(next);
  try {
    if (!m_port->SetTransformCommit(*token, 1, m_state.modelToWorld))
      Cancel(id, AlignmentStatus::InternalError);
  } catch (...) {
    // 接纳后异常不能遗失 token/callback；下一 tick 根据 Host
    // 的实际提交事实完成。
    try {
      Cancel(id, AlignmentStatus::InternalError);
    } catch (...) {
    }
  }
  return {AlignmentAdmissionStatus::Accepted, id};
}
bool AlignmentModelPose::Cancel(std::uint64_t id, AlignmentStatus reason) {
  if (!m_pending || (id && id != m_pending->id))
    return false;
  if (!m_pending->cancellation)
    m_pending->cancellation = reason;
  // 取消请求已记录；冻结帧等暂时拒绝由下一 tick 重试，不能拒绝请求却随后取消。
  try { if(m_port)(void)m_port->StopTransform(m_pending->token); } catch(...) {}
  return true;
}
std::optional<AlignmentModelPose::Completion>
AlignmentModelPose::Tick(const DataGraphSnapshot &graph) {
  if (!m_pending || !m_port)
    return {};
  const auto host = m_port->GetTransformState();
  const bool current = GetCurrent(graph);
  if (host && host->editToken == m_pending->token) {
    if (!current)
      Cancel(0, AlignmentStatus::Stale);
    else if (m_pending->cancellation)
      Cancel(0, *m_pending->cancellation);
    return {};
  }
  auto pending = std::move(*m_pending);
  m_pending.reset();
  const bool committed =
      host && host->completedToken == pending.token &&
      host->completion == ModelTransformStatus::Committed &&
      host->sessionGeneration == pending.before.sessionGeneration &&
      host->dataRevision == pending.before.dataRevision &&
      host->bindingRevision == pending.before.bindingRevision &&
      host->modelToWorld == m_state.modelToWorld;
  const auto status =
      !current    ? AlignmentStatus::Stale
      : committed ? AlignmentStatus::FullyDetermined
                  : pending.cancellation.value_or(AlignmentStatus::Stale);
  m_state.status = status;
  m_state.isPending = false;
  m_state.isCurrent = committed && current;
  m_state.transformRevision = host ? host->transformRevision : 0;
  m_state.sessionGeneration = host ? host->sessionGeneration : 0;
  m_state.bindingRevision = host ? host->bindingRevision : 0;
  AlignmentResult value;
  value.requestId = pending.id;
  value.result = m_state.alignmentResult;
  value.transform = pending.transform;
  value.status = status;
  value.isModelPoseApplied = committed && current;
  return Completion{std::move(pending.callback), std::move(value)};
}
AlignmentModelPoseState
AlignmentModelPose::GetState(const DataGraphSnapshot &graph) const {
  auto state = m_state;
  const auto host = m_port ? m_port->GetTransformState() : std::nullopt;
  state.isCurrent = state.isCurrent && GetCurrent(graph) && host &&
                    host->dataRevision == state.displayedData &&
                    host->sessionGeneration == state.sessionGeneration &&
                    host->bindingRevision == state.bindingRevision &&
                    host->transformRevision == state.transformRevision &&
                    host->modelToWorld == state.modelToWorld &&
                    !host->editToken;
  return state;
}
