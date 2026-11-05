# Experimental AD104M RTX 4080 Laptop (10de:27a0)

Status: one real local boot completed the firmware transaction and NVIDIA handoff, with matched stock/unlocked CUDA and Strata measurements. A/B/A recovery was subsequently confirmed; additional unlock boots and suspend/resume remain pending.

Target: MSI Raider A18 HX A7VHG, RTX 4080 Laptop 12 GiB, PCI 10de:27a0, Linux 7.1.5+kali-amd64, NVIDIA open 610.57.04. Stock RM reports FMLA32=1. CUDA identifies 58 SMs and sm_89.

The default-off ad104m_experimental flag admits only 27a0 at an explicit PCI address. targets=all, other SKU flags, and an already-bound device cannot unlock it. 27e0 remains unadmitted. Both firmware routing and transaction-family decisions use the existing ad102_family predicate. Firmware pins, payload bytes, transaction checks and power policy are unchanged.

Host tests cover admission, helper output and a full transaction using pinned real assets against the synthetic AD104 A1 / 12 GiB-class register fixture and existing synthetic ROM. This fixture tests routing and cleanup; its inputs are not captured MSI hardware data and do not prove hardware compatibility. The module built successfully for the running kernel. The full suite passed 34 tests with no skips after providing PyCryptodome in an isolated environment.

Boot experiment: an isolated initramfs adds the module and pinned firmware. An init-top script explicitly precedes udev coldplug via udev's PREREQS, checks the exact local PCI ID and absence of a bound driver, then loads the module. The normal image and default boot entry were unchanged during collection. Use a full power-off/start for the first attempt. A warm reboot does not establish fresh WPR2 state.

Required hardware acceptance: successful firmware/selector/post-FLR checks, normal NVIDIA handoff, no new Xid or GPU loss, expected RM rate, correct seeded arithmetic, matched stock/unlocked/recovered performance. Repeated cold/warm boots, idle power transitions, suspend/resume and extended sustained load still need validation. Never force-unload or retry a failed firmware transaction on a live driver-owned card.

Local comparison suite retains the same CUDA binaries, GPU UUID, kernel and driver across stock/unlocked/recovered phases. It collects three rounds of MMA, verified tensor modes and large GEMMs, plus shaped GEMMs, FlashAttention and device-copy controls, telemetry and kernel logs. The FP8 MMA probe is throughput-only; it does not establish general FP8 numerical correctness. Strata is measured separately with fixed requests and engine/config identities. Its engine-reported cache/timing counts must be inspected before accepting a prefill comparison.

## First real boot, 2026-11-06

The running command line selected 27a0 with its exact address and the independent flag. Hardware chip readback was AD104 A1 (0x194000a1). The card's own four-image ROM parsed successfully. FWSEC-FRTS, payload marker 0x67b00001, required permissions, SS0=0x88888888/SS1=0x8 and post-FLR full-rate readout passed. The stock NVIDIA 610.57.04 driver initialized normally. RM FMLA32/IMLA2/IMLA4 changed from 1 to 0.

| Probe (median) | Stock | Unlocked | Ratio |
|---|---:|---:|---:|
| MMA f16/f32 | 64.300 | 132.500 | 2.061x |
| MMA bf16/f32 | 64.300 | 132.400 | 2.059x |
| MMA fp8/e4m3 | 132.000 | 265.200 | 2.009x |
| cuBLAS FP16/FP32 8192^3 | 60.331 | 91.979 | 1.525x |
| cuBLAS BF16/FP32 8192^3 | 59.102 | 89.287 | 1.511x |
| Strata long prompt_per_second | 1555.000 | 1697.700 | 1.092x |
| Strata long predicted_per_second | 61.400 | 63.100 | 1.028x |

Three rounds of instruction and cuBLAS probes were collected. Seeded FP16/BF16 CPU-reference cases, full large-GEMM output checks, tensor-mode checked cells, and sampled PyTorch/Attention comparisons passed. FP8 is instruction throughput only, not general FP8 numerical validation. FP32 GEMM controls included slower cases; memory copy was nearly unchanged. All cases and repetitions are retained locally.

Strata used the same IQ3_XXS model, config/engine hashes, and ten identical JSON requests including warmup. Each measured size had three repetitions, 256 output tokens and zero prompt-cache reuse. Small decode differences cannot be established as an unlock effect from one pair.

No Xid or GPU loss was found. Two SBIOS query assertions were also present in the stock boot. The first automatic attempt reached all compute checks but raced the Strata router's HTTP startup. Endpoint readiness polling was added; failed evidence was preserved and the full Strata stage was rerun. This did not repeat the firmware transaction.

The original and unlocked runs used the same kernel, driver and compute binary hashes. Desktop activity, thermal state, Dynamic Boost and clocks were not fixed. Do not infer a fixed-power efficiency gain from these runs. The second user-described boot is not independently certified as a physical AC power cycle.

Sanitized observations, all measured shapes and repetitions, an offline verifier and matched input hashes are in [evidence/ada27a0](evidence/ada27a0/README.md). This report establishes one unlock boot, not long-term compatibility.

A subsequent 300-second BF16 4096-cubed soak completed 216,400 GEMMs with periodic full-output exact checks (36,305,895,424 cells) and zero mismatches. Seeded FP16/BF16 CPU-reference validation passed again afterward. Sampled peak was 82 C / 115.23 W. RM FMLA32 remained full and no Xid or GPU-loss message appeared. This is a short soak, not extended reliability validation.

## Stock recovery A/B/A, 2026-11-06

The next boot used the normal stock initramfs and no gpu_unlock command-line arguments. The module was absent. RM FMLA32/IMLA2/IMLA4 returned to 1 and target instruction/GEMM rates returned to the stock range. The same kernel, driver, GPU identity and hashed compute binaries passed the matched-run guards.

| Probe (median) | Stock A | Unlocked B | Restored A-prime |
|---|---:|---:|---:|
| MMA f16/f32 | 64.300 | 132.500 | 66.500 |
| MMA bf16/f32 | 64.300 | 132.400 | 65.700 |
| MMA fp8/e4m3 | 132.000 | 265.200 | 133.000 |
| cuBLAS FP16/FP32 8192^3 | 60.331 | 91.979 | 60.302 |
| cuBLAS BF16/FP32 8192^3 | 59.102 | 89.287 | 59.787 |
| Strata medium prompt_per_second | 1169.000 | 1191.700 | 1188.800 |
| Strata long prompt_per_second | 1555.000 | 1697.700 | 1565.800 |
| Strata long predicted_per_second | 61.400 | 63.100 | 61.700 |

All three phases passed six seeded FP16/BF16 cases, 18 large-GEMM shape/type/repetition cases, and three checked tensor-probe rounds. Strata's ten request payloads were identical across all phases, all responses completed and prompt cache counts were zero.

The 17K Strata prefill gain is about 9.2% against the first stock phase and 8.4% against restored stock. Decode differences are small and include speculative-decoding variability. The approximately 2% initial 4K prefill difference did not repeat against restored stock: restored stock nearly matched the unlocked result. It should not be claimed as an unlock benefit.

The three phases support a reversible tensor-path speedup on this MSI card, with large GEMM gains of roughly 50% and shape-specific gains up to about 71%. This does not establish multi-boot unlock reliability, suspend/resume, deep idle state persistence or long-term application correctness.

The restored-stock 300-second soak also passed: 136,400 GEMMs, periodic full-output checks of 22,884,122,624 cells, zero mismatches, and a final seeded CPU-reference pass. Sampled peak was 81 C / 115.28 W. FMLA32 remained 1 afterward. Both automatic benchmark services were disabled after successful collection; the optional experimental GRUB entry was retained, with the normal stock entry still the default.

## Upstream scope and related work

This targets **27a0 only**. [PR #123](https://github.com/JustAResearcher/gpu-unlock-ada/pull/123) independently targets **27e0** and introduces the same `ad104m_experimental` option. Neither ID is evidence for every AD104M board. If that PR merges first, share the option and retain distinct per-ID constants and reports; do not loosen exact-address admission.

The measured module was built from the local research commit `6b87a74`. The production C source proposed here changes only its experimental wording; `unlock_610.c` is unchanged from that hardware-tested tree. The source reference is local provenance, not a commit in this squashed public branch. The tested module SHA-256 is recorded in the evidence manifest. No module, ROM or NVIDIA firmware is redistributed. Local GRUB deployment is not part of this patch.

The test machine subsequently selected its existing isolated experimental image as its default at the owner's request. That configuration change is not another successful boot or additional stability evidence. A fresh power-off/start is preferred; warm reboot freshness and suspend/D3cold persistence remain untested.
