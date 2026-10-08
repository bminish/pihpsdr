import subprocess, sys, os, re, json, csv, concurrent.futures as cf
sys.path.insert(0,'test/diversity/devtools/py')
from divc import open_divc
SP=sys.argv[1]; TAUS=[0.5,2,6]; RES=[24,12,6,3]
rows=[l.rstrip('\n').split('\t') for l in open(f'{SP}/catalogue.tsv')]
COH=('band','carrier','digital')
caps=[(r[0],r[5],int(r[3]),int(r[2]),int(r[1])) for r in rows if r[5] in COH and float(r[4])>=20 and int(r[1])==192000]
os.makedirs(f'{SP}/rs2',exist_ok=True)
def resample(src,dst,Pc,nb):
    R=list(csv.DictReader(open(src)))
    if len(R)<2: return False
    t=[float(x['t']) for x in R]; Pe=t[1]-t[0]
    out=[]; j=-1
    for k in range(nb):
        lim=(k+1)*Pc+1e-9
        while j+1<len(R) and t[j+1]+Pe<=lim: j+=1
        if j<0: out.append((0,0.0,1.0,0.0))
        else: out.append((int(R[j]['ok']),float(R[j]['quality']),float(R[j]['wr']),float(R[j]['wi'])))
    with open(dst,'w') as f:
        f.write('block,t,locked,confirming,quality,snr,freq_off,ok,wr,wi\n')
        for k,(ok,q,wr,wi) in enumerate(out): f.write(f'{k},{k*Pc:.4f},0,0,{q},0,0,{ok},{wr},{wi}\n')
    return True
def score(c):
    name,ref,nb,nfft,rate=c
    Pc=nfft/rate; res={}
    for tau in TAUS:
        files=[]
        for r in RES:
            src=f'{SP}/runs2/{name}.{ref}.r{r}.t{tau}.csv'; dst=f'{SP}/rs2/{name}.{ref}.r{r}.t{tau}.csv'
            if not os.path.exists(src) or not resample(src,dst,Pc,nb): files=None; break
            files.append(dst)
        if not files: continue
        p=subprocess.run(['python3','score_wideband.py',f'../../../../captures/{name}']+files,cwd='test/diversity/devtools/py',capture_output=True,text=True)
        vals=[]
        for line in p.stdout.splitlines():
            m=re.search(r'([+-]\d+\.\d+)\s+([+-]\d+\.\d+)\s+([+-]\d+\.\d+)\s+[\d.]+%\s+[\d.]+%',line)
            if m and not line.startswith('#'): vals.append(float(m.group(2)))
        if len(vals)==len(RES): res[str(tau)]=dict(zip(map(str,RES),vals))
    return name,ref,res
out={}
with cf.ThreadPoolExecutor(12) as ex:
    for name,ref,res in ex.map(score,caps):
        if res: out[name]=dict(ref=ref,snr=res)
json.dump(out,open(f'{SP}/score2.json','w')); print(len(out),'of',len(caps))
