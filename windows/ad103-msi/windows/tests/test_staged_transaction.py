"""Real shared unlock + Windows EFI adapter integration; never accesses a GPU.

The fixture runs only with all five exact pinned firmware files. Missing assets
are an explicit unittest skip, never a successful full transaction. Set
GPU_UNLOCK_ASSETS to a prepared asset directory to run this validation.
"""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WIN = ROOT / "windows"
ASSETS = {
    "booter_load_ad102.bin": (57672, "6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49"),
    "v67-ad102-610.43.03-four-plm.bin": (64000, "44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb"),
    "gsp_ga10x.bin": (84277400, "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"),
    "gsp_boot_ad102.bin": (36864, "45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0"),
    "gsp_boot_desc_ad102.bin": (84, "83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42"),
}
PROFILE = """/* Synthetic host profile; never ship this as hardware authorization. */
#define GU_PROFILE_VERSION 1U
#define GU_PROFILE_ID 0x270510deU
#define GU_BOARD_SUBSYSTEM 0xe1301462U
#define GU_BOARD_CHIP 0x193000a1U
#define GU_PROFILE_SEGMENT 0U
#define GU_PROFILE_BUS 1U
#define GU_PROFILE_DEVICE 0U
#define GU_PROFILE_FUNCTION 0U
#define GU_PROFILE_BAR_BASE 0x83000000ULL
#define GU_PROFILE_BAR_SIZE 0x1000000ULL
#define GU_PROFILE_PROM_HASH "host-fixture-only"
#define GU_PROFILE_ROM_HASH "host-fixture-only"
#define GU_PROFILE_ROM_SIZE 4096U
#define GU_PROFILE_VRAM 0x3ff8U
#define GU_PROFILE_TAG "host-fixture-only"
"""


class StagedTransactionTests(unittest.TestCase):
    def test_real_core_with_real_windows_adapters_and_pinned_assets(self):
        value = os.environ.get("GPU_UNLOCK_ASSETS")
        if not value:
            self.skipTest("full EFI transaction NOT RUN: set GPU_UNLOCK_ASSETS to all five pinned Ada assets")
        assets = Path(value).resolve()
        for name, (size, digest) in ASSETS.items():
            path = assets / name
            self.assertTrue(path.is_file(), f"missing required integration asset: {path}")
            self.assertEqual(path.stat().st_size, size, name)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest, name)
        with tempfile.TemporaryDirectory(prefix="gu-real-efi-transaction-") as td:
            directory = Path(td)
            (directory / "board_profile.h").write_text(PROFILE)
            output = directory / "transaction"
            command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                       "-Wno-misleading-indentation", "-fshort-wchar",
                       "-D__KERNEL__", "-DGU_UEFI_PORT", "-DGU_ENABLE_UNLOCK=1",
                       "-I" + str(directory), "-I" + str(WIN / "src/compat"),
                       "-I" + str(ROOT / "kernel/gpu_unlock"),
                       "-I" + str(WIN / "deps/edk2"), "-I" + str(WIN / "deps/edk2/X64"),
                       str(WIN / "tests/test_staged_transaction.c"),
                       str(WIN / "src/compat.c"), str(WIN / "src/port.c"),
                       str(WIN / "src/staged_state.c"),
                       *[str(ROOT / "kernel/gpu_unlock" / (name + ".c"))
                         for name in ("gfw_eval", "prep_610", "layout_610", "vbios_610")],
                       "-o", str(output)]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            tested = subprocess.run([str(output), str(assets)], capture_output=True,
                                    text=True, timeout=180)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn("real shared core + real Windows EFI adapters: PASS", tested.stdout)
            print(tested.stdout, end="")


if __name__ == "__main__":
    unittest.main()
