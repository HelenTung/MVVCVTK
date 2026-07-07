#include "Interaction/CropCameraState.h"
#include <vtkCamera.h>
#include <vtkRenderer.h>

void CropCameraState::SetCameraState(vtkRenderer* renderer)
{
    m_cameraState = {};
    if (!renderer || !renderer->GetActiveCamera()) {
        m_hasCameraState = false;
        return;
    }

    auto* camera = renderer->GetActiveCamera();
    camera->GetPosition(m_cameraState.position.data());
    camera->GetFocalPoint(m_cameraState.focalPoint.data());
    camera->GetViewUp(m_cameraState.viewUp.data());
    camera->GetClippingRange(m_cameraState.clippingRange.data());
    m_cameraState.parallelScale = camera->GetParallelScale();
    m_cameraState.viewAngle = camera->GetViewAngle();
    m_cameraState.parallelProjection = camera->GetParallelProjection() != 0;
    m_cameraState.isValid = true;
    m_hasCameraState = true;
}

void CropCameraState::ResetCamera(vtkRenderer* renderer)
{
    if (!m_hasCameraState || !m_cameraState.isValid || !renderer || !renderer->GetActiveCamera()) {
        return;
    }

    auto* camera = renderer->GetActiveCamera();
    camera->SetPosition(
        m_cameraState.position[0],
        m_cameraState.position[1],
        m_cameraState.position[2]);
    camera->SetFocalPoint(
        m_cameraState.focalPoint[0],
        m_cameraState.focalPoint[1],
        m_cameraState.focalPoint[2]);
    camera->SetViewUp(
        m_cameraState.viewUp[0],
        m_cameraState.viewUp[1],
        m_cameraState.viewUp[2]);
    camera->SetParallelScale(m_cameraState.parallelScale);
    camera->SetViewAngle(m_cameraState.viewAngle);
    camera->SetParallelProjection(m_cameraState.parallelProjection ? 1 : 0);
    renderer->ResetCameraClippingRange();
    Clear();
}

void CropCameraState::Clear()
{
    m_cameraState = {};
    m_hasCameraState = false;
}

bool CropCameraState::GetSaved() const
{
    return m_hasCameraState;
}
