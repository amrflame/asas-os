param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string[]]$ExpectFile = @(),
    [string[]]$AbsentFile = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sectorSize = 2048

function Read-Le16([byte[]]$Buffer, [int]$Offset) {
    return [uint16]([uint16]$Buffer[$Offset] -bor
        ([uint16]$Buffer[$Offset + 1] -shl 8))
}

function Read-Le32([byte[]]$Buffer, [int]$Offset) {
    return [uint32]$Buffer[$Offset] -bor
        ([uint32]$Buffer[$Offset + 1] -shl 8) -bor
        ([uint32]$Buffer[$Offset + 2] -shl 16) -bor
        ([uint32]$Buffer[$Offset + 3] -shl 24)
}

function Read-Le64([byte[]]$Buffer, [int]$Offset) {
    return [uint64](Read-Le32 $Buffer $Offset) -bor
        ([uint64](Read-Le32 $Buffer ($Offset + 4)) -shl 32)
}

function Test-Tag([byte[]]$ImageBytes, [int]$Sector, [int]$TagId) {
    $offset = $Sector * $sectorSize
    return Test-TagAtOffset $ImageBytes $offset $TagId
}

function Test-TagAtOffset([byte[]]$ImageBytes, [int]$Offset, [int]$TagId) {
    $offset = $Offset
    if ($offset + 16 -gt $ImageBytes.Length) { return $false }
    if ((Read-Le16 $ImageBytes $offset) -ne $TagId) { return $false }
    if ($ImageBytes[$offset + 5] -ne 2) { return $false }
    $sum = 0
    for ($i = 0; $i -lt 16; $i++) {
        if ($i -ne 4) { $sum = ($sum + $ImageBytes[$offset + $i]) -band 0xFF }
    }
    return $sum -eq $ImageBytes[$offset + 4]
}

function ConvertFrom-Cs0([byte[]]$Data) {
    if ($Data.Length -eq 0) { return '' }
    if ($Data[0] -eq 8) {
        return [Text.Encoding]::ASCII.GetString($Data, 1, $Data.Length - 1)
    }
    if ($Data[0] -eq 16) {
        $chars = New-Object Text.StringBuilder
        for ($i = 1; $i + 1 -lt $Data.Length; $i += 2) {
            [void]$chars.Append([char](($Data[$i] -shl 8) -bor $Data[$i + 1]))
        }
        return $chars.ToString()
    }
    return ''
}

$path = (Resolve-Path -LiteralPath $Image).Path
$bytes = [IO.File]::ReadAllBytes($path)
if (-not (Test-Tag $bytes 256 2)) { throw 'UDF anchor tag is invalid.' }

$mainLba = Read-Le32 $bytes ((256 * $sectorSize) + 20)
$mainLength = Read-Le32 $bytes ((256 * $sectorSize) + 16)
$partitionStart = 0
$fileSetBlock = 0
$sawPartition = $false
$sawLogical = $false
for ($i = 0; $i -lt [Math]::Floor($mainLength / $sectorSize); $i++) {
    $sector = $mainLba + $i
    $tag = Read-Le16 $bytes ($sector * $sectorSize)
    if ($tag -eq 8) {
        if (-not (Test-Tag $bytes $sector 8)) { throw 'UDF terminating descriptor tag is invalid.' }
        break
    }
    if (-not (Test-Tag $bytes $sector $tag)) { throw "UDF descriptor tag $tag is invalid." }
    if ($tag -eq 5) {
        $partitionStart = Read-Le32 $bytes (($sector * $sectorSize) + 188)
        $sawPartition = $true
    } elseif ($tag -eq 6) {
        $logicalBlockSize = Read-Le32 $bytes (($sector * $sectorSize) + 212)
        if ($logicalBlockSize -ne $sectorSize) { throw 'Unexpected UDF logical block size.' }
        $fileSetBlock = Read-Le32 $bytes (($sector * $sectorSize) + 252)
        $sawLogical = $true
    }
}

if (-not $sawPartition -or -not $sawLogical) {
    throw 'Missing UDF partition or logical volume descriptor.'
}

$fsdSector = $partitionStart + $fileSetBlock
if (-not (Test-Tag $bytes $fsdSector 256)) { throw 'UDF file set descriptor tag is invalid.' }
$rootIcbBlock = Read-Le32 $bytes (($fsdSector * $sectorSize) + 404)
$rootFeSector = $partitionStart + $rootIcbBlock
if (-not (Test-Tag $bytes $rootFeSector 261)) { throw 'UDF root file entry tag is invalid.' }
$rootSize = Read-Le64 $bytes (($rootFeSector * $sectorSize) + 56)
$rootDataBlock = Read-Le32 $bytes (($rootFeSector * $sectorSize) + 180)
$rootOffset = ($partitionStart + $rootDataBlock) * $sectorSize

$names = @()
$cursor = 0
while ($cursor + 38 -le $rootSize) {
    $record = $rootOffset + $cursor
    if ((Read-Le16 $bytes $record) -ne 257) { break }
    if (-not (Test-TagAtOffset $bytes $record 257)) {
        throw 'UDF file identifier tag is invalid.'
    }
    $nameLength = $bytes[$record + 19]
    $implLength = Read-Le16 $bytes ($record + 36)
    $nameBytes = New-Object byte[] $nameLength
    [Array]::Copy($bytes, $record + 38 + $implLength, $nameBytes, 0, $nameLength)
    $name = ConvertFrom-Cs0 $nameBytes
    if ($name -ne '') { $names += $name }
    $length = 38 + $implLength + $nameLength
    $cursor += (($length + 3) -band -4)
}

foreach ($expected in $ExpectFile) {
    if (@($names | Where-Object { $_.ToUpperInvariant() -eq $expected.ToUpperInvariant() }).Count -eq 0) {
        throw "Expected UDF file '$expected' was not found."
    }
}
foreach ($absent in $AbsentFile) {
    if (@($names | Where-Object { $_.ToUpperInvariant() -eq $absent.ToUpperInvariant() }).Count -ne 0) {
        throw "Unexpected UDF file '$absent' was found."
    }
}

Write-Host "UDF image validation passed: $path" -ForegroundColor Green
if (@($names).Count -ne 0) {
    $names | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
}
