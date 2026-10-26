# Gigabyte RTX 4070 (10de:2786): native Linux stock and cold-unlock evidence

Captured 2026-10-24 on CachyOS kernel 7.3.0-5-cachyos / NVIDIA open 615.78.08.

- stock-registers.log: one read-only gpu_peek load while nvidia was bound; immediately unloaded. Four inaccessible 0xbadf reads are included, not hidden.
- rm-rates.json: independent public RM getter; GPU UUID omitted.
- cuda-baseline.log: unmodified sm_89 register-resident benchmark, 20,000 iterations, three repetitions. No unlock was run.
- vbios-analysis.json: offline parsing of the supplied NVGI-prefixed ROM file; two identical embedded option-ROM chains, exact FWSEC metadata, and C signature/FRTS-in-memory checks. Does not assert the wrapper is a live 1 MiB PROM capture.
- vbios-parser.log: unmodified kernel parser diagnostics used for that offline inspection. Initial -EINVAL is expected for the complete 2,048,000-byte container; embedded logical ROMs parse successfully.

See ../../ada-2786-report.md for interpretation, reproduction and remaining uncertainty. No ROM, extracted firmware or compiled binaries are distributed here. SHA256SUMS.json covers the evidence files, excluding this README and itself.

Follow-up after the user-reported full shutdown and test-entry boot: cold-boot-unlock.log contains GPU/module-specific kernel lines; unlocked-cuda-benchmark.log and unlocked-rm-rates.json preserve the after measurements. cold-boot-validation.json and comparison.md record the matched comparison, checks and limits. unlocked-gpu-identity.csv contains the before-benchmark point sample. Full kernel logs remain private; GPU UUID and root/resume UUID command-line records are excluded from this evidence directory. The module signing-key trust warning is retained. benchmark-binary.sha256 records the unchanged executable identity.

software-validation.json distinguishes the hardware-tested source/module from the publication metadata and helper/doc updates and records the final 31-test suite.
