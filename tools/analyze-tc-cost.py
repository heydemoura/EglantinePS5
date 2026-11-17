import re,sys
# analyze-tc-cost.py EDEN_LOG [T0 T1]: EDEN_NX2_TC_COST lines per 600-frame window, and their mean over windows ending in [T0,T1] s
f=sys.argv[1]; t0=float(sys.argv[2]) if len(sys.argv)>2 else 230; t1=float(sys.argv[3]) if len(sys.argv)>3 else 430
rows=[]
txt=open(f,errors='replace').read()
cre={float(m.group(1)):(int(m.group(2)),int(m.group(3))) for m in re.finditer(r'^\[ *([0-9.]+)\].*last 600 frames: created (\d+) images, deleted (\d+)',txt,re.M)}
up={float(m.group(1)):int(m.group(2)) for m in re.finditer(r'^\[ *([0-9.]+)\].*last 600 frames: \d+ uploads from guest memory, (\d+) MiB',txt,re.M)}
for m in re.finditer(r'^\[ *([0-9.]+)\].*cost last 600 frames: wall (\d+) ms .*thread CPU (-?\d+) ms, churn (\d+) ms.*\|(.*)$',txt,re.M):
    t=float(m.group(1)); slots={k:(float(a),int(b)) for k,a,b in re.findall(r' (\w+) ([0-9.]+) ms/(\d+)',m.group(5))}
    c=min(cre,key=lambda x:abs(x-t)) if cre else None; u=min(up,key=lambda x:abs(x-t)) if up else None
    rows.append(dict(t=t,wall=int(m.group(2)),cpu=int(m.group(3)),churn=int(m.group(4)),slots=slots,created=cre[c][0] if c else 0,deleted=cre[c][1] if c else 0,upMiB=up[u] if u else 0))
keys=['collect','destroy','insert','construct','allocate','view_create','refresh_new','upload','astc_dispatch','finish','refresh_old']
print('t     wall  cpu   churn  %wall %cpu  created deleted upMiB '+' '.join(k[:9] for k in keys))
sel=[]
for r in rows:
    pc=100*r['churn']/r['cpu'] if r['cpu']>0 else -1
    print(f"{r['t']:5.0f} {r['wall']:5d} {r['cpu']:5d} {r['churn']:5d} {100*r['churn']/r['wall']:5.1f} {pc:5.1f} {r['created']:7d} {r['deleted']:7d} {r['upMiB']:5d} "+' '.join(f"{r['slots'].get(k,(0,0))[0]:9.0f}" for k in keys))
    if t0<=r['t']<=t1: sel.append(r)
if sel:
    n=len(sel); W=sum(r['wall'] for r in sel); C=sum(r['churn'] for r in sel); CPU=sum(r['cpu'] for r in sel)
    print(f"mean {t0:.0f}-{t1:.0f}s over {n} windows: frame {W/n/600:.2f} ms, thread CPU {CPU/n/600:.2f} ms/frame ({100*CPU/W:.0f}% busy), churn {C/n/600:.2f} ms/frame = {100*C/W:.1f}% of wall, {100*C/CPU if CPU>0 else -1:.1f}% of thread CPU; created {sum(r['created'] for r in sel)/n:.0f}/600f, uploaded {sum(r['upMiB'] for r in sel)/n:.0f} MiB/600f")
    print('  per slot ms/600f: '+', '.join(f"{k} {sum(r['slots'].get(k,(0,0))[0] for r in sel)/n:.0f}" for k in keys))
