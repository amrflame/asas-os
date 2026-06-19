param(
    [Parameter(Mandatory = $true)]
    [string]$Image
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Image).Path)
function Read-U16([int]$Offset) { return [BitConverter]::ToUInt16($bytes, $Offset) }
function Read-U32([int]$Offset) { return [BitConverter]::ToUInt32($bytes, $Offset) }

$bytesPerSector = Read-U16 11
$sectorsPerCluster = $bytes[13]
$reservedSectors = Read-U16 14
$fatCount = $bytes[16]
$totalSectors = Read-U32 32
$sectorsPerFat = Read-U32 36
$fsInfoSector = Read-U16 48
$backupBootSector = Read-U16 50

if ($bytesPerSector -ne 512 -or $sectorsPerCluster -eq 0 -or $fsInfoSector -eq 0) {
    throw 'Image does not contain a supported FAT32 FSInfo layout.'
}

$dataStart = $reservedSectors + $fatCount * $sectorsPerFat
$clusterCount = [Math]::Floor(($totalSectors - $dataStart) / $sectorsPerCluster)
$fatOffset = $reservedSectors * $bytesPerSector
$actualFree = 0
for ($cluster = 2; $cluster -lt $clusterCount + 2; $cluster++) {
    if (((Read-U32 ($fatOffset + $cluster * 4)) -band 0x0FFFFFFF) -eq 0) {
        $actualFree++
    }
}

$primaryOffset = $fsInfoSector * $bytesPerSector
$reportedFree = Read-U32 ($primaryOffset + 488)
$nextFree = Read-U32 ($primaryOffset + 492)
if ($reportedFree -ne $actualFree) {
    throw "FAT32 FSInfo free count mismatch: reported=$reportedFree actual=$actualFree"
}
if ($nextFree -lt 2 -or $nextFree -ge $clusterCount + 2) {
    throw "FAT32 FSInfo next-free hint is out of range: $nextFree"
}

if ($backupBootSector -ne 0) {
    $backupOffset = ($backupBootSector + $fsInfoSector) * $bytesPerSector
    if ((Read-U32 ($backupOffset + 488)) -ne $reportedFree -or
        (Read-U32 ($backupOffset + 492)) -ne $nextFree) {
        throw 'FAT32 primary and backup FSInfo sectors differ.'
    }
}

Write-Host "FAT32 FSInfo checks passed: free=$reportedFree next=$nextFree" -ForegroundColor Green
