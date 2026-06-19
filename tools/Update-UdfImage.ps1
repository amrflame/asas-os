param(
    [Parameter(Mandatory = $true)][string]$InputImage,
    [string]$OutputImage = '',
    [string[]]$AddFile = @(),
    [string[]]$Delete = @(),
    [string]$Label = 'ASAS_UDF'
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

function Set-Le64([byte[]]$Buffer, [int]$Offset, [uint64]$Value) {
    Set-Le32 $Buffer $Offset ([long]($Value -band 0xFFFFFFFF))
    Set-Le32 $Buffer ($Offset + 4) ([long](($Value -shr 32) -band 0xFFFFFFFF))
}

function Set-Ascii([byte[]]$Buffer, [int]$Offset, [string]$Text, [int]$Length) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($Text.PadRight($Length).Substring(0, $Length))
    [Array]::Copy($bytes, 0, $Buffer, $Offset, $Length)
}

function Set-Tag([byte[]]$Image, [int]$Sector, [int]$TagId) {
    $offset = $Sector * $sectorSize
    Set-Le16 $Image $offset $TagId
    $Image[$offset + 5] = 2
    Set-Le32 $Image ($offset + 12) $Sector
    $sum = 0
    for ($i = 0; $i -lt 16; $i++) {
        if ($i -ne 4) { $sum = ($sum + $Image[$offset + $i]) -band 0xFF }
    }
    $Image[$offset + 4] = [byte]$sum
}

function Test-Tag([byte[]]$Image, [int]$Sector, [int]$TagId) {
    $offset = $Sector * $sectorSize
    if ($offset + 16 -gt $Image.Length) { return $false }
    if ((Read-Le16 $Image $offset) -ne $TagId) { return $false }
    if ($Image[$offset + 5] -ne 2) { return $false }
    $sum = 0
    for ($i = 0; $i -lt 16; $i++) {
        if ($i -ne 4) { $sum = ($sum + $Image[$offset + $i]) -band 0xFF }
    }
    return $sum -eq $Image[$offset + 4]
}

function ConvertTo-UdfName([string]$Name) {
    $leaf = [IO.Path]::GetFileName($Name)
    $safe = New-Object Text.StringBuilder
    foreach ($ch in $leaf.ToCharArray()) {
        if ([int][char]$ch -ge 32 -and [int][char]$ch -lt 127 -and $ch -ne '/' -and $ch -ne '\') {
            [void]$safe.Append($ch)
        } else {
            [void]$safe.Append('_')
        }
    }
    if ($safe.Length -eq 0) { throw "Invalid UDF file name: $Name" }
    return $safe.ToString()
}

function Get-Cs0Bytes([string]$Name) {
    $ascii = [Text.Encoding]::ASCII.GetBytes($Name)
    $data = New-Object byte[] ($ascii.Length + 1)
    $data[0] = 8
    [Array]::Copy($ascii, 0, $data, 1, $ascii.Length)
    return $data
}

function ConvertFrom-Cs0([byte[]]$Data) {
    if ($Data.Length -eq 0) { return '' }
    if ($Data[0] -eq 8) {
        return [Text.Encoding]::ASCII.GetString($Data, 1, $Data.Length - 1)
    }
    if ($Data[0] -eq 16) {
        $chars = New-Object Text.StringBuilder
        for ($i = 1; $i + 1 -lt $Data.Length; $i += 2) {
            $value = ($Data[$i] -shl 8) -bor $Data[$i + 1]
            [void]$chars.Append([char]$value)
        }
        return $chars.ToString()
    }
    return ''
}

function Set-LongAd([byte[]]$Image, [int]$Offset, [int]$Length, [int]$Block, [int]$Partition) {
    Set-Le32 $Image $Offset $Length
    Set-Le32 $Image ($Offset + 4) $Block
    Set-Le16 $Image ($Offset + 8) $Partition
}

function Set-ShortAd([byte[]]$Image, [int]$Offset, [int]$Length, [int]$Block) {
    Set-Le32 $Image $Offset $Length
    Set-Le32 $Image ($Offset + 4) $Block
}

function Set-FileEntry([byte[]]$Image, [int]$Sector, [int]$FileType, [uint64]$Size, [int]$DataBlock, [int]$DataLength) {
    $offset = $Sector * $sectorSize
    Set-Tag $Image $Sector 261
    $Image[$offset + 16 + 11] = [byte]$FileType
    Set-Le16 $Image ($offset + 16 + 18) 1
    Set-Le64 $Image ($offset + 56) $Size
    Set-Le64 $Image ($offset + 64) ([uint64]([Math]::Ceiling([double][Math]::Max($DataLength, 1) / $sectorSize)))
    Set-Le32 $Image ($offset + 168) 0
    Set-Le32 $Image ($offset + 172) 8
    Set-ShortAd $Image ($offset + 176) $DataLength $DataBlock
    Set-Tag $Image $Sector 261
}

function Set-FileIdentifier([byte[]]$Buffer, [int]$Offset, [string]$Name, [int]$IcbBlock) {
    $nameBytes = Get-Cs0Bytes $Name
    Set-Le16 $Buffer $Offset 257
    $Buffer[$Offset + 5] = 2
    $Buffer[$Offset + 19] = [byte]$nameBytes.Length
    Set-LongAd $Buffer ($Offset + 20) $sectorSize $IcbBlock 0
    Set-Le16 $Buffer ($Offset + 36) 0
    [Array]::Copy($nameBytes, 0, $Buffer, $Offset + 38, $nameBytes.Length)
    $length = 38 + $nameBytes.Length
    $length = ($length + 3) -band -4
    $sum = 0
    for ($i = 0; $i -lt 16; $i++) {
        if ($i -ne 4) { $sum = ($sum + $Buffer[$Offset + $i]) -band 0xFF }
    }
    $Buffer[$Offset + 4] = [byte]$sum
    return $length
}

function Read-UdfRootFiles([byte[]]$Image) {
    $files = @{}
    if ($Image.Length -lt (300 * $sectorSize)) { return $files }
    if (-not (Test-Tag $Image 256 2)) { return $files }
    $mainLba = Read-Le32 $Image ((256 * $sectorSize) + 20)
    $mainLength = Read-Le32 $Image ((256 * $sectorSize) + 16)
    $partitionStart = 0
    $fileSetBlock = 0
    for ($i = 0; $i -lt [Math]::Floor($mainLength / $sectorSize); $i++) {
        $sector = $mainLba + $i
        $tag = Read-Le16 $Image ($sector * $sectorSize)
        if ($tag -eq 8) { break }
        if (-not (Test-Tag $Image $sector $tag)) { break }
        if ($tag -eq 5) {
            $partitionStart = Read-Le32 $Image (($sector * $sectorSize) + 188)
        } elseif ($tag -eq 6) {
            $fileSetBlock = Read-Le32 $Image (($sector * $sectorSize) + 252)
        }
    }
    if ($partitionStart -eq 0) { return $files }
    $fsdSector = $partitionStart + $fileSetBlock
    if (-not (Test-Tag $Image $fsdSector 256)) { return $files }
    $rootIcbBlock = Read-Le32 $Image (($fsdSector * $sectorSize) + 404)
    $rootFeSector = $partitionStart + $rootIcbBlock
    if (-not (Test-Tag $Image $rootFeSector 261)) { return $files }
    $rootSize = Read-Le64 $Image (($rootFeSector * $sectorSize) + 56)
    $rootDataBlock = Read-Le32 $Image (($rootFeSector * $sectorSize) + 180)
    $rootData = ($partitionStart + $rootDataBlock) * $sectorSize
    $cursor = 0
    while ($cursor + 38 -le $rootSize) {
        $record = $rootData + $cursor
        if ($record + 38 -gt $Image.Length) { break }
        if ((Read-Le16 $Image $record) -ne 257) { break }
        $nameLength = $Image[$record + 19]
        $implLength = Read-Le16 $Image ($record + 36)
        $nameOffset = $record + 38 + $implLength
        if ($nameOffset + $nameLength -gt $Image.Length) { break }
        $nameBytes = New-Object byte[] $nameLength
        [Array]::Copy($Image, $nameOffset, $nameBytes, 0, $nameLength)
        $name = ConvertFrom-Cs0 $nameBytes
        $icbBlock = Read-Le32 $Image ($record + 24)
        $feSector = $partitionStart + $icbBlock
        if ($name -ne '' -and (Test-Tag $Image $feSector 261)) {
            $fileType = $Image[($feSector * $sectorSize) + 27]
            if ($fileType -eq 5) {
                $size = Read-Le64 $Image (($feSector * $sectorSize) + 56)
                $dataBlock = Read-Le32 $Image (($feSector * $sectorSize) + 180)
                $dataOffset = ($partitionStart + $dataBlock) * $sectorSize
                if ($dataOffset + $size -le $Image.Length) {
                    $data = New-Object byte[] $size
                    [Array]::Copy($Image, $dataOffset, $data, 0, $size)
                    $files[$name.ToUpperInvariant()] = [pscustomobject]@{
                        Name = $name
                        Data = $data
                    }
                }
            }
        }
        $length = 38 + $implLength + $nameLength
        $cursor += (($length + 3) -band -4)
    }
    return $files
}

function Build-UdfImage([hashtable]$Files, [string]$Path) {
    $mainVds = 257
    $partitionStart = 272
    $fileSetBlock = 0
    $rootFeBlock = 1
    $rootDataBlock = 2
    $fileFeBlock = 3
    $records = @()
    foreach ($key in ($Files.Keys | Sort-Object)) {
        $item = $Files[$key]
        $records += [pscustomobject]@{
            Name = $item.Name
            Data = [byte[]]$item.Data
            FeBlock = 0
            DataBlock = 0
        }
    }
    $nextBlock = $fileFeBlock
    foreach ($record in $records) {
        $record.FeBlock = $nextBlock
        $nextBlock++
    }
    foreach ($record in $records) {
        $record.DataBlock = $nextBlock
        $blocks = [Math]::Ceiling([Math]::Max($record.Data.Length, 1) / $sectorSize)
        $nextBlock += [int]$blocks
    }
    $totalSectors = $partitionStart + $nextBlock + 32
    if ($totalSectors -lt 300) { $totalSectors = 300 }
    $image = New-Object byte[] ($totalSectors * $sectorSize)

    Set-Tag $image 256 2
    Set-Le32 $image ((256 * $sectorSize) + 16) (16 * $sectorSize)
    Set-Le32 $image ((256 * $sectorSize) + 20) $mainVds

    $pd = $mainVds * $sectorSize
    Set-Le16 $image ($pd + 22) 0
    Set-Le32 $image ($pd + 188) $partitionStart
    Set-Le32 $image ($pd + 192) ($totalSectors - $partitionStart)
    Set-Tag $image $mainVds 5

    $lvdSector = $mainVds + 1
    $lvd = $lvdSector * $sectorSize
    Set-Ascii $image ($lvd + 84) $Label 128
    Set-Le32 $image ($lvd + 212) $sectorSize
    Set-LongAd $image ($lvd + 248) $sectorSize $fileSetBlock 0
    Set-Le32 $image ($lvd + 264) 6
    Set-Le32 $image ($lvd + 268) 1
    $image[$lvd + 440] = 1
    $image[$lvd + 441] = 6
    Set-Le16 $image ($lvd + 442) 0
    Set-Le16 $image ($lvd + 444) 0
    Set-Tag $image $lvdSector 6

    Set-Tag $image ($mainVds + 2) 8

    $fsdSector = $partitionStart + $fileSetBlock
    Set-LongAd $image (($fsdSector * $sectorSize) + 400) $sectorSize $rootFeBlock 0
    Set-Tag $image $fsdSector 256

    $rootDirectory = New-Object byte[] $sectorSize
    $cursor = 0
    foreach ($record in $records) {
        $cursor += Set-FileIdentifier $rootDirectory $cursor $record.Name $record.FeBlock
    }
    [Array]::Copy($rootDirectory, 0, $image, ($partitionStart + $rootDataBlock) * $sectorSize, $sectorSize)
    Set-FileEntry $image ($partitionStart + $rootFeBlock) 4 ([uint64]$cursor) $rootDataBlock $sectorSize

    foreach ($record in $records) {
        Set-FileEntry $image ($partitionStart + $record.FeBlock) 5 ([uint64]$record.Data.Length) $record.DataBlock $record.Data.Length
        [Array]::Copy($record.Data, 0, $image, ($partitionStart + $record.DataBlock) * $sectorSize, $record.Data.Length)
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

$files = Read-UdfRootFiles ([IO.File]::ReadAllBytes($inputPath))
foreach ($name in $Delete) {
    $udfName = ConvertTo-UdfName $name
    [void]$files.Remove($udfName.ToUpperInvariant())
}
foreach ($path in $AddFile) {
    $resolved = (Resolve-Path -LiteralPath $path).Path
    $udfName = ConvertTo-UdfName (Split-Path -Leaf $resolved)
    $files[$udfName.ToUpperInvariant()] = [pscustomobject]@{
        Name = $udfName
        Data = [IO.File]::ReadAllBytes($resolved)
    }
}

$directory = Split-Path -Parent $outputPath
if ($directory -ne '') {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$temp = "$outputPath.tmp"
Build-UdfImage $files $temp
Move-Item -LiteralPath $temp -Destination $outputPath -Force
Write-Host "Updated UDF image: $outputPath" -ForegroundColor Green
