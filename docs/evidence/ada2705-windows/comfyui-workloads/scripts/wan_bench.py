"""Repeatable benchmark harness for a local ComfyUI. This file holds the shared harness plus the Wan2.2 I2V A14B GGUF Q4_K_S + lightx2v 4-step LoRA workload.
  python wan_bench.py <label> [runs=3]
Seed = base+run number (defeats ComfyUI's result cache; run N is identical across labels, so frames can be compared).
Results -> results_<label>.json (+ gpu_<label>.csv sampled every 1 s) in BENCH_DIR.
Set BENCH_WARMUP=1 for one untimed warm-up job first so every timed run has the models loaded (run_bench_set.py does this).
Compare labels with: python wan_bench.py --compare <labelA> <labelB>

Config via env vars (all optional):
  BENCH_HOST       default http://127.0.0.1:8188
  BENCH_DIR        default ./results in the current directory (run the scripts from the repo root)
  BENCH_START_IMG  default ./assets/bench_start.png (shipped in the repo; it is uploaded to ComfyUI through /upload/image, so no input-folder path is needed)"""
import sys, os, json, time, threading, subprocess, urllib.request, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.environ.get("BENCH_HOST", "http://127.0.0.1:8188")
BENCH = os.environ.get("BENCH_DIR", os.path.join(os.getcwd(), "results"))
SRC_IMG = os.environ.get("BENCH_START_IMG", os.path.join(os.getcwd(), "assets", "bench_start.png"))
W, H, LENGTH, SEED = 832, 480, 49, 20261109
POS = "A night cyberpunk Minecraft city, neon signs glowing, flying cars moving along the sky lanes, locked camera, smooth motion"
NEG = "blurry, static, still image, jpeg artifacts, distorted, worst quality, low quality"


def graph(prefix, seed):
    return {
        "1": {"class_type": "UnetLoaderGGUF", "inputs": {"unet_name": "Wan2.2-I2V-A14B-HighNoise-Q4_K_S.gguf"}},
        "2": {"class_type": "UnetLoaderGGUF", "inputs": {"unet_name": "Wan2.2-I2V-A14B-LowNoise-Q4_K_S.gguf"}},
        "3": {"class_type": "LoraLoaderModelOnly", "inputs": {"model": ["1", 0], "lora_name": "wan2.2_i2v_lightx2v_4steps_lora_v1_high_noise.safetensors", "strength_model": 1.0}},
        "4": {"class_type": "LoraLoaderModelOnly", "inputs": {"model": ["2", 0], "lora_name": "wan2.2_i2v_lightx2v_4steps_lora_v1_low_noise.safetensors", "strength_model": 1.0}},
        "5": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["3", 0], "shift": 5.0}},
        "6": {"class_type": "ModelSamplingSD3", "inputs": {"model": ["4", 0], "shift": 5.0}},
        "7": {"class_type": "CLIPLoader", "inputs": {"clip_name": "umt5_xxl_fp8_e4m3fn_scaled.safetensors", "type": "wan", "device": "default"}},
        "8": {"class_type": "VAELoader", "inputs": {"vae_name": "wan_2.1_vae.safetensors"}},
        "9": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["7", 0], "text": POS}},
        "10": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["7", 0], "text": NEG}},
        "11": {"class_type": "LoadImage", "inputs": {"image": "bench_start.png"}},
        "12": {"class_type": "WanImageToVideo", "inputs": {"positive": ["9", 0], "negative": ["10", 0], "vae": ["8", 0], "width": W, "height": H, "length": LENGTH, "batch_size": 1, "start_image": ["11", 0]}},
        "13": {"class_type": "KSamplerAdvanced", "inputs": {"model": ["5", 0], "add_noise": "enable", "noise_seed": seed, "steps": 4, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple",
                                                            "positive": ["12", 0], "negative": ["12", 1], "latent_image": ["12", 2], "start_at_step": 0, "end_at_step": 2, "return_with_leftover_noise": "enable"}},
        "14": {"class_type": "KSamplerAdvanced", "inputs": {"model": ["6", 0], "add_noise": "disable", "noise_seed": seed, "steps": 4, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple",
                                                            "positive": ["12", 0], "negative": ["12", 1], "latent_image": ["13", 0], "start_at_step": 2, "end_at_step": 4, "return_with_leftover_noise": "disable"}},
        "15": {"class_type": "VAEDecode", "inputs": {"samples": ["14", 0], "vae": ["8", 0]}},
        "16": {"class_type": "SaveImage", "inputs": {"images": ["15", 0], "filename_prefix": prefix}},
    }


def api(path, data=None):
    req = urllib.request.Request(HOST + path, json.dumps(data).encode() if data is not None else None, {"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=60))


def upload_start_image():
    """Upload the start image through ComfyUI's HTTP API as input/bench_start.png (works with any input-folder layout)."""
    if not os.path.exists(SRC_IMG):
        sys.exit(f"start image not found: {SRC_IMG}. Run from the repo root, or point BENCH_START_IMG at a 2048x1152 image.")
    data = open(SRC_IMG, "rb").read(); bd = uuid.uuid4().hex
    head = ('--%s\r\nContent-Disposition: form-data; name="image"; filename="bench_start.png"\r\nContent-Type: image/png\r\n\r\n' % bd).encode()
    tail = ('\r\n--%s\r\nContent-Disposition: form-data; name="overwrite"\r\n\r\ntrue\r\n--%s--\r\n' % (bd, bd)).encode()
    req = urllib.request.Request(HOST + "/upload/image", head + data + tail, {"Content-Type": "multipart/form-data; boundary=" + bd})
    return json.load(urllib.request.urlopen(req, timeout=120))


def gpu_logger(label, stop):
    f = open(os.path.join(BENCH, f"gpu_{label}.csv"), "w")
    f.write("t,sm_mhz,mem_mhz,power_w,temp_c,util,mem_used_mib\n"); t0 = time.time()
    while not stop.is_set():
        try:
            o = subprocess.run(["nvidia-smi", "--query-gpu=clocks.sm,clocks.mem,power.draw,temperature.gpu,utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
                               capture_output=True, text=True, timeout=5).stdout.strip().replace(" ", "")
            f.write(f"{time.time() - t0:.0f},{o}\n"); f.flush()
        except Exception: pass
        stop.wait(1.0)
    f.close()


def run(label, runs):
    os.makedirs(BENCH, exist_ok=True)
    print("start image upload:", upload_start_image(), flush=True)
    stop = threading.Event(); th = threading.Thread(target=gpu_logger, args=(label, stop), daemon=True); th.start()
    res = []
    if os.environ.get("BENCH_WARMUP"):          # untimed warm-up so every timed run starts with the models loaded
        wp = api("/prompt", {"prompt": graph(f"bench/{label}_warmup", SEED)})["prompt_id"]
        while True:
            wh = api(f"/history/{wp}")
            if wp in wh and wh[wp]["status"].get("completed") is not None: break
            time.sleep(2)
        print("warm-up", wh[wp]["status"]["status_str"], flush=True)
    for r in range(1, runs + 1):
        pid = api("/prompt", {"prompt": graph(f"bench/{label}_run{r}", SEED + r)})["prompt_id"]; t0 = time.time()
        while True:
            h = api(f"/history/{pid}")
            if pid in h and h[pid]["status"].get("completed") is not None and (h[pid]["status"].get("completed") or h[pid]["status"].get("status_str") == "error"): break
            time.sleep(2)
        h = h[pid]; wall = time.time() - t0
        msgs = {m[0]: m[1].get("timestamp") for m in h["status"]["messages"]}
        srv = (msgs.get("execution_success", 0) - msgs.get("execution_start", 0)) / 1000.0 if msgs.get("execution_success") else None
        res.append(dict(run=r, wall_s=round(wall, 1), server_s=round(srv, 1) if srv else None, status=h["status"]["status_str"], cached=msgs.get("execution_cached") is not None))
        print(res[-1], flush=True)
    stop.set(); th.join(timeout=3)
    json.dump(dict(label=label, w=W, h=H, length=LENGTH, seed=SEED, runs=res), open(os.path.join(BENCH, f"results_{label}.json"), "w"), indent=1)
    print("bench done", label, flush=True)


def compare(a, b):
    ra, rb = (json.load(open(os.path.join(BENCH, f"results_{x}.json")))["runs"] for x in (a, b))
    for x, y in zip(ra, rb):
        print(f"run {x['run']}: {a} {x['wall_s']}s | {b} {y['wall_s']}s | change {100 * (y['wall_s'] / x['wall_s'] - 1):+.1f}%")


if __name__ == "__main__":
    a = sys.argv[1:]
    compare(a[1], a[2]) if a[0] == "--compare" else run(a[0], int(a[1]) if len(a) > 1 else 3)
