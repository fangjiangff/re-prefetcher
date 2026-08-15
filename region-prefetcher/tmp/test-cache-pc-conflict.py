#!/usr/bin/env python3
import argparse,csv,re,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent; REPO=HERE.parent
SRC=HERE/'test-cache-pc-conflict.c'; UTIL=REPO/'store-stride-prefetcher'/'until.c'; INC=REPO/'store-stride-prefetcher'; BIN=REPO/'bin'/'region-cache-pc-conflict'
RESULT=REPO/'res'/'region-prefetcher'/'cache-pc-conflict.csv'; RESULT.parent.mkdir(parents=True,exist_ok=True)
RES_RE=re.compile(r'RESULT a_off=(\d+) b_off=(\d+) pc_delta=(-?\d+) a_pc=0x([0-9a-fA-F]+) b_pc=0x([0-9a-fA-F]+) probe_delta=(-?\d+) baseline_avg=(\d+) after_b_avg=(\d+) baseline_hit_rate=([0-9.]+) after_b_hit_rate=([0-9.]+)')
def parse_list(s):
    out=[]
    for part in s.split(','):
        part=part.strip()
        if not part: continue
        if '..' in part:
            a,b=map(int,part.split('..',1)); step=1 if a<=b else -1; out.extend(range(a,b+step,step))
        elif ':' in part:
            a,b,st=map(int,part.split(':',2)); out.extend(range(a,b+1,st))
        else: out.append(int(part))
    return out
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--cpu',type=int,default=6); ap.add_argument('--rounds',type=int,default=500); ap.add_argument('--a-off',type=int,default=0); ap.add_argument('--b-offs',default='0:4096:8'); ap.add_argument('--probe-delta',type=int,default=2); ap.add_argument('--output',default=str(RESULT)); args=ap.parse_args()
    cmd=['gcc','-O2','-Wall','-Wextra','-march=armv8.5-a+predres','-fno-pie','-no-pie',f'-DROUNDS={args.rounds}',f'-I{INC}',str(SRC),str(UTIL),'-o',str(BIN)]
    print('# build',' '.join(cmd)); subprocess.run(cmd,check=True)
    rows=[]
    for off in parse_list(args.b_offs):
        run=subprocess.run(['taskset','-c',str(args.cpu),str(BIN),str(args.a_off),str(off),str(args.probe_delta)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        m=RES_RE.search(run.stdout)
        if not m: print(run.stdout); raise SystemExit(f'parse failed b_off={off}')
        row={'a_off':int(m.group(1)),'b_off':int(m.group(2)),'pc_delta':int(m.group(3)),'a_pc':int(m.group(4),16),'b_pc':int(m.group(5),16),'probe_delta':int(m.group(6)),'baseline_avg':int(m.group(7)),'after_b_avg':int(m.group(8)),'baseline_hit_rate':float(m.group(9)),'after_b_hit_rate':float(m.group(10))}
        row['evicted']=row['baseline_hit_rate']>=0.8 and row['after_b_hit_rate']<0.5
        rows.append(row)
        print(f"b={off:5d} delta={row['pc_delta']:5d} base={row['baseline_hit_rate']:.2f}/{row['baseline_avg']:3d} after={row['after_b_hit_rate']:.2f}/{row['after_b_avg']:3d} evict={row['evicted']}")
    with open(args.output,'w',newline='') as f: w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    print('# output',args.output)
if __name__=='__main__': main()
