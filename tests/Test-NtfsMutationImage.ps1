param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Path = (Resolve-Path -LiteralPath $Path).Path
$mounted = Mount-VHD -Path $Path -Passthru
try {
    $volume = Get-Disk -Number $mounted.DiskNumber | Get-Partition |
        Get-Volume | Where-Object FileSystem -eq 'NTFS' | Select-Object -First 1
    if (-not $volume -or -not $volume.DriveLetter) {
        throw 'The image does not contain a mounted NTFS volume.'
    }
    $drive = "$($volume.DriveLetter):"
    chkdsk.exe $drive /scan
    if ($LASTEXITCODE -ne 0) { throw 'chkdsk reported an NTFS error.' }
    @('ASAS-SMALL.TXT', 'ASAS-LARGE.BIN', 'ASAS-LARGE-DIR',
      'ASAS-DISK-FULL.BIN') | ForEach-Object {
        if (Test-Path -LiteralPath (Join-Path $drive $_)) {
            throw "Asas mutation test left an orphaned path: $_"
        }
    }
    $windowsFile = Join-Path $drive 'WINDOWS-POST-CHECK.TXT'
    $windowsRenamed = Join-Path $drive 'WINDOWS-POST-CHECK-RENAMED.TXT'
    Set-Content -LiteralPath $windowsFile -Value 'Windows NTFS post-check' `
        -Encoding ascii
    if ((Get-Content -LiteralPath $windowsFile -Raw).Trim() -ne
        'Windows NTFS post-check') {
        throw 'Windows could not read back the post-check file.'
    }
    Rename-Item -LiteralPath $windowsFile -NewName `
        (Split-Path -Leaf $windowsRenamed)
    Remove-Item -LiteralPath $windowsRenamed
    Get-ChildItem -LiteralPath "$drive\" -Force | Select-Object `
        Mode, Length, Name
} finally {
    Dismount-VHD -Path $Path -ErrorAction SilentlyContinue
}

Write-Host 'NTFS mutation image validation passed.'
