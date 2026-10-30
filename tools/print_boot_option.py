#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print exact NVIDIA PCI boot options, including experimental targets."""
import re
import subprocess
import sys

# PCI device ID -> (label, experimental opt-in or None).
TARGETS = {
    "2684": ("RTX 4090", None),
    "2685": ("RTX 4090 D (experimental)", "rtx4090d_experimental"),
    "2689": ("RTX 4070 Ti SUPER AD102 (experimental)", "rtx4070tis_ad102_experimental"),
    "2702": ("RTX 4080 SUPER (experimental)", "ad103_experimental"),
    "2704": ("RTX 4080 (experimental)", "rtx4080_experimental"),
    "2705": ("RTX 4070 Ti SUPER (experimental)", "ad103_experimental"),
    "2709": ("RTX 4070 AD103 (experimental)", "ad103_experimental"),
    "2757": ("RTX 4090 Laptop GPU (experimental)", "ad103m_experimental"),
    "27e0": ("RTX 4080 Laptop GPU (experimental)", "ad104m_experimental"),
    "2782": ("RTX 4070 Ti (experimental)", "ad104_experimental"),
    "2783": ("RTX 4070 SUPER (experimental)", "ad104_experimental"),
    "2786": ("RTX 4070 (experimental)", "ad104_experimental"),
    "2803": ("RTX 4060 Ti (experimental)", "ad106_experimental"),
    "2805": ("RTX 4060 Ti 16GB (experimental)", "ad106_experimental"),
    "2882": ("RTX 4060 (experimental)", "ad107_experimental"),
    "28a0": ("RTX 4060 Laptop GPU (experimental)", "ad107_experimental"),
    "28e0": ("RTX 4060 Laptop GPU (experimental)", "ad107_experimental"),
    "28e1": ("RTX 4050 Laptop GPU (experimental)", "ad107_experimental"),
}


def candidates(lspci_output: str) -> list[tuple[str, str]]:
    found = []
    for line in lspci_output.splitlines():
        address = re.match(r"^([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7])\s", line)
        device = re.search(r"\[10de:([0-9a-f]{4})\]", line, re.I)
        if address and device and device.group(1).lower() in TARGETS:
            found.append((address.group(1).lower(), device.group(1).lower()))
    return found


def main() -> int:
    try:
        result = subprocess.run(["lspci", "-Dnn"], check=True, capture_output=True,
                                text=True, timeout=10)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"Could not read PCI devices: {error}. Install pciutils and retry.", file=sys.stderr)
        return 1
    found = candidates(result.stdout)
    if not found:
        known = ", ".join(f"{label} (10de:{device})" for device, (label, _) in TARGETS.items())
        print(f"No supported card was found. Recognized: {known}.", file=sys.stderr)
        return 2
    for address, device in found:
        label, flag = TARGETS[device]
        options = f"gpu_unlock.targets={address}"
        if flag:
            options += f" gpu_unlock.{flag}=1"
        print(f"{label} at {address} [10de:{device}]:")
        print(options)
        print()
    if len(found) > 1:
        print("Choose one exact card for the first cold test; cards that each passed alone "
              "can then be listed together, comma-separated.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
