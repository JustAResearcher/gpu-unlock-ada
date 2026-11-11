# Notices and provenance

This port is GPL-2.0-only, like the rest of this repository.

## What it is derived from

- **Core transaction code** (`vendor/unlock_610.c`, `prep_610.c`, `layout_610.c`,
  `vbios_610.c`, `gfw_eval.c`, `compat/`, `checks.c`, `runtime.c`, `format.c`,
  `sha256.c`): from [`windows/ad104-msi`](../ad104-msi/README.md), the Windows
  UEFI port contributed by **Latence.app**, which adapts the Ada transaction
  (see `../ad104-msi/provenance.json` for the pinned upstream commits) and the
  UEFI strategy of
  [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK).
  Most of these files are byte-identical copies; `vendor/unlock_610.c` carries
  the one functional adaptation, described in its header comment (dispatch of
  `0x2705` and the board ROM check).
- **Board-specific files** (`port.c`, `profile.c`, `app.c`, `boot_io.c`,
  `z790_release.c`, the build scripts and `tools/`): written for this board,
  structured after their `ad104-msi` counterparts.
- **EDK II headers** are used from `../ad104-msi/vendor/edk2/` (TianoCore,
  BSD-2-Clause-Patent; licence at `../ad104-msi/vendor/edk2/License.txt`). They
  are not copied again here.

Original authorship and licences of all upstream code are preserved in the
files above and in `../ad104-msi/LICENSE` and `../ad104-msi/LICENSES/`.

## What is not included

No NVIDIA firmware, VBIOS/PROM image, driver file, Windows binary or built EFI
is distributed. The ESP partition GUID is kept out of the tree
(`local/boot_profile.h`, git-ignored). Firmware inputs are fetched and pinned
by the repository's own tooling under their own terms.

## Scope

One board (MSI MPG Z790 EDGE WIFI) and one card (RTX 4070 Ti SUPER, 10de:2705).
No Linux target admission is changed, and nothing here writes boot variables.
