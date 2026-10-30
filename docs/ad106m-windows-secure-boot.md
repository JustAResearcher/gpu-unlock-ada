# RTX 4070 Laptop / AD106M: bounded Windows Secure Boot result

One ASUS TUF Gaming A15 FA507XI with an RTX 4070 Laptop 8 GB (`10de:2860`,
AD106M) completed a locally developed preboot transaction, started the stock
Windows driver and showed higher FP16, BF16, TF32 and FP8 dense GEMM rates.
Secure Boot remained enabled during the successful internal ON/OFF and default
startup tests. A full OFF startup restored stock rates.

This is an owner-reported result from one laptop, with aggregate measurements
and a reusable validation workflow. **It does not add a public unlock profile,
portable EFI binary, generic installer, or Linux target admission.** The local
transaction/installer is not included in this contribution. Other laptops,
BIOS versions, display ownership graphs and driver lifecycles need independent
review. Do not use the existing MSI desktop profile just because it runs Windows.

This adds Windows evidence relevant to the questions in
[#56](https://github.com/JustAResearcher/gpu-unlock-ada/issues/56) and
[#20](https://github.com/JustAResearcher/gpu-unlock-ada/issues/20). It neither
reproduces nor resolves the different Linux machine's post-driver half-rate
report in #20.

## Measured scope

- Board family FA507XI, BIOS 314; stock RTX 4070 Laptop 8 GB.
- Windows 11, stock NVIDIA 621.01; PyTorch 2.6.0+cu124, CUDA 12.4.
- Hybrid AMD/NVIDIA graphics; no display or GPU-mode change between the final
  internal ON/OFF/default-startup controls.
- No GPU voltage/power change, driver patch, motherboard/VBIOS flash or TPM reset.
- Dense 4096-square GEMM, ten warmups and three timing batches of forty calls.
  These are application-kernel timings, not pure issue-rate or end-to-end AI gains.
- FP16/BF16 reduced-precision reductions disabled; TF32 explicitly selected for
  its case. FP8 E4M3 used FP32 unit scales, FP16 output and no fast accumulation;
  quantization was excluded from timing.
- All formats passed full-output finite checks and sampled FP64 references of
  the actual inputs. Samples cover 1,024 output positions per format.

| Format | Restored OFF, TFLOP/s | Default automatic ON, TFLOP/s | Change |
|---|---:|---:|---:|
| FP16 | 46.75 | 77.83 | +66.48% |
| BF16 | 47.00 | 81.63 | +73.66% |
| TF32 | 23.10 | 39.46 | +70.82% |
| FP8 E4M3 | 93.89 | 142.30 | +51.55% |
| FP32 control | 13.238 | 13.237 | -0.01% |

These columns are one process per phase, each using three timing batches.
The earlier manually selected internal ON boot measured FP16 70.77, BF16 81.24,
TF32 38.91 and FP8 139.44 TFLOP/s; FP32 stayed near stock. Clock/thermal variation
means the higher FP16 result is not evidence of a second improvement.

The same installed image also passed a separate mixed workload on that earlier
ON boot: five formats across 1536, 2048 and 4096 matrices, 15 cases and 105 sampled
numerical checks over approximately 68 seconds, peak sampled temperature 76.8 C.
It was not rerun on the final default startup. Desktop taskbar clicks, desktop
menu selection and app switching worked before and after the final checks;
the reviewed Windows desktop/graphics event queries were clean.

The restored OFF rates were all within 0.2% of the earlier stock baseline.
The [numeric-only summary](evidence/ad106m-windows/summary.json) includes the final
pair's timing batches. Raw logs, captures and individual machine fingerprints
are deliberately excluded.

## Failures and limits retained

### Subsequent persistence regression

Later everyday use exposed failures beyond the bounded startup results above.
With the saved preference still ON, the reviewed entry still first and the
installed image and Windows loader unchanged, a later Windows session measured
FP16 46.91, BF16 47.05, TF32 23.10 and FP8 93.51 TFLOP/s; FP32 control was 13.36.
All five numerical checks passed, but the tensor rates were stock-like. A
complete READY capture matched that session's startup. READY therefore does
not establish that acceleration survived Windows initialization or later use.

The same session included Modern Standby and engine-timeout reports.
The underlying timeout timestamps preceded the recorded sleep, even though
Windows reported them afterward. Local debugger analysis of the three retained
current-session timeout dumps attributes them to AMD's NPU driver, `ipustack.sys`,
rather than establishing an NVIDIA-driver timeout. Two older watchdog dumps,
predating this unlock experiment, are a different graphics-kernel live-dump code
(`0x193`) in desktop-compositor context; they are not the same NPU failure.
These observations do not establish whether sleep, driver
recovery, the transaction, or another factor caused the performance loss or
the owner's reported stuttering after waking. No continuous performance trace
exists between boot and the later measurement.

A different retained startup capture refused an unreviewed initial selector
state before payload, firmware, selector or reset writes. That refusal is a
separate startup-admission problem; the existing guard has not been broadened.
The owner could not identify the corresponding boot-screen message, so it is
not classified as a cold-start versus warm-restart failure.

**Always-on behavior, restart reliability and sleep/resume remain unresolved.**
A status tool should show saved preference separately from a fresh performance
measurement, report busy/error cases as unverified, and never reuse an earlier
accelerated result after sleep or graphics recovery. This follow-up does not
change the historical ON/OFF measurements or add a public runtime reapply path.

### Controlled Modern Standby comparison

A subsequent full shutdown/start restored acceleration with the same installed
image, Windows loader and Secure Boot databases. The automatic same-boot report
passed validation, and the owner reported a working desktop. Acceleration also
survived more than five minutes without an agent GPU probe while Windows stayed
awake. No intervening sleep was recorded.

One recorded Modern Standby cycle then provided a direct before/after comparison
in that same Windows kernel. Sleep lasted approximately four minutes. The
recorder stopped before the post-wake CUDA check; it reported zero lost buffers
and events. Both measurements used the same collector, software and arithmetic,
and all five numerical checks passed.

| Format | Awake after idle, TFLOP/s | After Modern Standby, TFLOP/s |
|---|---:|---:|
| FP16 | 72.26 | 46.81 |
| BF16 | 81.40 | 47.09 |
| TF32 | 38.16 | 23.07 |
| FP8 E4M3 | 142.74 | 92.79 |
| FP32 control | 13.235 | 13.104 |

The tensor rates returned to the retained stock range across sleep, while the
FP32 control changed by about 1%. The owner reported a normal-feeling desktop
after this wake: lost acceleration need not be accompanied by visible stutter.
The saved preference was still ON; resuming Windows does not execute the local
preboot transaction again. Re-published NPU reports referenced the same older
failures and are not evidence of new NVIDIA timeouts during this test.

This is one controlled observation of lost acceleration across Modern Standby,
not a direct post-resume register read or proof of a specific reset mechanism.
It does not resolve the separate guarded-startup refusal, intermittent desktop
stutter or warm-restart behavior. **This implementation does not maintain the
unlock across the tested sleep/wake cycle.** No runtime register writes, driver
patch, sleep-policy workaround or extra Secure Boot trust was installed.

### Loss during normal use without a recorded system sleep

A later same-kernel observation broadens the persistence problem. Three fresh
checks showed accelerated rates during normal use; roughly six hours after the
last successful check, two fresh checks showed stock-like rates. The owner
reported normal use without a noticed power, lid, display or GPU-mode change.

| Format | Earlier successful check, TFLOP/s | Later stock-like check, TFLOP/s |
|---|---:|---:|
| FP16 | 67.48 | 46.74 |
| BF16 | 70.81 | 47.00 |
| TF32 | 39.76 | 23.08 |
| FP8 E4M3 | 141.57 | 92.64 |
| FP32 control | 13.111 | 13.163 |

All numerical checks passed. Driver, workload and Status component identity
matched the retained reference. The bounded System-log query contained no
sleep/wake, Display 4101 or NVIDIA-provider events between those measurements.
Three error-report publications matched the already analyzed historical NPU
reports. The exact loss time is unknown; this was not a continuous GPU-power
trace and does not exclude an unlogged device-level transition or recovery.

Preventing automatic system sleep is therefore not established as sufficient.
[Issue #117](https://github.com/JustAResearcher/gpu-unlock-ada/issues/117) reports
a targeted NVIDIA runtime-power workaround on a different laptop and driver.
That is an experimental lead, not validation for this AD106M/621.01 case, and
does not establish persistence across system sleep.

The owner authorized a reversible NVIDIA-only policy trial after considering
the possible idle/battery cost. The actual adapter class key was resolved
dynamically, the installed signed driver was pinned, the absent original value
was backed up, and DWORD `EnableRuntimePowerManagement=0` passed readback while
the other adapter's value stayed unchanged. Static inspection found the option
name and a code reference in 621.01; that does not establish runtime behavior.
**The trial has not yet been validated after startup or prolonged use.** No
driver binary, EFI, Secure Boot configuration or system sleep policy was changed,
and no GPU reset or automatic restart was performed.

### Earlier observations

The **first internal attempt reached READY but measured stock speed in two
independent processes**. A subsequent full boot using the same image and unchanged
Secure Boot trust accelerated, as did the later default startup. The failed and
successful internal attempts shared the same admitted initial selector state.
The cause remains unresolved; neither READY, enrollment nor a PIN reset explains
or proves the performance outcome by itself.

Earlier development included a malformed report and desktop click failures.
The malformed report was rejected. The logging revision added full write/flush/
close/reopen/byte/EOF verification before reporting readiness. Desktop recovery
required restarting the Windows compositor on affected earlier boots; the cause
was not established. No automatic compositor-restart workaround was installed.
Later unchanged-trust boots, including the final ON/OFF/default controls, had
healthy desktop interaction and no further PIN-reset prompt.

This is not long-duration qualification. Sleep/resume, driver reload/reset,
updates to the pinned Windows loader or BIOS, battery operation and other laptop
models are unqualified. No real-model, game, NVENC, extra-VRAM, extra-SM or power-
limit unlock is claimed. Full-rate selector/readback evidence and Windows gains
must be checked separately. A performance report must retain unsuccessful boots.

## Reusable Windows qualification sequence

1. Keep an ordinary stock Windows startup available. Record hardware/firmware,
   ownership, driver and benchmark identity locally; collect a stock baseline.
   A desktop's NVIDIA GOP assumptions must not be applied to an iGPU-owned panel.
2. Review a device-specific preboot transaction and its cleanup before staging.
   The tested method used Ada firmware preparation, permission and selector
   checks, a reset and verified cleanup before handing off to Windows. It did
   not write registers from a running NVIDIA-owned Windows session.
3. For a firmware that supports image-hash authorization, authorize only the
   locally built, reviewed image in Secure Boot's allowed database. Preserve
   existing trust. Enrollment changes can affect Windows Hello; do not infer
   GPU state from a sign-in prompt. Other firmware may need a different reviewed
   signing/enrollment procedure. Never distribute another owner's trust dump.
4. Test an unchanged-image/trust full ON boot. Establish a fresh Windows kernel,
   a complete same-boot report, healthy devices and interactive desktop, then
   run matched correctness and performance checks. Disable Fast Startup during
   qualification so a retained kernel cannot masquerade as a new full startup.
5. Test OFF through the same startup entry, then verify stock performance.
   A saved Off setting changes the next full initialization, not the running GPU.
6. Only after ON/OFF pass, make the reviewed entry the default and verify a normal
   full startup without a USB or manual boot-menu selection. In this setup,
   hibernation was disabled too; retained-kernel initialization was not qualified.

The tested local startup wrapper authenticated/preloaded the original Microsoft
loader, applied the guarded transaction, then started that loader. It left the
stock Microsoft executable in place. A missing/malformed mode file meant OFF;
any key during a short countdown skipped one attempt. It limited attempts to one
per firmware session, bound file I/O to its own reviewed volume, and kept a
bounded rotating report set. Uncertain post-write cleanup halted for full power-
off instead of returning to Windows. A changed loader hash skipped GPU work
pending review while retaining the ordinary authenticated Windows path.

These are design and validation requirements for another port, not permission
to copy machine-specific addresses or remove admission checks. Keep ROM hashes,
partition identifiers, driver-image fingerprints, logs, recovery material and
private paths local. A public contribution should use generated local profile
inputs with refusal tests; profile generation alone is not hardware qualification.

The [Windows validation tools](../windows/validation/README.md) generalize the
measurement and privacy workflow independently of the unpublished transaction.
The historical measurements were made with the exact private collector; the
public collector preserves the arithmetic/timing procedure while replacing its
machine-specific identity assertion with a single-GPU precondition.
