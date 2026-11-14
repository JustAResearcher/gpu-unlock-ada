"""Check that two labels produced identical pixels (same seed -> same output when only a hardware/driver tweak changed).
  python diff_outputs.py <comfy_output_dir> <labelA> <labelB>        e.g. wan_default wan_unlocked, flux_default flux_unlocked
Compares bench/<labelA>_runN_*.png with bench/<labelB>_runN_*.png for N=1..; prints the mean absolute pixel difference (0.0 = identical).
Needs pillow + numpy (present in a ComfyUI venv). Different GPUs will NOT be bit-identical; use this only between states on one machine."""
import sys, os, glob
import numpy as np
from PIL import Image

out, a, b = sys.argv[1:4]
n = 1
while True:
    fa = sorted(glob.glob(os.path.join(out, "bench", f"{a}_run{n}_*.png")))
    fb = sorted(glob.glob(os.path.join(out, "bench", f"{b}_run{n}_*.png")))
    if not fa or not fb:
        break
    d = [float(np.abs(np.asarray(Image.open(x).convert("RGB"), dtype=np.int16) - np.asarray(Image.open(y).convert("RGB"), dtype=np.int16)).mean()) for x, y in zip(fa, fb)]
    print(f"run {n}: {len(d)} frames, worst mean abs diff {max(d):.4f}")
    n += 1
if n == 1:
    print("no matching files found")
