import os, struct, sys, glob
sys.path.insert(0,'test/diversity/devtools/py')
from divc import *
rows=[]
for p in sorted(glob.glob('captures/*.divc')):
    try:
        f,h=open_divc(p)
        blk=f.read(BLK)
        if len(blk)<BLK: continue
        m=block_meta(blk)
        size=os.path.getsize(p)
        nb=(size-HDR)//(BLK+h['block_bytes']) if h['block_bytes'] else 0
        dur=nb*h['nfft']/h['rate']
        rows.append((os.path.basename(p),h['rate'],h['nfft'],nb,round(dur,1),REFS.get(m['ref'],m['ref']),m['mode'],m['auto_mode'],round(m['tau'],2)))
    except Exception as e:
        print('ERR',p,e,file=sys.stderr)
for r in rows: print(*r,sep='\t')
