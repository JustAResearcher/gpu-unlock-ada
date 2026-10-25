# RTX 4070 SUPER Windows UEFI port

This source tree reproduces the manual RTX 4070 SUPER unlock tested on Windows
on 2026-10-25. The tested card is `10de:2783`, subsystem `1458:413b`, on an MSI
MAG Z790 TOMAHAWK WIFI with BIOS H.E0. Matched cuBLAS GEMM measurements at the
same 220 W power limit gained 49.5% for FP16/FP32 and 43.1% for BF16/FP32.

The repository's `windows/ad104-4070-super/README.md` contains the build,
capture, disk-boot and measurement workflow. `docs/ad104-4070-super-windows.md`
records the physical run and measured results. `provenance.json` identifies
the source revision, capture and tested binary.

Keep `local/boot_profile.h`, captures, firmware and build products private.
The example header describes the three local values required by this profile.
The disk geometry and Windows image hash are also pinned in `boot_io.c`.

The implementation builds on Latence.app's AD104 Windows port, this project's
Ada transaction code and TOTORONG's payload. `UPSTREAM-README.md` preserves the
original port's documentation and credits. Retain the included licences and
copyright notices when sharing source.
