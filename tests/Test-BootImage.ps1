param(
    [Parameter(Mandatory = $true)]
    [string]$Image
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Image))

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) {
        throw "Boot image check failed: $message"
    }
}

function Read-Ascii([int]$offset, [int]$length) {
    return [Text.Encoding]::ASCII.GetString($bytes, $offset, $length)
}

function Read-UInt16([int]$offset) {
    return [BitConverter]::ToUInt16($bytes, $offset)
}

function Read-UInt32([int]$offset) {
    return [BitConverter]::ToUInt32($bytes, $offset)
}

Assert ($bytes[510] -eq 0x55 -and $bytes[511] -eq 0xAA) 'missing boot signature'
Assert ((Read-Ascii 82 8) -eq 'FAT32   ') 'filesystem marker is not FAT32'

$bytesPerSector = Read-UInt16 11
$sectorsPerCluster = $bytes[13]
$reservedSectors = Read-UInt16 14
$fatCount = $bytes[16]
$sectorsPerFat = Read-UInt32 36
$rootCluster = Read-UInt32 44
$totalSectors = Read-UInt32 32
Assert ($bytes.Length -eq $totalSectors * $bytesPerSector) 'image size does not match BPB'
Assert ($totalSectors -ge 131072) 'FAT32 image is below the supported 64 MiB size'
$dataStartSector = $reservedSectors + $fatCount * $sectorsPerFat
$rootOffset = ($dataStartSector + ($rootCluster - 2) * $sectorsPerCluster) * $bytesPerSector
Assert ((Read-Ascii $rootOffset 11) -eq 'EFI        ') 'EFI directory is missing'
Assert ((Read-Ascii ($rootOffset + 32) 11) -eq 'ASAS       ') 'ASAS directory is missing'
Assert ((Read-Ascii ($rootOffset + 64) 11) -eq 'DISK    TXT') 'DISK.TXT entry is missing'
Assert ((Read-Ascii ($rootOffset + 96) 11) -eq 'HELLO   EXE') 'HELLO.EXE entry is missing'

$dataStart = $dataStartSector * $bytesPerSector
$bootDirOffset = $dataStart + 2 * $sectorsPerCluster * $bytesPerSector
$asasDirOffset = $dataStart + 3 * $sectorsPerCluster * $bytesPerSector
Assert ((Read-Ascii ($bootDirOffset + 64) 11) -eq 'BOOTX64 EFI') 'BOOTX64.EFI entry is missing'
Assert ((Read-Ascii ($asasDirOffset + 64) 11) -eq 'KERNEL  EFI') 'KERNEL.EFI entry is missing'
Assert ((Read-Ascii ($asasDirOffset + 96) 11) -eq 'APBOOT  BIN') 'APBOOT.BIN entry is missing'
Assert ($bytes[$asasDirOffset + 128 + 11] -eq 0x0F) 'README long-file-name entry is missing'
Assert ((Read-Ascii ($asasDirOffset + 192) 11) -eq 'README  TXT') 'README.TXT entry is missing'

Write-Host 'Boot image checks passed.' -ForegroundColor Green
