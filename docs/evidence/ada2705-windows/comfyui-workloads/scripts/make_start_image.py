"""Generate the benchmark start image (2048x1152) deterministically, as an optional stand-in.
  python scripts/make_start_image.py            -> assets/bench_start.png
Any 2048x1152 image works: all five workloads have fixed tensor shapes, step counts and seeds, so the compute cost does not depend on image content.
(The original image used for the published numbers is not shipped; this script only makes a stand-in.)"""
import os, numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 2048, 1152
rng = np.random.default_rng(20261109)
y = np.linspace(0, 1, H)[:, None]
sky = np.stack([np.broadcast_to(0.05 + 0.25 * y, (H, W)), np.broadcast_to(0.04 + 0.12 * y, (H, W)), np.broadcast_to(0.18 + 0.35 * y, (H, W))], -1)
img = Image.fromarray((sky * 255).astype(np.uint8))
d = ImageDraw.Draw(img)
for layer, (base, shade) in enumerate([(0.55, 30), (0.68, 18), (0.80, 10)]):          # three rows of "buildings"
    x = 0
    while x < W:
        w = int(rng.integers(60, 190)); h = int(rng.integers(120, 520) * (1.2 - 0.3 * layer))
        top = int(H * base) - h + 200
        d.rectangle([x, top, x + w, H], fill=(shade, shade, shade + 14))
        for wy in range(top + 14, H - 40, 34):                                           # lit windows
            for wx in range(x + 10, x + w - 14, 26):
                if rng.random() < 0.45:
                    c = [(255, 80, 200), (60, 220, 255), (255, 220, 90)][int(rng.integers(0, 3))]
                    d.rectangle([wx, wy, wx + 10, wy + 16], fill=c)
        x += w + int(rng.integers(0, 18))
img = img.filter(ImageFilter.GaussianBlur(0.8))
os.makedirs("assets", exist_ok=True)
img.save(os.path.join("assets", "bench_start.png"))
print("wrote assets/bench_start.png", img.size)
