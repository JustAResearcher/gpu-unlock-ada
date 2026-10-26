# Bounded AD102 canary protocol and CPU replay kit

This is a record of controls used in one AD102 investigation, not a turnkey installer or a claim of safety. It deliberately contains no firmware binaries, module-loading command, board ROM, local machine identifier or private filesystem location.

## What is included

The accompanying CPU kit validates the measured transaction-log format and tests the `0`/`0x0` parsing correction, missing/duplicate/contradictory evidence and target isolation. It has **no hardware action runner**. A syntactically valid log is not authorization to submit firmware or load a driver.

The register values in this validator describe the measured AD102 profile. They must not be generalized to another board, VBIOS, die or module revision. In particular, the post-FLR WPR values were compared with that board's approved cold baseline, not guessed from an earlier payload target.

CPU-only test, from the kit directory:

```sh
python3 -B test_log_gate.py
```

The fixture is synthetic and uses a synthetic PCI address. It is test data, not proof of a real successful transaction. The draft validator additionally rejects repeated key/value fields within one row; that CPU hardening was added during publication review and is not claimed to have been part of the already completed hardware trials.

## Preconditions for a separately authorized hardware trial

1. Exclusive GPU window, no active CUDA workload, saved application work, known-good stock boot and a physical recovery route. A remote power/boot plan must already have been tested. Do not improvise it after the machine goes offline.
2. Audit exact source commits, the module build for the running kernel, the read-only probe and every firmware input. Record SHA-256 manifests. No driver update or payload regeneration during the comparison.
3. Scope to one exact PCI target. Never use a wildcard/all-target mode.
4. Preserve the stock kernel/initramfs/configuration, the default boot selection and checksums. Construct a separate experimental image and one-shot boot entry. Do not replace stock or install automatic retry.
5. Pin controls and assets externally, not just in a manifest stored beside writable controls. Freeze reference results and benchmark inputs before build/review. Validate the unpacked image, hook ordering, driver barrier and effective module-load policy.
6. CPU tests and independent review must precede installation. A test double cannot prove physical DMA containment, Falcon halt, FLR correctness or recovery from a hardware hang.

## Establish a cold ROM reference before the unlock

A driver-initialized PROM view may differ from the cold view. If a full-window pin fails, do not erase the check or mask a convenient region.

Use a separate read-only cold trial, with a distinct one-shot/latch namespace and no unlock payload. Persist the complete dump, length and digest before later checks. Capture more than one cold read and compare them offline with initialized reads. Independently validate the ROM image chain, descriptor/signatures and FWSEC region.

Only after the difference is understood well enough for a board-specific decision should a cold full-window pin be approved. Preserve both the bytes and the decision. A hash without the bytes that produced it is insufficient for diagnosing a mismatch.

## Experimental boot sequence

- Run before the normal NVIDIA/udev path can bind to the target. Do not run NVML or CUDA as a cold preflight: they may initialize the driver and invalidate the premise.
- Confirm the exact target, kernel/module/assets, cold state and approved ROM/region pins. Persist raw cold evidence before mutation.
- Consume a durable one-shot marker before submission. A failure is not permission to clear it or retry.
- Keep a bounded external monitor and an independent observer outside the experimental host. Fresh readiness and heartbeat, a live monitor process and a tested wake path are admission requirements.
- Program and verify the bounded RTC return before the authorized S5; retain an independent WoL path where available. These are recovery aids, not assurances that every S5 removes power from the GPU.
- After submission, require the complete transaction: marker, halt, mandatory permissions, effective full-rate readouts before/after FLR, successful FLR/GFW result, DMA containment, expected WPR2 state, permission relock and SEC2/GSP handoff. Enforce exact cardinality and reject any explicit failure or contradictory row.
- Accept zero spellings `0` and `0x0` only where both denote the approved zero value. Do not relax nonzero values or failure detection to accommodate a formatting bug.
- Release the normal driver only after the full gate passes. Then verify driver/GSP identity, the public RM issue-rate getter, idle health and unchanged power policy before any CUDA test.

The one-shot campaign's recovery path was a separately bounded cold return to the unchanged stock entry. It did not automatically retry unlock. A clean software entry alone does not prove factory selector state: query the stock getter after a verified cold return.

## Fail-closed does not mean indiscriminate continuation

Before any mutation and with a proven cold baseline, an admission failure can leave the normal stock boot available. After a partial payload/selector write, uncertain FLR or dirty state inherited through warm reboot, simply ignoring the failure and loading NVIDIA cannot establish half-rate stock behavior.

Cold recovery may be necessary. It must have a finite budget and durable evidence of consumption before dispatch. If recovery is inconclusive, stop for physical intervention rather than looping or claiming success. No automatic-persistence design is supplied by this kit.

## Compare workloads without changing the question

Freeze sources, model files, prompts/embeddings, seed, LoRA, dtype, attention/offloading path and instrumentation. Record metadata, raw samples and output hashes, not just selected medians.

For GEMMs, specify accumulation and output types separately. Match operands, layout, library and algorithm settings. Check every output for finiteness and compare complete output bytes outside timing. Do not infer application gains from one square GEMM.

For a generative workload, compare all actual forward outputs and final media. A hash mismatch is a finding, not something to hide with a tolerance invented after the run. Precision-conversion experiments are a different question and need numerical errors and visual review, not the stock/unlocked bit-exact claim.

Use one bounded transient cgroup for a campaign, including children that create new sessions. Process-group timeouts alone are insufficient containment. Require no restart and a bounded stop/restore hook. Restore only services that were active before the trial and verify their health.

## Real service evidence, including after garbage collection

A transient unit started with collection enabled may no longer exist when a finalizer runs. Do not treat a missing unit's default MainPID=0 as proof of success.

Persist the real service result in ExecStopPost, before garbage collection: unit/boot identity, Result, ExecMainCode/Status/PID, MainPID, ControlGroup/KillMode and the whitelisted SERVICE_RESULT/EXIT_CODE/EXIT_STATUS environments. Separately record the real supervisor wait return and stop-hook return. Afterward require the expected cgroup to be empty or absent.

The finalizer must run a raw-artifact producer itself after verifying immutable controls/references. A pre-existing JSON of success booleans is not proof. Rehash the actual files and reject missing samples, invalid metadata, nonfinite results, incomplete forwards, wrong boot or a failed service. Isolate Python control invocations from inherited optimization settings so assertions cannot silently disappear.

## Close the trial

Preserve receipts, ROM evidence, manifests and output bytes. Archive the one-shot boot entry/image out of the active boot tree, clear the one-shot selection, disable the RTC alarm and reconfirm stock checksums. Do not remove consumed latches.

Declare the actual final hardware state: unlocked transiently or cold-restored stock. Also declare driver/service health, active GPU clients, cgroup/job state and intentionally retained artifacts. No additional test, retry, boot persistence or publication follows implicitly from a successful trial.

Physical/firmware hangs, unreliable S5/RTC behavior, power loss and storage failure remain outside the guarantees of a shell gate. This protocol reduces exposure; it does not make experimental firmware execution safe in all circumstances.
