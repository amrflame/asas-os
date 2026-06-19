param(
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'
$bootDir = Join-Path $buildDir 'EFI\BOOT'
$asasDir = Join-Path $buildDir 'ASAS'
$objDir = Join-Path $buildDir 'obj'
$sdkDir = Join-Path $buildDir 'sdk'

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

function Find-MsvcToolset {
    $roots = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installations = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        foreach ($installation in $installations) {
            if ($installation) {
                $roots += (Join-Path $installation 'VC\Tools\MSVC')
            }
        }
    }
    $roots += @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC'
    )
    foreach ($rootPath in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $rootPath)) { continue }
        $candidate = Get-ChildItem -LiteralPath $rootPath -Directory |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }
    return $null
}

$toolset = Find-MsvcToolset

if (-not $toolset) {
    throw 'Visual Studio 2022 C++ tools were not found. Install Desktop development with C++ or Visual Studio Build Tools.'
}

$compiler = Join-Path $toolset.FullName 'bin\Hostx64\x64\cl.exe'
$linker = Join-Path $toolset.FullName 'bin\Hostx64\x64\link.exe'

New-Item -ItemType Directory -Force -Path $bootDir, $asasDir, $objDir, $sdkDir | Out-Null
$nasm = Join-Path (Split-Path -Parent $root) 'tools\nasm-2.16.03\nasm.exe'

function Build-EfiImage {
    param(
        [string]$Name,
        [string[]]$Sources,
        [string[]]$AssemblySources = @(),
        [string]$EntryPoint,
        [string]$Output
    )

    $objectFiles = @()

    foreach ($source in $Sources) {
        $sourceName = [System.IO.Path]::GetFileNameWithoutExtension($source)
        $objectFile = Join-Path $objDir "$Name-$sourceName.obj"
        $objectFiles += $objectFile

        Write-Host "Compiling $Name/$sourceName..." -ForegroundColor Cyan
        & $compiler `
            /nologo /c /TC /W4 /WX /O2 /GS- /Zl /Oi- `
            /I (Join-Path $root 'include') `
            /Fo$objectFile `
            $source
        if ($LASTEXITCODE -ne 0) { throw "$Name/$sourceName compilation failed." }
    }

    foreach ($source in $AssemblySources) {
        $sourceName = [System.IO.Path]::GetFileNameWithoutExtension($source)
        $objectFile = Join-Path $objDir "$Name-$sourceName.obj"
        $objectFiles += $objectFile
        Write-Host "Assembling $Name/$sourceName..." -ForegroundColor Cyan
        & $nasm -f win64 $source -o $objectFile
        if ($LASTEXITCODE -ne 0) { throw "$Name/$sourceName assembly failed." }
    }

    Write-Host "Linking $Name..." -ForegroundColor Cyan
    & $linker `
        /nologo /nodefaultlib /machine:x64 `
        /subsystem:EFI_APPLICATION /entry:$EntryPoint `
        /fixed:no `
        /out:$Output `
        $objectFiles
    if ($LASTEXITCODE -ne 0) { throw "$Name linking failed." }

    $size = (Get-Item -LiteralPath $Output).Length
    Write-Host "  $Output ($size bytes)" -ForegroundColor Green
}

Write-Host ''
Write-Host 'Building Asas OS...' -ForegroundColor Yellow

$apTrampoline = Join-Path $asasDir 'APBOOT.BIN'
Write-Host 'Assembling AP trampoline...' -ForegroundColor Cyan
& $nasm -f bin (Join-Path $root 'src\kernel\ap_trampoline.asm') -o $apTrampoline
if ($LASTEXITCODE -ne 0) { throw 'AP trampoline assembly failed.' }

Build-EfiImage `
    -Name 'bootloader' `
    -Sources @((Join-Path $root 'src\boot\main.c')) `
    -EntryPoint 'efi_main' `
    -Output (Join-Path $bootDir 'BOOTX64.EFI')

Build-EfiImage `
    -Name 'kernel' `
    -Sources @(
        (Join-Path $root 'src\kernel\main.c'),
        (Join-Path $root 'src\kernel\architecture.c'),
        (Join-Path $root 'src\kernel\aslr.c'),
        (Join-Path $root 'src\kernel\audio.c'),
        (Join-Path $root 'src\kernel\acpi.c'),
        (Join-Path $root 'src\kernel\ahci.c'),
        (Join-Path $root 'src\kernel\apic.c'),
        (Join-Path $root 'src\kernel\block_device.c'),
        (Join-Path $root 'src\kernel\exfat.c'),
        (Join-Path $root 'src\kernel\ext2.c'),
        (Join-Path $root 'src\kernel\fat16.c'),
        (Join-Path $root 'src\kernel\fat32.c'),
        (Join-Path $root 'src\kernel\filesystem.c'),
        (Join-Path $root 'src\kernel\iso9660.c'),
        (Join-Path $root 'src\kernel\ntfs.c'),
        (Join-Path $root 'src\kernel\udf.c'),
        (Join-Path $root 'src\kernel\framebuffer.c'),
        (Join-Path $root 'src\kernel\safemode_gui.c'),
        (Join-Path $root 'src\kernel\gui_main.c'),
        (Join-Path $root 'src\kernel\gui_draw.c'),
        (Join-Path $root 'src\kernel\gui_compositor.c'),
        (Join-Path $root 'src\kernel\gui_icons.c'),
        (Join-Path $root 'src\kernel\gui_wm.c'),
        (Join-Path $root 'src\kernel\gui_desktop.c'),
        (Join-Path $root 'src\kernel\gui_taskbar.c'),
        (Join-Path $root 'src\kernel\gui_startmenu.c'),
        (Join-Path $root 'src\kernel\gui_app_terminal.c'),
        (Join-Path $root 'src\kernel\gui_app_files.c'),
        (Join-Path $root 'src\kernel\gui_app_editor.c'),
        (Join-Path $root 'src\kernel\gui_app_calc.c'),
        (Join-Path $root 'src\kernel\gui_app_diskmgmt.c'),
        (Join-Path $root 'src\kernel\gui_app_settings.c'),
        (Join-Path $root 'src\kernel\gui_app_about.c'),
        (Join-Path $root 'src\kernel\gfx.c'),
        (Join-Path $root 'src\kernel\virtio_gpu.c'),
        (Join-Path $root 'src\kernel\gui_widget.c'),
        (Join-Path $root 'src\kernel\gui_anim.c'),
        (Join-Path $root 'src\kernel\gui_font.c'),
        (Join-Path $root 'src\kernel\gui_notify.c'),
        (Join-Path $root 'src\kernel\heap.c'),
        (Join-Path $root 'src\kernel\hyperv_storage.c'),
        (Join-Path $root 'src\kernel\ide_ata.c'),
        (Join-Path $root 'src\kernel\ioapic.c'),
        (Join-Path $root 'src\kernel\ipc.c'),
        (Join-Path $root 'src\kernel\keyboard.c'),
        (Join-Path $root 'src\kernel\console.c'),
        (Join-Path $root 'src\kernel\crash.c'),
        (Join-Path $root 'src\kernel\disk_management.c'),
        (Join-Path $root 'src\kernel\laptop.c'),
        (Join-Path $root 'src\kernel\cpu.c'),
        (Join-Path $root 'src\kernel\logger.c'),
        (Join-Path $root 'src\kernel\memory.c'),
        (Join-Path $root 'src\kernel\mouse.c'),
        (Join-Path $root 'src\kernel\nvme.c'),
        (Join-Path $root 'src\kernel\paging.c'),
        (Join-Path $root 'src\kernel\pe_loader.c'),
        (Join-Path $root 'src\kernel\pci.c'),
        (Join-Path $root 'src\kernel\panic.c'),
        (Join-Path $root 'src\kernel\partition.c'),
        (Join-Path $root 'src\kernel\process.c'),
        (Join-Path $root 'src\kernel\power.c'),
        (Join-Path $root 'src\kernel\runtime.c'),
        (Join-Path $root 'src\kernel\scheduler.c'),
        (Join-Path $root 'src\kernel\security.c'),
        (Join-Path $root 'src\kernel\shell.c'),
        (Join-Path $root 'src\kernel\smp.c'),
        (Join-Path $root 'src\kernel\stability.c'),
        (Join-Path $root 'src\kernel\syscall.c'),
        (Join-Path $root 'src\kernel\virtio_block.c'),
        (Join-Path $root 'src\kernel\virtio_net.c'),
        (Join-Path $root 'src\kernel\virtual_disk.c'),
        (Join-Path $root 'src\kernel\vfs.c'),
        (Join-Path $root 'src\kernel\xhci.c')
    ) `
    -AssemblySources @((Join-Path $root 'src\kernel\x86_64.asm')) `
    -EntryPoint 'kernel_main' `
    -Output (Join-Path $asasDir 'KERNEL.EFI')

Write-Host 'Building Asas user library objects...' -ForegroundColor Cyan
$libcObject = Join-Path $sdkDir 'asas_libc.obj'
& $compiler `
    /nologo /c /TC /W4 /WX /O2 /GS- /Zl /Oi- `
    /I (Join-Path $root 'include') `
    /Fo$libcObject `
    (Join-Path $root 'src\user\asas_libc.c')
if ($LASTEXITCODE -ne 0) { throw 'Asas libc compilation failed.' }

& $nasm -f win64 `
    (Join-Path $root 'src\user\asas_syscall.asm') `
    -o (Join-Path $sdkDir 'asas_syscall.obj')
if ($LASTEXITCODE -ne 0) { throw 'Asas syscall wrapper assembly failed.' }

$helloObject = Join-Path $sdkDir 'hello.obj'
$helloProgram = Join-Path $sdkDir 'HELLO.EXE'
& $compiler `
    /nologo /c /TC /W4 /WX /O2 /GS- /Zl /Oi- `
    /I (Join-Path $root 'include') `
    /Fo$helloObject `
    (Join-Path $root 'src\user\programs\hello.c')
if ($LASTEXITCODE -ne 0) { throw 'Hello user program compilation failed.' }

& $linker `
    /nologo /nodefaultlib /machine:x64 `
    /subsystem:NATIVE /entry:user_main /base:0x0000600000200000 /fixed `
    /out:$helloProgram `
    $helloObject `
    $libcObject `
    (Join-Path $sdkDir 'asas_syscall.obj')
if ($LASTEXITCODE -ne 0) { throw 'Hello user program linking failed.' }

$cppObject = Join-Path $sdkDir 'cpp_demo.obj'
$cppProgram = Join-Path $sdkDir 'CPPDEMO.EXE'
& $compiler `
    /nologo /c /TP /std:c++20 /W4 /WX /O2 /GS- /Zl /Oi- /GR- /EHs-c- `
    /I (Join-Path $root 'include') `
    /Fo$cppObject `
    (Join-Path $root 'src\user\programs\cpp_demo.cpp')
if ($LASTEXITCODE -ne 0) { throw 'C++ user program compilation failed.' }

& $linker `
    /nologo /nodefaultlib /machine:x64 `
    /subsystem:NATIVE /entry:cpp_user_main /fixed:no `
    /out:$cppProgram `
    $cppObject `
    $libcObject `
    (Join-Path $sdkDir 'asas_syscall.obj')
if ($LASTEXITCODE -ne 0) { throw 'C++ user program linking failed.' }

Write-Host ''
Write-Host 'Build complete.' -ForegroundColor Green
Write-Host "Boot volume root: $buildDir" -ForegroundColor Gray

$imagePath = Join-Path $buildDir 'asas-os.img'
& (Join-Path $root 'tools\New-BootImage.ps1') `
    -Bootloader (Join-Path $bootDir 'BOOTX64.EFI') `
    -Kernel (Join-Path $asasDir 'KERNEL.EFI') `
    -ApTrampoline $apTrampoline `
    -UserProgram $helloProgram `
    -Output $imagePath
if ($LASTEXITCODE -ne 0) { throw 'Boot image creation failed.' }

& (Join-Path $root 'tests\Test-BootImage.ps1') -Image $imagePath
if ($LASTEXITCODE -ne 0) { throw 'Boot image verification failed.' }

Copy-Item -LiteralPath $imagePath -Destination (Join-Path $buildDir 'asas-data.img') -Force

& (Join-Path $root 'tools\New-BootImage.ps1') `
    -Bootloader (Join-Path $bootDir 'BOOTX64.EFI') `
    -Kernel (Join-Path $asasDir 'KERNEL.EFI') `
    -ApTrampoline $apTrampoline `
    -UserProgram $helloProgram `
    -Output (Join-Path $buildDir 'asas-data-4k.img') `
    -TotalSectors 16384 `
    -BytesPerSector 4096
if ($LASTEXITCODE -ne 0) { throw '4Kn FAT32 data image creation failed.' }
