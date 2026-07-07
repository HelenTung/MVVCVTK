#pragma once

#include "AppTypes.h"

class vtkRenderer;

class CropCameraState {
public:
    void SetCameraState(vtkRenderer* renderer);
    void ResetCamera(vtkRenderer* renderer);
    void Clear();
    bool GetSaved() const;

private:
    CameraStateSnapshot m_cameraState;
    bool m_hasCamera = false;
};
