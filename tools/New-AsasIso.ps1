param(
    [string]$Output = '',        # if empty, auto-generate versioned name
    [string]$Label  = 'ASAS_OS', # volume label embedded in ISO
    [switch]$Build
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root      = Split-Path -Parent $PSScriptRoot
$buildDir  = Join-Path $root 'build'
$releasesDir = Join-Path $buildDir 'releases'
$isoBootImage = Join-Path $buildDir 'asas-iso-efi.img'

# ---- Version stamp ----
$stamp   = (Get-Date -Format 'yyyyMMdd-HHmmss')
$version = "v1.0-$stamp"

if ($Output -eq '') {
    New-Item -ItemType Directory -Force -Path $releasesDir | Out-Null
    $Output = Join-Path $releasesDir "asas-os-$version.iso"
}

if ($Build) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1')
    if ($LASTEXITCODE -ne 0) {
        throw 'Build failed.'
    }
}

$bootloader = Join-Path $buildDir 'EFI\BOOT\BOOTX64.EFI'
$kernel = Join-Path $buildDir 'ASAS\KERNEL.EFI'
$trampoline = Join-Path $buildDir 'ASAS\APBOOT.BIN'
$userProgram = Join-Path $buildDir 'sdk\HELLO.EXE'

foreach ($required in @($bootloader, $kernel, $trampoline, $userProgram)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Build output is missing: $required. Run build.ps1 first or pass -Build."
    }
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'New-BootImage.ps1') `
    -Bootloader $bootloader `
    -Kernel $kernel `
    -ApTrampoline $trampoline `
    -UserProgram $userProgram `
    -Output $isoBootImage `
    -TotalSectors 131072

if ($LASTEXITCODE -ne 0) {
    throw 'ISO boot image generation failed.'
}

$sectorSize = 2048
$catalogLba = 22
$bootImageLba = 23
$bootImageBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $isoBootImage).Path)
$bootImageSectors = [Math]::Ceiling($bootImageBytes.Length / $sectorSize)
$volumeSpaceSize = $bootImageLba + $bootImageSectors
$iso = New-Object byte[] ($volumeSpaceSize * $sectorSize)

function Set-Ascii([byte[]]$Buffer, [int]$Offset, [string]$Text, [int]$Length) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($Text.PadRight($Length).Substring(0, $Length))
    [Array]::Copy($bytes, 0, $Buffer, $Offset, $Length)
}

function Set-Le16([byte[]]$Buffer, [int]$Offset, [int]$Value) {
    $Buffer[$Offset] = $Value -band 0xFF
    $Buffer[$Offset + 1] = ($Value -shr 8) -band 0xFF
}

function Set-Be16([byte[]]$Buffer, [int]$Offset, [int]$Value) {
    $Buffer[$Offset] = ($Value -shr 8) -band 0xFF
    $Buffer[$Offset + 1] = $Value -band 0xFF
}

function Set-Both16([byte[]]$Buffer, [int]$Offset, [int]$Value) {
    Set-Le16 $Buffer $Offset $Value
    Set-Be16 $Buffer ($Offset + 2) $Value
}

function Set-Le32([byte[]]$Buffer, [int]$Offset, [long]$Value) {
    $Buffer[$Offset] = $Value -band 0xFF
    $Buffer[$Offset + 1] = ($Value -shr 8) -band 0xFF
    $Buffer[$Offset + 2] = ($Value -shr 16) -band 0xFF
    $Buffer[$Offset + 3] = ($Value -shr 24) -band 0xFF
}

function Set-Be32([byte[]]$Buffer, [int]$Offset, [long]$Value) {
    $Buffer[$Offset] = ($Value -shr 24) -band 0xFF
    $Buffer[$Offset + 1] = ($Value -shr 16) -band 0xFF
    $Buffer[$Offset + 2] = ($Value -shr 8) -band 0xFF
    $Buffer[$Offset + 3] = $Value -band 0xFF
}

function Set-Both32([byte[]]$Buffer, [int]$Offset, [long]$Value) {
    Set-Le32 $Buffer $Offset $Value
    Set-Be32 $Buffer ($Offset + 4) $Value
}

function Set-DirectoryRecord(
    [byte[]]$Buffer,
    [int]$Offset,
    [int]$Extent,
    [int]$DataLength,
    [byte]$Flags,
    [byte[]]$NameBytes
) {
    $length = 33 + $NameBytes.Length
    if (($NameBytes.Length % 2) -eq 0) {
        $length++
    }
    $Buffer[$Offset] = [byte]$length
    $Buffer[$Offset + 1] = 0
    Set-Both32 $Buffer ($Offset + 2) $Extent
    Set-Both32 $Buffer ($Offset + 10) $DataLength
    $Buffer[$Offset + 18] = 126
    $Buffer[$Offset + 19] = 1
    $Buffer[$Offset + 20] = 1
    $Buffer[$Offset + 21] = 0
    $Buffer[$Offset + 22] = 0
    $Buffer[$Offset + 23] = 0
    $Buffer[$Offset + 24] = 0
    $Buffer[$Offset + 25] = $Flags
    $Buffer[$Offset + 26] = 0
    $Buffer[$Offset + 27] = 0
    Set-Both16 $Buffer ($Offset + 28) 1
    $Buffer[$Offset + 32] = [byte]$NameBytes.Length
    [Array]::Copy($NameBytes, 0, $Buffer, $Offset + 33, $NameBytes.Length)
    return $length
}

# Primary Volume Descriptor.
$pvd = 16 * $sectorSize
$iso[$pvd] = 1
Set-Ascii $iso ($pvd + 1) 'CD001' 5
$iso[$pvd + 6] = 1
Set-Ascii $iso ($pvd + 8) 'ASAS OS' 32
Set-Ascii $iso ($pvd + 40) $Label 32
Set-Both32 $iso ($pvd + 80) $volumeSpaceSize
Set-Both16 $iso ($pvd + 120) 1
Set-Both16 $iso ($pvd + 124) 1
Set-Both16 $iso ($pvd + 128) $sectorSize
Set-Both32 $iso ($pvd + 132) 10
Set-Le32 $iso ($pvd + 140) 20
Set-Be32 $iso ($pvd + 148) 21
[void](Set-DirectoryRecord $iso ($pvd + 156) 19 $sectorSize 0x02 ([byte[]](0)))
Set-Ascii $iso ($pvd + 813) 'ASAS OS' 128
Set-Ascii $iso ($pvd + 318) 'ASAS_OS' 128

# Boot Record Volume Descriptor.
$brvd = 17 * $sectorSize
$iso[$brvd] = 0
Set-Ascii $iso ($brvd + 1) 'CD001' 5
$iso[$brvd + 6] = 1
Set-Ascii $iso ($brvd + 7) 'EL TORITO SPECIFICATION' 32
Set-Le32 $iso ($brvd + 71) $catalogLba

# Volume Descriptor Set Terminator.
$term = 18 * $sectorSize
$iso[$term] = 255
Set-Ascii $iso ($term + 1) 'CD001' 5
$iso[$term + 6] = 1

# Root directory and path tables.
$root = 19 * $sectorSize
$rootLen = Set-DirectoryRecord $iso $root 19 $sectorSize 0x02 ([byte[]](0))
[void](Set-DirectoryRecord $iso ($root + $rootLen) 19 $sectorSize 0x02 ([byte[]](1)))

$pathL = 20 * $sectorSize
$iso[$pathL] = 1
$iso[$pathL + 1] = 0
Set-Le32 $iso ($pathL + 2) 19
Set-Le16 $iso ($pathL + 6) 1
$iso[$pathL + 8] = 0

$pathM = 21 * $sectorSize
$iso[$pathM] = 1
$iso[$pathM + 1] = 0
Set-Be32 $iso ($pathM + 2) 19
Set-Be16 $iso ($pathM + 6) 1
$iso[$pathM + 8] = 0

# El Torito boot catalog.
$catalog = $catalogLba * $sectorSize
$iso[$catalog] = 1
$iso[$catalog + 1] = 0xEF
$iso[$catalog + 30] = 0x55
$iso[$catalog + 31] = 0xAA
$sum = 0
for ($index = 0; $index -lt 32; $index += 2) {
    if ($index -ne 28) {
        $word = [int]$iso[$catalog + $index] -bor ([int]$iso[$catalog + $index + 1] -shl 8)
        $sum = ($sum + $word) -band 0xFFFF
    }
}
$checksum = ((0x10000 - $sum) -band 0xFFFF)
Set-Le16 $iso ($catalog + 28) $checksum

$entry = $catalog + 32
$iso[$entry] = 0x88
$iso[$entry + 1] = 0
Set-Le16 $iso ($entry + 2) 0
$iso[$entry + 4] = 0
Set-Le16 $iso ($entry + 6) ([Math]::Min([int]($bootImageBytes.Length / 512), 65535))
Set-Le32 $iso ($entry + 8) $bootImageLba

[Array]::Copy($bootImageBytes, 0, $iso, $bootImageLba * $sectorSize, $bootImageBytes.Length)

$outputDirectory = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
[IO.File]::WriteAllBytes($Output, $iso)

# ---- Version tracking ----
$latestFile  = Join-Path $releasesDir 'latest.txt'
$versionsLog = Join-Path $releasesDir 'VERSIONS.txt'

$isoName = Split-Path -Leaf $Output
$logLine = "$stamp  $isoName  $($iso.Length) bytes"

[IO.File]::WriteAllText($latestFile, $isoName)
Add-Content -LiteralPath $versionsLog -Value $logLine

Write-Host "Created UEFI ISO for Hyper-V: $Output ($($iso.Length) bytes)" -ForegroundColor Green
Write-Host "Version : $version" -ForegroundColor Cyan
Write-Host "Latest  : $latestFile" -ForegroundColor Gray
Write-Host "Log     : $versionsLog" -ForegroundColor Gray
