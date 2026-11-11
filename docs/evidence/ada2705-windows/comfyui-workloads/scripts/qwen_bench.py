"""Same harness as wan_bench.py, Qwen-Image-Edit 2511 (fp8mixed) + Lightning 4-step LoRA instruction edit of the 2K start image scaled to 1 MP; 4 steps, cfg 1, euler/simple.
  python qwen_bench.py <label> [runs=5]       (one untimed warm-up first)   seed = base + run number"""
import sys, time
import wan_bench as b

PROMPT = "Edit this image: keep the whole city exactly the same, but change the time to sunrise with warm orange light, and keep the MC.SESH billboard and all text unchanged."


def graph(prefix, seed):
    return {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "qwen_image_edit_2511_fp8mixed.safetensors", "weight_dtype": "default"}},
        "2": {"class_type": "LoraLoaderModelOnly", "inputs": {"model": ["1", 0], "lora_name": "Qwen-Image-Edit-2511-Lightning-4steps-V1.0-bf16.safetensors", "strength_model": 1.0}},
        "3": {"class_type": "ModelSamplingAuraFlow", "inputs": {"model": ["2", 0], "shift": 3.0}},
        "4": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_2.5_vl_7b_fp8_scaled.safetensors", "type": "qwen_image", "device": "default"}},
        "5": {"class_type": "VAELoader", "inputs": {"vae_name": "qwen_image_vae.safetensors"}},
        "6": {"class_type": "LoadImage", "inputs": {"image": "bench_start.png"}},
        "7": {"class_type": "ImageScaleToTotalPixels", "inputs": {"image": ["6", 0], "upscale_method": "lanczos", "megapixels": 1.0, "resolution_steps": 1}},
        "8": {"class_type": "TextEncodeQwenImageEditPlus", "inputs": {"clip": ["4", 0], "vae": ["5", 0], "image1": ["7", 0], "prompt": PROMPT}},
        "9": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["8", 0]}},
        "10": {"class_type": "VAEEncode", "inputs": {"pixels": ["7", 0], "vae": ["5", 0]}},
        "11": {"class_type": "KSampler", "inputs": {"model": ["3", 0], "seed": seed, "steps": 4, "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple",
                                                     "positive": ["8", 0], "negative": ["9", 0], "latent_image": ["10", 0], "denoise": 1.0}},
        "12": {"class_type": "VAEDecode", "inputs": {"samples": ["11", 0], "vae": ["5", 0]}},
        "13": {"class_type": "SaveImage", "inputs": {"images": ["12", 0], "filename_prefix": prefix}},
    }


if __name__ == "__main__":
    a = sys.argv[1:]
    b.graph = graph
    pid = b.api("/prompt", {"prompt": graph("bench/qwenwarm", 1)})["prompt_id"]
    while True:
        h = b.api(f"/history/{pid}")
        if pid in h and h[pid]["status"].get("completed") is not None: break
        time.sleep(2)
    st = h[pid]["status"]; print("warm-up", st["status_str"], flush=True)
    if st["status_str"] != "success":
        print([m[1] for m in st["messages"] if m[0] == "execution_error"][:1]); sys.exit(1)
    b.run(a[0], int(a[1]) if len(a) > 1 else 5)
