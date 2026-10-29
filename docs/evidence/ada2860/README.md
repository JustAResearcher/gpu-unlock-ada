# RTX 4070 Mobile 10de:2860 evidence

`benchmark-matched.txt` contains matched stock baseline and unlocked stdout
measured across multiple compute modes (FP16, TF32, BF16, FP8, INT8)
on one ASUS ROG Zephyrus G16 GA605WI laptop (AMD Ryzen AI 9 HX 370, NVIDIA RTX 4070
Laptop GPU AD106M `10de:2860` rev a1, subsystem `1043:1e83`, 36 SMs).

Note on OEM laptop variance: This hardware evidence is recorded strictly on the
ASUS ROG Zephyrus G16 GA605WI. Mobile laptops vary significantly across manufacturers
in VBIOS ROM BAR0 aperture exposure, Embedded Controller (EC) power rails, and
PCIe root complex FLR handling. Results may not translate to other laptop models
without prior read-only hardware verification.

`sm-rates-stock.json` and `sm-rates-unlocked.json` are output from the
read-only `tools/read_sm_rates.py` querying the official NVIDIA ResourceManager
getter, with the GPU UUID replaced by `GPU-UUID-OMITTED`. The stock reading was
taken on an earlier boot before this build was installed.

`transaction-excerpt.txt` contains this card's boot kernel messages
from the `entry:` line to `gpu_unlock: unlocked`, with ROM/FWSEC parsing,
asset loading and Falcon DMA lines removed. It is an excerpt, not a complete
kernel log, and contains no host name.

`dmesg-boot.txt` contains kernel messages from the boot command line, initramfs
loading, `gpu_unlock` FWSEC parsing, handoff, and successful `nvidia-drm` driver initialization.

`nvidia-smi.txt` contains live `nvidia-smi` output showing active graphical processes,
driver `580.178.04`, and stable thermals.

`lspci-device.txt` contains verbose `lspci -vvv -nn -s 0000:64:00.0` output verifying
chip identity (`10de:2860`), ASUS subsystem ID (`1043:1e83`), BAR0 layout, and PCIe capabilities.

`stability-validation.txt` contains raw stdout from the production stress battery,
recording numerical precision checks against FP64 reference, VRAM
boundary saturation, sustained TDP burn, and zero Xid kernel checks.

`SHA256SUMS` covers every other file in this directory.
