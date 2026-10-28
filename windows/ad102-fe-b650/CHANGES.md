# AD102 RTX 4090 FE / MSI B650 port, 2026-10-27

- Copy the AD104 port and pin it to an RTX 4090 Founders Edition on an MSI MAG B650 TOMAHAWK WIFI, using two cold-boot captures from the read-only probe.
- Replace the identity pins (10de:2684, subsystem 10de:16f4, BAR0 0xf5000000, chip 0x192000a1), the PROM hash, the FWSEC geometry, the PCI config dwords and the cold register values (FB size 0x5ff4, MMU high 0x5cf3ff0).
- Admit only the RTX 4090 in the vendor transaction and remove the AD104 device ID from its AD102-family lists.
- Pin the display release to this board's GPU, GOP and xHCI device paths, and to the code hashes, image sizes and Stop entry points of nvgop-ad1xx 0x7000d and the two firmware console drivers.
- Pin the Windows ESP as partition 1 at LBA 2048 with 204,800 sectors. The bootmgfw.efi size and hash move to the ignored local header because Windows updates replace that file.
- Generate the host-test graph, USB paths and config space from the capture with tools/make_test_fixtures.py, so tests no longer carry hand-copied handle numbers.
- Show the boot prompt in English and rename the USB folders to AD102-ASSETS and AD102-UNLOCK.
- RUN0001 (one cold manual launch): TRANSACTION OK and Windows handoff; tensor probe FP16/FP32 149.2 -> 297.6 T/s, TF32 81.6 -> 162.3 T/s, INT4 control unchanged.

The AD104 history below explains rules this port inherits.

# Latence.app AD104 Windows changes — 2026-10-24

- Adapt the pinned Ada transaction to checked UEFI PCI I/O, DMA and explicit error/retention state for the captured AD104 profile.
- Add MSI console/GOP ownership capture/validation and callback-based release, exclusive PCI claim and IO/MEM-only restoration.
- Preload a pinned Windows manager from a read-only ESP, require U for one attempt and contain all post-mutation failures.
- Handle periodic USB additions/removals within bounded preflight-classified USB ownership, while preserving GPU/display/console and host ownership invariants.
- Require payload marker, HALT, permission and selector/readout checks before/after one FLR; free DMA only after verified quiescence.
- Record one successful Windows handoff and three numerical/throughput runs; retain exact scope and pending A/B/A/long-duration qualification.
- Publish source-only with an ignored local ESP header, synthetic mock-test GUID, configurable compiler paths, diagnostic source and sanitized evidence. The configured public source rebuilds the historical hardware EFI byte-for-byte; 177 scenarios freshly pass.

Upstream commits and full notices remain in provenance.json and licence files. No Linux target admission changes.
