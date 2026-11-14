"""Build charts/speedup.png and results/summary.json from the per-run result files.
  python scripts/build_report.py [A_label=default] [B_label=unlocked]      (run from the repo root)
Reads results/results_<workload>_<label>.json and results/gpu_<workload>_<label>.csv.
Speedup = A time / B time - 1, using the median timed run; the mean-based figure is also stored."""
import json, csv, os, sys, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

A = sys.argv[1] if len(sys.argv) > 1 else "default"
B = sys.argv[2] if len(sys.argv) > 2 else "unlocked"
RES = "results"
WORKLOADS = [("Wan2.2 I2V A14B", "wan"), ("LTX-2.5 22B I2V", "ltx"), ("MiniMax H3 I2V (turbo)", "mm_turbo"), ("FLUX.1 dev fp8 T2I", "flux"), ("Qwen-Image-Edit 2511", "qwen")]


def runs(w, label):
    return [r["wall_s"] for r in json.load(open(os.path.join(RES, f"results_{w}_{label}.json")))["runs"]]


def gpu(w, label):
    rows = list(csv.DictReader(open(os.path.join(RES, f"gpu_{w}_{label}.csv"))))
    busy = [r for r in rows if float(r["util"]) > 90]
    f = lambda k: [float(r[k]) for r in busy]
    return dict(sm_mhz=round(st.mean(f("sm_mhz"))), power_w=round(st.mean(f("power_w"))),
                max_temp_c=round(max(float(r["temp_c"]) for r in rows)), peak_vram_gb=round(max(float(r["mem_used_mib"]) for r in rows) / 1024, 1))


out = {}
for name, w in WORKLOADS:
    ra, rb = runs(w, A), runs(w, B)
    o = dict(a_runs=ra, b_runs=rb, a_median=st.median(ra), b_median=st.median(rb), a_mean=round(st.mean(ra), 1), b_mean=round(st.mean(rb), 1), gpu_a=gpu(w, A), gpu_b=gpu(w, B))
    o["speedup_median_pct"] = round(100 * (o["a_median"] / o["b_median"] - 1), 1)
    o["speedup_mean_pct"] = round(100 * (o["a_mean"] / o["b_mean"] - 1), 1)
    o["time_saved_pct"] = round(100 * (1 - o["b_median"] / o["a_median"]), 1)
    out[name] = o
json.dump(dict(a_label=A, b_label=B, workloads=out), open(os.path.join(RES, "summary.json"), "w"), indent=1)

os.makedirs("charts", exist_ok=True)
fig, ax = plt.subplots(figsize=(9, 4.6), dpi=150)
names = list(out)
x = range(len(names))
w = 0.38
ax.bar([i - w / 2 for i in x], [out[n]["a_median"] for n in names], w, label=A, color="#8a8f98")
ax.bar([i + w / 2 for i in x], [out[n]["b_median"] for n in names], w, label=B, color="#2f8f5b")
for i, n in enumerate(names):
    ax.text(i + w / 2, out[n]["b_median"] + 1, f"+{out[n]['speedup_median_pct']:.0f}%", ha="center", fontsize=9, color="#1c5e3b", fontweight="bold")
    ax.text(i - w / 2, out[n]["a_median"] + 1, f"{out[n]['a_median']:.1f}s", ha="center", fontsize=8, color="#444")
    ax.text(i + w / 2, out[n]["b_median"] - 4.5 if out[n]["b_median"] > 9 else out[n]["b_median"] / 2, f"{out[n]['b_median']:.1f}s", ha="center", fontsize=8, color="white")
ax.set_xticks(list(x))
ax.set_xticklabels([n.replace(" ", "\n", 1) for n in names], fontsize=8.5)
ax.set_ylabel("seconds per job (median, lower is better)")
ax.set_title(f"RTX 4070 Ti SUPER: {A} vs {B}")
ax.legend(frameon=False)
ax.spines[["top", "right"]].set_visible(False)
fig.tight_layout()
fig.savefig("charts/speedup.png")
for n, v in out.items():
    print(f"{n:26} {v['a_median']:>6.1f}s -> {v['b_median']:>6.1f}s   median {v['speedup_median_pct']:+.1f}%   mean {v['speedup_mean_pct']:+.1f}%")
