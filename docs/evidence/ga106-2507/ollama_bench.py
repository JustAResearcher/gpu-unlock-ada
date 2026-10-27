#!/usr/bin/env python3
"""Ollama quantized-inference probe: prefill and decode tokens/s.

Each request starts with a unique nonce so Ollama cannot reuse a cached
prompt prefix; the prompt is ~1.5k tokens, generation is 256 tokens at
temperature 0 with a fixed seed. One warm-up request, then RUNS measured.
Prints one JSON object per line and a summary with medians.
"""
import json
import statistics
import sys
import urllib.request

MODEL = sys.argv[1] if len(sys.argv) > 1 else "llama3.2:3b"
RUNS = 5
URL = "http://127.0.0.1:11434/api/generate"
BODY = ("Le module noyau doit terminer sa transaction firmware avant que le pilote "
        "NVIDIA ne s'initialise, puis effectuer une réinitialisation FLR. ") * 60
OPTIONS = {"temperature": 0, "seed": 42, "num_predict": 256, "num_ctx": 4096}


def run(i):
    prompt = f"[requête {i}] {BODY}\nRésume ce texte en détail."
    req = urllib.request.Request(URL, data=json.dumps({
        "model": MODEL, "prompt": prompt, "stream": False, "options": OPTIONS,
        "keep_alive": "5m"}).encode(), headers={"Content-Type": "application/json"})
    r = json.load(urllib.request.urlopen(req, timeout=600))
    return {"run": i, "prompt_tokens": r["prompt_eval_count"],
            "prefill_tok_s": r["prompt_eval_count"] / (r["prompt_eval_duration"] / 1e9),
            "gen_tokens": r["eval_count"],
            "decode_tok_s": r["eval_count"] / (r["eval_duration"] / 1e9),
            "load_s": r["load_duration"] / 1e9, "total_s": r["total_duration"] / 1e9}


warm = run("warmup")
print(json.dumps({"warmup": warm}))
results = []
for i in range(RUNS):
    res = run(i)
    results.append(res)
    print(json.dumps(res))
print(json.dumps({"model": MODEL, "options": OPTIONS, "runs": RUNS,
                  "median_prefill_tok_s": round(statistics.median(r["prefill_tok_s"] for r in results), 2),
                  "median_decode_tok_s": round(statistics.median(r["decode_tok_s"] for r in results), 2)}))
