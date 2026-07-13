#pragma once

#include "AppTypes.h"  // VizMode, ToolMode

#include <string>

class vtkRenderWindowInteractor;

// ─────────────────────────────────────────────────────────────────────
// InteractionEvent — Router 内部流通的统一事件结构体
//
// 由 RenderContext（StdRenderContext 或未来的 QtRenderContext）负责
// 从 vtkRenderWindowInteractor 中提取字段并填充，
// 随后交给 InteractionRouter::Dispatch 分发给各 Handler。
//
// 设计约束：
//   • 纯数据，无虚函数，无动态内存
//   • Handler 通过此结构体获取所有信息，不应再裸访问 iren
// ─────────────────────────────────────────────────────────────────────
struct InteractionEvent
{
    // ── VTK 原始事件 ID（vtkCommand::LeftButtonPressEvent 等）；0 表示未填充 ──
    unsigned long vtkEventId = 0;

    // ── 非拥有 interactor 观察指针，仅在当前 Dispatch 调用期间有效 ──────────
    // Handler 通常不需要查询它，且不得借此修改 VTK 状态；中止标志由 Router 统一处理。
    vtkRenderWindowInteractor* iren = nullptr;

    // ── 鼠标屏幕坐标（像素，左下角为原点，与 VTK 约定一致） ───────────
    int x = 0;
    int y = 0;

    // ── 当前事件的键盘修饰键快照；true 表示分发时对应按键处于按下状态 ──────
    bool isShiftDown = false;
    bool isCtrlDown = false;
    bool isAltDown = false;

    // ── 键盘按键（KeyPressEvent 时有效） ─────────────────────────────
    char        keyCode = 0;    // iren->GetKeyCode()，单字符输入；具体功能键由宿主映射
    std::string keySym;         // iren->GetKeySym()，非字符按键的符号名；具体含义由宿主映射

    // ── RenderContext 在分发前写入的模式快照 ───────────────────────────
    VizMode  vizMode = VizMode::Volume; // 决定 2D/3D Handler 的视图分支
    ToolMode toolMode = ToolMode::Navigation; // 决定导航或模型变换路径
};

// ─────────────────────────────────────────────────────────────────────
// InteractionResult — Handler::Send() 的返回值
//
//   isHandled  = true  → Router 在 FirstMatch 模式下停止向后传递
//   hasVtkAbort = true  → RenderContext 应对当前 VTK 事件调用 SetAbortFlag(1)
//                       以阻止 VTK 默认的 interactor style 继续处理
// ─────────────────────────────────────────────────────────────────────
struct InteractionResult
{
    bool isHandled = false; // true 表示已消费；FirstMatch 据此停止，Broadcast 仅做 OR 聚合
    bool hasVtkAbort = false; // true 由 Router 做 OR 聚合，Context 随后中止 VTK 默认传播
};
