#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print exact, supported NVIDIA PCI boot options without changing the host."""
import re
import subprocess
import sys


def candidates(lspci_output: str) -> list[tuple[str, str]]:
    found = []
    for line in lspci_output.splitlines():
        address = re.match(r"^([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7])\s", line)
        device = re.search(r"\[10de:(2684|2685|2705|2805|28a0)\]", line, re.I)
        if address and device:
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
        print("No supported RTX 4090 (10de:2684), experimental RTX 4090 D "
              "(10de:2685), experimental RTX 4070 Ti SUPER (10de:2705) or "
              "experimental RTX 4060 Ti 16GB (10de:2805) was found.",
              file=sys.stderr)
        return 2
    for address, device in found:
        options = f"gpu_unlock.targets={address}"
        label = {"2684": "RTX 4090",
                 "2685": "RTX 4090 D (experimental)",
                 "2705": "RTX 4070 Ti SUPER (experimental)",
                 "2805": "RTX 4060 Ti 16GB (experimental)",
                 "28a0": "RTX 4060 Laptop (experimental)"}[device]
        if device == "2685":
            options += " gpu_unlock.rtx4090d_experimental=1"
        if device == "2705":
            options += " gpu_unlock.ad103_experimental=1"
        if device == "2805":
            options += " gpu_unlock.ad106_experimental=1"
        if device == "28a0":
            options += " gpu_unlock.ad107_experimental=1"
        print(f"{label} at {address} [10de:{device}]:")
        print(options)
        print()
    if len(found) > 1:
        print("Choose one exact card for the first cold test; do not combine addresses.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
