import json,sys,statistics as st
SP=sys.argv[1]; d=json.load(open(f'{SP}/score1.json'))
T=['0.2','0.5','1.0','2.0','4.0','6.0','8.0','10.0']
def key(t): return str(float(t))
by={}
for n,v in d.items():
    s={key(k):x[1] for k,x in v['snr'].items()}   # vs better arm
    if '6.0' not in s: continue
    by.setdefault(v['ref'],[]).append((n,s))
    by.setdefault('all',[]).append((n,s))
for grp,L in by.items():
    print(f'\n== {grp}: {len(L)} captures; mean change in dB vs the better arm, relative to 6 s')
    print('tau   mean   median  better>0.1  worse>0.1  worst   best')
    for t in T:
        ds=[s[t]-s['6.0'] for n,s in L if t in s]
        if not ds: continue
        print(f'{t:>5} {st.mean(ds):+6.2f} {st.median(ds):+6.2f}   {sum(x>0.1 for x in ds):4d}     {sum(x<-0.1 for x in ds):4d}     {min(ds):+6.2f} {max(ds):+6.2f}')
    # best tau distribution
    from collections import Counter
    c=Counter(max(T,key=lambda t:s.get(t,-99)) for n,s in L)
    print('best tau per capture:',dict(sorted(c.items(),key=lambda x:float(x[0]))))
    # oracle gain
    print('mean gain of per-capture best over fixed 6 s: %+.2f dB'%st.mean(max(s.values())-s['6.0'] for n,s in L))
    for cap in (6.0,10.0):
        # best allowed tau <=cap vs best any
        pass
