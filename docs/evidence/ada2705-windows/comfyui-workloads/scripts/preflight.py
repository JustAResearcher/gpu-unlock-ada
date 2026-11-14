"""Check which of the 5 benchmark workloads can run on this ComfyUI (node classes + model files present).
  python preflight.py          prints a table. run_bench_set.py calls check() and skips workloads that fail.
Model names are matched exactly as the graphs reference them (path separators and case ignored). A missing file means: either download it,
or edit the name in the workload script to an equivalent file you have AND record that substitution in the report (numbers are then not comparable to the reference machine)."""
import sys, json, urllib.request, os

HOST = os.environ.get("BENCH_HOST", "http://127.0.0.1:8188")

REQ = {
    "wan": dict(nodes=["UnetLoaderGGUF", "LoraLoaderModelOnly", "ModelSamplingSD3", "CLIPLoader", "VAELoader", "WanImageToVideo", "KSamplerAdvanced", "VAEDecode"],
                files=[("UnetLoaderGGUF", "unet_name", "Wan2.2-I2V-A14B-HighNoise-Q4_K_S.gguf"), ("UnetLoaderGGUF", "unet_name", "Wan2.2-I2V-A14B-LowNoise-Q4_K_S.gguf"),
                       ("LoraLoaderModelOnly", "lora_name", "wan2.2_i2v_lightx2v_4steps_lora_v1_high_noise.safetensors"),
                       ("LoraLoaderModelOnly", "lora_name", "wan2.2_i2v_lightx2v_4steps_lora_v1_low_noise.safetensors"),
                       ("CLIPLoader", "clip_name", "umt5_xxl_fp8_e4m3fn_scaled.safetensors"), ("VAELoader", "vae_name", "wan_2.1_vae.safetensors")]),
    "ltx": dict(nodes=["UNETLoader", "CLIPLoader", "VAELoader", "LTXVConditioning", "LTXVPreprocess", "LTXVImgToVideo", "LTXVEmptyLatentAudio", "LTXVConcatAVLatent",
                       "LTXVDualCFGGuider", "ManualSigmas", "SamplerCustomAdvanced", "LTXVSeparateAVLatent", "VAEDecodeTiled"],
                files=[("UNETLoader", "unet_name", "ltx-2.5-22b-distilled-transformer-comfy-int8-convrot.safetensors"), ("VAELoader", "vae_name", "ltx-2.5-video-vae-bf16.safetensors"),
                       ("VAELoader", "vae_name", "ltx-2.5-audio-vae-bf16.safetensors"), ("CLIPLoader", "clip_name", "gemma4-12b-with-proj-ltx-2.5-comfy-int8-convrot.safetensors")]),
    "mm_turbo": dict(nodes=["UNETLoader", "CLIPLoader", "VAELoader", "MiniMaxH3ImageToVideo", "LoraLoaderModelOnly", "BasicScheduler", "BasicGuider", "SamplerCustomAdvanced", "RandomNoise"],
                     files=[("UNETLoader", "unet_name", "minimax_h3_fl2va_pruned_int8_convrot.safetensors"), ("CLIPLoader", "clip_name", "qwen3vl_32b_minimax_h3_int8_convrot.safetensors"),
                            ("VAELoader", "vae_name", "minimax_h3_video_vae_fp16.safetensors"), ("LoraLoaderModelOnly", "lora_name", "minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors")]),
    "flux": dict(nodes=["CheckpointLoaderSimple", "FluxGuidance", "EmptySD3LatentImage", "KSampler"],
                 files=[("CheckpointLoaderSimple", "ckpt_name", "FLUX1\\flux1-dev-fp8.safetensors")]),
    "qwen": dict(nodes=["UNETLoader", "LoraLoaderModelOnly", "ModelSamplingAuraFlow", "CLIPLoader", "VAELoader", "ImageScaleToTotalPixels", "TextEncodeQwenImageEditPlus", "ConditioningZeroOut", "VAEEncode", "KSampler"],
                 files=[("UNETLoader", "unet_name", "qwen_image_edit_2511_fp8mixed.safetensors"), ("LoraLoaderModelOnly", "lora_name", "Qwen-Image-Edit-2511-Lightning-4steps-V1.0-bf16.safetensors"),
                        ("CLIPLoader", "clip_name", "qwen_2.5_vl_7b_fp8_scaled.safetensors"), ("VAELoader", "vae_name", "qwen_image_vae.safetensors")]),
}


def norm(s):
    return s.replace("\\", "/").lower()


def check(only=None):
    """returns {workload: [problems]} (empty list = runnable)"""
    cache = {}
    try:
        urllib.request.urlopen(HOST + "/system_stats", timeout=10)
    except Exception as e:
        sys.exit(f"ComfyUI is not reachable at {HOST} ({e}). Start ComfyUI (or set BENCH_HOST) and retry.")

    def info(cls):
        if cls not in cache:
            try:
                cache[cls] = json.load(urllib.request.urlopen(f"{HOST}/object_info/{cls}", timeout=30)).get(cls)
            except Exception:
                cache[cls] = None
        return cache[cls]

    out = {}
    for w, r in REQ.items():
        if only and w not in only:
            continue
        probs = []
        for cls in r["nodes"]:
            if not info(cls):
                probs.append(f"node class missing: {cls}")
        for cls, inp, val in r["files"]:
            i = info(cls)
            if not i:
                continue
            spec = i["input"].get("required", {}).get(inp) or i["input"].get("optional", {}).get(inp)
            opts = spec[0] if spec and isinstance(spec[0], list) else []
            if norm(val) not in [norm(o) for o in opts]:
                probs.append(f"model file not found for {cls}.{inp}: {val}")
        out[w] = probs
    return out


if __name__ == "__main__":
    res = check()
    for w, p in res.items():
        print(f"{w:<9} {'READY' if not p else 'BLOCKED'}")
        for x in p:
            print("   -", x)
    try:
        s = json.load(urllib.request.urlopen(HOST + "/system_stats", timeout=10))
        d = s["devices"][0]
        print("\nComfyUI", s["system"].get("comfyui_version"), "| torch", s["system"].get("pytorch_version"), "|", d["name"], "|", round(d["vram_total"] / 2**30, 1), "GiB VRAM")
    except Exception as e:
        print("could not read /system_stats:", e)
