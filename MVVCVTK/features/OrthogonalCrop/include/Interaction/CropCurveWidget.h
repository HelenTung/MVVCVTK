#pragma once
#include "OrthogonalCropTypes.h"
#include <functional>
#include <memory>

class vtkHandleWidget;
class vtkRenderer;
class vtkRenderWindowInteractor;

// 模型几何是唯一真源；手柄和轮廓通过完整显示仿射投影，保留剪切与镜像。
class CropCurveWidget final {
public:
    CropCurveWidget();
    ~CropCurveWidget();
    CropCurveWidget(const CropCurveWidget&)=delete;
    CropCurveWidget& operator=(const CropCurveWidget&)=delete;
    void SetContext(vtkRenderWindowInteractor* interactor,vtkRenderer* renderer);
    bool SetGeometry(CropOpItem operation,const CropMatrixDouble16Array& modelToWorld);
    CropOpItem GetGeometry() const;
    bool GetTransformSame(const CropMatrixDouble16Array& matrix) const;
    void SetContextGate(std::function<bool()> gate);
    void SetCallback(std::function<void(CropInteractionPhase)> callback);
    void SetRenderCallback(std::function<void()> callback);
    bool SetEnabled(bool enabled);
    bool GetEnabled() const;
    // Private feature controller integration point; never part of the SDK surface.
    vtkHandleWidget* GetHandle(std::size_t index) const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
