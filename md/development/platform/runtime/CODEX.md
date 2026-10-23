# Codex 会话运行时适配

> 本文件只适配 md/development/core/COLLABORATION.core.md 第 13.6 节。角色、模型与思考强度使用项目配置，不在运行时层新增。

## 1. 配置目录与角色

Codex 配置根目录优先读取环境变量 CODEX_HOME；未设置时采用用户主目录下的 .codex，并确认目标文件存在。子代理使用 `<CODEX_HOME>/agent/default.toml` 的默认角色；配置值必须符合项目模型表，不能因配置文件出现其他角色而选用。

本机个人规则文件为 `<CODEX_HOME>/AGENTS.md`，只按 md/development/core/AGENTS.core.md 第 1.1 节补充未规定细节，不扩大项目范围或授权。

下级会话派发与原生机制的使用限制见 md/development/core/COLLABORATION.core.md 第 13.5 节。执行者在会话内不一定能看到自己的启动设置，创建后的实际核实由派发者负责。

启动自检与派发方实际快照的使用见 md/development/core/LOOP.core.md 第 0.1 节，不能以会话内不可见为由跳过自检，也不要求固定向协调者重复索取已有核实结果。

## 2. 免确认模式

Codex 必须使用 `full-access`。不使用 `auto` 或 `auto-review`；后者会调用表外的自动审批模型，不属于允许的模型集合。

启动默认值为 `auto-review` 或 `auto` 时，派发者在首轮运行前显式设为 `full-access`，创建后核实实际模式。发现偏差先纠正并复核；当前会话无法自行切换时向派发者报告，暂停依赖正确模式的工具操作。配置能力缺失按 md/development/core/COLLABORATION.core.md 第 13.1 节处理，不启用自动审批替代。免确认与授权的关系见该文件第 13.6 节。

## 3. 必需配置与核验步骤

权限模式按第 2 节显式设置并从实际会话快照核实；下级会话只按第 1 节创建。原生机制如有可核实的关闭能力，在已授权范围内关闭并核对实际工具表；没有此能力时，只能靠规则约束，不能把规则禁止写成工具已经不可用。

启动自检按 md/development/core/LOOP.core.md 第 0.1 节。本文件不记录本机配置值、功能开关快照或设置位置，配置文件文本不能代替实际运行结果。
<!-- 规范文件结束：md/development/platform/runtime/CODEX.md -->
