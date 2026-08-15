#!/usr/bin/env python3
import argparse, csv, re, subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
REPO=HERE.parent
SRC=HERE/'test-cache-entries.c'
UTIL=REPO/'store-stride-prefetcher'/'until.c'
INC=REPO/'store-stride-prefetcher'
BIN=REPO/'bin'/'region-cache-entries'
RESULT=REPO/'res'/'region-prefetcher'/'cache-entries.csv'
RESULT.parent.mkdir(parents=True, exist_ok=True)
RES_RE=re.compile(r'RESULT active_regions=(\d+) anchor=(\d+) candidate=(\d+) trained_avg=(\d+) control_avg=(\d+) trained_hit_rate=([0-9.]+) control_hit_rate=([0-9.]+)')
def parse_range(s):
    out=[]
    for part in s.split(','):
        if '-' in part:
            a,b=map(int,part.split('-',1)); out.extend(range(a,b+1))
        elif part.strip(): out.append(int(part))
    return out
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--cpu',type=int,default=6)
    ap.add_argument('--rounds',type=int,default=1000)
    ap.add_argument('--active-regions',default='1-32,40,48,56,64')
    ap.add_argument('--output',default=str(RESULT))
    args=ap.parse_args()
    cmd=['gcc','-O2','-Wall','-Wextra','-march=armv8.5-a+predres',f'-DROUNDS={args.rounds}',f'-I{INC}',str(SRC),str(UTIL),'-o',str(BIN)]
    print('# build',' '.join(cmd)); subprocess.run(cmd,check=True)
    rows=[]
    for n in parse_range(args.active_regions):
        run=subprocess.run(['taskset','-c',str(args.cpu),str(BIN),str(n)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        m=RES_RE.search(run.stdout)
        if not m:
            print(run.stdout); raise SystemExit(f'parse failed n={n}')
        row={'active_regions':int(m.group(1)),'anchor':int(m.group(2)),'candidate':int(m.group(3)),
             'trained_avg':int(m.group(4)),'control_avg':int(m.group(5)),
             'trained_hit_rate':float(m.group(6)),'control_hit_rate':float(m.group(7))}
        row['survived']=row['trained_hit_rate']>=0.8 and row['control_hit_rate']<0.5
        rows.append(row)
        print(f"n={n:2d} train_avg={row['trained_avg']:3d} ctrl_avg={row['control_avg']:3d} train_hit={row['trained_hit_rate']:.2f} survived={row['survived']}")
    with open(args.output,'w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    print('# output',args.output)
if __name__=='__main__': main()
