#pragma once

#include "AppTypes.h"  // VizMode, ToolMode

#include <string>

// Interaction 层只传播稳定语义；VTK 原始事件编号由 RenderContext 在适配边界统一转换。
enum class InteractionEventKind
{
    None,
    Timer,
    WheelForward,
    WheelBackward,
    PrimaryPress,
    PrimaryRelease,
    SecondaryPress,
    SecondaryRelease,
    PointerMove,
    KeyPress,
    KeyRelease,
    TextInput,
    ViewInteraction,
    Exit
};

// ─────────────────────────────────────────────────────────────────────
// InteractionEvent — Router 内部流通的统一事件结构体
//
// 由 RenderContext 从底层输入对象提取字段并填充，随后交给
// InteractionRouter::Dispatch 分发给各 Handler。
//
// 设计约束：
//   • 纯数据，无虚函数；keySym 由 std::string 自主管理文本内存
//   • Handler 通过此结构体获取全部输入快照，不依赖底层工具包对象
// ─────────────────────────────────────────────────────────────────────
struct InteractionEvent
{
    InteractionEventKind eventKind = InteractionEventKind::None;

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
//   isHandled            = true → Router 在 FirstMatch 模式下停止向后传递
//   isPropagationStopped = true → RenderContext 应阻止底层事件继续传播
// ─────────────────────────────────────────────────────────────────────
struct InteractionResult
{
    bool isHandled = false; // true 表示已消费；FirstMatch 据此停止，Broadcast 仅做 OR 聚合
    bool isPropagationStopped = false; // true 由 Router 做 OR 聚合，Context 随后停止底层传播
};
