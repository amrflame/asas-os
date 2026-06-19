# Asas OS Hardware Compatibility

This list documents the hardware targets covered by the current plan.

## Verified In QEMU

| Area | Device or feature | Status |
|---|---|---|
| Firmware | UEFI q35 machine | Verified |
| CPU | x86_64, APIC, TSC, MSR, NX | Verified |
| SMP | 4 virtual CPUs | Verified |
| Display | GOP framebuffer | Verified |
| Input | PS/2 keyboard and mouse | Verified |
| USB | qemu-xhci, USB keyboard, USB mouse | Verified |
| USB storage | USB Mass Storage BOT/SCSI sector read | Verified |
| Block storage | VirtIO Block | Verified |
| Network | VirtIO Network legacy PCI | Verified |
| SATA | AHCI controller and active port discovery | Verified |
| Power | ACPI shutdown, reboot, sleep availability | Verified |
| Audio | PC speaker beep path | Verified |
| Laptop probes | ACPI battery, ACPI touchpad, PCI Wi-Fi class | Probe verified; hardware absent in QEMU |

## Real Hardware Validation Targets

| Area | Minimum target | Current status |
|---|---|---|
| Laptop | UEFI x86_64 laptop with GOP framebuffer | Pending physical validation |
| PC | UEFI x86_64 desktop with PS/2 or USB input | Pending physical validation |
| Battery | ACPI battery exposing `PNP0C0A`, `_BIF`, or `_BST` | Probe implemented; pending hardware |
| Touchpad | ACPI namespace with common Touchpad identifiers | Probe implemented; pending hardware |
| Wi-Fi | PCI network controller class `0x02/0x80` | Probe implemented; pending hardware |
| SATA | AHCI controller with active SATA disk | Sector read verified in QEMU |
| NVMe | PCI NVMe controller class `0x01/0x08/0x02` | Queue initialization and sector read verified in QEMU |
