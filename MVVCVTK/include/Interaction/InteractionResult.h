#pragma once

// ───────────��─────────────────────────────────────────────────────────
// InteractionResult — Handler::Send() 的返回值
//
//   isHandled  = true  → Router 在 FirstMatch 模式下停止向后传递
//   hasVtkAbort = true  → RenderContext 应对当前 VTK 事件调用 SetAbortFlag(1)
//                       以阻止 VTK 默认的 interactor style 继续处理
// ─────────────────────────────────────────────────────────────────────
struct InteractionResult
{
    bool isHandled = false;  // 是否已消费此事件
    bool hasVtkAbort = false;  // 是否中止 VTK 默认事件传播
};