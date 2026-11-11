"""Run the whole ComfyUI bench suite (5 workloads) under one state label and record the machine + software environment next to the results,
so a version drift between two states' runs is caught instead of becoming a silent caveat.

  python run_bench_set.py <label> [video_runs=3] [image_runs=5]

Writes env_<label>.json, then runs (skipping any workload whose nodes/models are missing, see preflight.py):
  wan_<label>, ltx_<label>, mm_turbo_<label> (turbo), flux_<label>, qwen_<label>
Each workload does one untimed warm-up, then the timed runs. A failure is logged and the set continues.
Compare two labels afterwards: python compare_all.py <labelA> <labelB>
Env vars: BENCH_HOST, BENCH_DIR (see wan_bench.py)."""
import sys, os, json, subprocess, time, platform, urllib.request
import wan_bench as wb
import preflight

HERE = os.path.dirname(os.path.abspath(__file__))


def smi(q):
    try:
        return subprocess.run(["nvidia-smi", f"--query-gpu={q}", "--format=csv,noheader"], capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception as e:
        return f"nvidia-smi failed: {e}"


def capture_env(label):
    try:
        s = json.load(urllib.request.urlopen(wb.HOST + "/system_stats", timeout=10))
        sy = s.get("system", {})
        d = (s.get("devices") or [{}])[0]
        comfy = dict(comfyui_version=sy.get("comfyui_version"), python_version=sy.get("python_version"), pytorch_version=sy.get("pytorch_version"),
                     argv=sy.get("argv"), device=d.get("name"), vram_total_gib=round(d.get("vram_total", 0) / 2**30, 1))
    except Exception as e:
        comfy = dict(error=f"could not reach ComfyUI /system_stats: {e}")
    env = dict(label=label, captured=time.strftime("%Y-%m-%d %H:%M:%S"), comfyui=comfy,
               gpu=smi("name,driver_version"), vbios_and_power=smi("vbios_version,power.limit,power.default_limit,pcie.link.gen.current,pcie.link.width.current"),
               os=platform.platform(), cpu=platform.processor(), host=platform.node())
    os.makedirs(wb.BENCH, exist_ok=True)
    path = os.path.join(wb.BENCH, f"env_{label}.json")
    json.dump(env, open(path, "w"), indent=1)
    print("environment ->", path, "::", json.dumps(env), flush=True)


def run(label, video_runs=3, image_runs=5):
    capture_env(label)
    os.environ["BENCH_WARMUP"] = "1"
    jobs = {"wan": ["wan_bench.py", f"wan_{label}", str(video_runs)],
            "ltx": ["ltx_bench.py", f"ltx_{label}", str(video_runs)],
            "mm_turbo": ["minimax_bench.py", f"mm_turbo_{label}", str(video_runs), "turbo"],
            "flux": ["flux_bench.py", f"flux_{label}", str(image_runs)],
            "qwen": ["qwen_bench.py", f"qwen_{label}", str(image_runs)]}
    pf = preflight.check()
    status = {}
    for w, job in jobs.items():
        if pf[w]:
            print(f"\n=== SKIP {w}: " + "; ".join(pf[w]), flush=True)
            status[w] = "skipped: " + "; ".join(pf[w])
            continue
        print(f"\n=== {' '.join(job)} ===", flush=True)
        rc = subprocess.run([sys.executable, os.path.join(HERE, job[0])] + job[1:]).returncode
        status[w] = "ok" if rc == 0 else f"FAILED (exit {rc})"
    json.dump(status, open(os.path.join(wb.BENCH, f"status_{label}.json"), "w"), indent=1)
    print(f"\nALL BENCHES DONE for label '{label}': {json.dumps(status)}", flush=True)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        sys.exit("usage: python run_bench_set.py <label> [video_runs=3] [image_runs=5]")
    run(a[0], int(a[1]) if len(a) > 1 else 3, int(a[2]) if len(a) > 2 else 5)
