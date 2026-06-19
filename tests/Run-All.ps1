param(
    [switch]$Clean,
    [int]$QemuTimeoutSeconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $root 'build.ps1'
$qemuTest = Join-Path $PSScriptRoot 'Run-QemuBootTest.ps1'

if ($Clean) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -Clean
} else {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript
}
if ($LASTEXITCODE -ne 0) {
    throw 'Build failed.'
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $qemuTest -TimeoutSeconds $QemuTimeoutSeconds
if ($LASTEXITCODE -ne 0) {
    throw 'QEMU boot test failed.'
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'Test-Fat32FsInfo.ps1') `
    -Image (Join-Path $root 'build\asas-data.img')
if ($LASTEXITCODE -ne 0) {
    throw 'FAT32 FSInfo validation failed.'
}

Write-Host 'All automated QEMU tests passed.' -ForegroundColor Green
