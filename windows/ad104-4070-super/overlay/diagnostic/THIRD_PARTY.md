# Provenance

`src/`, `tests/` and `tools/` contain the new diagnostic application and its tooling, under MIT. No GPU-unlock core or NVIDIA firmware is linked into this application.

`../vendor/edk2/Include/` contains 43 unmodified EDK II headers, originally from TianoCore commit `c1415771ac592ad8509351c46cd3cda4f38d7d52`, obtained from the header snapshot in `impacifist/Windows-GPU-UNLOCK` at commit `9352abd27480203ea146f766e7c96957912d148a`. `../vendor/edk2/headers.json` records each included file's SHA-256. Retain `../vendor/edk2/License.txt` and individual copyright notices. The program links no EDK II library binary.

The reviewed reference trees remain in the task's `work/` directory. The diagnostic code uses the UEFI protocols declared by these headers; the GA102 or Ada GPU transaction implementations have not been copied into the probe.
