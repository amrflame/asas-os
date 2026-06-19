# Contributing to Asas OS

Thank you for considering a contribution. Asas OS is an experimental operating
system, so small, well-tested changes are much more valuable than large rewrites.

## Project Status

Asas OS is not a production operating system. It is a research and learning
project focused on UEFI boot, kernel fundamentals, storage drivers, filesystems,
VFS, GUI, and virtualization support.

Disk, filesystem, partition, and virtual-disk code can destroy data if it is
wrong. Contributions in these areas must be conservative and include clear test
notes.

## Getting Started

1. Fork the repository.
2. Create a topic branch from the default branch.
3. Build locally on Windows:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-AsasIso.ps1 -Build
```

4. Run the relevant test script when possible:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Run-QemuBootTest.ps1
```

5. Open a pull request with a clear description and test evidence.

## Contribution Rules

- Keep changes focused. One feature or fix per pull request is best.
- Prefer simple C interfaces between kernel subsystems.
- Keep kernel code freestanding-friendly. Do not assume a hosted C runtime.
- Do not remove fallback hardware paths unless the replacement is verified.
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

## License

By contributing, you agree that your contribution is licensed under the
GNU General Public License v3.0 only, matching this repository.

