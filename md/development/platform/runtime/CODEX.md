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

## 4. 读取规范的命令

在 Windows 上通过 `exec_command` 读取规范时，设置 `login: false`，PowerShell 命令首句设置 `[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;`（也可用 `[System.Text.UTF8Encoding]::new($false)`），并用 `Get-Content -Encoding UTF8` 解码。每次输出约不超过 6000 个字符，`max_output_tokens` 可设为 8000；若仍截断，缩小分段或增大输出预算。以下模板从仓库根目录执行，均以 `login: false` 调用。

全文读取示例（`CODEX.md`）：

```powershell
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;
$path = 'md/development/platform/runtime/CODEX.md'
$text = Get-Content -LiteralPath $path -Raw -Encoding UTF8
$offset = 0 # 后续调用依次改为 6000、12000……，直到输出末段
$text.Substring($offset, [Math]::Min(6000, $text.Length - $offset))
```

节级读取示例（`AGENTS.md#load-gate`，同时带出文件导言和文件结束标记供首次节读核对）：

```powershell
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;
$path = 'AGENTS.md'
$unit = 'gate'
$source = Get-Content -LiteralPath $path -Raw -Encoding UTF8
$begin = "<!-- 规范节开始：${path}#load-$unit -->"
$end = "<!-- 规范节结束：${path}#load-$unit -->"
$fileEnd = "<!-- 规范文件结束：$path -->"
$start = $source.IndexOf($begin)
$stop = $source.IndexOf($end)
$tail = $source.LastIndexOf($fileEnd)
if ($start -lt 0 -or $stop -lt $start -or $tail -lt $stop) { throw '未找到有效的规范标记' }
$intro = $source.Substring(0, $source.IndexOf('<!-- 规范节开始：'))
$text = $intro + $source.Substring($start, $stop + $end.Length - $start) + "`n" + $fileEnd
$offset = 0 # 后续调用依次改为 6000、12000……，直到输出末段
$text.Substring($offset, [Math]::Min(6000, $text.Length - $offset))
```

读取范围及加载确认仍以[根 AGENTS.md 的开工闸门](../../../../AGENTS.md#load-gate)为准。出现中文乱码或工具输出截断时，用上述参数和模板补读受影响范围；补读时调整 `$offset` 与分段大小，按开工闸门要求核对正文和结束标记。

<!-- 规范文件结束：md/development/platform/runtime/CODEX.md -->
