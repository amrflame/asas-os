param(
    [Parameter(Mandatory = $true)]
    [string]$Bootloader,

    [Parameter(Mandatory = $true)]
    [string]$Kernel,

    [Parameter(Mandatory = $true)]
    [string]$ApTrampoline,

    [Parameter(Mandatory = $true)]
    [string]$UserProgram,

    [Parameter(Mandatory = $true)]
    [string]$Output,

    [int]$TotalSectors = 131072,
    [ValidateSet(512, 1024, 2048, 4096)]
    [int]$BytesPerSector = 512
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$bytesPerSector   = $BytesPerSector
$totalSectors     = $TotalSectors
$sectorsPerCluster = 1            # Keeps the volume above the FAT32 cluster threshold.
$reservedSectors  = 32            # FAT32 standard minimum
$fatCount         = 2
$sectorsPerFat32  = 1024          # enough entries for the 64 MiB volume
$rootCluster      = 2             # FAT32 root starts at cluster 2
$dataStartSector  = $reservedSectors + $fatCount * $sectorsPerFat32
$image = New-Object byte[] ($totalSectors * $bytesPerSector)

function Set-UInt16([int]$offset, [int]$value) {
    $image[$offset] = $value -band 0xFF
    $image[$offset + 1] = ($value -shr 8) -band 0xFF
}

function Set-UInt32([int]$offset, [long]$value) {
    $image[$offset] = $value -band 0xFF
    $image[$offset + 1] = ($value -shr 8) -band 0xFF
    $image[$offset + 2] = ($value -shr 16) -band 0xFF
    $image[$offset + 3] = ($value -shr 24) -band 0xFF
}

function Set-Ascii([int]$offset, [string]$text, [int]$length) {
    $encoded = [Text.Encoding]::ASCII.GetBytes($text.PadRight($length).Substring(0, $length))
    [Array]::Copy($encoded, 0, $image, $offset, $length)
}

function Set-Fat32Entry([int]$cluster, [long]$value) {
    foreach ($fatIndex in 0..($fatCount - 1)) {
        $offset = ($reservedSectors + $fatIndex * $sectorsPerFat32) * $bytesPerSector + $cluster * 4
        Set-UInt32 $offset ($value -band 0x0FFFFFFF)
    }
}

function Get-ClusterOffset([int]$cluster) {
    return ($dataStartSector + ($cluster - 2) * $sectorsPerCluster) * $bytesPerSector
}

function Set-DirectoryEntry(
    [int]$offset,
    [string]$name,
    [string]$extension,
    [int]$attributes,
    [int]$cluster,
    [long]$size
) {
    Set-Ascii $offset $name 8
    Set-Ascii ($offset + 8) $extension 3
    $image[$offset + 11] = $attributes
    # High 16 bits of first cluster at offset +20, low 16 at +26
    Set-UInt16 ($offset + 20) (($cluster -shr 16) -band 0xFFFF)
    Set-UInt16 ($offset + 26) ($cluster -band 0xFFFF)
    Set-UInt32 ($offset + 28) $size
}

function Set-DotEntry([int]$offset, [bool]$parent, [int]$cluster) {
    $name = if ($parent) { '..' } else { '.' }
    Set-Ascii $offset $name 11
    $image[$offset + 11] = 0x10
    Set-UInt16 ($offset + 20) (($cluster -shr 16) -band 0xFFFF)
    Set-UInt16 ($offset + 26) ($cluster -band 0xFFFF)
}

function Get-ShortNameChecksum([string]$name, [string]$extension) {
    $short = [Text.Encoding]::ASCII.GetBytes($name.PadRight(8).Substring(0, 8) + $extension.PadRight(3).Substring(0, 3))
    [int]$sum = 0
    foreach ($value in $short) {
        $sum = ((($sum -band 1) -shl 7) + ($sum -shr 1) + $value) -band 0xFF
    }
    return $sum
}

function Set-LfnEntry([int]$offset, [int]$order, [int]$checksum, [char[]]$characters) {
    $image[$offset] = $order
    $image[$offset + 11] = 0x0F
    $image[$offset + 12] = 0
    $image[$offset + 13] = $checksum
    Set-UInt16 ($offset + 26) 0
    $positions = @(1,3,5,7,9,14,16,18,20,22,24,28,30)
    for ($index = 0; $index -lt 13; $index++) {
        $value = if ($index -lt $characters.Length) { [int]$characters[$index] } elseif ($index -eq $characters.Length) { 0 } else { 0xFFFF }
        Set-UInt16 ($offset + $positions[$index]) $value
    }
}

function Write-FileClusters([byte[]]$data, [int]$firstCluster) {
    $clusterCount = [Math]::Ceiling([Math]::Max($data.Length, 1) / ($bytesPerSector * $sectorsPerCluster))

    for ($index = 0; $index -lt $clusterCount; $index++) {
        $cluster      = $firstCluster + $index
        $sourceOffset = $index * $bytesPerSector * $sectorsPerCluster
        $copyLength   = [Math]::Min($bytesPerSector * $sectorsPerCluster, $data.Length - $sourceOffset)
        if ($copyLength -gt 0) {
            [Array]::Copy($data, $sourceOffset, $image, (Get-ClusterOffset $cluster), $copyLength)
        }
        $nextVal = if ($index -eq $clusterCount - 1) { 0x0FFFFFFF } else { $cluster + 1 }
        Set-Fat32Entry $cluster $nextVal
    }

    return $clusterCount
}

$bootData        = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Bootloader))
$kernelData      = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Kernel))
$trampolineData  = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ApTrampoline))
$userProgramData = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $UserProgram))

# Cluster allocation plan (FAT32: cluster 2 = root dir)
# rootCluster  = 2   (root directory - 1 cluster for up to 16 entries)
# efiDirCluster = 3
# bootDirCluster= 4
# asasDirCluster= 5
# boot EFI file starts at 6
$efiDirCluster    = 3
$bootDirCluster   = 4
$asasDirCluster   = 5
$bootFirstCluster = 6
$bootClusterCount = [Math]::Ceiling($bootData.Length / ($bytesPerSector * $sectorsPerCluster))
$kernelFirstCluster      = $bootFirstCluster + $bootClusterCount
$kernelClusterCount      = [Math]::Ceiling($kernelData.Length / ($bytesPerSector * $sectorsPerCluster))
$trampolineFirstCluster  = $kernelFirstCluster + $kernelClusterCount
$trampolineClusterCount  = [Math]::Ceiling($trampolineData.Length / ($bytesPerSector * $sectorsPerCluster))
$diskTextFirstCluster    = $trampolineFirstCluster + $trampolineClusterCount
$diskTextData            = [Text.Encoding]::ASCII.GetBytes('Asas OS FAT32 storage is online.')
$diskTextClusterCount    = 1
$userProgramFirstCluster = $diskTextFirstCluster + $diskTextClusterCount
$userProgramClusterCount = [Math]::Ceiling($userProgramData.Length / ($bytesPerSector * $sectorsPerCluster))
$asasReadmeFirstCluster  = $userProgramFirstCluster + $userProgramClusterCount
$asasReadmeData          = [Text.Encoding]::ASCII.GetBytes('Asas OS system directory is online.')
$asasReadmeClusterCount  = [Math]::Ceiling($asasReadmeData.Length / ($bytesPerSector * $sectorsPerCluster))
$lastAllocatedCluster    = $asasReadmeFirstCluster + $asasReadmeClusterCount - 1
$dataClusterCount        = [Math]::Floor(($totalSectors - $dataStartSector) / $sectorsPerCluster)
$allocatedClusterCount   = $lastAllocatedCluster - 1
$freeClusterCount        = $dataClusterCount - $allocatedClusterCount
$nextFreeCluster         = $lastAllocatedCluster + 1

# ── FAT32 BPB (BIOS Parameter Block) ──────────────────────────────────────────
$image[0] = 0xEB   # jmp short ...
$image[1] = 0x58
$image[2] = 0x90
Set-Ascii  3  'MSWIN4.1' 8          # OEM Name (standard)
Set-UInt16 11 $bytesPerSector       # Bytes per sector
$image[13]  = $sectorsPerCluster    # Sectors per cluster
Set-UInt16 14 $reservedSectors      # Reserved sectors
$image[16]  = $fatCount             # Number of FATs
Set-UInt16 17 0                     # Root entry count = 0 (FAT32)
Set-UInt16 19 0                     # Total sectors 16 = 0 (FAT32)
$image[21]  = 0xF8                  # Media type
Set-UInt16 22 0                     # Sectors per FAT 16 = 0 (FAT32)
Set-UInt16 24 32                    # Sectors per track
Set-UInt16 26 64                    # Number of heads
Set-UInt32 28 0                     # Hidden sectors
Set-UInt32 32 $totalSectors         # Total sectors 32
# ── FAT32 Extended BPB ────────────────────────────────────────────────────────
Set-UInt32 36 $sectorsPerFat32      # Sectors per FAT 32
Set-UInt16 40 0                     # FAT flags
Set-UInt16 42 0                     # FS version 0.0
Set-UInt32 44 $rootCluster          # Root cluster = 2
Set-UInt16 48 1                     # FS info sector = 1
Set-UInt16 50 6                     # Backup boot sector = 6
# bytes 52..63 reserved = 0
$image[64]  = 0x80                  # Drive number
$image[65]  = 0                     # Reserved
$image[66]  = 0x29                  # Extended boot signature
Set-UInt32 67 0x41534153            # Volume serial
Set-Ascii  71 'ASAS DISK  ' 11     # Volume label
Set-Ascii  82 'FAT32   ' 8         # FS type string
$image[510] = 0x55
$image[511] = 0xAA

# ── FSInfo sector at sector 1 ──────────────────────────────────────────────────
$fi = 1 * $bytesPerSector
Set-UInt32 $fi            0x41615252   # Lead sig
Set-UInt32 ($fi + 484)    0x61417272   # Struc sig
Set-UInt32 ($fi + 488)    $freeClusterCount
Set-UInt32 ($fi + 492)    $nextFreeCluster
Set-UInt32 ($fi + 508)    0xAA550000  # Trail sig
$image[$fi + 510] = 0x55
$image[$fi + 511] = 0xAA

# Standard FAT32 backup boot sector and backup FSInfo sector.
[Array]::Copy($image, 0, $image, 6 * $bytesPerSector, $bytesPerSector)
[Array]::Copy($image, $fi, $image, 7 * $bytesPerSector, $bytesPerSector)

# ── FAT32 reserved entries ────────────────────────────────────────────────────
Set-Fat32Entry 0 0x0FFFFFF8   # media descriptor
Set-Fat32Entry 1 0x0FFFFFFF   # end-of-chain

# Mark directory clusters as end-of-chain
Set-Fat32Entry $rootCluster   0x0FFFFFFF
Set-Fat32Entry $efiDirCluster 0x0FFFFFFF
Set-Fat32Entry $bootDirCluster 0x0FFFFFFF
Set-Fat32Entry $asasDirCluster 0x0FFFFFFF

# ── Root directory (cluster 2) ────────────────────────────────────────────────
$rootOffset = Get-ClusterOffset $rootCluster
Set-DirectoryEntry $rootOffset                   'EFI'   ''    0x10 $efiDirCluster   0
Set-DirectoryEntry ($rootOffset + 32)            'ASAS'  ''    0x10 $asasDirCluster  0
Set-DirectoryEntry ($rootOffset + 64)            'DISK'  'TXT' 0x20 $diskTextFirstCluster  $diskTextData.Length
Set-DirectoryEntry ($rootOffset + 96)            'HELLO' 'EXE' 0x20 $userProgramFirstCluster $userProgramData.Length

# ── EFI directory (cluster 3) ─────────────────────────────────────────────────
$efiOffset = Get-ClusterOffset $efiDirCluster
Set-DotEntry $efiOffset $false $efiDirCluster
Set-DotEntry ($efiOffset + 32) $true $rootCluster
Set-DirectoryEntry ($efiOffset + 64) 'BOOT' '' 0x10 $bootDirCluster 0

# ── EFI/BOOT directory (cluster 4) ───────────────────────────────────────────
$bootDirOffset = Get-ClusterOffset $bootDirCluster
Set-DotEntry $bootDirOffset $false $bootDirCluster
Set-DotEntry ($bootDirOffset + 32) $true $efiDirCluster
Set-DirectoryEntry ($bootDirOffset + 64) 'BOOTX64' 'EFI' 0x20 $bootFirstCluster $bootData.Length

# ── ASAS directory (cluster 5) ───────────────────────────────────────────────
$asasOffset = Get-ClusterOffset $asasDirCluster
Set-DotEntry $asasOffset $false $asasDirCluster
Set-DotEntry ($asasOffset + 32) $true $rootCluster
Set-DirectoryEntry ($asasOffset + 64)  'KERNEL' 'EFI' 0x20 $kernelFirstCluster     $kernelData.Length
Set-DirectoryEntry ($asasOffset + 96)  'APBOOT' 'BIN' 0x20 $trampolineFirstCluster $trampolineData.Length
$longReadme = 'System Readme.txt'.ToCharArray()
$longChecksum = Get-ShortNameChecksum 'README' 'TXT'
Set-LfnEntry ($asasOffset + 128) 0x42 $longChecksum $longReadme[13..($longReadme.Length - 1)]
Set-LfnEntry ($asasOffset + 160) 0x01 $longChecksum $longReadme[0..12]
Set-DirectoryEntry ($asasOffset + 192) 'README' 'TXT' 0x20 $asasReadmeFirstCluster $asasReadmeData.Length

# ── File data ─────────────────────────────────────────────────────────────────
[void](Write-FileClusters $bootData       $bootFirstCluster)
[void](Write-FileClusters $kernelData     $kernelFirstCluster)
[void](Write-FileClusters $trampolineData $trampolineFirstCluster)
[void](Write-FileClusters $diskTextData   $diskTextFirstCluster)
[void](Write-FileClusters $userProgramData $userProgramFirstCluster)
[void](Write-FileClusters $asasReadmeData  $asasReadmeFirstCluster)

$outputDirectory = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
[IO.File]::WriteAllBytes($Output, $image)

Write-Host "Created FAT32 UEFI boot image: $Output ($($image.Length) bytes)" -ForegroundColor Green
