#!/usr/bin/env python3
import argparse, csv, re, subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
REPO=HERE.parent
SRC=HERE/'test-cache-direction.c'
UTIL=REPO/'store-stride-prefetcher'/'until.c'
INC=REPO/'store-stride-prefetcher'
BIN=REPO/'bin'/'region-cache-direction'
RESULT=REPO/'res'/'region-prefetcher'/'cache-direction.csv'
RESULT.parent.mkdir(parents=True, exist_ok=True)
RES_RE=re.compile(r'RESULT base=(\d+) second_trigger=(\d+) probe=(\d+) delta_from_second=(-?\d+) trained_avg=(\d+) control_avg=(\d+) trained_hit_rate=([0-9.]+) control_hit_rate=([0-9.]+)')
SECOND_TRIGGER_DELTA=15
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--cpu',type=int,default=6)
    ap.add_argument('--rounds',type=int,default=1000)
    ap.add_argument('--anchors',default='20')
    ap.add_argument('--deltas',default='-31..28')
    ap.add_argument('--output',default=str(RESULT))
    args=ap.parse_args()
    def parse_range(s):
        out=[]
        for part in s.split(','):
            part=part.strip()
            if not part:
                continue
            if '..' in part:
                a,b=map(int,part.split('..',1))
                step=1 if a<=b else -1
                out.extend(range(a,b+step,step))
            else:
                out.append(int(part))
        return out
    anchors=parse_range(args.anchors); deltas=parse_range(args.deltas)
    cmd=['gcc','-O2','-Wall','-Wextra','-march=armv8.5-a+predres',f'-DROUNDS={args.rounds}',f'-I{INC}',str(SRC),str(UTIL),'-o',str(BIN)]
    print('# build',' '.join(cmd)); subprocess.run(cmd,check=True)
    rows=[]
    for a in anchors:
        for d in deltas:
            c=a+SECOND_TRIGGER_DELTA+d
            if c<0 or c>=64 or c==a or c==a+SECOND_TRIGGER_DELTA: continue
            run=subprocess.run(['taskset','-c',str(args.cpu),str(BIN),str(a),str(c)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            m=RES_RE.search(run.stdout)
            if not m:
                print(run.stdout); raise SystemExit(f'parse failed anchor={a} cand={c}')
            row={'base':int(m.group(1)),'second_trigger':int(m.group(2)),'probe':int(m.group(3)),'delta_from_second':int(m.group(4)),
                 'trained_avg':int(m.group(5)),'control_avg':int(m.group(6)),
                 'trained_hit_rate':float(m.group(7)),'control_hit_rate':float(m.group(8))}
            row['prefetched']=row['trained_hit_rate']>=0.8 and row['control_hit_rate']<0.5
            rows.append(row)
            print(f"base={a:2d} second={a+SECOND_TRIGGER_DELTA:2d} d2={d:3d} probe={c:2d} train_avg={row['trained_avg']:3d} ctrl_avg={row['control_avg']:3d} train_hit={row['trained_hit_rate']:.2f} pref={row['prefetched']}")
    with open(args.output,'w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
    print('# output',args.output)
if __name__=='__main__': main()
