#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fetch pinned inputs without executing/installing them; atomically verify cache."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import urllib.request

MANIFEST = Path(__file__).with_name("artifacts-610.43.03.json")


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def inputs(manifest: dict) -> list[dict]:
    """The NVIDIA source archive and the nvidia-utils package the build uses."""
    source, _reference, package = manifest["source_artifacts"]
    return [source, package]


def fetch(entry: dict, output: Path, offline: bool = False) -> Path:
    output.mkdir(parents=True, exist_ok=True)
    destination = output / entry["filename"]
    if destination.exists():
        if destination.is_symlink() or digest(destination) != entry["sha256"]:
            raise ValueError(f"invalid cached artifact: {destination}; remove it to retry")
        return destination
    if offline:
        raise FileNotFoundError(f"missing cached artifact: {destination}")
    if not entry["url"].startswith("https://"):
        raise ValueError("artifact downloads require HTTPS")
    temporary = None
    try:
        request = urllib.request.Request(entry["url"], headers={"User-Agent": "gpu-unlock-artifacts/1.1.0"})
        with urllib.request.urlopen(request, timeout=60) as response:
            if not response.url.startswith("https://"):
                raise ValueError("refusing non-HTTPS redirect")
            with tempfile.NamedTemporaryFile(dir=output, prefix=".download-", delete=False) as stream:
                temporary = Path(stream.name)
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > 1024 * 1024 * 1024:
                        raise ValueError("artifact exceeds 1 GiB download limit")
                    stream.write(block)
                stream.flush()
                os.fsync(stream.fileno())
        if digest(temporary) != entry["sha256"]:
            raise ValueError(f"SHA-256 mismatch: {entry['filename']}")
        temporary.chmod(0o644)
        temporary.replace(destination)
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="cache directory (for example scratchpad/artifacts)")
    parser.add_argument("--offline", action="store_true", help="verify cached inputs without network")
    args = parser.parse_args()
    try:
        for entry in inputs(json.loads(MANIFEST.read_text())):
            print(fetch(entry, args.output, args.offline))
    except (OSError, ValueError) as error:
        parser.exit(1, f"fetch failed: {error}\n")


if __name__ == "__main__":
    main()
