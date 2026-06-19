param(
    [string]$Image = (Join-Path $PSScriptRoot '..\build\asas-os.img'),
    [string]$NtfsImage = '',
    [string]$ExFatImage = '',
    [int]$TimeoutSeconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$qemuCandidates = @(@(
    (Get-Command qemu-system-x86_64.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
    'C:\Program Files\qemu\qemu-system-x86_64.exe',
    'C:\Program Files (x86)\qemu\qemu-system-x86_64.exe'
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) })

$firmwareCandidates = @(@(
    'C:\Program Files\qemu\share\edk2-x86_64-code.fd',
    'C:\Program Files\qemu\share\edk2-x86_64-secure-code.fd',
    'C:\Program Files (x86)\qemu\share\edk2-x86_64-code.fd'
) | Where-Object { Test-Path -LiteralPath $_ })

if (-not $qemuCandidates) {
    throw 'QEMU is not installed. Install qemu-system-x86_64 before running the boot test.'
}

if (-not $firmwareCandidates) {
    throw 'UEFI firmware for QEMU was not found.'
}

$resolvedImage = (Resolve-Path -LiteralPath $Image).Path
$serialLog = Join-Path (Split-Path -Parent $resolvedImage) 'qemu-serial.log'
$errorLog = Join-Path (Split-Path -Parent $resolvedImage) 'qemu-error.log'
Remove-Item -LiteralPath $serialLog -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $errorLog -Force -ErrorAction SilentlyContinue
$qemuImage = $resolvedImage.Replace('\', '/')
$qemuDataImage = (Join-Path (Split-Path -Parent $resolvedImage) 'asas-data-4k.img').Replace('\', '/')
$ahciImagePath = Join-Path (Split-Path -Parent $resolvedImage) 'asas-ahci.img'
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $resolvedImage) 'asas-data.img') -Destination $ahciImagePath -Force
$qemuAhciImage = $ahciImagePath.Replace('\', '/')
$nvmeImagePath = Join-Path (Split-Path -Parent $resolvedImage) 'asas-nvme.img'
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $resolvedImage) 'asas-data.img') -Destination $nvmeImagePath -Force
$qemuNvmeImage = $nvmeImagePath.Replace('\', '/')
$qemuFirmware = $firmwareCandidates[0].Replace('\', '/')
$httpServerResponse = ''

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse('127.0.0.1'), 0)
    try {
        $listener.Start()
        return $listener.LocalEndpoint.Port
    } finally {
        $listener.Stop()
    }
}

$qmpPort = Get-FreeTcpPort
$httpForwardPort = Get-FreeTcpPort

$arguments = @(
    '-machine', 'q35',
    '-m', '128M',
    '-smp', '4',
    '-drive', "if=pflash,format=raw,readonly=on,file=$qemuFirmware",
    '-drive', "format=raw,file=$qemuImage",
    '-drive', "if=none,format=raw,file=$qemuDataImage,id=virtio-disk",
    '-drive', "if=none,format=raw,file=$qemuAhciImage,id=ahci-disk",
    '-drive', "if=none,format=raw,file=$qemuNvmeImage,id=nvme-disk",
    '-drive', "if=none,format=raw,readonly=on,file=$qemuDataImage,id=usb-disk",
    '-netdev', "user,id=net0,hostfwd=tcp:127.0.0.1:$httpForwardPort-10.0.2.15:80",
    '-device', 'virtio-blk-pci,drive=virtio-disk,disable-modern=on,disable-legacy=off,logical_block_size=4096,physical_block_size=4096',
    '-device', 'ich9-ahci,id=ahci',
    '-device', 'ide-hd,drive=ahci-disk,bus=ahci.0',
    '-device', 'nvme,drive=nvme-disk,serial=asasnvme',
    '-device', 'virtio-net-pci,netdev=net0,disable-modern=on,disable-legacy=off',
    '-device', 'qemu-xhci,id=xhci',
    '-device', 'usb-kbd,bus=xhci.0',
    '-device', 'usb-mouse,bus=xhci.0',
    '-device', 'usb-storage,bus=xhci.0,drive=usb-disk',
    '-qmp', "tcp:127.0.0.1:$qmpPort,server=on,wait=off",
    '-serial', 'stdio',
    '-display', 'none',
    '-no-reboot',
    '-no-shutdown'
)

if ($NtfsImage -ne '') {
    $resolvedNtfsImage = (Resolve-Path -LiteralPath $NtfsImage).Path.Replace('\', '/')
    $arguments += @(
        '-drive', "if=none,format=vhdx,file=$resolvedNtfsImage,id=ntfs-test",
        '-device', 'ide-hd,drive=ntfs-test,bus=ahci.1'
    )
}

if ($ExFatImage -ne '') {
    $resolvedExFatImage = (Resolve-Path -LiteralPath $ExFatImage).Path.Replace('\', '/')
    $arguments += @(
        '-drive', "if=none,format=vhdx,file=$resolvedExFatImage,id=exfat-test",
        '-device', 'ide-hd,drive=exfat-test,bus=ahci.2'
    )
}

function Quote-Argument([string]$argument) {
    if ($argument.Contains(' ') -or $argument.Contains('"')) {
        return '"' + $argument.Replace('"', '\"') + '"'
    }

    return $argument
}

function Invoke-HttpServerProbe([int]$Port) {
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = $null
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $connectTask = $client.ConnectAsync('127.0.0.1', $Port)
            if (-not $connectTask.Wait(500)) {
                $client.Close()
                Start-Sleep -Milliseconds 250
                continue
            }
            $stream = $client.GetStream()
            $stream.ReadTimeout = 1000
            $requestBytes = [Text.Encoding]::ASCII.GetBytes("GET / HTTP/1.0`r`nHost: asas`r`n`r`n")
            $stream.Write($requestBytes, 0, $requestBytes.Length)
            $buffer = New-Object byte[] 512
            $count = $stream.Read($buffer, 0, $buffer.Length)
            return [Text.Encoding]::ASCII.GetString($buffer, 0, $count)
        } catch {
            Start-Sleep -Milliseconds 250
        } finally {
            if ($client) {
                $client.Close()
            }
        }
    }
    return ''
}

$processInfo = New-Object System.Diagnostics.ProcessStartInfo
$processInfo.FileName = $qemuCandidates[0]
$processInfo.Arguments = (($arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
$processInfo.UseShellExecute = $false
$processInfo.CreateNoWindow = $true
$processInfo.RedirectStandardOutput = $true
$processInfo.RedirectStandardError = $true

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $processInfo
[void]$process.Start()
$outputTask = $process.StandardOutput.ReadToEndAsync()
$errorTask = $process.StandardError.ReadToEndAsync()

try {
    Start-Sleep -Seconds 2
    $inputJob = Start-Job -ArgumentList $qmpPort -ScriptBlock {
        param([int]$Port)
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true
        $writer.WriteLine('{ "execute": "qmp_capabilities" }')
        Start-Sleep -Milliseconds 100
        foreach ($attempt in 1..40) {
            $writer.WriteLine('{ "execute": "human-monitor-command", "arguments": { "command-line": "sendkey h" } }')
            $writer.WriteLine('{ "execute": "human-monitor-command", "arguments": { "command-line": "mouse_move 10 5" } }')
            Start-Sleep -Milliseconds 500
        }
        $client.Close()
    }
    $httpServerResponse = Invoke-HttpServerProbe $httpForwardPort
    Wait-Job $inputJob -Timeout 1 | Out-Null
    if ($inputJob.State -eq 'Failed') {
        Write-Warning "Could not inject USB keyboard input through QMP: $($inputJob.ChildJobs[0].JobStateInfo.Reason.Message)"
    }
    Remove-Job $inputJob -Force
    Start-Sleep -Seconds $TimeoutSeconds
} finally {
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
}

$log = $outputTask.GetAwaiter().GetResult()
$errorText = $errorTask.GetAwaiter().GetResult()
[IO.File]::WriteAllText($serialLog, $log)
[IO.File]::WriteAllText($errorLog, $errorText)

if ([string]::IsNullOrWhiteSpace($log)) {
    throw "QEMU produced no serial log. $errorText"
}
$requiredMessages = @(
    'crash log ring initialized',
    'crash log analysis verified',
    'BootInfo accepted',
    'ACPI MADT processor discovery completed',
    'ACPI power management initialized',
    'ACPI shutdown command available',
    'platform reboot command available',
    'ACPI battery namespace',
    'PC speaker audio initialized',
    'PC speaker beep command available',
    'boot services exited; kernel is independent',
    'physical frame allocator verified',
    'virtual page mapping verified',
    'kernel heap verified',
    'kernel heap NX protection verified',
    'kernel GDT installed',
    'kernel IDT installed',
    'local APIC timer calibrated and started',
    'local APIC timer tick verified',
    'ASLR seed initialized',
    'ASLR user stack randomization verified',
    'secondary processors are online',
    'PCI device discovery completed',
    'ACPI touchpad namespace',
    'PCI Wi-Fi network controller',
    'VirtIO network PCI device discovered',
    'VirtIO network queues initialized',
    'VirtIO network receive buffers posted',
    'VirtIO network Ethernet transmit verified',
    'DHCP discover transmitted',
    'DHCP offer received',
    'DHCP request transmitted',
    'DHCP ack received',
    'ARP request transmit verified',
    'VirtIO network receive polling verified',
    'ARP reply received',
    'ICMP echo request transmit verified',
    'ICMP echo reply received',
    'DNS query transmitted',
    'xHCI USB controller discovered',
    'xHCI connected USB ports',
    'xHCI connected USB device port detected',
    'xHCI command and event rings running',
    'xHCI enabled device slot',
    'xHCI command completion event verified',
    'xHCI addressed USB port',
    'xHCI USB port speed',
    'xHCI Address Device command verified',
    'USB device vendor ID',
    'USB device product ID',
    'USB device descriptor read verified',
    'USB interface class',
    'USB interface protocol',
    'USB HID keyboard interface detected',
    'USB HID interrupt endpoint address',
    'USB HID interrupt endpoint packet size',
    'USB HID interrupt endpoint configured',
    'USB device configuration selected',
    'USB HID keyboard report transfer queued',
    'USB HID mouse interface detected',
    'USB HID mouse interrupt endpoint configured',
    'USB HID mouse report transfer queued',
    'USB Mass Storage interface detected',
    'USB Mass Storage bulk endpoints configured',
    'USB Mass Storage SCSI INQUIRY completed',
    'USB Mass Storage READ CAPACITY completed',
    'USB Mass Storage sector read verified',
    'USB Mass Storage direct block device registered',
    'USB Mass Storage multi-block request verified',
    'USB HID keyboard report received',
    'USB HID character injected',
    'USB HID mouse report received',
    'USB HID mouse state updated',
    'xHCI controller registers verified',
    'VirtIO block PCI device discovered',
    'VirtIO direct block device registered',
    'VirtIO multi-block request verified',
    'FAT32 4096-byte logical sectors verified',
    'block sector read verified',
    'loading HELLO.EXE from VFS',
    'PS2 keyboard controller initialized',
    'PS2 keyboard IRQ routed through IOAPIC',
    'PS2 mouse initialized',
    'AHCI storage controller discovered',
    'AHCI identify and geometry verified',
    'AHCI direct read write flush verified',
    'AHCI direct block device registered',
    'AHCI multi-block request verified',
    'NVMe storage controller discovered',
    'NVMe queues initialized',
    'NVMe sector read verified',
    'NVMe sector write and flush verified',
    'NVMe direct block device registered',
    'NVMe multi-block request verified',
    'scheduler context switch verified',
    'preemptive scheduler verified',
    'process address space isolation verified',
    'IPC message queue verified',
    'partition manager safety self tests verified',
    'MBR partition mutation transactions verified',
    'mounted volume partition mutation guard verified',
    'GPT rare entry layout rejection verified',
    'block device capabilities and bounds verified',
    'NTFS strict USA fixup validation verified',
    'NTFS MFT writeback USA protection verified',
    'NTFS 4Kn USA and sparse runlist verified',
    'NTFS MFT mirror attribute list and large directory support ready',
    'NTFS mutation journal rollback and barriers verified',
    'NTFS bitmap preflight and MFT record builder verified',
    'exFAT transactional mutation and remount self test verified',
    'mount manager namespace slots and busy unmount verified',
    'GPT type label and UUID metadata verified',
    'GPT primary backup mutation transaction verified',
    'VFS open read interfaces verified',
    'VFS storage backend verified',
    'FAT32 LFN Unicode lookup verified',
    'FAT32 LFN create rename delete verified',
    'FAT32 FAT mirror policy and timestamps verified',
    'FAT32 disk-full preflight and rollback verified',
    'security users initialized',
    'security permissions verified',
    'kernel heap stress leak check verified',
    'frame allocator stress leak check verified',
    'memory stress and leak tests verified',
    '[Asas][SHELL] root',
    '[Asas][SHELL] block devices',
    'permission read',
    'permission write',
    'permission execute',
    'permission admin',
    '[Asas][SHELL] /',
    '[Asas][SHELL] /ASAS',
    'DISK.TXT',
    'System Readme.txt',
    'Asas OS FAT32 storage is online.',
    'Asas OS system directory is online.',
    'Asas writable FAT32 file',
    'Nested FAT32 file',
    'Copy and move verified',
    'NEW.TXT',
    'LARGE.BIN',
    'WORK',
    'VFS multi-sector write verified',
    'VFS directory create delete verified',
    'VFS nested modification verified',
    'FAT32 directory chain growth verified',
    'FAT32 native rename and move verified',
    'shell copy move commands verified',
    'shell filesystem commands verified',
    'active process count',
    'ping ok',
    'wget saved /WGET.TXT',
    'HTTP/',
    'http-server listening on 0.0.0.0:80',
    'http-server served one request',
    'power acpi ready',
    'battery namespace',
    'beep ok',
    'touchpad namespace',
    'wifi controller',
    'AsasGUI initialized',
    'AsasGUI initialized and thread started',
    'keyboard shell input pipeline ready',
    'user mode system call verified',
    'Hello from an Asas OS C user program',
    'user program exited'
)

if ($NtfsImage -ne '') {
    $requiredMessages += @(
        'NTFS runlist growth attribute list and move verified',
        'NTFS large directory split and remount verified',
        'NTFS index merge cleanup and allocation release verified',
        'NTFS disk full rollback and remount verified',
        'NTFS mutation integration suite passed'
    )
}

if ($ExFatImage -ne '') {
    $requiredMessages += 'exFAT create write overwrite move delete and remount verified'
}

foreach ($message in $requiredMessages) {
    if (-not $log.Contains($message)) {
        throw "QEMU boot test failed: missing serial message '$message'."
    }
}

if (-not $httpServerResponse.Contains('Asas http ok')) {
    throw 'QEMU boot test failed: host HTTP probe did not receive the Asas http-server response.'
}

Write-Host 'QEMU boot test passed.' -ForegroundColor Green
