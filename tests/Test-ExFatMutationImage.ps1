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
        Get-Volume | Where-Object FileSystem -eq 'exFAT' | Select-Object -First 1
    if (-not $volume -or -not $volume.DriveLetter) {
        throw 'The image does not contain a mounted exFAT volume.'
    }
    $drive = "$($volume.DriveLetter):"
    chkdsk.exe $drive /scan
    if ($LASTEXITCODE -ne 0) { throw 'chkdsk reported an exFAT error.' }
    @('ASAS-CREATED.TXT', 'ASAS-DIR') | ForEach-Object {
        if (Test-Path -LiteralPath (Join-Path $drive $_)) {
            throw "Asas exFAT mutation left a temporary path: $_"
        }
    }
    $file = Join-Path $drive 'WINDOWS-EXFAT-CHECK.TXT'
    Set-Content -LiteralPath $file -Value 'Windows exFAT post-check' -Encoding ascii
    Rename-Item -LiteralPath $file -NewName 'WINDOWS-EXFAT-RENAMED.TXT'
    Remove-Item -LiteralPath (Join-Path $drive 'WINDOWS-EXFAT-RENAMED.TXT')
} finally {
    Dismount-VHD -Path $Path -ErrorAction SilentlyContinue
}
Write-Host 'exFAT mutation image validation passed.'
