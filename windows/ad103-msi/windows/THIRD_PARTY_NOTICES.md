# Windows port attribution and licensing

The application is GPL-2.0-only. See [the repository license](../LICENSE) and
[full GPL text](../licenses/GPL-2.0.txt). Existing per-file licenses remain in
force. Corresponding source accompanies distributed test binaries.

The UEFI infrastructure in `src/` (application, IO, ownership, checks, PCI/DMA
adapter, compatibility shims, runtime, formatter, SHA-256 and handoff) is adapted
from TOTORONG's `AD103_win_unlocker`, revision
`51df9c17ec89eb8486df52cc9079dde4bd1f9974`, under its code's GPL-2.0-only and
applicable third-party licenses:

https://huggingface.co/TOTORONG/AD103_win_unlocker/tree/51df9c17ec89eb8486df52cc9079dde4bd1f9974/source/unlock

Modified 2026-10-24: added the diagnostic-only build and exact-board profile,
replaced the private board's hardcoded IDs, paths and ROM offsets with runtime
validation and the repository's VBIOS parser, connected the existing shared Linux
transaction, required full-rate post-FLR/post-GOP state before Windows handoff,
and added reproducible build metadata and host tests. The old private vendor
transaction and pinned 4080 ROM profile are not included. These changes are an
experimental adaptation, not an endorsement by or collaboration with the
upstream authors.

The 52 headers in `deps/edk2/` are a transitive subset copied without content
changes from the same revision's `source/deps/edk2/MdePkg/Include/` tree. Their
original copyright notices and per-file licenses are preserved. The BSD-2-Clause-
Patent license is in `LICENSES/EDK2-BSD-2-Clause-Patent.txt`.

The shared transaction, original GA102 work and Ada payload retain the provenance
already documented in [firmware/NOTICE.md](../firmware/NOTICE.md). Applicable
NVIDIA and CMP40HX MIT notices are also retained under `LICENSES/`.

The new port documentation is written for this repository. TOTORONG's separately
licensed narrative documentation has not been copied into it. NVIDIA binary
firmware, ROM dumps, firmware disassembly and Windows binaries are not included
in the diagnostic package. Locally extracted NVIDIA firmware remains subject to
its own accompanying license.
