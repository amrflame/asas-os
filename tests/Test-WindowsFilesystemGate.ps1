param(
    [Parameter(Mandatory = $true)]
    [string]$NtfsPath,

    [Parameter(Mandatory = $true)]
    [string]$ExFatPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$ntfsTest = Join-Path $PSScriptRoot 'Test-NtfsMutationImage.ps1'
$exfatTest = Join-Path $PSScriptRoot 'Test-ExFatMutationImage.ps1'

if (-not (Get-Command Mount-VHD -ErrorAction SilentlyContinue)) {
    throw 'Mount-VHD is required. Run this gate on Windows with Hyper-V PowerShell tools enabled.'
}

if (-not (Get-Command chkdsk.exe -ErrorAction SilentlyContinue)) {
    throw 'chkdsk.exe is required for Windows filesystem compatibility gates.'
}

$ntfsImage = (Resolve-Path -LiteralPath $NtfsPath).Path
$exfatImage = (Resolve-Path -LiteralPath $ExFatPath).Path

Write-Host 'Running Windows NTFS compatibility gate...' -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File $ntfsTest -Path $ntfsImage
if ($LASTEXITCODE -ne 0) { throw 'NTFS Windows compatibility gate failed.' }

Write-Host 'Running Windows exFAT compatibility gate...' -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File $exfatTest -Path $exfatImage
if ($LASTEXITCODE -ne 0) { throw 'exFAT Windows compatibility gate failed.' }

Write-Host 'Windows filesystem compatibility gates passed.' -ForegroundColor Green

(Get-Item -LiteralPath $root) | Out-Null
