param(
    [Parameter(Mandatory = $true)]
    [string]$Target,

    [switch]$Build,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'
$bootSource = Join-Path $buildDir 'EFI'
$asasSource = Join-Path $buildDir 'ASAS'

if ($Build) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1')
    if ($LASTEXITCODE -ne 0) {
        throw 'Build failed.'
    }
}

if (-not (Test-Path -LiteralPath $bootSource) -or -not (Test-Path -LiteralPath $asasSource)) {
    throw 'Build output was not found. Run build.ps1 first or pass -Build.'
}

if (-not (Test-Path -LiteralPath $Target)) {
    if (-not $Force) {
        throw 'Target does not exist. Create it first or pass -Force to create the directory.'
    }
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
}

$resolvedTarget = (Resolve-Path -LiteralPath $Target).Path
if ((Get-Item -LiteralPath $resolvedTarget).PSIsContainer -ne $true) {
    throw 'Target must be a directory or mounted FAT USB root.'
}

New-Item -ItemType Directory -Force -Path (Join-Path $resolvedTarget 'EFI') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $resolvedTarget 'ASAS') | Out-Null

Copy-Item -LiteralPath $bootSource -Destination $resolvedTarget -Recurse -Force
Copy-Item -LiteralPath $asasSource -Destination $resolvedTarget -Recurse -Force

$bootloader = Join-Path $resolvedTarget 'EFI\BOOT\BOOTX64.EFI'
$kernel = Join-Path $resolvedTarget 'ASAS\KERNEL.EFI'
$trampoline = Join-Path $resolvedTarget 'ASAS\APBOOT.BIN'

foreach ($required in @($bootloader, $kernel, $trampoline)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Install verification failed: missing $required"
    }
}

Write-Host "Asas OS installed to $resolvedTarget" -ForegroundColor Green
