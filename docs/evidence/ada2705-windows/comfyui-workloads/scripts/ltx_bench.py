"""Same benchmark harness as wan_bench.py, but LTX-2.5 22B distilled (int8 convrot) image-to-video with audio latent, 8 distilled sigmas.
  python ltx_bench.py <label> [runs=3]        compare: python wan_bench.py --compare <labelA> <labelB>   (same results_<label>.json format)
832x480, 49 frames, 24 fps conditioning, seed = base + run number, start image = same 2K start image as the Wan test."""
import sys
import wan_bench as b

SIGMAS = "1.0, 0.99375, 0.9875, 0.98125, 0.975, 0.909375, 0.725, 0.421875, 0.0"


def graph(prefix, seed):
    return {
        "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "ltx-2.5-22b-distilled-transformer-comfy-int8-convrot.safetensors", "weight_dtype": "default"}},
        "2": {"class_type": "VAELoader", "inputs": {"vae_name": "ltx-2.5-video-vae-bf16.safetensors"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": "ltx-2.5-audio-vae-bf16.safetensors"}},
        "4": {"class_type": "CLIPLoader", "inputs": {"clip_name": "gemma4-12b-with-proj-ltx-2.5-comfy-int8-convrot.safetensors", "type": "ltxv", "device": "default"}},
        "5": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["4", 0], "text": b.POS}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["4", 0], "text": b.NEG}},
        "7": {"class_type": "LTXVConditioning", "inputs": {"positive": ["5", 0], "negative": ["6", 0], "frame_rate": 24.0}},
        "8": {"class_type": "LoadImage", "inputs": {"image": "bench_start.png"}},
        "9": {"class_type": "LTXVPreprocess", "inputs": {"image": ["8", 0], "img_compression": 35}},
        "10": {"class_type": "LTXVImgToVideo", "inputs": {"positive": ["7", 0], "negative": ["7", 1], "vae": ["2", 0], "image": ["9", 0], "width": b.W, "height": b.H,
                                                          "length": b.LENGTH, "batch_size": 1, "strength": 1.0}},
        "11": {"class_type": "LTXVEmptyLatentAudio", "inputs": {"frames_number": b.LENGTH, "frame_rate": 24, "batch_size": 1, "audio_vae": ["3", 0]}},
        "12": {"class_type": "LTXVConcatAVLatent", "inputs": {"video_latent": ["10", 2], "audio_latent": ["11", 0]}},
        "13": {"class_type": "LTXVDualCFGGuider", "inputs": {"model": ["1", 0], "positive": ["10", 0], "negative": ["10", 1], "video_cfg": 1.0, "audio_cfg": 1.0}},
        "14": {"class_type": "ManualSigmas", "inputs": {"sigmas": SIGMAS}},
        "15": {"class_type": "KSamplerSelect", "inputs": {"sampler_name": "euler"}},
        "16": {"class_type": "RandomNoise", "inputs": {"noise_seed": seed}},
        "17": {"class_type": "SamplerCustomAdvanced", "inputs": {"noise": ["16", 0], "guider": ["13", 0], "sampler": ["15", 0], "sigmas": ["14", 0], "latent_image": ["12", 0]}},
        "18": {"class_type": "LTXVSeparateAVLatent", "inputs": {"av_latent": ["17", 1]}},
        "19": {"class_type": "VAEDecodeTiled", "inputs": {"samples": ["18", 0], "vae": ["2", 0], "tile_size": 768, "overlap": 96, "temporal_size": 64, "temporal_overlap": 16}},
        "20": {"class_type": "SaveImage", "inputs": {"images": ["19", 0], "filename_prefix": prefix}},
    }


if __name__ == "__main__":
    b.graph = graph
    a = sys.argv[1:]
    b.run(a[0], int(a[1]) if len(a) > 1 else 3)
