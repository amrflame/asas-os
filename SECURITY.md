# Security Policy

Asas OS is experimental and not suitable for protecting production data or
production machines.

## Supported Versions

Security fixes are accepted for the default development branch and the latest
public release branch, if one exists.

## Reporting A Vulnerability

Please do not publish exploit details in a public issue before maintainers have
had time to review the report.

If a private security contact is configured on GitHub, use it. If not, open a
short public issue asking maintainers to provide a private contact path, without
including sensitive details.

Useful reports include:

- affected commit or release;
- environment, such as QEMU, Hyper-V, or physical hardware;
- reproduction steps;
- whether disk writes, filesystem metadata, boot, privilege boundaries, or
  memory safety are involved;
- expected and actual behavior.

## High-Risk Areas

Please treat these as security-sensitive:

- bootloader and kernel image loading;
- paging, memory allocation, and user/kernel boundaries;
- syscall validation;
- disk writes, partition mutation, and filesystem repair;
- virtual disk parsing;
- network packet parsing.

## Disclosure

Maintainers will triage reports as time permits. Because this is an experimental
project, no fixed response SLA is promised yet.

