# Contributing to Asas OS

Thank you for considering a contribution. Asas OS is an experimental operating
system with a deliberately open development style: learn the system, improve one
layer, leave it clearer for the next person, and help turn a small OS into a
serious community project.

You do not need to arrive as a kernel expert. Good issues include documentation,
test scripts, screenshots, shell commands, UI polish, hardware notes, VM
verification, and careful bug reports. If you are already comfortable with
systems programming, there is plenty of deeper work in storage, filesystems,
memory, scheduling, networking, and boot.

## Project Status

Asas OS is not a production operating system. It is a research and learning
project focused on UEFI boot, kernel fundamentals, storage drivers, filesystems,
VFS, GUI, and virtualization support. The goal is to make real OS engineering
approachable without pretending the risky parts are simple.

Disk, filesystem, partition, and virtual-disk code can destroy data if it is
wrong. Contributions in these areas must be conservative and include clear test
notes.

## Getting Started

1. Pick a small area that interests you: docs, tests, GUI, shell, storage,
   filesystems, networking, or boot.
2. Fork the repository.
3. Create a topic branch from the default branch.
4. Build locally on Windows:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

5. Run the relevant test script when possible:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Run-QemuBootTest.ps1
```

6. Open a pull request with a clear description and test evidence.

## Good First Contributions

- Improve README wording, screenshots, diagrams, or setup notes.
- Add missing command examples to shell documentation.
- Add small QEMU/Hyper-V test notes for a configuration you tried.
- Improve error messages in shell or GUI paths.
- Add safe read-only diagnostics for unsupported disk/filesystem states.
- Write or extend tests for path parsing, VFS behavior, or image tools.

## Deeper Contribution Areas

- Block device reliability, telemetry, cache behavior, and flush barriers.
- FAT32, NTFS, exFAT, ISO9660, UDF, ext2/ext4 validation and mutation paths.
- Disk Manager UX for safe mount, remount, format, check, and repair flows.
- Scheduler, process lifecycle, syscalls, and PE user-program loading.
- Network stack experiments and better packet diagnostics.
- Hardware compatibility reports for QEMU, Hyper-V, and physical machines.

## Contribution Rules

- Keep changes focused. One feature or fix per pull request is best.
- Prefer simple C interfaces between kernel subsystems.
- Keep kernel code freestanding-friendly. Do not assume a hosted C runtime.
- Do not remove fallback hardware paths unless the replacement is verified.
- Explain the tradeoff in the pull request when changing boot, storage, memory,
  or security-sensitive code.
- Do not enable writes to a filesystem or disk format unless rollback, bounds
  checks, and flush behavior are understood.
- Treat read-only mode as a temporary safety state, not a final feature, unless
  the hardware is truly read-only.
- Avoid committing generated files such as ISO, IMG, VHD, VHDX, QCOW2, object
  files, or local logs.

## Storage And Filesystem Changes

For storage, partitioning, filesystem, and virtual-disk work, include:

- the image or device type used for testing;
- whether the test was QEMU, Hyper-V, or host-side only;
- read/write/mount/remount behavior checked;
- any corruption, fsck, chkdsk, rollback, or power-loss test performed;
- known unsupported feature flags or safe read-only gates.

Dangerous operations must have validation and dry-run paths before destructive
execution is exposed to users.

## Coding Style

- Use the existing style in the surrounding file.
- Prefer explicit fixed-width types already used by the project.
- Keep comments short and useful.
- Avoid broad refactors in the same pull request as a behavior change.

## Commit And Pull Request Checklist

- The project builds.
- Generated artifacts are not committed.
- User-visible behavior is documented when relevant.
- Tests or manual verification notes are included.
- New commands or GUI actions fail safely when unsupported.
- Reviewers can understand the intent without reconstructing it from the diff.

## License

By contributing, you agree that your contribution is licensed under the
GNU General Public License v3.0 only, matching this repository.
