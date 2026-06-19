# Asas OS Roadmap

This roadmap summarizes the public development direction. The detailed Arabic
trackers remain in `OS_DEVELOPMENT_PLAN_AR.md` and `DISK_MANAGEMENT_PLAN_AR.md`.

## Current Focus

- Reliable Hyper-V Generation 2 and QEMU boot flows.
- Stable block-device registry and mount manager.
- Safer disk management UX for physical and virtual disks.
- FAT32, NTFS, and exFAT read/write behavior with rollback and validation.
- Documentation and community onboarding for public development.

## Near-Term Work

- Improve release packaging and screenshots.
- Expand GitHub Actions coverage.
- Add clearer developer setup docs for Windows, QEMU, and Hyper-V.
- Continue disk-management GUI polish.
- Add more automated checks around filesystem mutations.

## Storage And Filesystems

- Keep FAT32 as a stable read/write baseline.
- Continue guarded NTFS write support and Windows compatibility gates.
- Continue exFAT validation on real Windows-created images.
- Treat ISO9660, UDF, ext2/ext4, QCOW2, and VHDX full write support as gated
  production work requiring rollback, flush barriers, and external fsck/chkdsk
  validation.

## Hardware And Platforms

- Hyper-V Generation 2 remains the main public VM target.
- QEMU remains the main automated test target.
- Physical hardware validation is welcome, but changes must preserve VM paths.

## Community

- Label small tasks as `good first issue`.
- Prefer reviewable pull requests with test notes.
- Keep unsafe disk operations behind validation, dry-run, and explicit user
  confirmation.

