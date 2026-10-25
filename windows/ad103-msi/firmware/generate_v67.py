#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract pinned 610.43.03 GA10x firmware and generate GA102/AD102 V67 images."""

from __future__ import annotations

import argparse
import json
import hashlib
import subprocess
import tarfile
from pathlib import Path


PACKAGE_SHA256 = "e17e7c92eeffa178b0fe1f40eb17a47095f8fc3ce4a77bf115cf5bd5a4df86e9"
GSP_SHA256 = "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"
PAYLOAD_SHA256 = "2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197"
AD102_PAYLOAD_SHA256 = "44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb"
AD102_PAYLOAD_NAME = "v67-ad102-610.43.03-four-plm.bin"
PACKAGE_VERSION = "610.43.03-1"
FIRMWARE_PREFIX = "usr/lib/firmware/nvidia/610.43.03/"
MANIFEST = Path(__file__).with_name("artifacts-610.43.03.json")

PAYLOAD_SIZE = 0xFA00
FILL_DWORD = 0x0000019C
PATCHED_WORDS = {
    0x1100: 0x00000007,
    0xF948: 0xFFFFFFFF,
    0xF950: 0x00000D44,
    0xF960: 0x00823804,
    0xF968: 0x00001FCE,
    0xF974: 0x00000000,
    0xF97C: 0x00001101,
    0xF980: 0x000084C8,
    0xF984: 0x00008E18,
    0xF98C: 0x000084C8,
    0xF990: 0x00000000,
    0xF998: 0x00001FCE,
    0xF9A4: 0x0000FFBC,
    0xF9AC: 0x00005789,
    0xF9BC: 0x00000D44,
    0xF9CC: 0x00000003,
    0xF9D4: 0x00001FCE,
    0xF9E8: 0x00000D52,
    0xF9EC: 0x000081EE,
}

# Adapted from TOTORONG/AD103_win_unlocker, source/firmware-tools/ad102_payload.py,
# revision 51df9c17ec89eb8486df52cc9079dde4bd1f9974 (GPL-2.0-only).
# Modified 2026-10-23: keep only the deterministic payload construction here;
# the upstream interpreter and firmware disassembly are not needed to build it.
# This reproduces an experimental debug-derived chain, not proof of execution
# by the production booter. See NOTICE.md and the manifest for provenance.
AD102_PATCHED_WORDS = {
    0x1100: 0x00000007,
    0xF948: 0xFFFFF0FF,
    0xF950: 0x000021B2,
    0xF960: 0x001FA7CC,
    0xF968: 0x000033FC,
    0xF974: 0xFFFFFFFF,
    0xF97C: 0x0000256F,
    0xF980: 0x00007A02,
    0xF984: 0x009A0148,
    0xF98C: 0x000033FC,
    0xF998: 0xFFFFFFFF,
    0xF9A0: 0x0000256F,
    0xF9A4: 0x00007A02,
    0xF9A8: 0x001FA7C4,
    0xF9B0: 0x000033FC,
    0xF9BC: 0xFFFFFFFF,
    0xF9C4: 0x0000256F,
    0xF9C8: 0x00007A02,
    0xF9CC: 0x00823804,
    0xF9D4: 0x000033FC,
    0xF9E0: 0x67B00001,
    0xF9E8: 0x0000256F,
    0xF9EC: 0x00007A02,
    0xF9F8: 0x000031B5,
    0xF9FC: 0x0000771E,
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_member(package: Path, name: str) -> bytes:
    if package.name.endswith(".zst"):
        process = subprocess.Popen(["zstd", "-dc", str(package)], stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
                result = read_member(archive, name)
            if process.wait() != 0:
                raise ValueError("zstd could not read the pinned package")
            return result
        finally:
            if process.stdout:
                process.stdout.close()
            if process.poll() is None:
                process.kill()
                process.wait()
    with tarfile.open(package, mode="r:*") as archive:
        return read_member(archive, name)


def read_member(archive: tarfile.TarFile, name: str) -> bytes:
    result = None
    for member in archive:
        if member.name != name:
            continue
        if result is not None:
            raise ValueError(f"duplicate package member: {name}")
        if not member.isfile() or member.size > 128 * 1024 * 1024:
            raise ValueError(f"not a regular file in package: {name}")
        stream = archive.extractfile(member)
        if stream is None:
            raise ValueError(f"could not read package member: {name}")
        result = stream.read()
    if result is None:
        raise ValueError(f"missing package member: {name}")
    return result


def build_payload() -> bytes:
    """Build the original GA102 rejoin15 payload (public API retained)."""
    return _build_payload(PATCHED_WORDS)


def build_ad102_payload() -> bytes:
    """Build the pinned AD102 four-write chain and explicit mailbox marker."""
    return _build_payload(AD102_PATCHED_WORDS)


def _build_payload(patched_words: dict[int, int]) -> bytes:
    word = FILL_DWORD.to_bytes(4, "little")
    payload = bytearray(word * (PAYLOAD_SIZE // len(word)))
    for offset, value in patched_words.items():
        payload[offset : offset + 4] = value.to_bytes(4, "little")
    return bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path, help="pinned nvidia-utils-610.43.03 package")
    parser.add_argument("output", type=Path, help="directory for extracted firmware and payload")
    args = parser.parse_args()

    package_bytes_hash = hashlib.sha256()
    with args.package.open("rb") as package_file:
        for block in iter(lambda: package_file.read(1024 * 1024), b""):
            package_bytes_hash.update(block)
    if package_bytes_hash.hexdigest() != PACKAGE_SHA256:
        raise SystemExit("package SHA-256 does not match the pinned nvidia-utils artifact")

    pkginfo = extract_member(args.package, ".PKGINFO").decode("utf-8", errors="replace")
    if f"pkgver = {PACKAGE_VERSION}" not in pkginfo.splitlines():
        raise SystemExit("package metadata version does not match 610.43.03-1")

    gsp = extract_member(args.package, FIRMWARE_PREFIX + "gsp_ga10x.bin")
    if sha256(gsp) != GSP_SHA256:
        raise SystemExit("GA10x GSP firmware SHA-256 does not match the pinned artifact")

    payload = build_payload()
    if sha256(payload) != PAYLOAD_SHA256:
        raise SystemExit("generated GA102 V67 image differs from its pinned SHA-256")
    ad102_payload = build_ad102_payload()
    if sha256(ad102_payload) != AD102_PAYLOAD_SHA256:
        raise SystemExit("generated AD102 V67 image differs from its pinned SHA-256")
    manifest = json.loads(MANIFEST.read_text())
    notice = manifest["source_artifacts"][2]["notice"]
    license_text = extract_member(args.package, notice["path"])
    if sha256(license_text) != notice["sha256"]:
        raise SystemExit("NVIDIA license notice differs from pinned package")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "gsp_ga10x.bin").write_bytes(gsp)
    (args.output / "v67-ga102-610.43.03.bin").write_bytes(payload)
    (args.output / AD102_PAYLOAD_NAME).write_bytes(ad102_payload)
    (args.output / "NVIDIA-LICENSE").write_bytes(license_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
