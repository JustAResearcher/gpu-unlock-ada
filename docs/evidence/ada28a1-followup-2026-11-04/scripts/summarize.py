import json,sys,os,statistics as st,datetime as dt,collections
for d in sys.argv[1:]:
    env=dict(l.split('=',1) for l in open(f'{d}/env.txt').read().split('\n') if '=' in l)
    mma={l.split()[0]:float(l.split()[1]) for l in open(f'{d}/mma.txt') if l[:1].isalpha() and l.split()[0]!='mode'}
    t=json.load(open(f'{d}/tensor.json'))['modes']
    g={(r['n'],r['mode'][:4]):r['median_TF_s'] for r in map(json.loads,open(f'{d}/gemm.jsonl'))}
    fails=sum(r['failures'] for r in map(json.loads,open(f'{d}/gemm.jsonl')))
    num=open(f'{d}/numerical.txt').read().count('failures=0')
    steps=[(float(l.split()[0]),l.split()[2]) for l in open(f'{d}/steps.txt')]
    tele=[]
    for l in open(f'{d}/telemetry.csv'):
        p=[x.strip() for x in l.split(',')]
        try:
            ts=dt.datetime.strptime(p[0],'%Y/%m/%d %H:%M:%S.%f').timestamp()
            tele.append((ts,p[1],float(p[2].split()[0]),float(p[3].split()[0]),float(p[4].split()[0]),p[5],float(p[6]),p[7]))
        except Exception: pass
    print(f"\n### {os.path.basename(d)}  profile={env.get('profile')} ac={env.get('ac_online')} {env.get('dpm','').strip()} pci={env.get('pci_power_state')}")
    print(f"  mma f16/f32={mma.get('f16/f32')} bf16={mma.get('bf16/f32')} s8={mma.get('s8/s32')} fp8={mma.get('fp8/e4m3')} dp4a={mma.get('dp4a')} ffma={mma.get('ffma')}")
    print(f"  tensor best f16_f32={t['f16_f32']['best_Top_per_s']:.2f} tf32={t['tf32']['best_Top_per_s']:.2f} u4={t['u4_s32']['best_Top_per_s']:.1f} runs_f16={[round(x,1) for x in t['f16_f32']['rates_Top_per_s']]}")
    print("  gemm median "+' '.join(f"{n}{m}:{v:.2f}" for (n,m),v in g.items())+f"  failures={fails} validate_ok={num}")
    for (a,name),(b,_) in zip(steps,steps[1:]):
        w=[x for x in tele if a<=x[0]<b and x[1]!='P8']
        if not w: continue
        thr=collections.Counter(x[7] for x in w).most_common(3)
        print(f"  [{name:13}] n={len(w):3} SM mean={st.mean(x[2] for x in w):6.0f} min={min(x[2] for x in w):5.0f} mem={st.mean(x[3] for x in w):5.0f} W mean={st.mean(x[4] for x in w):5.1f} max={max(x[4] for x in w):5.1f} limit={w[0][5]} Tmax={max(x[6] for x in w):.0f} thr={thr}")
