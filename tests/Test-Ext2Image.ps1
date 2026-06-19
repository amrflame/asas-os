param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string[]]$ExpectFile = @(),
    [string[]]$AbsentFile = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$blockSize = 1024
$inodeSize = 128
$rootInode = 2

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

$path = (Resolve-Path -LiteralPath $Image).Path
$bytes = [IO.File]::ReadAllBytes($path)
if ($bytes.Length -lt 64 * $blockSize) { throw 'Image is too small for ext2 validation.' }
if ((Read-Le16 $bytes 1080) -ne 0xEF53) { throw 'Invalid ext2 magic.' }
if ((Read-Le32 $bytes 1048) -ne 0) { throw 'Only 1KiB ext2 test images are supported.' }
if ((Read-Le32 $bytes 1100) -ne 1) { throw 'Unexpected ext2 revision.' }
if ((Read-Le16 $bytes 1112) -ne $inodeSize) { throw 'Unexpected ext2 inode size.' }

$group = 2 * $blockSize
$inodeTable = Read-Le32 $bytes ($group + 8)
if ($inodeTable -eq 0) { throw 'Missing inode table.' }
$rootOffset = ($inodeTable * $blockSize) + (($rootInode - 1) * $inodeSize)
if (((Read-Le16 $bytes $rootOffset) -band 0xF000) -ne 0x4000) {
    throw 'Root inode is not a directory.'
}
$rootBlock = Read-Le32 $bytes ($rootOffset + 40)
if ($rootBlock -eq 0) { throw 'Root directory has no data block.' }
$rootSize = Read-Le32 $bytes ($rootOffset + 4)
$rootData = $rootBlock * $blockSize

$names = @()
$cursor = 0
while ($cursor + 8 -le $rootSize -and $cursor + 8 -le $blockSize) {
    $entry = $rootData + $cursor
    $inode = Read-Le32 $bytes $entry
    $recLen = Read-Le16 $bytes ($entry + 4)
    $nameLen = $bytes[$entry + 6]
    if ($recLen -lt 8 -or $cursor + $recLen -gt $blockSize) { break }
    if ($inode -ne 0 -and $nameLen -ne 0) {
        $name = [Text.Encoding]::ASCII.GetString($bytes, $entry + 8, $nameLen)
        if ($name -ne '.' -and $name -ne '..') { $names += $name }
    }
    $cursor += $recLen
}

foreach ($expected in $ExpectFile) {
    if (@($names | Where-Object { $_.ToUpperInvariant() -eq $expected.ToUpperInvariant() }).Count -eq 0) {
        throw "Expected ext2 file '$expected' was not found."
    }
}
foreach ($absent in $AbsentFile) {
    if (@($names | Where-Object { $_.ToUpperInvariant() -eq $absent.ToUpperInvariant() }).Count -ne 0) {
        throw "Unexpected ext2 file '$absent' was found."
    }
}

Write-Host "ext2 image validation passed: $path" -ForegroundColor Green
if (@($names).Count -ne 0) {
    $names | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
}
