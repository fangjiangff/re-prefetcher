#!/usr/bin/env python3
import argparse,csv,re,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent; REPO=HERE.parent
SRC=HERE/'test-cache-pc-group-conflict.c'; UTIL=REPO/'store-stride-prefetcher'/'until.c'; INC=REPO/'store-stride-prefetcher'; BIN=REPO/'bin'/'region-cache-pc-group-conflict'
RESULT=REPO/'res'/'region-prefetcher'/'cache-pc-group-conflict.csv'; RESULT.parent.mkdir(parents=True,exist_ok=True)
RES_RE=re.compile(r'RESULT a_off=(\d+) stride=(\d+) count=(\d+) addend=(-?\d+) a_pc=0x([0-9a-fA-F]+) first_group_pc=0x([0-9a-fA-F]+) probe_delta=(-?\d+) baseline_avg=(\d+) after_group_avg=(\d+) baseline_hit_rate=([0-9.]+) after_group_hit_rate=([0-9.]+)')
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
    ap=argparse.ArgumentParser(); ap.add_argument('--cpu',type=int,default=6); ap.add_argument('--rounds',type=int,default=300); ap.add_argument('--train-regions',type=int,default=32); ap.add_argument('--a-off',type=int,default=0); ap.add_argument('--strides',default='8,16,32,64,128,256,512,1024,2048,4096,8192,16384'); ap.add_argument('--addends',default='0'); ap.add_argument('--counts',default='4,8,12,16,24,32,48,64'); ap.add_argument('--probe-delta',type=int,default=2); ap.add_argument('--output',default=str(RESULT)); args=ap.parse_args()
    cmd=['gcc','-O2','-Wall','-Wextra','-march=armv8.5-a+predres','-fno-pie','-no-pie',f'-DROUNDS={args.rounds}',f'-DTRAIN_REGIONS={args.train_regions}',f'-I{INC}',str(SRC),str(UTIL),'-o',str(BIN)]
    print('# build',' '.join(cmd)); subprocess.run(cmd,check=True)
    rows=[]
    for stride in parse_list(args.strides):
        for addend in parse_list(args.addends):
          for count in parse_list(args.counts):
            run=subprocess.run(['taskset','-c',str(args.cpu),str(BIN),str(args.a_off),str(stride),str(count),str(args.probe_delta),str(addend)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            m=RES_RE.search(run.stdout)
            if not m: print(run.stdout); raise SystemExit(f'parse failed stride={stride} addend={addend} count={count}')
            row={'a_off':int(m.group(1)),'stride':int(m.group(2)),'count':int(m.group(3)),'addend':int(m.group(4)),'a_pc':int(m.group(5),16),'first_group_pc':int(m.group(6),16),'probe_delta':int(m.group(7)),'baseline_avg':int(m.group(8)),'after_group_avg':int(m.group(9)),'baseline_hit_rate':float(m.group(10)),'after_group_hit_rate':float(m.group(11))}
            row['evicted']=row['baseline_hit_rate']>=0.8 and row['after_group_hit_rate']<0.5
            rows.append(row)
            print(f"stride={stride:5d} add={addend:4d} count={count:2d} base={row['baseline_hit_rate']:.2f}/{row['baseline_avg']:3d} after={row['after_group_hit_rate']:.2f}/{row['after_group_avg']:3d} evict={row['evicted']}")
    with open(args.output,'w',newline='') as f: w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    print('# output',args.output)
if __name__=='__main__': main()
