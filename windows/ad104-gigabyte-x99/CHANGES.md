# AD104 Windows Changes — Gigabyte RTX 4070 Ti + Huananzhi X99 Port - 2026-10-27

- Ported AD104 Windows UEFI unlock to Gigabyte GeForce RTX 4070 Ti (`10de:2782`, subsystem `1458:40e8`, VBIOS `95.04.3c.c0.d2`).
- Adapted console release architecture for Huananzhi X99-F8D Plus (Intel C612 / AMI Aptio IV/V UEFI BIOS): replaced static topological handle tables with dynamic UEFI driver binding discovery (`OpenProtocolInformation` -> `DisconnectController` -> `OpenProtocol(EXCLUSIVE)` -> decode restore).
- Supported Huananzhi X99 BAR0 address (`0xc6000000ULL`) and validated VBIOS payload hash (`35a6c1ad3225b1266cfacaf6ea014f137f32c54888eb676e230a33a8ac9d19c2`).
- Fixed Windows bootmgfw image sizing and DevicePath fallback in `boot_io.c` for standard Windows 11 ESP partitions.
- Validated on real physical hardware: latch achieved (`SS0=0x88888888`, `SS1=0x8`), clean boot into Windows 11 with NVIDIA driver 616.92 without Code 43 error.
- Verified GEMM throughput via PyTorch 2.6 cuBLAS benchmark: BF16 GEMM 143.76 TFLOPS (>91% of physical dense silicon peak) at full 285 W power limit.
- Provided dual-toolchain build support (GCC / GNU ld with automatic PE import zeroing, and MSVC link), and standalone `build.py`.
