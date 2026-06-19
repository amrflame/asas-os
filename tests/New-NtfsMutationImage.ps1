param(
    [string]$Path = '',
    [int64]$SizeBytes = 268435456
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if ($Path -eq '') {
    $Path = Join-Path $root 'build\tests\ntfs-mutation.vhdx'
}
$Path = [IO.Path]::GetFullPath($Path)
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $root 'build\tests'))
if (-not $Path.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Test image must remain under $allowedRoot"
}
if (Test-Path -LiteralPath $Path) {
    throw "Refusing to overwrite existing test image: $Path"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null

$vhd = New-VHD -Path $Path -Fixed -SizeBytes $SizeBytes
$mounted = Mount-VHD -Path $vhd.Path -Passthru
try {
    $disk = Get-Disk -Number $mounted.DiskNumber
    if ($disk.PartitionStyle -ne 'RAW') {
        throw 'New test disk is unexpectedly initialized.'
    }
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null
    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize `
        -AssignDriveLetter
    $volume = Format-Volume -Partition $partition -FileSystem NTFS `
        -NewFileSystemLabel 'ASAS_NTFS_TEST' -Confirm:$false
    $drive = "$($volume.DriveLetter):"
    Set-Content -LiteralPath (Join-Path $drive 'WINDOWS-SEED.TXT') `
        -Value 'Asas NTFS mutation verification image' -Encoding ascii
    New-Item -ItemType Directory -Path (Join-Path $drive 'SMALLDIR') | Out-Null
    1..96 | ForEach-Object {
        Set-Content -LiteralPath (Join-Path $drive ("LARGE-{0:D3}.TXT" -f $_)) `
            -Value "index seed $_" -Encoding ascii
    }
    $fragmentRoot = Join-Path $drive 'FRAGMENT-SEED'
    New-Item -ItemType Directory -Path $fragmentRoot | Out-Null
    1..512 | ForEach-Object {
        $fragment = Join-Path $fragmentRoot ("FRAGMENT-{0:D3}.BIN" -f $_)
        $stream = [IO.File]::Open($fragment, [IO.FileMode]::CreateNew,
                                 [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.SetLength(4096) } finally { $stream.Dispose() }
    }
    $filler = Join-Path $drive 'ASAS-PREFILL.BIN'
    $free = (Get-Volume -DriveLetter $volume.DriveLetter).SizeRemaining
    $targetFree = 196608
    if ($free -gt ($targetFree + 1048576)) {
        $stream = [IO.File]::Open($filler, [IO.FileMode]::CreateNew,
                                 [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.SetLength($free - $targetFree) } finally { $stream.Dispose() }
    }
    1..512 | Where-Object { ($_ % 2) -eq 0 } | ForEach-Object {
        Remove-Item -LiteralPath (Join-Path $fragmentRoot `
            ("FRAGMENT-{0:D3}.BIN" -f $_))
    }
    chkdsk.exe $drive /scan
    if ($LASTEXITCODE -ne 0) { throw 'Initial chkdsk validation failed.' }
} finally {
    Dismount-VHD -Path $Path -ErrorAction SilentlyContinue
}

Write-Host "Created NTFS mutation test image: $Path"
