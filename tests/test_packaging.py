# SPDX-License-Identifier: GPL-2.0-only
"""Check the generated payload and the shared install layout."""
import hashlib
import json
import os
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

    def test_initramfs_tools_loads_before_udev_coldplug(self):
        # /init runs init-top before its module list, and init-top/udev
        # coldplugs nvidia; the init-top script must run ahead of udev.
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["make", "-s", f"DESTDIR={directory}", "install-initramfs-tools"],
                           cwd=ROOT, check=True)
            script = Path(directory) / "usr/share/initramfs-tools/scripts/init-top/00-gpu-unlock"
            self.assertTrue(script.is_file())
            # mkinitramfs drops scripts that aren't executable or fail sh -n.
            self.assertTrue(script.stat().st_mode & 0o111)
            subprocess.run(["sh", "-n", str(script)], check=True)
            prereqs = subprocess.run(["sh", str(script), "prereqs"], capture_output=True, text=True, check=True)
            self.assertEqual(prereqs.stdout.strip(), "")
            # Same pairs mkinitramfs feeds tsort (get_prereq_pairs) for a
            # typical Ubuntu init-top: scripts without prerequisites come out
            # in name order, so 00-gpu-unlock precedes udev.
            pairs = "".join(f"{name} {name}\n" for name in
                            ("udev", "plymouth", "keymap", "framebuffer", "blacklist",
                             "all_generic_ide", script.name))
            order = subprocess.run(["tsort"], input=pairs + "udev plymouth\n", capture_output=True,
                                   text=True, check=True).stdout.split()
            self.assertEqual(order[0], script.name)
            self.assertLess(order.index(script.name), order.index("udev"))

    def test_initramfs_tools_script_loads_the_module(self):
        # Same as load_modules, only earlier; a failing modprobe must not
        # stop the boot (init-top runs under set -e via ORDER).
        script = ROOT / "packaging/boot/initramfs-tools-init-top"
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            log = tmp / "modprobe.log"
            for status in (0, 1):
                log.unlink(missing_ok=True)
                shim = tmp / "modprobe"
                shim.write_text(f'#!/bin/sh\necho "$@" >> "{log.as_posix()}"\nexit {status}\n')
                shim.chmod(0o755)
                env = dict(os.environ, PATH=f"{tmp}{os.pathsep}{os.environ['PATH']}")
                result = subprocess.run(["sh", str(script)], env=env)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(log.read_text().strip(), "gpu_unlock")

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
