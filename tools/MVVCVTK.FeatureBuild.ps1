# Feature 名称与开关取自实际 add_subdirectory，供配置审计和发行共用。
function Get-MVVCVTKFeatureSwitches([string]$RepoRoot) {
    $source = Get-Content -LiteralPath (Join-Path $RepoRoot 'MVVCVTK/features/CMakeLists.txt') -Raw
    $features = [ordered]@{}
    foreach ($match in [regex]::Matches($source, 'if\((MVVCVTK_BUILD_[A-Z_]+)\)\s+add_subdirectory\((\w+)\)')) {
        $name = $match.Groups[2].Value
        if ($features.Contains($name) -or $features.Values -contains $match.Groups[1].Value) { throw 'Duplicate Feature build switch' }
        $features[$name] = $match.Groups[1].Value
    }
    if (-not $features.Count) { throw 'No Feature build switches discovered' }
    return $features
}
