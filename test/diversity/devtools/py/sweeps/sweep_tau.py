# Phase 1: averaging sweep, Sum, flat, on wideband captures, at the capture's own resolution.
import subprocess, sys, os, re, csv, concurrent.futures as cf
SP=sys.argv[1]; TAUS=[0.2,0.5,1,2,4,6,8,10]
rows=[l.rstrip('\n').split('\t') for l in open(f'{SP}/catalogue.tsv')]
COH={'band':0.20,'carrier':0.30,'digital':0.30}
jobs=[]
for r in rows:
    name,rate,nfft,nb,dur,ref=r[:6]
    if ref not in COH or float(dur)<10: continue
    for tau in TAUS: jobs.append((name,ref,tau))
os.makedirs(f'{SP}/runs',exist_ok=True)
def run(j):
    name,ref,tau=j
    out=f'{SP}/runs/{name}.{ref}.{tau}.csv'
    if not os.path.exists(out):
        subprocess.run(['test/diversity/devtools/run_ref',f'captures/{name}','--ref',ref,'--mode','sum','--weighting','flat','--cohmin',str(COH[ref]),'--follow','1','--tau',str(tau),'--out',out],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=600)
    return j
with cf.ThreadPoolExecutor(16) as ex:
    n=0
    for j in ex.map(run,jobs):
        n+=1
        if n%50==0: print(n,len(jobs),flush=True)
print('done',len(jobs))
