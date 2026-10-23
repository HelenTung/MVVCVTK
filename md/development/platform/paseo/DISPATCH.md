# Paseo 派发平台适配

> 本文件只实现 md/development/core/COLLABORATION.core.md 第 13.5 节的抽象接口，不定义角色、模型、授权或裁决规则。项目选择值见 AGENTS.md 第 0、13.1 节。

## 1. 接口映射

| core 能力 | Paseo 接口或参数 |
| --- | --- |
| 显式创建会话 | `create_agent`；`provider` 为提供方/模型，`settings.thinkingOptionId` 为思考强度，`settings.modeId` 为运行时权限模式；提供中文 `title` 与自包含 `initialPrompt` |
| 创建后核实实际配置 | `get_agent_status`；核对 provider、model、thinkingOptionId/effectiveThinkingOptionId、runtimeInfo 和 currentModeId |
| 后台会话 | 不传 `workspaceId`；下级会话属于当前会话，只显示在子代理栏，不作为普通前台会话标签页出现 |
| 完成、出错、需许可通知 | `notifyOnFinish: true` |
| 状态和活动查询 | `get_agent_status`、`list_agents`、`get_agent_activity` |
| 续接与配置纠正 | `send_agent_prompt`；`update_agent.settings`，纠正后再核实 |
| 归档 | `archive_agent`；归档会中断仍在运行的会话并移出活动列表 |

本表落实[派发接口与会话生命周期](../../core/COLLABORATION.core.md#dispatch-interface)；权限模式参数取当前会话运行时适配文件，管理与查询范围按[会话权限与逐级裁决](../../core/COLLABORATION.core.md#load-session-boundaries)核对。

## 2. 工作区与工具行为

派发、检查、审查、测试和验证均不调用 `create_workspace`、`paseo workspace create` 或 `paseo run --new-workspace`。文件隔离按 md/development/core/GIT_WORKFLOW.core.md 第 2.1 节创建 Git 工作树。用户明确要求的 schedule 可能自行生成会话或工作区，授权范围按 md/development/core/COLLABORATION.core.md 第 13.5 节。

能力缺失与配置核验失败的处理见[派发接口与会话生命周期](../../core/COLLABORATION.core.md#dispatch-interface)及其引用的“执行者不可用”规则。

<!-- 规范文件结束：md/development/platform/paseo/DISPATCH.md -->
