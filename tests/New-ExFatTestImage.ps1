param(
    [string]$Path = '',
    [int64]$SizeBytes = 134217728
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Path -eq '') { $Path = Join-Path $root 'build\tests\exfat-test.vhdx' }
$Path = [IO.Path]::GetFullPath($Path)
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $root 'build\tests'))
if (-not $Path.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Test image must remain under $allowedRoot"
}
if (Test-Path -LiteralPath $Path) { throw "Refusing to overwrite: $Path" }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
$vhd = New-VHD -Path $Path -Fixed -SizeBytes $SizeBytes
$mounted = Mount-VHD -Path $vhd.Path -Passthru
try {
    $disk = Get-Disk -Number $mounted.DiskNumber
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null
    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
    $volume = Format-Volume -Partition $partition -FileSystem exFAT `
        -NewFileSystemLabel 'ASAS_EXFAT' -Confirm:$false
    $drive = "$($volume.DriveLetter):"
    Set-Content -LiteralPath (Join-Path $drive 'EXFAT-SEED.TXT') `
        -Value 'Asas exFAT integration image' -Encoding ascii
    $unicodeName = 'Unicode-' + [char]0x03A9 + '.txt'
    Set-Content -LiteralPath (Join-Path $drive $unicodeName) `
        -Value 'Unicode exFAT lookup' -Encoding utf8
    New-Item -ItemType Directory -Path (Join-Path $drive 'MEDIA') | Out-Null
    $stream = [IO.File]::Open((Join-Path $drive 'MEDIA\LARGE.BIN'),
        [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.SetLength(1048576) } finally { $stream.Dispose() }
} finally {
    Dismount-VHD -Path $Path -ErrorAction SilentlyContinue
}
Write-Host "Created exFAT test image: $Path"
