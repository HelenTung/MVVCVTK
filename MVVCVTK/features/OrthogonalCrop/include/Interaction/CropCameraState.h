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
    // submit 事务的一次性值快照：SetCameraState 先清旧值再从 renderer 生产，
    // ResetCamera 成功写回后 Clear；无有效 renderer 时不持有任何 VTK 指针或所有权。
    CameraStateSnapshot m_cameraState;
};
