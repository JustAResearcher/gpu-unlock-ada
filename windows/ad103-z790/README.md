# MSI Z790 / RTX 4070 Ti SUPER (AD103, 10de:2705) - Windows port

Adapted from `windows/ad104-msi/`, which is pinned to a different card and board.
Like that port it is **manual and USB-only**: you boot the stick yourself from
the firmware boot menu and press `U`. It does not write boot variables, install
a boot entry, or touch the internal ESP (the Windows boot manager is opened
read-only to be hashed and chain-started).

**Status: one successful run on one board.** On an MSI MPG Z790 EDGE WIFI
(BIOS H.E0) with one RTX 4070 Ti SUPER, VBIOS 95.03.45.80.07, the transaction
completed: the SS0/SS1 selector registers read back at their unlocked values,
the PCIe FLR completed, and Windows (driver 621.14) started normally. Sustained
5-minute stock-vs-unlocked measurements, same script and binaries, are in
[`docs/evidence/ada2705-windows/`](../../docs/evidence/ada2705-windows/README.md):
FP16/BF16 with FP32 accumulate roughly double (about 89 -> 196 TF/s, ratio to
the FP16-accumulate path 0.49 -> 1.00) while the FP16-accumulate, INT8, dp4a and
FFMA controls move only a few percent. This is one card; it does not establish
support for other AD103 boards.

The unlock lives in volatile registers: it survives a warm reboot and is
cleared by a full power-off, so the stick must be run again after every cold
start.

## Running it

1. Build (below). On a USB stick (GPT, FAT32) put the built binary at
   `\EFI\BOOT\BOOTX64.EFI` (or add it as a firmware boot entry) and the five
   locally generated assets in `\AD103-ASSETS\` under the names `profile.c` pins. Run logs
   are written to `\AD103-UNLOCK\RUNxxxx.TXT` on the stick.
2. Disable Secure Boot (the unsigned build refuses to run with it on), boot the
   stick from the firmware boot menu, and press `U` within 30 s. `S`, `Esc` or
   the timeout continues to normal Windows with no GPU access.
3. The screen may stay frozen while the GPU is released. If Windows does not
   return after about 4 minutes, power off completely, wait 10 s and choose
   Windows Boot Manager.

`combined_probe/` is the read-only capture tool (config space, registers, VBIOS
hash, device paths) used to pin this board's profile. Keep the generated logs
local: they contain board identifiers.

An optional persistent-boot layer (resident internal partition, auto-unlock,
watchdog, Secure Boot signing) exists as a separate project and is deliberately
not part of this contribution.


## Host tests

`Build.ps1 -RunTests -Prom <1 MiB VBIOS image> -Assets <pinned Ada assets dir>` runs two
simulated suites (no GPU is touched): 17 admission/guard cases and 90 full-transaction
scenarios against the real core, compatibility layer and UEFI port with fake PCI/MMIO/DMA.

- `-Assets` is the folder with the five pinned NVIDIA files this project does not
  redistribute (the same inputs the build pins by SHA-256).
- `-Prom` is a 1 MiB slice of your card's VBIOS starting at the legacy image (use
  `tools/fwsec_dump.c <dump> <offset>` to find the offset whose FWSEC geometry matches
  `GuAd103FwsecMatches`). The tests pin that file's own hash through the host-only
  `GU_HOST_TEST_PROM_SHA256` define, which firmware builds reject. The production
  `GuAd103PromMatches` pin is the live pre-driver BAR0 capture, which an OS-level dump
  does not reproduce, so the suites do not exercise that exact hash.
- The PCI config fixture is the AD104 capture with this board's pinned dwords substituted.

## Building the full transaction library

```powershell
.\Build-Experimental.ps1
```

Needs the five firmware assets this project doesn't redistribute (GPL
restriction -- see `../../firmware/NOTICE.md`) at `local/assets/` or
wherever you point boot_io.c's expected USB path; see
`../../firmware/README.md` for how to generate them from NVIDIA's own
pinned 610.43.03 release. Needs `local/boot_profile.h` with this board's
real ESP partition GUID (see `boot_profile.h`'s own comment and
`boot_profile.example.h`) -- **never commit that file**; `../.gitignore`
already excludes `local/`.

Output: `build\AD103-EXPERIMENTAL.EFI`, verified by `tools/verify_experimental.py`
(valid PE32+ EFI application, zero imports/TLS/CLR, correct fixed image
base, every function's stack usage under 4096 bytes).

## Captured so far

| Field | Value |
|---|---|
| GPU | RTX 4070 Ti SUPER, `10de:2705`, subsystem `1462:513e` |
| VBIOS | `95.03.45.80.07`. The hash that matters for the gate (below) is from a live pre-driver BAR0 read, **not** an `nvflash` dump — see "FWSEC geometry and VBIOS hash" for why those two differ. |
| Board | MSI MPG Z790 EDGE WIFI (MS-7D91), BIOS H.E0 (2024-09-25) |
| ESP | Disk 1, partition 1, start LBA 2048, 204800 sectors (100 MiB) |
| Boot manager | `bootmgfw.efi`, 3086848 bytes, sha256 `3f8a4cbbc97b89836df92c4895236be1fac4a127665420d90509e897a7298511` |

The VBIOS dump itself, and anything else board-identifying, stays on the local
machine. Only hashes and small derived numbers belong in this tree.

## STATUS: verification gate complete; transaction library not started

`GuAd103FwsecMatches()`, `GuAd103PromMatches()`, and now `Config()`/
`Registers()` (and `port.h`'s `GU_BOARD_ID`/`GU_BOARD_SUBSYSTEM`/
`GU_BAR0_BASE`) are all filled in from real captures. See below for exactly
what's pinned and why.

**What this is not, yet:** `profile.c`/`profile.h`/`port.h` only implement the
read-only verification gate (`GuAd103Prepare`/`GuAd103ColdEntry`) — the
functions that check whether this machine's hardware matches what was
captured. The actual write-capable transaction library that `ad104-msi` has
— `port.c`, `checks.c`, `compat/`, `sha256.c`, `runtime.c`, `format.c`, the
`vendor/unlock_610.c` family, and `app.c`/`boot_io.c`/its MSI-specific
ownership module — has not been adapted for this board at all. There is
currently a verification gate with nothing behind it to verify; that's the
next piece of work, not a small one.

### FWSEC geometry and VBIOS hash — done, after a real correction

`nvflash --save` wraps its dump in an NVGI container; its bare legacy-image
slice is at offset `0x9400`. `tools/fwsec_dump.c` takes that offset
explicitly (an earlier version guessed offset 0 and only got a correct
result by luck, since the FWSEC data happened to still fall within its
truncated read — fixed so it no longer relies on that):

```
fwsec_dump vbios-current.rom 0x9400
```

```
image_off=0x45b48  image_size=0x10500  imem_pa=0x0  imem_size=0xf700  imem_va=0x0
dmem_pa=0x0  data_off=0xf700  dmem_size=0xd80  pkc_off=0xb24  interface_off=0x1c
ucode_id=9  engine_mask=0x400  sig_count=2  sig_versions=3  sig_off=0x45848  sig_bytes=768
```

**But `GuAd103PromMatches()`'s *hash* was derived from that same nvflash
slice, and that turned out to be wrong.** A review correctly flagged this as
an unverified assumption (`profile.c`'s `GuAd103Prepare` does a *live*
read of BAR0+0x300000 and compares against this hash — it needs to match the
real pre-driver aperture content, not a dump taken from inside a running,
driver-loaded Windows session). The probe was extended to do that live read
(still read-only) and compare — **it did not match**, fail-closed exactly as
designed. The live capture's hash
(`1a6631d6317739908aa6fb246858306dfef15901b43237b35b73d424b3880d9e`,
confirmed against the saved 1 MiB file directly, not just the printed log) is
what's actually pinned in `GuAd103PromMatches()` now. Re-running
`fwsec_dump` against that live-captured file gave byte-identical FWSEC
geometry to the numbers above — so the FWSEC region happens to be unaffected
by whatever differs elsewhere in the image, which is why the (wrong) nvflash
derivation still looked consistent. **Lesson for any other board:** this
particular hash must come from a live pre-driver EFI read, never from an
OS-level dump tool.

### Live PCI config-space and register capture — done, confirmed across two boot sessions

Captured with `probe/` (built with MSVC on the target machine, 2026-11-10),
across **two genuinely separate boot sessions**, not just repeated runs in
place:

```
found 10de:2705 at segment=0 bus=1 device=0 function=0
GuCheckConfigProbe=1 pm=0x60 pcie=0x78 pcie_version=2 msi=0x68 msix=0x0
BAR0 base = 0x81000000 (config dword 4)
SS1=0x5 both sessions; SS0 varied (0x3634054 -> 0x3630050)
```

Every config dword and all 19 register offsets (plus SS1) matched exactly
across both sessions; the one value that changed was SS0, which is correctly
*not* pinned to a literal in `Registers()` (only structurally checked) since
it's the dynamic selector value itself. Of the 19 fixed registers, 16 read
identically to `ad104-msi`'s own pinned values (same architecture, same
SEC2/GSP/FBPA addresses) — only offset `0` (chip/stepping-identifying,
expected to differ), `0x1183a4`, and `0x1fa830` (plausibly SM-count/
die-dependent) differ. All 19 are pinned in `Registers()` from this board's
own capture, not copied from `ad104-msi` even where they happened to match.
`ad104-msi`'s own `Registers()` requires `*Ss1==1` as part of its cold-entry
check. **This file originally pinned `*Ss1==5` to match the capture above —
that pin was wrong**, exactly for the reason the next section already
flagged before it was resolved: these two sessions were warm reboots/
restarts, not verified full power-offs. Two later real hardware attempts
(`RUN0009.TXT`/`RUN0010.TXT`), each an explicitly confirmed genuine full
shutdown (power fully off >1 minute, Windows Fast Startup disabled first),
both read `*Ss1==1` — matching `ad104-msi`'s board after all, not the
board-specific outlier this file assumed. Corrected 2026-11-10; see
`profile.c`'s own header comment.

`bus=1` also happens to match the literal `Bus != 1` assertion hardcoded in
`checks.c` (shared, unmodified, via `GuCheckConfig`) — convenient, but that's
this board's current PCIe slot, not something guaranteed to survive moving
the card to a different slot.

**A review also caught that the capability-offset check in `Config()` wasn't
really an independent pin:** `app.c`'s `Discover()` sets `Gu.pm_cap` etc.
from the *same* capability walk `Config()` later re-derives and compares
against, over config-space bytes that can't have changed in between — so
that comparison alone only checks internal self-consistency, not a real pin
against expected values (`ad104-msi`'s own `Config()` has this same gap — not
something introduced here). Fixed by adding literal pins alongside the
existing comparison: `pm==0x60 && pcie==0x78 && pcie_version==2 && msi==0x68
&& msix==0`, this board's own captured values.

A small standalone harness (not a full EFI build — see next section) checked
these literal constants against the captured log before they went into
`profile.c`, to catch transcription mistakes; it isn't included here since
it isn't part of the actual port, just a one-off check.

### Resolved: SS1 isn't a per-board constant at all — no longer pinned to one

A review had flagged that the project's own documented procedure for a fresh
state is full shutdown, AC power cycled, ~30 seconds, then power on — not a
warm restart — and that the captures above came from warm reboots/restarts,
never a verified full power-off. This mattered: real hardware attempts
`RUN0005.TXT`/`RUN0006.TXT`/`RUN0008.TXT` all read `ss1=1` under
uncontrolled shutdown conditions, contradicting the `*Ss1==5` pin.
`RUN0007.TXT` read `ss1=5` once, passing `Registers()`, but its preceding
shutdown was never explicitly confirmed as genuine the way the two below
were. Windows Fast Startup (`HiberbootEnabled`) was found enabled, which can
make an ordinary Shut Down not a true power-off — disabled before the next
attempts. `RUN0009.TXT` and `RUN0010.TXT` then each followed an explicitly
confirmed complete shutdown (power fully off, >1 minute) and both read
`ss0=0x3630044, ss1=1` — identical to each other, consistent with this
firmware's already-established deterministic boot-time behavior (see the
ownership-probe findings below). Two independently confirmed cold boots
giving the same result looked like enough to trust — the pin was corrected
to `*Ss1==1` — but a third confirmed-cold run, `RUN0011.TXT`, under the
*same* operator-verified shutdown procedure, read `ss0=0x3630050, ss1=5`.
That falsifies the premise, not just the guess: SS1's low nibble
legitimately varies run to run at a genuine cold boot (a third value, `0x2`,
appears in `vendor/unlock_610.c`'s own comment, matching neither pin tried
here). Full writeup: `SS1_FINDINGS.md`.

**Resolved: SS1 is no longer pinned to any literal.**
It's handled the same structural way its sibling SS0 already is — reject
only unambiguously-bad readings (`!(Ss1 & 0x8)`, the unlock-target bit;
out-of-range is already excluded by `GuLatchEntrySelectors`' `Ss1<=0xf`
bound) — while the existing latch/`GuEntrySelectorsMatch` machinery, already
value-agnostic everywhere else in the transaction, continues to carry the
actual consistency guarantee through the rest of the sequence.

### Next: the transaction library itself

`profile.c` can now tell whether this exact machine matches what was
captured, but there's no code yet that does anything with a "yes" answer.
A full read of `ad104-msi`'s `port.c`, `checks.c`, `compat.c`, `sha256.c`,
`runtime.c`, `format.c`, `vendor/*.c`, `boot_io.c`, `app.c` and
`msi_release.c` (not just their comments) found the scope is smaller than
it first looked in most places, and larger in one:

- `port.c`, `checks.c`, `compat.c`, `sha256.c`, `runtime.c`, `format.c` are
  **generic**, parameterized entirely through `port.h` (already correct for
  this board) and runtime state. Their only literals (`Bus==1`,
  `C[2]==0x030000a1`) match this board's own real capture, confirmed above
  -- not reused "by analogy".
- `vendor/unlock_610.c` and its siblings are **already upstream-generic**:
  `vendor/arch_610.h` has a first-class `GU_AD103_RTX_4070_TI_SUPER
  0x2705u` constant, used right alongside AD102/RTX 4090 in the shared
  device-dispatch logic. Nothing to port here at all.
- `boot_io.c` needs **mechanical substitution**, and the values are
  already in hand: this machine's own bootmgfw.efi hash/size (above) and
  its ESP's real GPT partition GUID (read live from Windows, not guessed).
  `app.c` needs only its two `GuAd104*` calls renamed to `GuAd103*`.
- `msi_release.c` -- the piece that actually detaches the console/GOP/
  NVIDIA UEFI drivers and claims exclusive ownership before the
  transaction -- is **not a reuse target and not board-generic**. It is
  built entirely from byte-exact ACPI/PCI device-path arrays for that
  MSI Z490 board's own USB/GPU/GOP topology, plus SHA-256 hashes of
  specific code ranges inside three loaded UEFI driver binaries, captured
  in a separate campaign its own comment calls "CAP0004". None of that is
  derivable from anything captured so far, and guessing it would repeat
  exactly the kind of fabricated-pin mistake already caught and fixed
  twice above. This board's own equivalent needs its own capture.

`ownership_probe/` (next to `probe/`) is that capture tool: a second
read-only UEFI application, same discipline as `probe/`, built to walk
this exact machine's own boot-time protocol-ownership graph and driver
identities. See `ownership_probe/README.md` for what it does and what it
found on this board's real hardware -- all three drivers (console-out,
GOP, the GPU's own PCI_IO protocol) identified exactly once each, with
real device paths and driver-identity hashes captured.

### `z790_release.c`/`.h` -- the ownership module, read-only half only

Adapted from `ad104-msi/msi_release.c` using those real captures: the
device paths, the three drivers' file identities and code-section hashes,
and `Bar()`'s expected BAR0 address range (recomputed for this board's own
`GU_BAR0_BASE`, not reused from `ad104-msi`'s literal). Compiles clean
against the real headers with the real transaction-library build flags.
**`GuZ790Release()` and `GuZ790Finish()` -- the two functions that would
actually call `DisconnectController`, claim the GPU's PCI_IO protocol
exclusively, and later release it -- are deliberately left returning
`EFI_NOT_READY`.** Nothing else in this file calls anything that opens,
closes, connects, disconnects, or writes to any protocol; confirmed by
grep, not just by description. Writing those two functions for real is
the next, and first genuinely mutating, piece of this port -- it gets its
own review before anything runs, same as every other write-capable step.

One structural fact this capture found, now confirmed across two separate
boots: `Consumers()`'s equivalent here tolerates either 1 or 2 device-path
"child" edges on the GPU's PCI handle (`ad104-msi` only ever saw 1) --
this board has 2, byte-for-byte identical both times, most likely a second
physical display connector with nothing attached, not the Intel iGPU
(ruled out: no second PCI_IO-bound handle exists anywhere in either
capture, and both edges are opened by the discrete GPU's own driver). The
three drivers' section/`.text` hashes -- the values `Driver()` actually
pins -- also matched exactly across both boots. See
`FINDINGS_2026-11-10.md` for the independent re-check of this, including
one claim from a second review that didn't survive its own suggested
test (a hash difference in `out`'s non-code sections that isn't explained
by relocation, since this firmware's addresses turned out to be fully
deterministic across boots) -- flagged as a loose end, not something
blocking the module.

## Licensing

GPL-2.0-only, same as the rest of this repository. `tools/fwsec_dump.c` is a
thin wrapper around `kernel/gpu_unlock/vbios_610.c`, used unmodified.
