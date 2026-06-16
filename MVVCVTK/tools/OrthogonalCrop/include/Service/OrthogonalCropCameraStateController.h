#pragma once

#include "AppTypes.h"

class vtkRenderer;

class OrthogonalCropCameraStateController {
public:
    void Save(vtkRenderer* renderer);
    void Restore(vtkRenderer* renderer);
    void Clear();
    bool HasSaved() const;

private:
    CameraStateSnapshot m_cameraState;
    bool m_hasCameraState = false;
};
