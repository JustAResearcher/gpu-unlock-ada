"""Compare two full bench-set labels (all workloads) and flag environment drift (ComfyUI / PyTorch / GPU / driver / power limit) between them.
  python compare_all.py <labelA> <labelB>       (speedup = A time / B time - 1, using the median timed run)
Reads env_<label>.json and results_<workload>_<label>.json from BENCH_DIR (default ./results)."""
import sys, os, json
import wan_bench as wb

WORKLOADS = [("Wan2.2", "wan"), ("LTX-2.5", "ltx"), ("MiniMax H3", "mm_turbo"), ("FLUX.1 dev", "flux"), ("Qwen-Image-Edit", "qwen")]


def load(p):
    try:
        return json.load(open(p))
    except FileNotFoundError:
        return None


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def check_drift(a, b):
    ea, eb = (load(os.path.join(wb.BENCH, f"env_{x}.json")) for x in (a, b))
    if ea is None or eb is None:
        print("(env file missing for one label; drift check skipped)")
        return
    drift = []
    for k, label in (("comfyui_version", "ComfyUI version"), ("pytorch_version", "PyTorch version")):
        va, vb = ea["comfyui"].get(k), eb["comfyui"].get(k)
        if va and vb and va != vb:
            drift.append(f"{label}: {a}={va} vs {b}={vb}")
    for k in ("gpu", "vbios_and_power"):
        if ea.get(k) != eb.get(k):
            drift.append(f"{k}: {a}={ea.get(k)!r} vs {b}={eb.get(k)!r}")
    if drift:
        print("*** WARNING: environment differs between these labels ***")
        for d in drift:
            print("  -", d)
        print("  Numbers may be confounded by this.\n")


def compare(a, b):
    check_drift(a, b)
    print(f"{'workload':<16} {a:>14} {b:>14}   speedup (A/B-1)")
    for name, p in WORKLOADS:
        ra, rb = (load(os.path.join(wb.BENCH, f"results_{p}_{x}.json")) for x in (a, b))
        if not ra or not rb:
            print(f"{name:<16} (missing results, skipped)")
            continue
        ma, mb = median(r["wall_s"] for r in ra["runs"]), median(r["wall_s"] for r in rb["runs"])
        print(f"{name:<16} {ma:>13.1f}s {mb:>13.1f}s   {100 * (ma / mb - 1):+.1f}%")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: python compare_all.py <labelA> <labelB>")
    compare(sys.argv[1], sys.argv[2])
