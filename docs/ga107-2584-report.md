# RTX 3050 6GB on GA107 (10de:2584)

On October 25, 2026, the existing GA102 Booter path and v67 payload ran on one
GeForce RTX 3050 6GB (`10de:2584`, GA107, 18 SMs, VBIOS `94.07.A0.00.4E`)
before the unmodified NVIDIA `595.84` open kernel driver. Every gate the
module enforces passed, the NVIDIA RM getter changed FMLA32 and IMLA4 from
half to full rate, and matched FP16/BF16/TF32 tensor instruction rates with
FP32 accumulation doubled. This is one physical card. It does not establish
support for other GA107 boards; only `10de:2584` is admitted.
The [evidence directory](evidence/ga107-2584/README.md) contains the
transaction excerpt, rate readbacks and unedited benchmark output with the GPU
UUID replaced.

## Why GA107 takes the GA102 path

NVIDIA `610.43.03` binds GA107 to the same HAL entries as GA102, GA103, GA104
and GA106 (`src/nvidia/generated/g_kernel_gsp_nvoc.c`):

```c
/* ChipHal: GA102 | GA103 | GA104 | GA106 | GA107 */
pThis->__kgspGetBinArchiveBooterLoadUcode__ = &kgspGetBinArchiveBooterLoadUcode_GA102;
pThis->__kgspGetBinArchiveGspRmBoot__ = &kgspGetBinArchiveGspRmBoot_GA102;
```

So the change is only the admission of `0x2584` in `supported_ids`; the
pinned GA102 Booter, GSP boot image and payload are used unchanged.

## Host and sequence

The host ran Ubuntu 24.04.4 with kernel `6.8.0-146-generic`, Secure Boot
disabled, and the card on a PCIe 4.0 x8 link. The integrated GPU drives the
console (`boot_vga=0` on the 3050). The module was built by DKMS from this
branch through `make deb`.

The card had been in use by `nvidia` for ten days. Following the live
procedure from issue #14, without a reboot:

```sh
sudo systemctl stop nvidia-persistenced
sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia
echo 1 | sudo tee /sys/bus/pci/devices/0000:01:00.0/reset   # FLR
sudo modprobe gpu_unlock targets=0000:01:00.0
sudo modprobe nvidia && sudo modprobe nvidia_uvm && sudo modprobe nvidia_drm
sudo systemctl start nvidia-persistenced
```

The device advertises `FLReset+` with `reset_method` = `flr bus`. On this
bare-metal card the FLR cleared WPR2 just as it did for the 3070 under VFIO:
the entry readback was `wpr2_hi=0x0`. The whole module transaction took about
six seconds.

## Firmware and driver checks

The kernel log recorded:

- entry: `plm=0xffffff8f`, `SS0=0x62662003`, `SS1=0x7`, WPR2 high `0x0`,
  chip `0xb77000a1`;
- layout: 6 GiB FB (`fb_mb=0x1800`), all reservations above the
  pre-scrubbed top 256 MiB (`nonwpr=0x175300000`, floor `0x170000000`);
- Booter signature `fuse_raw=0x1 version=1`, FWSEC fuse version 2;
- FWSEC-FRTS set WPR2 at the expected page (`low=0x17fe000 high=0x17fee00`);
- selector writes read back `SS0=0x88888888` and `SS1=0x8`;
- after PCIe FLR the selectors were still `SS0=88888888 SS1=00000008`,
  GFW completed (`error=0`), SEC2 was reset and GSP halted;
- `gpu_unlock: unlocked`.

The stock driver then loaded, CUDA work completed and the kernel log had no
NVRM Xid or GPU-fallen-off message.

The read-only RM getter reported:

| Field | Stock | Unlocked |
| --- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0–3, FMLA16, DP, FFMA | 0 | 0 |

## Matched measurements

Both states used the same card, boot, kernel, driver, 70 W cap (the board's
default and maximum), binaries and cubin (CUDA 13.3, `sm_86`, cubin SHA-256
`de6ac71f7488359ad1e63ba79ee1e2ea75be7748f2b9fcdff19a5138f866af52`).

`diagnostics/bench/mma_bench.cu`, 20,000 iterations, best of five:

| Mode | Stock TF/s | Unlocked TF/s |
| --- | ---: | ---: |
| FP16 → FP16 accumulate | 36.6 | 36.6 |
| FP16 → FP32 accumulate | 18.3 | 36.6 |
| BF16 → FP32 accumulate | 18.2 | 36.3 |
| INT8 → INT32 | 72.8 | 72.6 |
| dp4a | 17.7 | 17.7 |
| scalar FFMA | 8.4 | 8.5 |

`tensor_modes_issue` through the driver runner (0 mismatches in every mode):

| Mode | Stock | Unlocked |
| --- | ---: | ---: |
| FP16 → FP32 accumulate | 18.07 TF/s | 36.01 TF/s |
| TF32 → FP32 accumulate | 9.04 TF/s | 18.00 TF/s |
| Unsigned INT4 → INT32 | 143.76 TOP/s | 143.75 TOP/s |

cuBLAS GEMM median (`docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`). The
unlocked column lists three consecutive runs:

| Mode | n | Stock TF/s | Unlocked TF/s (r1 / r2 / r3) | Change (r1) |
| --- | ---: | ---: | ---: | ---: |
| FP16/FP32 | 1024 | 13.39 | 22.61 / 18.3 / 22.6 | +69% |
| BF16/FP32 | 1024 | 11.58 | 14.29 / 12.6 / 14.2 | +23% |
| FP16/FP32 | 4096 | 15.83 | 22.47 / 22.5 / 22.3 | +42% |
| BF16/FP32 | 4096 | 16.22 | 26.49 / 26.7 / 26.3 | +63% |
| FP16/FP32 | 8192 | 16.05 | 19.66 / 19.5 / 19.9 | +22% |
| BF16/FP32 | 8192 | 15.90 | 19.47 / 20.0 / 19.5 | +22% |

`validate_gemm` passed every FP16/FP32 and BF16/FP32 case with
`failures=0, max_abs=0` in both states, and `gemm_bench` reported zero failed
cells. Telemetry sampled every 200 ms showed the 70 W software power cap
active in 89 of 197 loaded samples (stock) and 75 of 149 (unlocked), so the
large GEMMs are power-limited and gain far less than 2×. The 1024 runs are
short and noisy.

## Warm reboot with the module in the boot path

The boot options were then made permanent
(`GRUB_CMDLINE_LINUX_DEFAULT="gpu_unlock.targets=0000:01:00.0"`) and the host
was rebooted once with `systemctl reboot`. The boot also moved to the pending
kernel `6.8.0-147-generic`, for which DKMS had already built the module. The
platform reset cleared the card: the entry readback was `SS0=0x0`, `SS1=0x0`,
WPR2 high `0x0`. The module ran the full transaction again from initramfs
before `nvidia` loaded and ended with `gpu_unlock: unlocked`; the RM getter
again reported every field at full rate and the boot had no NVRM Xid. No AC
power cut was needed. The boot excerpt is `transaction-excerpt-boot.txt` and
the readback `sm-rates-boot.json` in the evidence directory.

## Not yet done

- a cold-boot (AC cut) attempt;
- an extended stability run and application workloads such as training;
- other GA107 boards (for example the 8 GB GA107 RTX 3050, `10de:2582`), which
  are not admitted by this change.
