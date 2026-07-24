#pragma once
#include "BaseVisualStrategy.h" 
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>


// --- 组合策略: 体渲染/等值面 + 切片平面 ---
class CompositeStrategy : public BaseVisualStrategy {
public:
    CompositeStrategy(VizMode mode);

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetInputMask(
        vtkSmartPointer<vtkImageData> validityMask) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    void SetCamera(vtkSmartPointer<vtkRenderer> renderer);
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    int GetPlaneAxis(vtkActor* actor) override;
    vtkProp3D* GetMainProp() override; //
    bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect,
        RenderBindingUse bindingUse) override;
    bool DetachRenderEffect(const RenderEffect* effect) override;
    bool SetRenderInputStamp(RenderInputStamp inputStamp) override;
    bool SetRenderEffectUse(RenderBindingUse bindingUse) override;
    RenderEffectState GetRenderEffectState() const override;
    bool SetRenderEffectCommit(std::uint64_t revision) override;
    bool ClearRenderEffectStage(std::uint64_t revision) override;
private:
    std::shared_ptr<AbstractVisualStrategy> GetMainStrategy() { return m_mainStrategy; }

    // 组合策略强拥有主 3D 子策略；构造时按 m_mode 选择 Volume 或 IsoSurface，之后不在运行期替换。
    std::shared_ptr<AbstractVisualStrategy> m_mainStrategy;
    // 强拥有三向参考平面子策略；它与主策略接收同一输入和 RenderParams，但分别管理自己的 props。
    std::shared_ptr<AbstractVisualStrategy> m_referencePlanes;
    // 最近一次输入的强引用和身份缓存；只避免重复分发同一 VTK 对象，不冻结对象内部数据。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    // 构造期模式快照，仅决定主子策略类型：CompositeVolume 或 CompositeIsoSurface。
    VizMode m_mode;
};
