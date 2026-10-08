import subprocess, sys, os, re, json, concurrent.futures as cf
SP=sys.argv[1]; TAUS=[0.2,0.5,1,2,4,6,8,10]
rows=[l.rstrip('\n').split('\t') for l in open(f'{SP}/catalogue.tsv')]
caps=[(r[0],r[5]) for r in rows if r[5] in ('band','carrier','digital') and float(r[4])>=10]
def score(c):
    name,ref=c
    csvs=[f'{SP}/runs/{name}.{ref}.{t}.csv' for t in TAUS]
    if not all(os.path.exists(x) for x in csvs): return name,ref,None
    p=subprocess.run(['python3','score_wideband.py',f'../../../../captures/{name}']+csvs,cwd='test/diversity/devtools/py',capture_output=True,text=True)
    res={}
    vals=[]
    for line in p.stdout.splitlines():
        m=re.search(r'([+-]\d+\.\d+)\s+([+-]\d+\.\d+)\s+([+-]\d+\.\d+)\s+[\d.]+%\s+[\d.]+%',line)
        if m and not line.startswith('#'): vals.append((float(m.group(1)),float(m.group(2))))
    if len(vals)==len(TAUS):
        for t,v in zip(TAUS,vals): res[t]=v
    return name,ref,res
out={}
with cf.ThreadPoolExecutor(12) as ex:
    for name,ref,res in ex.map(score,caps):
        if res: out[name]=dict(ref=ref,snr={str(k):v for k,v in res.items()})
json.dump(out,open(f'{SP}/score1.json','w'))
print(len(out),'scored of',len(caps))
