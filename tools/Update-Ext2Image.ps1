param(
    [Parameter(Mandatory = $true)][string]$InputImage,
    [string]$OutputImage = '',
    [string[]]$AddFile = @(),
    [string[]]$Delete = @(),
    [string]$Label = 'ASAS_EXT2'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$blockSize = 1024
$inodeSize = 128
$inodeCount = 128
$blockCount = 1024
$firstDataBlock = 1
$blockBitmap = 3
$inodeBitmap = 4
$inodeTable = 5
$inodeTableBlocks = 16
$rootInode = 2
$rootBlock = 21
$firstFileInode = 12
$firstFileBlock = 22

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

function Set-Le16([byte[]]$Buffer, [int]$Offset, [int]$Value) {
    $Buffer[$Offset] = [byte]($Value -band 0xFF)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xFF)
}

function Set-Le32([byte[]]$Buffer, [int]$Offset, [long]$Value) {
    $Buffer[$Offset] = [byte]($Value -band 0xFF)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 2] = [byte](($Value -shr 16) -band 0xFF)
    $Buffer[$Offset + 3] = [byte](($Value -shr 24) -band 0xFF)
}

function Set-Bit([byte[]]$Buffer, [int]$Index) {
    $byte = [Math]::Floor($Index / 8)
    $bit = $Index % 8
    $Buffer[$byte] = [byte]($Buffer[$byte] -bor (1 -shl $bit))
}

function ConvertTo-Ext2Name([string]$Name) {
    $leaf = [IO.Path]::GetFileName($Name)
    if ($leaf.Length -eq 0 -or $leaf.Length -gt 255) {
        throw "Invalid ext2 file name: $Name"
    }
    foreach ($ch in $leaf.ToCharArray()) {
        if ($ch -eq '/' -or [int][char]$ch -eq 0) {
            throw "Invalid ext2 file name: $Name"
        }
    }
    return $leaf
}

function Set-Inode([byte[]]$Image, [int]$Inode, [int]$Mode, [int]$Size, [int[]]$Blocks, [int]$Links) {
    $offset = ($inodeTable * $blockSize) + (($Inode - 1) * $inodeSize)
    Set-Le16 $Image $offset $Mode
    Set-Le32 $Image ($offset + 4) $Size
    Set-Le32 $Image ($offset + 24) 0
    Set-Le16 $Image ($offset + 26) $Links
    Set-Le32 $Image ($offset + 28) ([Math]::Ceiling([Math]::Max($Size, 1) / 512.0))
    Set-Le32 $Image ($offset + 32) 0
    for ($i = 0; $i -lt $Blocks.Count -and $i -lt 12; $i++) {
        Set-Le32 $Image ($offset + 40 + ($i * 4)) $Blocks[$i]
    }
}

function Add-DirEntry([byte[]]$Buffer, [int]$Offset, [int]$Inode, [string]$Name, [int]$FileType, [int]$RecordLength) {
    $nameBytes = [Text.Encoding]::ASCII.GetBytes($Name)
    Set-Le32 $Buffer $Offset $Inode
    Set-Le16 $Buffer ($Offset + 4) $RecordLength
    $Buffer[$Offset + 6] = [byte]$nameBytes.Length
    $Buffer[$Offset + 7] = [byte]$FileType
    [Array]::Copy($nameBytes, 0, $Buffer, $Offset + 8, $nameBytes.Length)
}

function Read-Ext2RootFiles([byte[]]$Image) {
    $files = @{}
    if ($Image.Length -lt 64 * $blockSize) { return $files }
    if ((Read-Le16 $Image 1080) -ne 0xEF53) { return $files }
    $descriptor = 2 * $blockSize
    $table = Read-Le32 $Image ($descriptor + 8)
    if ($table -eq 0) { return $files }
    $rootOffset = ($table * $blockSize) + (($rootInode - 1) * $inodeSize)
    $rootSize = Read-Le32 $Image ($rootOffset + 4)
    $rootDataBlock = Read-Le32 $Image ($rootOffset + 40)
    if ($rootDataBlock -eq 0) { return $files }
    $cursor = 0
    $rootData = $rootDataBlock * $blockSize
    while ($cursor + 8 -le $rootSize -and $cursor + 8 -le $blockSize) {
        $entry = $rootData + $cursor
        $inode = Read-Le32 $Image $entry
        $recLen = Read-Le16 $Image ($entry + 4)
        $nameLen = $Image[$entry + 6]
        if ($recLen -lt 8 -or $cursor + $recLen -gt $blockSize) { break }
        if ($inode -ne 0 -and $nameLen -ne 0 -and $nameLen -le 255) {
            $name = [Text.Encoding]::ASCII.GetString($Image, $entry + 8, $nameLen)
            if ($name -ne '.' -and $name -ne '..') {
                $inodeOffset = ($table * $blockSize) + (($inode - 1) * $inodeSize)
                $mode = Read-Le16 $Image $inodeOffset
                if (($mode -band 0xF000) -eq 0x8000) {
                    $size = Read-Le32 $Image ($inodeOffset + 4)
                    $dataBlock = Read-Le32 $Image ($inodeOffset + 40)
                    $data = New-Object byte[] $size
                    if ($size -ne 0 -and $dataBlock -ne 0) {
                        [Array]::Copy($Image, $dataBlock * $blockSize, $data, 0, $size)
                    }
                    $files[$name.ToUpperInvariant()] = [pscustomobject]@{
                        Name = $name
                        Data = $data
                    }
                }
            }
        }
        $cursor += $recLen
    }
    return $files
}

function Build-Ext2Image([hashtable]$Files, [string]$Path) {
    $records = @()
    foreach ($key in ($Files.Keys | Sort-Object)) {
        $item = $Files[$key]
        $records += [pscustomobject]@{
            Name = $item.Name
            Data = [byte[]]$item.Data
            Inode = 0
            Blocks = @()
        }
    }
    $nextInode = $firstFileInode
    $nextBlock = $firstFileBlock
    foreach ($record in $records) {
        $record.Inode = $nextInode
        $nextInode++
        $need = [int][Math]::Ceiling([Math]::Max($record.Data.Length, 1) / [double]$blockSize)
        for ($i = 0; $i -lt $need; $i++) {
            $record.Blocks += $nextBlock
            $nextBlock++
        }
    }
    if ($nextInode -gt $inodeCount -or $nextBlock -gt $blockCount) {
        throw 'ext2 image is full.'
    }

    $image = New-Object byte[] ($blockCount * $blockSize)
    $super = 1024
    $usedInodes = 11 + $records.Count
    $usedBlocks = $firstFileBlock + ($nextBlock - $firstFileBlock)
    Set-Le32 $image ($super + 0) $inodeCount
    Set-Le32 $image ($super + 4) $blockCount
    Set-Le32 $image ($super + 12) ($blockCount - $usedBlocks)
    Set-Le32 $image ($super + 16) ($inodeCount - $usedInodes)
    Set-Le32 $image ($super + 20) $firstDataBlock
    Set-Le32 $image ($super + 24) 0
    Set-Le32 $image ($super + 28) 0
    Set-Le32 $image ($super + 32) $blockCount
    Set-Le32 $image ($super + 36) $blockCount
    Set-Le32 $image ($super + 40) $inodeCount
    Set-Le16 $image ($super + 56) 0xEF53
    Set-Le16 $image ($super + 58) 1
    Set-Le32 $image ($super + 76) 1
    Set-Le32 $image ($super + 84) 11
    Set-Le16 $image ($super + 88) $inodeSize
    Set-Le32 $image ($super + 92) 0
    Set-Le32 $image ($super + 96) 2
    Set-Le32 $image ($super + 100) 3
    $labelBytes = [Text.Encoding]::ASCII.GetBytes($Label.PadRight(16).Substring(0, 16))
    [Array]::Copy($labelBytes, 0, $image, $super + 120, 16)

    $gd = 2 * $blockSize
    Set-Le32 $image ($gd + 0) $blockBitmap
    Set-Le32 $image ($gd + 4) $inodeBitmap
    Set-Le32 $image ($gd + 8) $inodeTable
    Set-Le16 $image ($gd + 12) ($blockCount - $usedBlocks)
    Set-Le16 $image ($gd + 14) ($inodeCount - $usedInodes)
    Set-Le16 $image ($gd + 16) 0

    $bb = New-Object byte[] $blockSize
    for ($i = 0; $i -lt $usedBlocks; $i++) { Set-Bit $bb $i }
    [Array]::Copy($bb, 0, $image, $blockBitmap * $blockSize, $blockSize)
    $ib = New-Object byte[] $blockSize
    for ($i = 0; $i -lt 11; $i++) { Set-Bit $ib $i }
    foreach ($record in $records) { Set-Bit $ib ($record.Inode - 1) }
    [Array]::Copy($ib, 0, $image, $inodeBitmap * $blockSize, $blockSize)

    $rootDir = New-Object byte[] $blockSize
    Add-DirEntry $rootDir 0 $rootInode '.' 2 12
    Add-DirEntry $rootDir 12 $rootInode '..' 2 12
    $cursor = 24
    for ($i = 0; $i -lt $records.Count; $i++) {
        $nameLen = [Text.Encoding]::ASCII.GetByteCount($records[$i].Name)
        $minLen = (8 + $nameLen + 3) -band -4
        $recLen = if ($i -eq $records.Count - 1) { $blockSize - $cursor } else { $minLen }
        if ($recLen -lt $minLen) { throw 'ext2 root directory is full.' }
        Add-DirEntry $rootDir $cursor $records[$i].Inode $records[$i].Name 1 $recLen
        $cursor += $recLen
    }
    [Array]::Copy($rootDir, 0, $image, $rootBlock * $blockSize, $blockSize)
    Set-Inode $image $rootInode 0x41ED $blockSize @($rootBlock) 2

    foreach ($record in $records) {
        Set-Inode $image $record.Inode 0x81A4 $record.Data.Length ([int[]]$record.Blocks) 1
        $remaining = $record.Data.Length
        $offset = 0
        foreach ($block in $record.Blocks) {
            $copy = [Math]::Min($remaining, $blockSize)
            if ($copy -gt 0) {
                [Array]::Copy($record.Data, $offset, $image, $block * $blockSize, $copy)
            }
            $offset += $copy
            $remaining -= $copy
        }
    }
    [IO.File]::WriteAllBytes($Path, $image)
}

$inputPath = (Resolve-Path -LiteralPath $InputImage).Path
if ($OutputImage -eq '') {
    $OutputImage = $inputPath
}
$outputPath = if (Test-Path -LiteralPath $OutputImage) {
    (Resolve-Path -LiteralPath $OutputImage).Path
} else {
    [IO.Path]::GetFullPath($OutputImage)
}

$files = Read-Ext2RootFiles ([IO.File]::ReadAllBytes($inputPath))
foreach ($name in $Delete) {
    $ext2Name = ConvertTo-Ext2Name $name
    [void]$files.Remove($ext2Name.ToUpperInvariant())
}
foreach ($path in $AddFile) {
    $resolved = (Resolve-Path -LiteralPath $path).Path
    $ext2Name = ConvertTo-Ext2Name (Split-Path -Leaf $resolved)
    $files[$ext2Name.ToUpperInvariant()] = [pscustomobject]@{
        Name = $ext2Name
        Data = [IO.File]::ReadAllBytes($resolved)
    }
}

$directory = Split-Path -Parent $outputPath
if ($directory -ne '') {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$temp = "$outputPath.tmp"
Build-Ext2Image $files $temp
Move-Item -LiteralPath $temp -Destination $outputPath -Force
Write-Host "Updated ext2 image: $outputPath" -ForegroundColor Green
