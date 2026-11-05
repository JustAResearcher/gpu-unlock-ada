#!/usr/bin/env python3
import pathlib,json,csv,statistics,collections
ROOT=pathlib.Path(__file__).resolve().parent
all_phases={}
for phase in ('stock','unlocked','recovered'):
 d=ROOT/phase
 if not (d/'COMPLETE').exists():continue
 results={}
 def add(key,unit,values,direction='higher'):
  results[key]={'unit':unit,'median':statistics.median(values),'min':min(values),'max':max(values),'samples':len(values),'better':direction}
 groups=collections.defaultdict(list)
 for p in d.glob('mma-*.txt'):
  for line in p.read_text().splitlines()[2:]:
   x=line.split()
   if len(x)==3:groups[x[0]].extend(float(v) for v in x[2].split('/'))
 for key,v in groups.items():add('MMA '+key,'TOP/s' if key in ('s8/s32','dp4a') else 'TF/s',v)
 groups=collections.defaultdict(list)
 for p in d.glob('gemm-*.jsonl'):
  for line in p.read_text().splitlines():
   r=json.loads(line);assert r['failures']==0
   groups[f"cuBLAS {r['mode']} {r['n']}^3"].extend(2*r['n']**3/(ms*1e9) for ms in r['times_ms'])
 for key,v in groups.items():add(key,'TF/s',v)
 groups=collections.defaultdict(list)
 for p in d.glob('tensor-*.json'):
  payload=json.loads(p.read_text())
  for mode,v in payload['modes'].items():
   assert v['mismatches']==0
   groups[mode].extend(v['rates_Top_per_s'])
 for key,v in groups.items():add('Verified tensor '+key,'TOP/s' if key=='u4_s32' else 'TF/s',v)
 for line in (d/'torch.jsonl').read_text().splitlines():
  r=json.loads(line)
  if 'name' not in r:continue
  add(r['name']+' '+str(r.get('shape','')),'ms',r['ms'],'lower')
 strata=d/'strata/results.jsonl'
 if strata.exists() and (d/'strata/COMPLETE').exists():
  groups=collections.defaultdict(list)
  for line in strata.read_text().splitlines():
   r=json.loads(line)
   if r['excluded_warmup']:continue
   assert r['done'] and r['timings']['cache_n']==0
   for metric in ['prompt_per_second','predicted_per_second']:
    groups[(r['case'],metric)].append(r['timings'][metric])
   for metric in ['ttft_s','elapsed_s']:groups[(r['case'],metric)].append(r[metric])
  for (case,metric),v in groups.items():add('Strata '+case+' '+metric,'s' if metric.endswith('_s') else 'tokens/s',v,'lower' if metric.endswith('_s') else 'higher')
 all_phases[phase]=results
(ROOT/'summary.json').write_text(json.dumps(all_phases,indent=2))
with (ROOT/'comparison.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['test','unit','stock_median','unlocked_median','recovered_median','speedup_ratio','stock_min','stock_max','stock_samples','speedup_vs_recovered','recovered_vs_stock_ratio','unlocked_min','unlocked_max','unlocked_samples','recovered_min','recovered_max','recovered_samples'])
 for key,r in all_phases.get('stock',{}).items():
  u=all_phases.get('unlocked',{}).get(key,{}).get('median');a=all_phases.get('recovered',{}).get(key,{}).get('median')
  speed=(u/r['median'] if r['better']=='higher' else r['median']/u) if u else ''
  ur=all_phases.get('unlocked',{}).get(key,{});ar=all_phases.get('recovered',{}).get(key,{})
  speed_recovered=(u/a if r['better']=='higher' else a/u) if u and a else ''
  w.writerow([key,r['unit'],r['median'],u if u else 'PENDING',a if a else 'PENDING',speed,r['min'],r['max'],r['samples'],speed_recovered,a/r['median'] if a else '',ur.get('min',''),ur.get('max',''),ur.get('samples',''),ar.get('min',''),ar.get('max',''),ar.get('samples','')])
print('Summarized:',', '.join(all_phases))
