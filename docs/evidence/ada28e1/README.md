# RTX 4050 Mobile 10de:28e1 evidence

`benchmark-matched.txt` contains matched stock baseline and unlocked stdout
measured using the pure CUDA Driver API JIT SM89 tensor issue-rate benchmark
on one ASUS TUF Gaming A16 FA607NUG laptop (AMD Ryzen 7 7445HS, NVIDIA RTX 4050
Mobile AD107M `10de:28e1` rev a1, subsystem `1043:14f4`, 20 SMs, BIOS FA607NUG.306).

Note on OEM laptop variance: This hardware evidence is recorded strictly on the
ASUS TUF Gaming A16 FA607NUG. Mobile laptops vary significantly across manufacturers
in VBIOS ROM BAR0 aperture exposure, Embedded Controller (EC) power rails, and
PCIe root complex FLR handling. Results may not translate to other laptop models
without prior read-only hardware verification.

`sm-rates-stock.json` and `sm-rates-unlocked.json` are output from the
read-only `tools/read_sm_rates.py` querying the official NVIDIA ResourceManager
getter, with the GPU UUID replaced by `GPU-UUID-OMITTED`. The stock reading was
taken on an earlier boot before this build was installed; the stock 1.1.1
module present then does not accept `10de:28e1`.

`transaction-excerpt.txt` contains this card's current-boot kernel messages
from the `entry:` line to `gpu_unlock: unlocked`, with ROM/FWSEC parsing,
asset loading and Falcon DMA lines removed. It is an excerpt, not a complete
kernel log, and contains no host name.

`dmesg-boot.txt` contains kernel messages from the boot command line, initramfs
loading, `gpu_unlock` FWSEC parsing, handoff, and successful `nvidia-drm` driver initialization.

`nvidia-smi.txt` contains live `nvidia-smi` output showing active graphical processes,
driver `615.78.08`, and stable thermals.

`lspci-device.txt` contains verbose `lspci -vvv -nn -s 0000:01:00.0` output verifying
chip identity (`10de:28e1`), ASUS subsystem ID (`1043:14f4`), BAR0 layout, and PCIe capabilities.

`stability-validation.txt` contains raw stdout from the production stress battery,
recording cuBLAS numerical precision checks against FP64 reference, 4.04 GB VRAM
boundary saturation, 30-second 100% TDP burn, NVENC ASIC encoding, and zero Xid kernel checks.

`SHA256SUMS` covers every other file in this directory.

