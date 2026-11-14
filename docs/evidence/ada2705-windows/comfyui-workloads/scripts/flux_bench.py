"""Same harness as wan_bench.py, FLUX.1 dev fp8 text-to-image: 1024x1024, 20 steps, euler/simple, FluxGuidance 3.5, seed = base + run number.
  python flux_bench.py <label> [runs=5]       (one untimed warm-up image first so every timed run is warm)"""
import sys, time
import wan_bench as b

PROMPT = ("A night cyberpunk Minecraft city, glowing neon signs, flying cars on sky lanes, pixel-art style, crisp black outlines, "
          "a huge billboard reading MC.SESH, detailed, vibrant magenta and cyan lighting")


def graph(prefix, seed):
    return {
        "1": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": "FLUX1\\flux1-dev-fp8.safetensors"}},
        "2": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["1", 1], "text": PROMPT}},
        "3": {"class_type": "FluxGuidance", "inputs": {"conditioning": ["2", 0], "guidance": 3.5}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["1", 1], "text": ""}},
        "5": {"class_type": "EmptySD3LatentImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1}},
        "6": {"class_type": "KSampler", "inputs": {"model": ["1", 0], "seed": seed, "steps": 20, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple",
                                                    "positive": ["3", 0], "negative": ["4", 0], "latent_image": ["5", 0], "denoise": 1.0}},
        "7": {"class_type": "VAEDecode", "inputs": {"samples": ["6", 0], "vae": ["1", 2]}},
        "8": {"class_type": "SaveImage", "inputs": {"images": ["7", 0], "filename_prefix": prefix}},
    }


if __name__ == "__main__":
    a = sys.argv[1:]
    b.graph = graph
    pid = b.api("/prompt", {"prompt": graph("bench/fluxwarm", 1)})["prompt_id"]
    while True:
        h = b.api(f"/history/{pid}")
        if pid in h and h[pid]["status"].get("completed") is not None: break
        time.sleep(2)
    print("warm-up", h[pid]["status"]["status_str"], flush=True)
    b.run(a[0], int(a[1]) if len(a) > 1 else 5)
