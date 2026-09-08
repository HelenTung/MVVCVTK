<#
.SYNOPSIS
使用可追溯RAW输入运行仓内Feature审计，结果留在本工作树out目录。
.DESCRIPTION
InputManifest为JSON，schemaVersion=2，dataKind="real"。
必需字段：sampleId、provenance、path、sha256、dimensions[3]、spacing[3]、
origin[3]、direction[9]、coordinateFrame="LPS"、unit="mm"、
storage="float32-le-xfastest"。
caseFile/caseSha256 指向现有 Qt 自动化 JSON 用例及其哈希；第一步必须是当前 RAW 的 Data.Load。
metricChecks数组每项：record、operationId、key、minimum、maximum、referencePath、referenceSha256。
key 是该操作 result 中的点分路径；不再解析已删除的 standalone 控制台输出。
record使用AUDIT_THRESHOLD/PART/GAP/SURFACE/ALIGNMENT/ARTIFACT。
无参考时仅显式AllowIntrinsicOnly可运行，仍不产生精度验收通过声明。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputManifest,
    [string]$RepoRoot,
    [string]$DepsRoot,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateRange(1,3600)][int]$TimeoutSeconds = 600,
    [ValidateRange(16,131072)][int]$ToolBudgetMiB = 4096,
    [switch]$AllowIntrinsicOnly,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$culture = [Globalization.CultureInfo]::InvariantCulture
if (-not $RepoRoot) { $RepoRoot = Join-Path $PSScriptRoot '..' }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if (-not $DepsRoot) { $DepsRoot = Join-Path $RepoRoot 'deps/official' }
$DepsRoot = [IO.Path]::GetFullPath($DepsRoot)
$outputRoot = Join-Path $RepoRoot ('out/feature-real-audit/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $outputRoot)
$reportPath = Join-Path $outputRoot 'result.json'
$report = [ordered]@{
    schemaVersion=2; status='BLOCKED'; sourceHead=''; sourceStatus=@()
    manifest=''; manifestHash=''; inputHash=''; configuration=$Configuration
    processExit=$null; elapsedMs=0; sampleIntervalMs=25; memorySamples=0
    peakPrivateBytes=0L; peakWorkingSetBytes=0L; metricChecks=@()
    limitations=@(
        '裁切入口验证不等于非空裁切有效域到DefX的精度验收；该组合仍受G1限制',
        'NIST数据、SDK安装态和未声明的工业精度不在本次进程运行的完成声明内',
        '采样峰值可能漏掉短于采样间隔的峰；不等于算法预算或全部GPU占用'
    )
}
$process = $null
$inputLease = $null
$referenceLeases = New-Object 'System.Collections.Generic.List[System.IDisposable]'
$originalPath = $env:PATH
$originalQtPluginPath = $env:QT_PLUGIN_PATH
$exitCode = 3
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Read-Numbers($Values, [int]$Count, [bool]$Positive) {
    Require (@($Values).Count -eq $Count) "Expected $Count numeric components"
    foreach ($value in $Values) {
        $number = [double]$value
        Require (-not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)) 'Non-finite metadata'
        Require ((-not $Positive) -or $number -gt 0) 'Expected positive metadata'
        $number
    }
}
function Join-Numbers($Values) { return (@($Values | ForEach-Object { ([double]$_).ToString('R',$culture) }) -join ',') }
function Resolve-InputPath([string]$Value, [string]$Directory) {
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path $Directory $Value))
}
try {
    $manifestPath = [IO.Path]::GetFullPath($InputManifest)
    Require (Test-Path -LiteralPath $manifestPath -PathType Leaf) 'Acquisition manifest is missing'
    $referenceLeases.Add([IO.File]::Open($manifestPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
    $manifest = Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
    Require ($manifest.schemaVersion -eq 2 -and $manifest.dataKind -eq 'real') 'Manifest must identify an actual real acquisition'
    Require (-not [string]::IsNullOrWhiteSpace($manifest.sampleId) -and
        -not [string]::IsNullOrWhiteSpace($manifest.provenance)) 'Sample identity and acquisition provenance are required'
    Require ($manifest.coordinateFrame -eq 'LPS' -and $manifest.unit -eq 'mm') 'Host input geometry must be LPS/mm'
    Require ($manifest.storage -eq 'float32-le-xfastest') 'Unsupported RAW storage contract'
    $dimensions = @(Read-Numbers $manifest.dimensions 3 $true)
    $spacing = @(Read-Numbers $manifest.spacing 3 $true)
    $origin = @(Read-Numbers $manifest.origin 3 $false)
    foreach ($value in $spacing) { Require ($value -le [single]::MaxValue -and [single]$value -gt 0) 'Spacing exceeds Host float storage' }
    foreach ($value in $origin) { Require ([math]::Abs($value) -le [single]::MaxValue) 'Origin exceeds Host float storage' }
    $direction = @(Read-Numbers $manifest.direction 9 $false)
    [decimal]$expectedBytes = 4
    foreach ($dimension in $dimensions) {
        Require ($dimension -eq [math]::Floor($dimension) -and $dimension -le [int]::MaxValue) 'Dimensions must be positive int32 values'
        $expectedBytes *= [decimal]$dimension
    }
    for ($column = 0; $column -lt 3; ++$column) {
        for ($other = 0; $other -lt 3; ++$other) {
            $dot = 0.0
            for ($row = 0; $row -lt 3; ++$row) { $dot += $direction[$row*3+$column] * $direction[$row*3+$other] }
            $expected = if ($column -eq $other) { 1.0 } else { 0.0 }
            Require ([math]::Abs($dot-$expected) -le 1e-6) 'Direction must be orthonormal'
        }
    }
    $inputPath = Resolve-InputPath $manifest.path (Split-Path -Parent $manifestPath)
    Require (Test-Path -LiteralPath $inputPath -PathType Leaf) 'RAW input is missing'
    Require ((Get-Item -LiteralPath $inputPath).Length -eq $expectedBytes) 'RAW byte count does not match geometry/storage'
    Require ($manifest.sha256 -match '^[0-9A-Fa-f]{64}$') 'Expected SHA256 is required'
    # 在校验和实际加载之间保持只读租约，防止RAW内容被并发改写。
    $inputLease = [IO.File]::Open($inputPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $actualHash = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
    Require ($actualHash -eq $manifest.sha256) 'RAW content hash mismatch'
    $report['acquisition'] = $manifest
    $report.manifest = $manifestPath
    $report.manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    $report.inputHash = $actualHash
    $casePath = Resolve-InputPath $manifest.caseFile (Split-Path -Parent $manifestPath)
    Require (Test-Path -LiteralPath $casePath -PathType Leaf) 'Qt automation case is missing'
    $referenceLeases.Add([IO.File]::Open($casePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
    Require ($manifest.caseSha256 -match '^[0-9A-Fa-f]{64}$' -and
        (Get-FileHash -LiteralPath $casePath -Algorithm SHA256).Hash -eq $manifest.caseSha256) 'Qt case hash mismatch'
    $case = Get-Content -LiteralPath $casePath -Encoding UTF8 -Raw | ConvertFrom-Json
    Require (@($case.steps).Count -gt 0) 'Qt case must contain actual operations'
    $first = $case.steps[0]
    Require ($first.module -eq 'Data' -and $first.action -eq 'Load' -and $first.expectStatus -eq 'Succeeded') 'First operation must load the declared real input'
    $input = $first.parameters
    Require ([IO.Path]::GetFullPath($input.filePath) -ieq $inputPath -and
        $input.datasetId -eq $manifest.sampleId -and $input.sourceDigest -eq $actualHash -and
        $input.evidenceKind -in @('real-data','real-data-derived-roi')) 'Case acquisition identity mismatch'
    foreach ($pair in @(@('dimensions','dimensions'),@('spacingLPS','spacing'),@('originLPS','origin'),@('directionLPS','direction'))) {
        Require ((Join-Numbers $input.($pair[0])) -ceq (Join-Numbers $manifest.($pair[1]))) "Case geometry mismatch: $($pair[0])"
    }
    foreach ($module in @('Crop','Gap','Part','PartEdit','Surface','Rotation','Alignment','Artifact')) {
        Require (@($case.steps | Where-Object { $_.module -eq $module }).Count -gt 0) "Case omits a required Feature: $module"
    }
    $report['casePath'] = $casePath
    $report['caseHash'] = $manifest.caseSha256
    $checks = @()
    if ($manifest.PSObject.Properties.Name -contains 'metricChecks') { $checks = @($manifest.metricChecks) }
    $records = @('AUDIT_THRESHOLD','AUDIT_PART','AUDIT_GAP','AUDIT_SURFACE','AUDIT_ALIGNMENT','AUDIT_ARTIFACT')
    if (-not $AllowIntrinsicOnly) {
        foreach ($record in $records) {
            Require (@($checks | Where-Object { $_.record -eq $record }).Count -gt 0) "Missing independent reference checks: $record"
        }
    }
    foreach ($check in $checks) {
        Require ($records -contains $check.record) 'Unknown metric record'
        Require ($check.key -match '^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*|\.[0-9]+)*$') 'Invalid metric path'
        Require ([string]$check.operationId -match '^[1-9][0-9]*$') 'Metric operationId is required'
        $limits = @(Read-Numbers @($check.minimum,$check.maximum) 2 $false)
        Require ($limits[0] -le $limits[1]) 'Invalid reference tolerance interval'
        $reference = Resolve-InputPath $check.referencePath (Split-Path -Parent $manifestPath)
        Require (Test-Path -LiteralPath $reference -PathType Leaf) 'Independent reference file is missing'
        Require ($check.referenceSha256 -match '^[0-9A-Fa-f]{64}$') 'Reference SHA256 is required'
        $referenceLeases.Add([IO.File]::Open($reference,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read))
        Require ((Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash -eq $check.referenceSha256) 'Reference hash mismatch'
    }
    Push-Location $RepoRoot
    try {
        $report.sourceHead = (& git rev-parse HEAD).Trim()
        Require ($LASTEXITCODE -eq 0) 'Cannot identify tested source'
        $report.sourceStatus = @(& git status --porcelain --untracked-files=all)
        & git diff --binary HEAD -- . ':!out' > (Join-Path $outputRoot 'source.patch')
        @(& git ls-files --others --exclude-standard) | ForEach-Object {
            Get-FileHash -LiteralPath $_ -Algorithm SHA256 | Select-Object Path,Hash
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'untracked-source-hashes.json') -Encoding UTF8
    } finally { Pop-Location }
    if ($ValidateOnly) {
        $report.status = 'INPUT VERIFIED; EXECUTION NOT RUN'
        $exitCode = 0
    } else {
        $binaryDir = Join-Path $RepoRoot 'out/build/vs2026-x64'
        $cachePath = Join-Path $binaryDir 'CMakeCache.txt'
        Require (Test-Path -LiteralPath $cachePath -PathType Leaf) 'Configured build cache is missing'
        $cache = @{}
        foreach ($line in Get-Content -LiteralPath $cachePath) {
            if ($line -match '^([^/#][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
        }
        Require ($cache.ContainsKey('CMAKE_HOME_DIRECTORY') -and
            [IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']) -ieq $RepoRoot.TrimEnd('\','/')) 'Build cache belongs to another source tree'
        Require ($cache.ContainsKey('MVVCVTK_DEPS_ROOT') -and
            [IO.Path]::GetFullPath($cache['MVVCVTK_DEPS_ROOT']) -ieq $DepsRoot.TrimEnd('\','/')) 'Build dependency root mismatch'
        foreach ($feature in @('ORTHOGONAL_CROP','GAP_ANALYSIS','MODEL_ROTATION','PART_SEGMENTATION',
            'SURFACE_DETERMINATION','METROLOGY_ALIGNMENT','ARTIFACT_REDUCTION','QT_TESTING')) {
            Require ($cache.ContainsKey("MVVCVTK_BUILD_$feature") -and $cache["MVVCVTK_BUILD_$feature"] -in @('ON','TRUE','1')) "Required Feature is not configured: $feature"
        }
        # 先从已核验的配置源码重建公共入口，再记录二进制哈希。
        # 旧可执行文件与新源码diff并列不能构成当前实现的运行证据。
        $buildLog = Join-Path $outputRoot 'build.log'
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & cmake --build $binaryDir --config $Configuration --target qt_feature_tests --parallel 6 *> $buildLog
            $buildExit = $LASTEXITCODE
        } finally { $ErrorActionPreference = $previousPreference }
        Require ($buildExit -eq 0) "Qt automation rebuild failed: $buildLog"
        $report['cacheHash'] = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash
        $report['dependencyLockHash'] = (Get-FileHash -LiteralPath (Join-Path $RepoRoot 'tools/MVVCVTK.Dependencies.lock.psd1') -Algorithm SHA256).Hash
        $executable = Join-Path $binaryDir "bin/$Configuration/QtFeatureTests.exe"
        Require (Test-Path -LiteralPath $executable -PathType Leaf) 'Build the configured Qt automation first'
        $report['executableHash'] = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        $recordPath = Join-Path $outputRoot 'operations.json'
        $arguments = @('--case',$casePath,'--record',$recordPath,'--memory-budget-mib',"$ToolBudgetMiB")
        $env:QT_PLUGIN_PATH = Join-Path $DepsRoot 'qt/plugins'
        foreach ($argument in $arguments) { Require ($argument -notmatch '["\r\n]') 'Quotes/control characters are not supported in audit arguments' }
        $quoted = @($arguments | ForEach-Object { '"' + $_ + '"' })
        $env:PATH = (Join-Path $DepsRoot 'vtk/bin') + ';' + (Join-Path $DepsRoot 'opencv/x64/vc16/bin') + ';' +
            (Join-Path $DepsRoot 'qt/bin') + ';' + $env:PATH
        $report['arguments'] = $arguments
        $report.status = 'RUNNING'
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $executable -ArgumentList $quoted -WorkingDirectory $outputRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $outputRoot 'stdout.log') -RedirectStandardError (Join-Path $outputRoot 'stderr.log')
        while (-not $process.WaitForExit(25)) {
            $process.Refresh()
            if (-not $process.HasExited) {
                $report.memorySamples++
                $report.peakPrivateBytes = [math]::Max($report.peakPrivateBytes,$process.PrivateMemorySize64)
                $report.peakWorkingSetBytes = [math]::Max($report.peakWorkingSetBytes,$process.WorkingSet64)
            }
            if ($watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
                Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
                throw 'Audit process exceeded the explicit deadline'
            }
        }
        $process.WaitForExit()
        $report.processExit = $process.ExitCode
        $report.elapsedMs = $watch.Elapsed.TotalMilliseconds
        $report.status = 'FAILED'
        $exitCode = 1
        Require ($process.ExitCode -eq 0) 'Feature audit process failed'
        $actual = Get-Content -LiteralPath $recordPath -Encoding UTF8 -Raw | ConvertFrom-Json
        Require ($actual.codeHead -eq $report.sourceHead -and $actual.buildConfig -eq $Configuration) 'Qt record belongs to another source/configuration'
        $completed = @($actual.records)
        Require ($completed.Count -ge $case.steps.Count -and $completed.Count -gt 0) 'Missing operation records'
        foreach ($entry in $completed) {
            Require ($entry.isTerminal -and $entry.completeCount -eq 1) 'Operation did not finish exactly once'
        }
        $modules = @{AUDIT_THRESHOLD='Surface';AUDIT_PART='Part';AUDIT_GAP='Gap';AUDIT_SURFACE='Surface';AUDIT_ALIGNMENT='Alignment';AUDIT_ARTIFACT='Artifact'}
        foreach ($check in $checks) {
            $entries = @($completed | Where-Object { $_.operationId -eq [string]$check.operationId })
            Require ($entries.Count -eq 1 -and $entries[0].module -eq $modules[$check.record]) 'Metric does not identify its actual Feature operation'
            Require ($entries[0].status -in @('Succeeded','Observed','Ready','PreviewReady','FullyDetermined')) 'Metric operation did not succeed'
            $value = $entries[0].result
            foreach ($component in ($check.key -split '\.')) {
                if ($value -is [array] -and $component -match '^[0-9]+$') {
                    Require ([int]$component -lt $value.Count) 'Metric array index is missing'
                    $value = $value[[int]$component]
                } else {
                    Require ($null -ne $value -and $value.PSObject.Properties.Name -contains $component) 'Metric field is missing'
                    $value = $value.$component
                }
            }
            $number = [double]::Parse([string]$value,$culture)
            $passed = -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number) -and
                $number -ge [double]$check.minimum -and $number -le [double]$check.maximum
            $report.metricChecks += @{record=$check.record;operationId=$check.operationId;key=$check.key;value=$number;passed=$passed}
            Require $passed "Reference mismatch: $($check.record).$($check.key)=$number"
        }
        $report['operations'] = $completed.Count
        $report['operationRecordHash'] = (Get-FileHash -LiteralPath $recordPath -Algorithm SHA256).Hash
        $report.status = if ($AllowIntrinsicOnly) { 'INTRINSIC PASSED; ACCURACY NOT VERIFIED' } else { 'DECLARED METRIC CHECKS PASSED; LIMITED SCOPE' }
        $exitCode = 0
    }
} catch {
    if ($process) { $report.status = 'FAILED'; $exitCode = 1 }
    $report['failure'] = $_.Exception.Message
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
} finally {
    if ($process) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue; $process.WaitForExit() }
        $process.Dispose()
    }
    if ($inputLease) { $inputLease.Dispose() }
    foreach ($lease in $referenceLeases) { $lease.Dispose() }
    $env:PATH = $originalPath
    $env:QT_PLUGIN_PATH = $originalQtPluginPath
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Output "REAL_AUDIT_REPORT=$reportPath"
}
exit $exitCode
