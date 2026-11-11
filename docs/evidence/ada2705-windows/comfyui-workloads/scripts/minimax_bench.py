"""Same harness as wan_bench.py, MiniMax H3 image-to-video (local int8 fl2va model, joint video+audio latent), graph taken from ComfyUI's own template video_minimax_h3_i2v.
  python minimax_bench.py <label> [runs=3] [turbo|full]
turbo = fl2v turbo 8-step LoRA + 6 steps (template's turbo setting); full = no LoRA, 20 steps (template default).
832x480, start image = same 2K start image, 24 fps, 56 frames (MiniMax length grid is 17k+5, so 49 is not valid; 56 is the closest), seed = base + run."""
import sys
import wan_bench as b

MODE = "turbo"
LENGTH = 56


def graph(prefix, seed):
    turbo = MODE == "turbo"
    g = {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "minimax_h3_fl2va_pruned_int8_convrot.safetensors", "weight_dtype": "default"}},
        "3": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen3vl_32b_minimax_h3_int8_convrot.safetensors", "type": "minimax", "device": "default"}},
        "4": {"class_type": "VAELoader", "inputs": {"vae_name": "minimax_h3_video_vae_fp16.safetensors"}},
        "5": {"class_type": "LoadImage", "inputs": {"image": "bench_start.png"}},
        "6": {"class_type": "MiniMaxH3ImageToVideo", "inputs": {"clip": ["3", 0], "vae": ["4", 0], "prompt": b.POS, "width": b.W, "height": b.H, "length": LENGTH, "first_frame": ["5", 0]}},
        "7": {"class_type": "KSamplerSelect", "inputs": {"sampler_name": "res_multistep"}},
        "9": {"class_type": "RandomNoise", "inputs": {"noise_seed": seed}},
        "11": {"class_type": "SaveImage", "inputs": {"images": ["12", 0], "filename_prefix": prefix}},
    }
    model = ["1", 0]
    if turbo:
        g["2"] = {"class_type": "LoraLoaderModelOnly", "inputs": {"model": ["1", 0], "lora_name": "minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors", "strength_model": 1.0}}
        model = ["2", 0]
    g["8"] = {"class_type": "BasicScheduler", "inputs": {"model": model, "scheduler": "simple", "steps": 6 if turbo else 20, "denoise": 1.0}}
    g["10"] = {"class_type": "BasicGuider", "inputs": {"model": model, "conditioning": ["6", 0]}}
    g["13"] = {"class_type": "SamplerCustomAdvanced", "inputs": {"noise": ["9", 0], "guider": ["10", 0], "sampler": ["7", 0], "sigmas": ["8", 0], "latent_image": ["6", 1]}}
    g["12"] = {"class_type": "VAEDecode", "inputs": {"samples": ["13", 1], "vae": ["4", 0]}}
    return g


if __name__ == "__main__":
    a = sys.argv[1:]
    MODE = a[2] if len(a) > 2 else "turbo"
    b.LENGTH = LENGTH
    b.graph = graph
    b.run(a[0], int(a[1]) if len(a) > 1 else 3)
