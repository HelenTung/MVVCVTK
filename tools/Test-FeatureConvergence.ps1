[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$DepsRoot,
    [string]$DefXRoot,
    [ValidateSet('Debug','Release')][string[]]$Configuration = @('Debug','Release'),
    [string[]]$Case = @('all','host-only','only-OrthogonalCrop','only-GapAnalysis',
        'only-ModelRotation','only-PartSegmentation','only-SurfaceDetermination',
        'only-MetrologyAlignment','only-ArtifactReduction','without-OrthogonalCrop',
        'without-GapAnalysis','without-ModelRotation','without-PartSegmentation',
        'without-SurfaceDetermination','without-MetrologyAlignment','without-ArtifactReduction'),
    [switch]$InventoryOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $RepoRoot) { $RepoRoot = Join-Path $PSScriptRoot '..' }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot 'CMakePresets.json'))) { throw 'Invalid repository root' }
if (-not $DepsRoot) { $DepsRoot = Join-Path $RepoRoot 'deps/official' }
if (-not $DefXRoot) { $DefXRoot = Join-Path $RepoRoot 'deps/third_party/defx' }
$DepsRoot = [IO.Path]::GetFullPath($DepsRoot)
$DefXRoot = [IO.Path]::GetFullPath($DefXRoot)
$features = [ordered]@{
    OrthogonalCrop='ORTHOGONAL_CROP'; GapAnalysis='GAP_ANALYSIS'; ModelRotation='MODEL_ROTATION'
    PartSegmentation='PART_SEGMENTATION'; SurfaceDetermination='SURFACE_DETERMINATION'
    MetrologyAlignment='METROLOGY_ALIGNMENT'; ArtifactReduction='ARTIFACT_REDUCTION'
}
$minimum = @{
    OrthogonalCrop=1; GapAnalysis=4; ModelRotation=2; PartSegmentation=7
    SurfaceDetermination=4; MetrologyAlignment=2; ArtifactReduction=3
}
$cases = @{ 'all'=@($features.Keys); 'host-only'=@() }
foreach ($feature in $features.Keys) {
    $cases["only-$feature"] = @($feature)
    $cases["without-$feature"] = @($features.Keys | Where-Object { $_ -ne $feature })
}
foreach ($name in $Case) { if (-not $cases.ContainsKey($name)) { throw "Unknown matrix case: $name" } }
$outputRoot = Join-Path $RepoRoot ('out/feature-convergence/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $outputRoot)
$results = New-Object 'System.Collections.Generic.List[object]'
$report = [ordered]@{
    schemaVersion=1; sourceHead=''; sourceStatus=@(); lockHash=''
    worktree=$RepoRoot; dependencies=$DepsRoot; defx=$DefXRoot
    configurations=$Configuration; cases=$Case; results=$results
    realDataStatus='NOT RUN: use Invoke-FeatureRealAudit.ps1 with verified acquisition metadata and references'
    sdkStatus='NOT RUN: approved SDK scope must be reconciled before release/install validation'
}
$reportPath = Join-Path $outputRoot 'matrix-results.json'
function Save-Report {
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding UTF8
}
function Invoke-Checked([string]$Executable, [string[]]$Arguments, [string]$Log) {
    # Windows PowerShell 5 会把原生stderr封装为ErrorRecord；捕获后检查
    # 原生退出码，避免诊断输出绕过失败报告。
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments *> $Log
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExit -ne 0) { throw "$Executable failed ($nativeExit); see $Log" }
}
function Assert-Cache([string]$BinaryDir, [hashtable]$Expected) {
    $cachePath = Join-Path $BinaryDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) { throw "Missing configured cache: $cachePath" }
    $cache = @{}
    foreach ($line in Get-Content -LiteralPath $cachePath) {
        if ($line -match '^([^/#][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    foreach ($key in @('CMAKE_HOME_DIRECTORY','CMAKE_CACHEFILE_DIR','MVVCVTK_DEPS_ROOT','MVVCVTK_DEFX_ROOT')) {
        $wanted = switch ($key) {
            'CMAKE_HOME_DIRECTORY' { $RepoRoot }; 'CMAKE_CACHEFILE_DIR' { $BinaryDir }
            'MVVCVTK_DEPS_ROOT' { $DepsRoot }; 'MVVCVTK_DEFX_ROOT' { $DefXRoot }
        }
        if (-not $cache.ContainsKey($key) -or
            [IO.Path]::GetFullPath($cache[$key]).TrimEnd('\','/') -ine [IO.Path]::GetFullPath($wanted).TrimEnd('\','/')) {
            throw "Cache path mismatch: $key in $BinaryDir"
        }
    }
    foreach ($key in $Expected.Keys) {
        if (-not $cache.ContainsKey($key) -or $cache[$key] -cne $Expected[$key]) {
            throw "Cache option mismatch: $key in $BinaryDir"
        }
    }
    foreach ($config in $Configuration) {
        if (-not $cache.ContainsKey('CMAKE_CONFIGURATION_TYPES') -or
            ($cache['CMAKE_CONFIGURATION_TYPES'] -split ';') -notcontains $config) {
            throw "Configuration missing from cache: $config"
        }
    }
}
Push-Location $RepoRoot
try {
    $report.sourceHead = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source HEAD' }
    $report.sourceStatus = @(& git status --porcelain --untracked-files=all)
    $report.lockHash = (Get-FileHash -LiteralPath tools/MVVCVTK.Dependencies.lock.psd1 -Algorithm SHA256).Hash
    # 保存当前源码输入差异；未跟踪源码另列哈希，不把工作树名字当作被测状态。
    & git diff --binary HEAD -- . ':!out' > (Join-Path $outputRoot 'source.patch')
    $untracked = @(& git ls-files --others --exclude-standard)
    $sourceHashes = @($untracked | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        ForEach-Object { Get-FileHash -LiteralPath $_ -Algorithm SHA256 | Select-Object Path,Hash })
    $sourceHashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'untracked-source-hashes.json') -Encoding UTF8
    foreach ($name in $Case) {
        $enabled = @($cases[$name])
        # 七个显式开关组成稳定的短目录ID，完整名称仍保留在报告中。
        # 较长的工作树和配置名会超过MSBuild临时tlog的路径限制。
        $featureMask = 0
        $featureBit = 1
        foreach ($feature in $features.Keys) {
            if ($enabled -contains $feature) { $featureMask = $featureMask -bor $featureBit }
            $featureBit = $featureBit -shl 1
        }
        $binaryDir = if ($name -eq 'all') { Join-Path $RepoRoot 'out/build/vs2026-x64' }
            else { Join-Path $RepoRoot ('out/matrix/' + $featureMask.ToString('x2')) }
        $testFlag = if ($enabled.Count) { 'ON' } else { 'OFF' }
        $manual = if ($name -eq 'all') { 'ON' } else { 'OFF' }
        $qt = if ($name -eq 'all') { 'ON' } else { 'OFF' }
        $arguments = @('--preset','vs2026-x64','-B',$binaryDir,
            "-DMVVCVTK_DEPS_ROOT=$DepsRoot","-DMVVCVTK_DEFX_ROOT=$DefXRoot",
            "-DMVVCVTK_BUILD_TESTING=$testFlag","-DMVVCVTK_BUILD_QT_TESTING=$qt",
            "-DMVVCVTK_BUILD_QT_MANUAL=$manual",'-DMVVCVTK_ALIGNMENT_REFERENCE_ROOT=','-DMVVCVTK_REAL_AUDIT_MANIFEST=')
        $expected = @{
            MVVCVTK_BUILD_TESTING=$testFlag; MVVCVTK_BUILD_QT_TESTING=$qt
            MVVCVTK_BUILD_QT_MANUAL=$manual; MVVCVTK_ALIGNMENT_REFERENCE_ROOT=''
            MVVCVTK_REAL_AUDIT_MANIFEST=''
        }
        foreach ($feature in $features.Keys) {
            $flag = if ($enabled -contains $feature) { 'ON' } else { 'OFF' }
            $arguments += "-DMVVCVTK_BUILD_$($features[$feature])=$flag"
            $expected["MVVCVTK_BUILD_$($features[$feature])"] = $flag
        }
        $entry = [ordered]@{ case=$name; binaryDir=$binaryDir; enabled=$enabled; configure=$arguments; status='RUNNING'; configurations=@() }
        $results.Add($entry)
        Save-Report
        Write-Output "MATRIX case=$name"
        if (-not $InventoryOnly) { Invoke-Checked cmake $arguments (Join-Path $outputRoot "$name-configure.log") }
        Assert-Cache $binaryDir $expected
        foreach ($config in $Configuration) {
            $logPrefix = Join-Path $outputRoot "$name-$config"
            if (-not $InventoryOnly) {
                Invoke-Checked cmake @('--build',$binaryDir,'--config',$config,'--parallel','6') "$logPrefix-build.log"
            }
            if ($enabled.Count -eq 0) {
                $entry.configurations += @{ configuration=$config; status=$(if($InventoryOnly){'NOT RUN'}else{'BUILD PASSED'}); tests=0 }
                continue
            }
            $inventoryText = & ctest --test-dir $binaryDir -C $config --show-only=json-v1
            if ($LASTEXITCODE -ne 0) { throw "Cannot read inventory: $name/$config" }
            $inventoryText | Set-Content -LiteralPath "$logPrefix-inventory.json" -Encoding UTF8
            $inventory = ($inventoryText -join "`n") | ConvertFrom-Json
            foreach ($feature in $features.Keys) {
                $pattern = '^' + [regex]::Escape($feature) + '\.'
                $registered = @($inventory.tests | Where-Object { $_.name -match $pattern }).Count
                if ($enabled -contains $feature) {
                    if ($registered -lt $minimum[$feature]) { throw "Missing required tests: $feature/$registered" }
                } elseif ($registered -ne 0) { throw "Disabled feature has tests: $feature" }
            }
            foreach ($required in @('Host.StandaloneInput') + $(if($name -eq 'all') {
                @('QtFeature.Automation','QtHost.Methods','QtHost.Scheduling')
            } else { @() })) {
                if (@($inventory.tests | Where-Object { $_.name -eq $required }).Count -ne 1) { throw "Missing test: $required" }
            }
            if ($InventoryOnly) {
                $entry.configurations += @{ configuration=$config; status='INVENTORY ONLY'; tests=@($inventory.tests).Count }
                continue
            }
            Invoke-Checked ctest @('--test-dir',$binaryDir,'-C',$config,'--output-on-failure','--no-tests=error',
                '--test-output-size-passed','10485760','--test-output-size-failed','10485760',
                '--output-junit',"$logPrefix-results.xml") "$logPrefix-tests.log"
            $fullLog = Join-Path $binaryDir 'Testing/Temporary/LastTest.log'
            if (Test-Path -LiteralPath $fullLog -PathType Leaf) {
                Copy-Item -LiteralPath $fullLog -Destination "$logPrefix-full.log"
            }
            [xml]$junit = Get-Content -LiteralPath "$logPrefix-results.xml" -Raw
            $testcases = @($junit.SelectNodes('//testcase'))
            if ($testcases.Count -ne @($inventory.tests).Count -or
                $junit.SelectNodes('//testcase/skipped | //testcase/failure | //testcase/error').Count -ne 0 -or
                @($testcases | Where-Object { $_.GetAttribute('status') -ne 'run' }).Count -ne 0) {
                throw "Required tests were incomplete/failed/skipped: $name/$config"
            }
            foreach ($test in $inventory.tests) {
                if (@($testcases | Where-Object { $_.GetAttribute('name') -ceq $test.name }).Count -ne 1) {
                    throw "Executed test identity mismatch: $($test.name)"
                }
            }
            foreach ($suite in $junit.SelectNodes('//testsuite')) {
                foreach ($attribute in @('disabled','skipped','failures','errors')) {
                    if ($suite.HasAttribute($attribute) -and [int]$suite.GetAttribute($attribute) -ne 0) {
                        throw "JUnit $attribute is nonzero: $name/$config"
                    }
                }
            }
            $entry.configurations += @{ configuration=$config; status='PASSED'; tests=@($inventory.tests).Count }
            Save-Report
        }
        $entry.status = if ($InventoryOnly) { 'INVENTORY ONLY' } else { 'PASSED' }
        Save-Report
    }
} catch {
    if ($results.Count) { $results[$results.Count - 1].status = 'FAILED' }
    $report['failure'] = $_.Exception.Message
    Save-Report
    throw
} finally {
    Save-Report
    Pop-Location
    Write-Output "MATRIX_REPORT=$reportPath"
}
