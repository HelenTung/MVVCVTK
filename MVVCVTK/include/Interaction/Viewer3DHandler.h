#pragma once
#include "IInteractionHandler.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/NavigationBindings.h"
#include <cstdint>

class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer3DHandler — 处理 CompositeVolume / CompositeIsoSurface 模式下的交互
//
// 支持的交互：
//   默认左键拾取切片平面 → 按有效绑定启动，在 world 单轴约束下更新光标
//   拖拽期间降低渲染更新率（15 fps），释放后恢复静态高精度（0.001）
// ─────────────────────────────────────────────────────────────────────
class Viewer3DHandler : public IInteractionHandler
{
public:
    Viewer3DHandler(
        InteractionStatePort* statePort,
        SliceInputPort* slicePort,
        ModelInputPort* modelPort,
        RenderUpdatePort* updatePort,
        vtkPropPicker* picker,
        vtkRenderer* renderer,
        const NavigationBindings* bindings = nullptr);
    ~Viewer3DHandler() override;

    InteractionResult Send(const InteractionEvent& eve) override;

private:
    InteractionResult SetModelDrag(const InteractionEvent& event);
    // 非拥有观察指针；StdViewContext 持有 ports 与 VTK 对象，Router 重建会先销毁本 Handler。
    InteractionStatePort* m_statePort = nullptr;
    SliceInputPort* m_slicePort = nullptr;
    ModelInputPort* m_modelPort = nullptr;
    RenderUpdatePort* m_updatePort = nullptr;
    vtkPropPicker* m_picker = nullptr;
    vtkRenderer* m_renderer = nullptr;
    InteractionSource m_source;
    NavigationBindings m_defaultBindings;
    const NavigationBindings* m_bindings = nullptr;
    InteractionEventKind m_planeReleaseKind = InteractionEventKind::None;
    InteractionEventKind m_modelReleaseKind = InteractionEventKind::None;

    bool m_isDragging = false; // 命中参考平面后置位，由启动按钮的 release 清零
    int  m_dragAxis = -1;    // 单轴约束：0/1/2 为 world X/Y/Z，-1 表示未拖拽
    // 上一帧 VTK display 坐标，单位像素、左下角为原点；用于反投影鼠标增量。
    int  m_lastMouseX = 0;
    int  m_lastMouseY = 0;
    bool m_isModelDrag = false;
    bool m_isModelScale = false;
    std::uint64_t m_modelToken = 0;
    std::uint64_t m_modelSequence = 0;
    std::array<double, 16> m_modelStart{};
    std::array<double, 3> m_modelCenter{};
    std::array<double, 3> m_panStart{};
    double m_modelDepth = 0;
    int m_modelStartY = 0;
};
