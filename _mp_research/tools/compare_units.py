"""Compares the unit position dumps (out\\mp_units_<pid>_<sim ms>.txt) of two instances."""
import collections
import glob
import math
import os
import re

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out')
by_time = collections.defaultdict(dict)
for f in glob.glob(os.path.join(out, 'mp_units_*.txt')):
    pid, sim = re.search(r'mp_units_(\d+)_(\d+)', f).groups()
    by_time[int(sim)][pid] = {l.split()[0]: tuple(map(float, l.split()[1:])) for l in open(f)}
for sim in sorted(by_time):
    runs = by_time[sim]
    if len(runs) < 2:
        continue
    (pa, a), (pb, b) = sorted(runs.items())[:2]
    diffs = []
    for name in a:
        if name in b:
            d = math.dist(a[name], b[name])
            if d > 0:
                diffs.append((d, name))
    diffs.sort(reverse=True)
    kinds = collections.Counter(n.split(':')[0] for _, n in diffs)
    print('sim %6d ms: %d units, %d differ %s, largest %.4f m' % (
        sim, len(a), len(diffs), dict(kinds), diffs[0][0] if diffs else 0))
    for d, name in diffs[:5]:
        print('     %-24s %.5f m  %s  vs  %s' % (name, d, a[name], b[name]))
