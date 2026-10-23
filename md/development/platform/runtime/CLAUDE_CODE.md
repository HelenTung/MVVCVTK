# Claude Code 会话运行时适配

> 本文件只适配 md/development/core/COLLABORATION.core.md 第 13.6 节。项目选择的角色、模型与思考强度见项目配置。

## 1. 免确认模式

Claude Code 必须使用 `bypassPermissions`；模式落实、偏差处理与授权关系见[运行时权限规则](../../core/COLLABORATION.core.md#runtime-permissions)。

## 2. 下级会话

Claude Code 自带的子代理、工作流入口包括 Agent、Workflow；其使用限制与下级会话派发见[派发接口与会话生命周期](../../core/COLLABORATION.core.md#dispatch-interface)。

## 3. 必需配置与核验步骤

实际设置的证据与启动自检按[会话启动自检](../../core/LOOP.core.md#startup-check)执行；原生入口禁用能力的核验按[运行时权限规则](../../core/COLLABORATION.core.md#runtime-permissions)执行。
<!-- 规范文件结束：md/development/platform/runtime/CLAUDE_CODE.md -->
