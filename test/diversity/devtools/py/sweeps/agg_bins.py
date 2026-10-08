# Aggregate score_bins.py's output: bin width against 12 Hz at the same averaging.
import json, sys, statistics as st
SP = sys.argv[1]
d = json.load(open(f'{SP}/score2.json'))
R = ['24', '12', '6', '3']
for grp in ('all', 'band', 'digital', 'carrier'):
    L = [(n, v) for n, v in d.items() if grp == 'all' or v['ref'] == grp]
    print(f'\n== {grp} ({len(L)} captures): mean dB vs 12 Hz bins at the same averaging')
    print('tau     24 Hz            6 Hz             3 Hz     (mean / better>0.1 / worse>0.1)')
    for tau in ('0.5', '2', '6'):
        line = f'{tau:>4}  '
        for r in ('24', '6', '3'):
            ds = [v['snr'][tau][r] - v['snr'][tau]['12'] for n, v in L if tau in v['snr']]
            line += f'{st.mean(ds):+5.2f} {sum(x > 0.1 for x in ds):3d}/{sum(x < -0.1 for x in ds):3d}      '
        print(line)
