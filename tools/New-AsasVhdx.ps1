param(
    [string]$RawImage = (Join-Path $PSScriptRoot "..\build\asas-os.img"),
    [string]$Output   = (Join-Path $PSScriptRoot "..\build\asas-os.vhdx"),
    [switch]$Build
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Build) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}
if (-not (Test-Path -LiteralPath $RawImage)) { throw "Raw image not found. Run build.ps1 first or pass -Build." }
$outputDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$qemuImg = @("C:\Program Files\qemu\qemu-img.exe","C:\Program Files (x86)\qemu\qemu-img.exe") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $qemuImg) { try { $qemuImg = (Get-Command qemu-img.exe -ErrorAction Stop).Source } catch {} }
if (-not $qemuImg) { throw "qemu-img.exe not found." }
$tempVhdx = [IO.Path]::GetTempFileName() + ".vhdx"
Write-Host "Step 1: raw -> VHDX (qemu-img)..." -ForegroundColor Cyan
if (Test-Path -LiteralPath $tempVhdx) { Remove-Item -LiteralPath $tempVhdx -Force }
& $qemuImg convert -f raw -O vhdx -o subformat=fixed $RawImage $tempVhdx
if ($LASTEXITCODE -ne 0) { throw "qemu-img VHDX conversion failed." }
Write-Host "Step 2: stripping sparse flag..." -ForegroundColor Cyan
$data = [IO.File]::ReadAllBytes($tempVhdx)
Remove-Item -LiteralPath $tempVhdx -Force
if (Test-Path -LiteralPath $Output) { Remove-Item -LiteralPath $Output -Force }
[IO.File]::WriteAllBytes($Output, $data)
$f = Get-Item -LiteralPath $Output
$sizeMB = [Math]::Round($f.Length / 1MB, 1)
$sparse = ($f.Attributes -band [IO.FileAttributes]::SparseFile) -ne 0
if ($sparse) {
    Write-Host "WARNING: still sparse - Hyper-V may reject it." -ForegroundColor Red
} else {
    Write-Host ("VHDX created: {0} ({1} MB)  Sparse=False" -f $Output, $sizeMB) -ForegroundColor Green
}
Write-Host ""
Write-Host "Hyper-V Gen2 steps:" -ForegroundColor Yellow
Write-Host "  1. New VM > Generation 2 > Use existing virtual hard disk" -ForegroundColor Yellow
Write-Host ("  2. Browse to: {0}" -f $Output) -ForegroundColor Yellow
Write-Host "  3. Settings > Security > disable Secure Boot" -ForegroundColor Yellow
