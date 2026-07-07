#pragma once

#include "AppTypes.h"

class vtkRenderer;

class OrthogonalCropCameraStateController {
public:
    void SetCameraState(vtkRenderer* renderer);
    void ResetCamera(vtkRenderer* renderer);
    void Clear();
    bool GetSaved() const;

private:
    CameraStateSnapshot m_cameraState;
    bool m_hasCameraState = false;
};
