# Paseo 派发平台适配

> 本文件只实现 core/COLLABORATION.core.md 第 13.5 节的抽象接口，不定义角色、模型、授权或裁决规则。项目选择值见 AGENTS.md 第 0、13.1 节。

## 1. 接口映射

| core 能力 | Paseo 接口或参数 |
| --- | --- |
| 显式创建会话 | `create_agent`；`provider` 为提供方/模型，`settings.thinkingOptionId` 为思考强度，`settings.modeId` 为运行时权限模式；提供中文 `title` 与自包含 `initialPrompt` |
| 创建后核实实际配置 | `get_agent_status`；核对 provider、model、thinkingOptionId/effectiveThinkingOptionId、runtimeInfo 和 currentModeId |
| 后台会话 | 不传 `workspaceId`；下级会话属于当前会话，只显示在子代理栏，不作为普通前台会话标签页出现 |
| 完成、出错、需许可通知 | `notifyOnFinish: true`；等待自动通知 |
| 状态和活动查询 | `get_agent_status`、`list_agents`、`get_agent_activity` |
| 续接与配置纠正 | `send_agent_prompt`；`update_agent.settings`，纠正后再核实 |
| 归档 | `archive_agent`；归档会中断仍在运行的会话并移出活动列表，因此只在满足 core 归档条件后调用 |

下级会话派发与宿主、模型原生机制的使用限制见 core/COLLABORATION.core.md 第 13.5 节，本表落实其创建参数。权限模式从相应会话运行时文件读取，不使用平台默认模式替代项目要求。

会话管理权限与查询范围见 core/COLLABORATION.core.md 第 13.5 节。

## 2. 工作区与工具行为

派发、检查、审查、测试和验证均不调用 `create_workspace`、`paseo workspace create` 或 `paseo run --new-workspace`。文件隔离按 core/GIT_WORKFLOW.core.md 第 2.1 节创建 Git 工作树。用户明确要求的 schedule 可能自行生成会话或工作区，授权范围按 core/COLLABORATION.core.md 第 13.5 节。

接口缺失、配置无法落实或创建后无法核实，按 core 的“执行者不可用”处理；不得换模型、换派发途径绕过。API 请求参数只证明请求，实际快照才支持相应配置结论；全局事实的结论范围按 AGENTS.core.md 第 1.2 节。

## 4. 必需配置与核验步骤

创建参数与核实字段按第 1 节填写；提供方、模型与思考强度取项目表，权限模式取实际会话运行时适配文件。不得用提供方默认值代替显式派发参数。

核验按 core/COLLABORATION.core.md 第 13.5 节：创建后用 get_agent_status 的实际快照逐项比对，必要时纠正并复核；旧会话状态不能证明新会话。模式或模型无法落实时按第 2 节处理。本文件不记录本机默认配置值、设置位置快照或待填项，也不把请求值或配置文本当作运行态事实。
