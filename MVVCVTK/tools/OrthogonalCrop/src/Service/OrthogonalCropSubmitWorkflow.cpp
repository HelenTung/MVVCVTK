#include "OrthogonalCropSubmitWorkflow.h"

#include <iostream>

OrthogonalCropSubmitWorkflow::OrthogonalCropSubmitWorkflow(
    std::shared_ptr<OrthogonalCropInteractionBridgeService> bridge,
    ReloadSubmitter reloadSubmitter,
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<IVisualConfigService> visualConfigService)
    : m_bridge(std::move(bridge))
    , m_reloadSubmitter(std::move(reloadSubmitter))
    , m_dataMgr(std::move(dataMgr))
    , m_visualConfigService(std::move(visualConfigService))
{
}

void OrthogonalCropSubmitWorkflow::ApplySubmit()
{
    if (!m_bridge || !m_reloadSubmitter) {
        std::cerr << "[Main] Orthogonal crop submit failed: submit workflow is not ready." << std::endl;
        return;
    }

    OrthogonalCropSubmitReloadPayload payload;
    if (!m_bridge->BuildSubmitReloadPayload(payload)) {
        return;
    }

    m_reloadBuffer = payload.buffer;
    m_bridge->SetSubmitReloadStarted();

    std::weak_ptr<OrthogonalCropSubmitWorkflow> weakSelf;
    try {
        weakSelf = shared_from_this();
    }
    catch (const std::bad_weak_ptr&) {
        std::cerr << "[Main] Orthogonal crop submit failed: workflow must be owned by shared_ptr." << std::endl;
        m_reloadBuffer.reset();
        m_bridge->SetSubmitReloadFailed();
        return;
    }

    if (!m_reloadSubmitter(
            m_reloadBuffer->data(),
            payload.dims,
            payload.spacing,
            payload.origin,
            [weakSelf](bool success) {
                if (const auto self = weakSelf.lock()) {
                    self->HandleReloadComplete(success);
                }
            })) {
        std::cerr << "[Main] Orthogonal crop submit failed: reload request was rejected." << std::endl;
        m_reloadBuffer.reset();
        m_bridge->SetSubmitReloadFailed();
    }
}

void OrthogonalCropSubmitWorkflow::HandleReloadComplete(bool success)
{
    if (!m_bridge) {
        m_reloadBuffer.reset();
        return;
    }

    if (!success) {
        std::cerr << "[Main] Orthogonal crop submit reload failed." << std::endl;
        m_reloadBuffer.reset();
        m_bridge->SetSubmitReloadFailed();
        return;
    }

    if (m_dataMgr) {
        m_bridge->SetInputImage(m_dataMgr->GetVtkImage());
        const auto range = m_dataMgr->GetScalarRange();
        if (m_visualConfigService) {
            m_visualConfigService->SetIsoThreshold(range[0] + (range[1] - range[0]) * 0.55);
        }
    }

    std::cout << "[Main] Orthogonal crop submit applied to main image data." << std::endl;
    m_reloadBuffer.reset();
    m_bridge->SetSubmitReloadSynced();
}
