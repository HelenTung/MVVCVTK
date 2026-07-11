#pragma once

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
