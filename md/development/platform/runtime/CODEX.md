# Codex 会话运行时适配

> 本文件只适配 md/development/core/COLLABORATION.core.md 第 13.6 节。角色、模型与思考强度使用项目配置，不在运行时层新增。

## 1. 配置目录与角色

Codex 配置根目录优先读取环境变量 CODEX_HOME；未设置时采用用户主目录下的 .codex，并确认目标文件存在。子代理使用 `<CODEX_HOME>/agent/default.toml` 的默认角色；配置值必须符合项目模型表，不能因配置文件出现其他角色而选用。

本机个人规则文件为 `<CODEX_HOME>/AGENTS.md`，只按 md/development/core/AGENTS.core.md 第 1.1 节补充未规定细节，不扩大项目范围或授权。

下级会话派发与原生机制的使用限制见[派发接口与会话生命周期](../../core/COLLABORATION.core.md#dispatch-interface)。

## 2. 免确认模式

Codex 必须使用 `full-access`。不使用 `auto` 或 `auto-review`；后者会调用表外的自动审批模型，不属于允许的模型集合。

模式落实、偏差处理与授权关系见[运行时权限规则](../../core/COLLABORATION.core.md#runtime-permissions)及[启动自检](../../core/LOOP.core.md#startup-check)；本适配只接受上述 `full-access` 值。

## 3. 必需配置与核验步骤

实际设置的证据与启动自检按[会话启动自检](../../core/LOOP.core.md#startup-check)执行；原生入口禁用能力的核验按[运行时权限规则](../../core/COLLABORATION.core.md#runtime-permissions)执行。
<!-- 规范文件结束：md/development/platform/runtime/CODEX.md -->
