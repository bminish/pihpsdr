# Phase 2: bin width x averaging, Window/Carrier/Digital Sum flat, 192k captures >= 20 s
import subprocess, sys, os, concurrent.futures as cf
SP=sys.argv[1]; TAUS=[0.5,2,6]; RES=[24,12,6,3]
rows=[l.rstrip('\n').split('\t') for l in open(f'{SP}/catalogue.tsv')]
COH={'band':0.20,'carrier':0.30,'digital':0.30}
jobs=[]
for r in rows:
    name,rate,nfft,nb,dur,ref=r[:6]
    if ref not in COH or float(dur)<20 or int(rate)!=192000: continue
    for res in RES:
        for tau in TAUS: jobs.append((name,ref,res,tau))
def run(j):
    name,ref,res,tau=j
    out=f'{SP}/runs2/{name}.{ref}.r{res}.t{tau}.csv'
    if not os.path.exists(out):
        subprocess.run(['test/diversity/devtools/run_ref',f'captures/{name}','--ref',ref,'--mode','sum','--weighting','flat','--cohmin',str(COH[ref]),'--follow','1','--tau',str(tau),'--resolution',str(res),'--out',out],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=900)
    return j
os.makedirs(f'{SP}/runs2',exist_ok=True)
with cf.ThreadPoolExecutor(16) as ex:
    for i,j in enumerate(ex.map(run,jobs)):
        if i%50==0: print(i,len(jobs),flush=True)
print('done',len(jobs))
