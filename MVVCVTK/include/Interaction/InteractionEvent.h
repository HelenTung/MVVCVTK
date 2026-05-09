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
    // ── VTK 原始事件 ID（vtkCommand::LeftButtonPressEvent 等） ────────
    unsigned long vtkEventId = 0;

    // ── 原始 interactor 指针（供 Handler 在必要时做额外查询，通常不需要） ──
    // 注意：Handler 不应通过此指针修改 VTK 状态；中止标志由 Router 统一处理
    vtkRenderWindowInteractor* iren = nullptr;

    // ── 鼠标屏幕坐标（像素，左下角为原点，与 VTK 约定一致） ───────────
    int x = 0;
    int y = 0;

    // ── 键盘修饰键 ─────────────────────────────────────────────────────
    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    // ── 键盘按键（KeyPressEvent 时有效） ─────────────────────────────
    char        keyCode = 0;    // iren->GetKeyCode()，单字符（如 'm'）
    std::string keySym;         // iren->GetKeySym()，符号名（如 "Escape"）

    // ── 当前渲染/工具模式（由 RenderContext 在分发前填入） ─────────────
    VizMode  vizMode = VizMode::Volume;
    ToolMode toolMode = ToolMode::Navigation;
};