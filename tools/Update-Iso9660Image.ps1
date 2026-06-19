param(
    [Parameter(Mandatory = $true)][string]$InputIso,
    [string]$OutputIso = '',
    [string[]]$AddFile = @(),
    [string[]]$Delete = @(),
    [string]$Label = 'ASAS_EDITED'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sectorSize = 2048

function Read-Le32([byte[]]$Buffer, [int]$Offset) {
    return [uint32]$Buffer[$Offset] -bor
        ([uint32]$Buffer[$Offset + 1] -shl 8) -bor
        ([uint32]$Buffer[$Offset + 2] -shl 16) -bor
        ([uint32]$Buffer[$Offset + 3] -shl 24)
}

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

function Set-Both16([byte[]]$Buffer, [int]$Offset, [int]$Value) {
    Set-Le16 $Buffer $Offset $Value
    Set-Be16 $Buffer ($Offset + 2) $Value
}

function Set-Both32([byte[]]$Buffer, [int]$Offset, [long]$Value) {
    Set-Le32 $Buffer $Offset $Value
    Set-Be32 $Buffer ($Offset + 4) $Value
}

function Convert-IsoName([string]$Name) {
    $leaf = [IO.Path]::GetFileName($Name).ToUpperInvariant()
    $safe = New-Object Text.StringBuilder
    foreach ($ch in $leaf.ToCharArray()) {
        if (($ch -ge 'A' -and $ch -le 'Z') -or
            ($ch -ge '0' -and $ch -le '9') -or $ch -eq '_' -or $ch -eq '.') {
            [void]$safe.Append($ch)
        } else {
            [void]$safe.Append('_')
        }
    }
    $text = $safe.ToString()
    if ($text -notmatch ';[0-9]+$') {
        $text = "$text;1"
    }
    return $text
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
    if (($NameBytes.Length % 2) -eq 0) { $length++ }
    $Buffer[$Offset] = [byte]$length
    $Buffer[$Offset + 1] = 0
    Set-Both32 $Buffer ($Offset + 2) $Extent
    Set-Both32 $Buffer ($Offset + 10) $DataLength
    $now = Get-Date
    $Buffer[$Offset + 18] = [byte]($now.Year - 1900)
    $Buffer[$Offset + 19] = [byte]$now.Month
    $Buffer[$Offset + 20] = [byte]$now.Day
    $Buffer[$Offset + 21] = [byte]$now.Hour
    $Buffer[$Offset + 22] = [byte]$now.Minute
    $Buffer[$Offset + 23] = [byte]$now.Second
    $Buffer[$Offset + 24] = 0
    $Buffer[$Offset + 25] = $Flags
    $Buffer[$Offset + 26] = 0
    $Buffer[$Offset + 27] = 0
    Set-Both16 $Buffer ($Offset + 28) 1
    $Buffer[$Offset + 32] = [byte]$NameBytes.Length
    [Array]::Copy($NameBytes, 0, $Buffer, $Offset + 33, $NameBytes.Length)
    return $length
}

function Read-RootFiles([byte[]]$Iso) {
    $pvd = 16 * $sectorSize
    if ($Iso[$pvd] -ne 1 -or
        [Text.Encoding]::ASCII.GetString($Iso, $pvd + 1, 5) -ne 'CD001') {
        throw 'Input is not a primary ISO9660 image.'
    }
    $rootRecord = $pvd + 156
    $rootExtent = Read-Le32 $Iso ($rootRecord + 2)
    $rootSize = Read-Le32 $Iso ($rootRecord + 10)
    $files = @{}
    $offset = $rootExtent * $sectorSize
    $end = $offset + $rootSize
    while ($offset -lt $end) {
        $length = $Iso[$offset]
        if ($length -eq 0) {
            $offset = ([Math]::Floor($offset / $sectorSize) + 1) * $sectorSize
            continue
        }
        $flags = $Iso[$offset + 25]
        $nameLength = $Iso[$offset + 32]
        $rawName = $Iso[($offset + 33)..($offset + 32 + $nameLength)]
        if (-not ($nameLength -eq 1 -and ($rawName[0] -eq 0 -or $rawName[0] -eq 1)) -and
            (($flags -band 0x02) -eq 0)) {
            $name = [Text.Encoding]::ASCII.GetString($rawName)
            $extent = Read-Le32 $Iso ($offset + 2)
            $size = Read-Le32 $Iso ($offset + 10)
            $data = New-Object byte[] $size
            [Array]::Copy($Iso, $extent * $sectorSize, $data, 0, $size)
            $files[$name.ToUpperInvariant()] = $data
        }
        $offset += $length
    }
    return $files
}

function Build-Iso([hashtable]$Files, [string]$Path) {
    $descriptorSectors = 4
    $rootLba = 20
    $pathTableLba = 21
    $fileStartLba = 22
    $records = @()
    foreach ($key in ($Files.Keys | Sort-Object)) {
        $records += [pscustomobject]@{
            Name = $key
            Data = [byte[]]$Files[$key]
            Lba = 0
        }
    }
    $currentLba = $fileStartLba
    foreach ($record in $records) {
        $record.Lba = $currentLba
        $currentLba += [Math]::Ceiling([Math]::Max($record.Data.Length, 1) / $sectorSize)
    }
    $volumeSpaceSize = [Math]::Max($currentLba, $descriptorSectors + $fileStartLba)
    $iso = New-Object byte[] ($volumeSpaceSize * $sectorSize)

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
    Set-Le32 $iso ($pvd + 140) $pathTableLba
    Set-Be32 $iso ($pvd + 148) ($pathTableLba + 1)
    [void](Set-DirectoryRecord $iso ($pvd + 156) $rootLba $sectorSize 0x02 ([byte[]](0)))

    $term = 18 * $sectorSize
    $iso[$term] = 255
    Set-Ascii $iso ($term + 1) 'CD001' 5
    $iso[$term + 6] = 1

    $root = $rootLba * $sectorSize
    $cursor = $root
    $cursor += Set-DirectoryRecord $iso $cursor $rootLba $sectorSize 0x02 ([byte[]](0))
    $cursor += Set-DirectoryRecord $iso $cursor $rootLba $sectorSize 0x02 ([byte[]](1))
    foreach ($record in $records) {
        $nameBytes = [Text.Encoding]::ASCII.GetBytes($record.Name)
        if ($cursor + 40 + $nameBytes.Length -ge ($root + $sectorSize)) {
            throw 'Root directory is full; nested directories are not supported by this tool yet.'
        }
        $cursor += Set-DirectoryRecord $iso $cursor $record.Lba $record.Data.Length 0x00 $nameBytes
        [Array]::Copy($record.Data, 0, $iso, $record.Lba * $sectorSize, $record.Data.Length)
    }

    $pathL = $pathTableLba * $sectorSize
    $iso[$pathL] = 1
    $iso[$pathL + 1] = 0
    Set-Le32 $iso ($pathL + 2) $rootLba
    Set-Le16 $iso ($pathL + 6) 1
    $iso[$pathL + 8] = 0

    $pathM = ($pathTableLba + 1) * $sectorSize
    $iso[$pathM] = 1
    $iso[$pathM + 1] = 0
    Set-Be32 $iso ($pathM + 2) $rootLba
    Set-Be16 $iso ($pathM + 6) 1
    $iso[$pathM + 8] = 0

    [IO.File]::WriteAllBytes($Path, $iso)
}

$inputPath = (Resolve-Path -LiteralPath $InputIso).Path
if ($OutputIso -eq '') {
    $OutputIso = $inputPath
}
$outputPath = if (Test-Path -LiteralPath $OutputIso) {
    (Resolve-Path -LiteralPath $OutputIso).Path
} else {
    [IO.Path]::GetFullPath($OutputIso)
}

$files = Read-RootFiles ([IO.File]::ReadAllBytes($inputPath))
foreach ($name in $Delete) {
    $isoName = Convert-IsoName $name
    [void]$files.Remove($isoName.ToUpperInvariant())
}
foreach ($path in $AddFile) {
    $resolved = (Resolve-Path -LiteralPath $path).Path
    $isoName = Convert-IsoName (Split-Path -Leaf $resolved)
    $files[$isoName.ToUpperInvariant()] = [IO.File]::ReadAllBytes($resolved)
}

$directory = Split-Path -Parent $outputPath
if ($directory -ne '') {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$temp = "$outputPath.tmp"
Build-Iso $files $temp
Move-Item -LiteralPath $temp -Destination $outputPath -Force
Write-Host "Updated ISO9660 image: $outputPath" -ForegroundColor Green
