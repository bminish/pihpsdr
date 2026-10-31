import subprocess, sys, os, concurrent.futures as cf
SP=sys.argv[1]; TAUS=[0.2,0.5,1,2,4,6,10]
caps=['20260902-165826','20260903-190516','20260903-190715','20260903-190932','20260830-202743','20260903-190822','20260903-193105','20260902-234508','20260902-234624','20260930-180235','20260930-180949','20260930-180822','20260930-175445']
os.makedirs(f'{SP}/runs3',exist_ok=True)
jobs=[(c,t) for c in caps for t in TAUS]
def run(j):
    c,t=j; out=f'{SP}/runs3/{c}.{t}.csv'
    if not os.path.exists(out):
        subprocess.run(['test/diversity/devtools/run_ref',f'captures/divcap-{c}.divc','--ref','rade','--mode','sum','--weighting','flat','--follow','1','--tau',str(t),'--pace','20000','--out',out],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=900)
with cf.ThreadPoolExecutor(4) as ex: list(ex.map(run,jobs))
print('done',len(jobs))
