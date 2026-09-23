param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$RoiPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSha256
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Real input bindings test executable is missing: $Executable"
}
if (-not (Test-Path -LiteralPath $RoiPath -PathType Leaf)) {
    throw "Real input bindings ROI is missing: $RoiPath"
}
$inputStream = [System.IO.File]::OpenRead($RoiPath)
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $actualSha256 = [System.BitConverter]::ToString(
        $sha256.ComputeHash($inputStream)).Replace('-', '').ToLowerInvariant()
}
finally {
    $sha256.Dispose()
    $inputStream.Dispose()
}
if ($actualSha256 -ine $ExpectedSha256) {
    throw "Real input bindings ROI SHA-256 mismatch: $actualSha256"
}
Write-Output "real_input_bindings_roi_sha256=$($actualSha256.ToLowerInvariant())"
& $Executable --real-input-bindings $RoiPath
exit $LASTEXITCODE
