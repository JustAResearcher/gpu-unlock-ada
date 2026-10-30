# SPDX-License-Identifier: GPL-2.0-only
"""Check the generated payload and the shared install layout."""
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSETS = Path(os.environ.get("GPU_UNLOCK_ASSETS", "/nonexistent"))
sys.path.insert(0, str(ROOT / "firmware"))
import generate_v67  # noqa: E402


class PackageTests(unittest.TestCase):
    @unittest.skipUnless((ASSETS / "booter_load_ad102.bin").is_file(),
                         "set GPU_UNLOCK_ASSETS to the pinned GA/Ada asset directory")
    def test_firmware_package_contains_pinned_ada_assets_and_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["make", "-s", f"DESTDIR={directory}", f"ASSETS={ASSETS}",
                            "install-firmware"], cwd=ROOT, check=True)
            installed = Path(directory) / "usr/lib/firmware/gpu-unlock/610.43.03"
            manifest = json.loads((ROOT / "firmware/ada-610.43.03.json").read_text())
            for name, digest in manifest["derived_assets"].items():
                self.assertEqual(hashlib.sha256((installed / name).read_bytes()).hexdigest(), digest)
            self.assertEqual(hashlib.sha256((installed / "gsp_ga10x.bin").read_bytes()).hexdigest(),
                             manifest["shared_gsp"]["sha256"])
            payload_pin = self.manifest()["generated_ad102_payload"]
            self.assertEqual(hashlib.sha256((installed / payload_pin["filename"]).read_bytes()).hexdigest(),
                             payload_pin["sha256"])

    def test_v67_payload_matches_pin(self):
        digest = hashlib.sha256(generate_v67.build_payload()).hexdigest()
        self.assertEqual(digest, generate_v67.PAYLOAD_SHA256)
        self.assertEqual(digest, self.manifest()["generated_payload"]["sha256"])

    def test_install_uses_standard_paths(self):
        version = (ROOT / "VERSION").read_text().strip()
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["make", "-s", f"DESTDIR={directory}", "install-dkms",
                            "install-mkinitcpio", "install-initramfs-tools",
                            "install-dracut"],
                           cwd=ROOT, check=True)
            root = Path(directory)
            paths = {path.relative_to(root).as_posix() for path in root.rglob("*")
                     if path.is_file()}
            for expected in (f"usr/src/gpu-unlock-{version}/dkms.conf",
                             f"usr/src/gpu-unlock-{version}/main.c",
                             "etc/mkinitcpio.conf.d/zz-gpu-unlock.conf",
                             "usr/share/initramfs-tools/modules.d/gpu-unlock.conf",
                             "usr/lib/dracut/dracut.conf.d/gpu-unlock.conf",
                             "usr/share/doc/gpu-unlock/README.md"):
                self.assertIn(expected, paths)
            self.assertFalse(any(path.endswith(".mod.c") for path in paths))
            # Another package's "softdep nvidia" line would silently win over
            # ours (kmod honours only the first), so the order comes from the
            # initramfs instead: gpu_unlock goes right before nvidia.
            self.assertFalse(any("modprobe.d" in path for path in paths))
            dropin = root / "etc/mkinitcpio.conf.d/zz-gpu-unlock.conf"
            for before, after in (("zfs nvidia nvidia_drm", "zfs gpu_unlock nvidia nvidia_drm"),
                                  ("zfs", "zfs gpu_unlock"),
                                  ("zfs gpu_unlock? nvidia", "zfs gpu_unlock nvidia")):
                result = subprocess.run(["bash", "-c", f"MODULES=({before}); . {dropin}; echo ${{MODULES[*]}}"],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout.strip(), after)

    def test_void_scripts_drive_dkms_and_initramfs(self):
        version = (ROOT / "VERSION").read_text().strip()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "log"
            stubs = {"usr/libexec/xbps-triggers/dkms": f"echo trigger $dkms_modules $* >> {log}",
                     "etc/kernel.d/post-install/20-initramfs": f"echo initramfs $* >> {log}"}
            for path, body in stubs.items():
                stub = root / path
                stub.parent.mkdir(parents=True, exist_ok=True)
                stub.write_text(f"#!/bin/sh\n{body}\n")
                stub.chmod(0o755)
            for kver in ("6.12.10_1", "6.6.50_1"):
                (root / "usr/lib/modules" / kver / "kernel").mkdir(parents=True)
            for script, action, update in (("INSTALL", "post", "no"), ("REMOVE", "pre", "yes"),
                                           ("REMOVE", "post", "no")):
                text = (ROOT / "packaging/void" / script).read_text()
                self.assertIn("@VERSION@", text)
                # dkms changes directory, so every PATH entry must be absolute.
                path = re.search(r'^export PATH="([^"]*)"$', text, re.M).group(1)
                self.assertTrue(all(entry.startswith("/") for entry in path.split(":")), path)
                # Only 6.12 has the module; 6.6 is another installed kernel.
                text = 'modinfo() { [ "$2" = 6.12.10_1 ]; }\n' + text.replace("@VERSION@", version)
                subprocess.run(["sh", "-c", text, script, action, "gpu-unlock-dkms",
                                f"{version}_1", update, "no", "x86_64"],
                               cwd=root, check=True)
            self.assertEqual(log.read_text().splitlines(), [
                f"trigger gpu-unlock {version} run post-install gpu-unlock-dkms {version}_1 no",
                "initramfs linux6.12 6.12.10_1",
                f"trigger gpu-unlock {version} run pre-remove gpu-unlock-dkms {version}_1 yes",
                "initramfs linux6.12 6.12.10_1",
                "initramfs linux6.6 6.6.50_1"])

    def test_pkgbuild_pins_match_manifest(self):
        manifest = json.loads((ROOT / "firmware/artifacts-610.43.03.json").read_text())
        pkgbuild = (ROOT / "packaging/arch/PKGBUILD").read_text()
        for index in (0, 2):
            artifact = manifest["source_artifacts"][index]
            self.assertIn(artifact["sha256"], pkgbuild)
            self.assertIn(artifact["url"].rsplit("/", 1)[0], pkgbuild)
        self.assertIn(f"pkgver={(ROOT / 'VERSION').read_text().strip()}\n", pkgbuild)

    def test_native_versions_match(self):
        version = (ROOT / "VERSION").read_text().strip()
        spec = (ROOT / "packaging/rpm/gpu-unlock.spec").read_text()
        self.assertIn(f"Version: {version}\n", spec)
        for index in (0, 2):
            self.assertIn(self.manifest()["source_artifacts"][index]["sha256"], spec)
        changelog = (ROOT / "debian/changelog").read_text()
        self.assertTrue(changelog.startswith(f"gpu-unlock ({version}) "))

    @staticmethod
    def manifest():
        return json.loads((ROOT / "firmware/artifacts-610.43.03.json").read_text())
