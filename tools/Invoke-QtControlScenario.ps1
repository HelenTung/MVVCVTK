<#
.SYNOPSIS
在 Qt 进程内模拟真实控件点击，运行阶段主动作脚本并保存记录。
.DESCRIPTION
InputParameters 为 Data.Load 参数 JSON。窗口不抢占桌面焦点，尺寸固定。
真实数据只证明所覆盖操作链；名义参考及物理精度验收由独立输入和参考确定。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputParameters,
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [string]$BinaryDir,
    [string]$Scenario,
    [string]$OutputDir,
    [ValidateRange(1,2147483647)][int]$StepTimeoutMs=900000,
    [switch]$Build
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $BinaryDir){$BinaryDir=Join-Path $repoRoot 'out/build/vs2026-x64-cuda'}
if(-not $Scenario){$Scenario=Join-Path $repoRoot 'test/QtHost/Automation/Scenarios/business-primary.json'}
if(-not $OutputDir){$OutputDir=Join-Path $repoRoot ('out/qt-control-scenarios/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))}
$BinaryDir=[IO.Path]::GetFullPath($BinaryDir);$OutputDir=[IO.Path]::GetFullPath($OutputDir)
$inputPath=(Resolve-Path -LiteralPath $InputParameters).Path
$parameters=Get-Content -LiteralPath $inputPath -Raw -Encoding UTF8 | ConvertFrom-Json
$rawPath=(Resolve-Path -LiteralPath $parameters.filePath).Path
$beforeHash=(Get-FileHash -LiteralPath $rawPath -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedDigest=if($parameters.PSObject.Properties.Name -contains 'sourceDigest'){[string]$parameters.sourceDigest}else{''}
if($expectedDigest -and $expectedDigest.ToLowerInvariant() -ne $beforeHash){throw 'RAW 摘要与输入参数不一致'}
$case=Get-Content -LiteralPath (Resolve-Path -LiteralPath $Scenario).Path -Raw -Encoding UTF8 | ConvertFrom-Json
$load=@($case.steps | Where-Object { $_.PSObject.Properties.Name -contains 'action' -and $_.module -eq 'Data' -and $_.action -eq 'Load' })
if($load.Count -ne 1){throw '控件脚本必须有唯一 Data.Load 步骤'}
$parameters.filePath=$rawPath;$load[0] | Add-Member -NotePropertyName parameters -NotePropertyValue $parameters -Force
foreach($step in $case.steps){if(-not ($step.PSObject.Properties.Name -contains 'timeoutMs')){$step | Add-Member -NotePropertyName timeoutMs -NotePropertyValue $StepTimeoutMs}}
[void](New-Item -ItemType Directory -Force -Path $OutputDir)
$casePath=Join-Path $OutputDir 'resolved-case.json'
$caseText=($case | ConvertTo-Json -Depth 30).Replace('@Output/',$OutputDir.Replace('\','/')+'/')
[IO.File]::WriteAllText($casePath,$caseText,(New-Object Text.UTF8Encoding($false)))
if($Build){
    & cmake --build $BinaryDir --config $Configuration --target qt_feature_tests --parallel 6
    if($LASTEXITCODE -ne 0){throw 'Qt 控件脚本执行程序构建失败'}
}
$launcher=Join-Path $BinaryDir ('RunQtFeatureTests-'+$Configuration+'.cmd')
if(-not (Test-Path -LiteralPath $launcher)){throw '缺少已配置的 Qt 测试启动入口'}
$record=Join-Path $OutputDir 'control-record.json'
$log=Join-Path $OutputDir 'control-run.log'
Push-Location $OutputDir
try { & $launcher --background --case $casePath --record $record *> $log;$runExit=$LASTEXITCODE }
finally { Pop-Location }
$afterHash=(Get-FileHash -LiteralPath $rawPath -Algorithm SHA256).Hash.ToLowerInvariant()
if($afterHash -ne $beforeHash){throw '测试期间原始输入内容发生变化'}
$dimensions=if($parameters.PSObject.Properties.Name -contains 'dimensions'){$parameters.dimensions}else{$null}
$summary=[ordered]@{exitCode=$runExit;configuration=$Configuration;binaryDir=$BinaryDir;caseFile=$casePath;recordFile=$record;logFile=$log;inputFile=$rawPath;inputBytes=(Get-Item -LiteralPath $rawPath).Length;inputDimensions=$dimensions;inputSha256=$beforeHash;originalInputUnchanged=$true}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDir 'run-summary.json') -Encoding UTF8
Write-Output ($summary | ConvertTo-Json)
if($runExit -ne 0){Get-Content -LiteralPath $log -Tail 30;exit $runExit}
