# Claude Code 会话运行时适配

> 本文件只适配 md/development/core/COLLABORATION.core.md 第 13.6 节。项目选择的角色、模型与思考强度见项目配置。

## 1. 免确认模式

Claude Code 必须使用 `bypassPermissions`，由项目派发平台显式设置并核实实际模式。无法落实按 md/development/core/COLLABORATION.core.md 第 13.1 节处理；免确认与授权的关系见该文件第 13.6 节。

## 2. 下级会话

Claude Code 自带的子代理、工作流入口包括 Agent、Workflow；其使用限制与下级会话派发见 md/development/core/COLLABORATION.core.md 第 13.5 节。

## 3. 必需配置与核验步骤

权限模式与宿主机制禁用要求分别按第 1、2 节落实。通过项目派发平台显式设置并取得实际快照，依 md/development/core/LOOP.core.md 第 0.1 节记录启动自检结果。

宿主提供可核实的禁用能力时，在已授权范围内关闭相关入口并核对实际工具表；没有可核实的关闭能力时，只能靠规则约束，不声称工具已不可调用。本文件不记录本机配置值或设置位置，配置声明也不能代替实际模式或工具可用性证据。
