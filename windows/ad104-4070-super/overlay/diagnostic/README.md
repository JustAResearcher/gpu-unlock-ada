# RTX 4070 SUPER cold-state collector

This collector supplied the pre-Windows evidence for the successful 4070 SUPER
port. It targets PCI `10de:2783`, subsystem `1458:413b`, revision `a1`, at
`0000:01:00.0`. The assembled tree also contains the separate manual unlock
application; this collector only reads the GPU.

## Captured evidence

The schema-5 report includes 4096 bytes of PCI configuration, BAR descriptors,
display/console paths, firmware protocol ownership and bounded loaded-driver
code. It also records the cached option ROM, 22 fixed register observations
and the full 1 MiB physical PROM. The physical dump supplies the ROM layout
needed for FWSEC parsing; the smaller cached GOP ROM does not replace it.

Register/PROM reads require the exact card, BAR0 at `0x80000000` with a 16 MiB
window, memory decoding enabled, bus mastering disabled and chip register
`0x194000a1`. The collector rechecks identity and reports incomplete reads as
such. It has no GPU/PCI write, DMA, reset, driver-release or payload path.

The application writes a new `AD104-PROBE/CAPxxxx.JSON` to its own loaded
volume. An existing internal FAT32 ESP or a USB volume uses the same path;
there is no disk search or boot-variable edit. Existing capture names are
skipped. The report is limited to 8 MiB, and short writes are failures.

## Build and capture

Run from this directory:

```powershell
./Build.ps1 -VcVars 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat' -Python python
```

The build runs the application and ownership host suites, checks the PE image
and runs the Python validators. It produces `build/AD104-PROBE.EFI` and
`build/manifest.json`. The manifest identifies the source/header/build-script
bytes used by the collector.

The repository's `windows/ad104-4070-super/README.md` gives the full internal-
disk setup. Place the EFI in its own ESP directory, select it for one cold
boot with Secure Boot disabled, and let it write the capture. It returns to
firmware after a short delay; select the existing Windows Boot Manager.
Leave at least 8 MiB free per capture. No partition creation or formatting is
needed on the tested system.

## Inspect the capture

Use the manifest from the binary that actually collected the file:

```powershell
python tools/analyze.py ../private/CAP0002.JSON --manifest build/manifest.json
```

Replace the capture path with its actual location. The analyzer checks schema,
build identity, card identity and captured-byte hashes. The parent tree's
`tools/extract_private_capture.py` extracts the five private inputs for the
4070 SUPER host suites and requires this profile's exact ROM/code hashes.

Keep raw captures local: they contain ROM and driver bytes plus device paths.
Public reproduction data consists of hashes, reviewed structural fixtures,
matrix measurements and a sanitized transaction log. Capture validation checks
recorded data; it does not automatically qualify a different hardware profile.

The successful run's capture had one unavailable optional register observation
(`SEC2_CPUCTL`, `0xbadf5620`), so its overall register snapshot was marked
incomplete. The 19 cold registers required by the unlock profile were present
and matched; later engine checks remained enabled.

Licence: `LICENSE.txt`, `THIRD_PARTY.md` and the adjacent
`vendor/edk2/License.txt`. Original copyright notices are retained.
