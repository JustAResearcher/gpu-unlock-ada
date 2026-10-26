# Reading a boot log: where the transaction stopped

`journalctl -k -b | grep gpu_unlock` says whether the module ran, not why it stopped. The module logs
a fixed sequence of stages, and each stage has its own failure text, so the **last line printed** is
enough to localise a failure. This page collects both, taken from the module sources and from a
successful AD102 cold boot.

Addresses, host names and device identity are replaced with placeholders here; real logs keep them.

## The stages of a successful run

A working AD102 cold boot prints these in this order. The right column is what that stage establishes.

| # | Line (abbreviated) | What it establishes |
|---:|---|---|
| 1 | `pci 0000:XX:00.0: entry: plm=… ss0=… ss1=… wpr2_hi=0x0` | Cold-state fingerprint. `wpr2_hi=0` is what makes the attempt possible at all |
| 2 | `FWSEC: reading PROM at 0x300000` / `PROM bytes=1048576 signature=0xaa55` | The board ROM is readable and looks like a VBIOS |
| 3 | `ROM chain: pos=… type=… last=…` | ROM structure: BIT, ucode table, FWSEC descriptor |
| 4 | `FWSEC descriptor: …` / `FWSEC geometry: image_off=… imem=… dmem=…` | Geometry parsed from **this board's** ROM. Geometry differs per VBIOS |
| 5 | `FWSEC fuse: raw=… version=…` / `signature selection: versions=… sig_bytes=…` | Which signature set the fuse selects |
| 6 | `asset request:` / `asset result:` per file | Each pinned firmware asset loaded and hash-checked |
| 7 | `FWSEC result: scratch=… expected_hi16=…` | The signed firmware completed |
| 8 | `FWSEC WPR2: low=… high=… expected_page=…` | The WPR2 window was set as expected |
| 9 | `post-FLR BAR ready: chip=…` / `post-FLR GFW result: error=0 …` | FLR completed and BARs respond again |
| 10 | `engine reset …` then `post-FLR SEC2 reset released, GSP halted, BME=0 pending=0 WPR2=… SS0=88888888 SS1=00000008` | **The selectors are written and held across the reset** |
| 11 | `AD102 permission post-FLR: WPR_CFG/FBPA/WPR/FEAT … required=… valid=1` | Each permission register read back |
| 12 | `AD102 post-FLR: readout1=0x0 … valid=1 full=1` then `gpu_unlock: unlocked` | The readout is zero and the card reports full rate |

Only after stage 12 does the stock NVIDIA driver bind. A driver-side confirmation is separate: the public
RM getter (`read_sm_rates.py`) must show `fmla32/imla2/imla4` going from `1` to `0`.

## Text the module prints when it refuses or aborts

| Message (source string) | Meaning | Action |
|---|---|---|
| `card already initialised since power on (WPR2 enabled); power off first` | The card was already initialised in this power cycle — a warm reboot normally leaves it that way | Power off for at least 30 s, then boot again. The refusal is deliberate |
| `firmware did not stop; keeping its DMA buffers until power off` | Firmware did not halt; the module keeps its DMA buffers alive on purpose | **Power off. Never unload the module in this state** |
| `gpu_unlock: no supported card at "…"` | The requested PCI address is not an accepted target | Compare with `lspci -D` and with the accepted IDs |
| `experimental … requires its exact PCI address in the list` | AD103/AD106/RTX 4090 D targets must be selected by one exact address, never `all` | Use `gpu_unlock.targets=0000:XX:00.0` |
| `AD102 payload copy changed before SEC2 start; aborting` | The payload bytes changed between staging and start | Regenerate the assets; do not edit firmware files by hand |
| `firmware load failed (%d)` / `unlock failed (%d)` | An asset failed to load, or the transaction failed. The percentage is the error code | Read the preceding `asset result: … error=…` line to see which file |
| `layout rejected: usable FB below 256 MiB` / `VGA base at or above usable FB` | The ROM layout does not match what the profile expects | Confirm with a dry run before widening a profile; do not delete the check |

## Diagnosis: the last line you see

| Last line printed | Conclusion |
|---|---|
| No `gpu_unlock:` line at all | The module never loaded: check that it is in `/conf/modules`, and that the initramfs is for **the kernel that booted** |
| Stops after `entry:` with no `FWSEC:` | PROM/BAR access was refused — target mismatch or BAR unavailable |
| Stops at `FWSEC geometry` or `layout rejected` | This board's VBIOS geometry does not match the profile |
| `asset result: … error=` non-zero | Missing, unreadable or hash-mismatched asset (frequently: firmware not included in the initramfs) |
| `FWSEC result` present but no `post-FLR BAR ready` | FLR or reset recovery failed |
| `SS0=88888888` present but no `unlocked` | One of the permission readbacks is not `required=… valid=1`; the line names which register |
| `unlocked` present but the RM getter still reads half rate | The module succeeded and the driver did not pick it up: look at the driver version and handoff, not at the module |

## Two things that look like failures but are not

- **A blank display during the transaction** is expected: the firmware framebuffer is removed before
  FLR and the display stays dark until `nvidia-drm` takes over.
- **A warm reboot that does not unlock** is the documented behaviour, not a regression: the card keeps
  state, so the module refuses (first row of the refusal table).
