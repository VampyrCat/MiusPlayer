"""Compares the per-second unit fingerprints written by each game instance (out\\mp_trace_*.txt).
Prints the first simulated second at which the instances disagree."""
import glob
import os
import re
import sys

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out')
files = sorted(glob.glob(os.path.join(out, 'mp_trace_*.txt')))
runs = []
for f in files:
    rows = {}
    for line in open(f):
        m = dict(kv.split('=', 1) for kv in line.split())
        if 'hash' in m:
            rows[int(m['sim']) // 1000] = m
    runs.append(rows)
    print('%s: %d seconds traced' % (os.path.basename(f), len(rows)))
if len(runs) < 2:
    sys.exit('need at least two traces')
common = sorted(set(runs[0]).intersection(*runs[1:]))
print('seconds present in all traces:', len(common))
same = 0
for sec in common:
    rows = [r[sec] for r in runs]
    if all(r['hash'] == rows[0]['hash'] and r['sim'] == rows[0]['sim'] for r in rows):
        same += 1
        continue
    print('first difference at simulated second %d (after %d identical seconds):' % (sec, same))
    for f, r in zip(files, rows):
        print('  %s sim=%s step=%s frame=%s units=%s hash=%s first=%s' % (
            os.path.basename(f), r['sim'], r['step'], r['frame'], r['units'], r['hash'], r['first']))
    break
else:
    print('all %d common seconds identical' % same)
show = common[:3] + common[-2:]
for sec in show:
    print(sec, [(r[sec]['sim'], r[sec]['step'], r[sec]['hash'][-8:], r[sec]['first']) for r in runs])
